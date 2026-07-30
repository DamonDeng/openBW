// See viewer_window.h for the design.
//
// IMPORTANT: this .cpp deliberately does NOT include bwgame.h or
// ui.h. `korean.h` (pulled in by bwgame.h) defines uint16_t data
// tables at namespace scope without `inline`, so if two .cpp files
// in the same target both included it, the linker sees duplicate
// symbols. The rule for this target: only sim_harness.cpp touches
// openBW headers; everything else goes through SimHarness's opaque
// interface.

#include "viewer_window.h"
#include "sim_harness.h"
#include "hd_asset_loader.h"
#include "mapping_tab.h"
#include "frames_tab.h"

#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtGui/QPainter>
#include <QtGui/QPaintEvent>
#include <QtGui/QImage>
#include <QtWidgets/QApplication>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <cstring>

namespace sprite_viewer {

// -------------------------------------------------------------------
// SpriteCanvas: right-hand pane. On paint, asks SimHarness for the
// current frame's pixels (already rendered by openBW's draw_sprite
// into an RGBA surface) and blits them onto this widget.

// One instance renders one mode. We put two of them side by side --
// the SD canvas skips HD and always shows the classic openBW-rendered
// bitmap; the HD canvas skips classic and only paints HD. Both share
// the same SimHarness so iscript state advances in lockstep and the
// two views represent the same tick.
enum class CanvasMode { SD, HD };

// No global HD scale constant: the correct per-sprite scale is
// derived from the ratio SD_grp_width / HD_sprite_w so a Marine's
// HD render occupies exactly the same on-widget pixels as its SD
// render. See paint_hd_image() for the derivation.

class SpriteCanvas : public QWidget {
public:
	// Nearest-neighbor upscale factor. 3x lets you see individual
	// pixels of the sprite without them being too small; higher
	// values just eat screen real estate for no additional detail.
	// Applied only in classic mode; HD sprites are already
	// high-resolution so we draw them 1:1.
	static constexpr int SCALE = 3;

	explicit SpriteCanvas(ViewerWindow* owner_, CanvasMode mode,
	                      QWidget* parent = nullptr)
		: QWidget(parent), owner(owner_), sim(owner_->sim.get()),
		  mode_(mode) {
		setAttribute(Qt::WA_OpaquePaintEvent);
		setAttribute(Qt::WA_NoSystemBackground);
		// Small minimum so both canvases fit side by side on a
		// laptop screen. Startup uses sizeHint() below; user can
		// resize freely. HD canvas is 2x the SD baseline so the
		// hi-res sprites are readable without zooming; the
		// internal tile/sprite ratio (see paint_hd_image) is
		// derived from widget size so doubling here scales both
		// together and preserves proportions.
		const int min_side = (mode_ == CanvasMode::HD) ? 640 : 320;
		setMinimumSize(min_side, min_side);
		background_color = QColor(0x80, 0x80, 0x80);
	}

	QSize sizeHint() const override {
		return (mode_ == CanvasMode::HD) ? QSize(960, 800)
		                                 : QSize(480, 400);
	}
	QSize minimumSizeHint() const override {
		return (mode_ == CanvasMode::HD) ? QSize(640, 640)
		                                 : QSize(320, 320);
	}

	void request_repaint() { update(); }

protected:
	// HD paint path: draw the currently-selected frame from the
	// HdSprite's diffuse atlas, centered on the widget. `frame_idx`
	// comes from iscript state via SimHarness (image->frame_index).
	// Returns true iff HD rendering was attempted (so classic can
	// be skipped this tick).
	// Draw one HD image (shadow, body, overlay, ...) at the widget
	// center. Returns false iff no HD sprite is available for the
	// given grp_filename_index (caller falls back to whatever the
	// SD tile background already contains).
	bool paint_hd_image(QPainter& p,
	                    const SimHarness::SpriteImage& img)
	{
		if (img.hidden || img.grp_filename_index < 0) return false;
		HdAssetLoader* loader = owner ? owner->hd_loader.get() : nullptr;
		if (!loader) return false;

		HdSprite* hd = nullptr;

		// Shadows: images.rel does contain rows for shadow image_ids
		// (SCV shadow is image_id=248), but those rows are marked
		// SD-only (flag=0x1, anim_num=0xffffffff) -- SC:R keeps the
		// actual HD shadow anim as an orphan main_NNN.anim file with
		// no images.rel entry. Direct image_id -> anim_num lookup
		// therefore returns nothing.
		//
		// Retail pattern (verified via casc_probe --anim-range on
		// Marine, SCV, Tank, Vulture, Academy): the shadow lives at
		// body_anim_num + 1 (usually), with only the diffuse layer
		// populated, and with the same sprite bounding box as the
		// body. Neighborhood-probe it via shadow_sprite_for(), seeded
		// with the body's sc_r_row.
		if (img.is_shadow) {
			// Find the body image (is_shadow=false) on this sprite
			// so we know which HD anim to seed the neighborhood
			// search from. draw_sprite iterates shadow first, so the
			// body is later in the list.
			if (!sim) return false;
			auto sibs = sim->current_sprite_images();
			int body_bw_id = -1;
			for (const auto& s : sibs) {
				if (!s.is_shadow && !s.hidden
				    && s.grp_filename_index >= 0) {
					body_bw_id = s.grp_filename_index;
					break;
				}
			}
			if (body_bw_id < 0) {
				std::fprintf(stderr,
					"[hd-dbg] shadow: no body sibling on sprite\n");
				return false;
			}
			int body_sc_r = loader->sc_r_row_for_bw_id(body_bw_id);
			if (body_sc_r < 0) {
				std::fprintf(stderr,
					"[hd-dbg] shadow: body bw_id=%d has no sc_r_row\n",
					body_bw_id);
				return false;
			}
			hd = loader->shadow_sprite_for(body_sc_r);
			if (!hd) {
				std::fprintf(stderr,
					"[hd-dbg] shadow: shadow_sprite_for(body_sc_r=%d) "
					"returned null\n", body_sc_r);
				return false;
			}
		} else {
			int sc_r = loader->sc_r_row_for_bw_id(
				img.grp_filename_index);
			if (sc_r < 0) return false;

			// Cache HD sprites per (grp_filename_index) so
			// subsequent frames of the same anim reuse the
			// decoded atlas.
			auto& cache = owner->hd_image_cache;
			auto it = cache.find(img.grp_filename_index);
			if (it == cache.end()) {
				auto sp = loader->load_sprite(sc_r);
				if (!sp) return false;
				it = cache.emplace(img.grp_filename_index,
					std::move(sp)).first;
			}
			hd = it->second.get();
		}
		if (!hd || hd->frames.empty()) return false;

		int f = img.frame_index;
		if (f < 0 || f >= (int)hd->frames.size()) f = 0;
		const auto& fr = hd->frames[f];

		// Prefer composited (diffuse + teamcolor tint). Shadow
		// images ignore teamcolor -- they're solid black masks --
		// but our composited path leaves non-teamcolor pixels
		// untouched, so it's safe to always use it when present.
		const QImage& src_atlas =
			hd->composited.isNull() ? hd->diffuse : hd->composited;
		QImage sub = src_atlas.copy(
			fr.atlas_x, fr.atlas_y, fr.w, fr.h);
		if (img.flipped) sub = sub.mirrored(true, false);

		// Anchor: place the sprite at the same visual center as
		// the SD viewport (centered square). paint_hd() computed
		// hd_vp_scale_ / hd_vp_ox_ / hd_vp_oy_ from the SD tile
		// surface size.
		double cx = hd_vp_ox_ + (width()  - 2 * hd_vp_ox_) / 2.0;
		double cy = hd_vp_oy_ + (height() - 2 * hd_vp_oy_) / 2.0;

		// HD sprite scale: pixel-to-pixel with the HD terrain.
		//
		// HD megatile is 128 atlas pixels wide, painted into a
		// (32 * vp_scale) widget rect -- so one HD tile pixel
		// takes up (vp_scale / 4) widget pixels. To keep the
		// sprite at the same pixel density as the ground it
		// stands on, one HD sprite pixel must also take up
		// (vp_scale / 4) widget pixels.
		//
		//   s = vp_scale / 4
		//
		// This falls out of SC:R's HD tier: sprites (2048x1520
		// atlas for Marine) and tiles (128x128 megatile) both
		// authored at ~4x SD reference. If we were using HD2
		// instead (1024x760 sprite, 64x64 tile), the ratio would
		// be vp_scale / 2. Keep in sync with the tileset path
		// selected in hd_asset_loader.cpp::open_hd_tileset.
		double s = hd_vp_scale_ / 4.0;

		// Two offsets contribute to placement:
		//   * fr.offset_{x,y}: per-frame atlas anchor. Positions the
		//     frame within the sprite's own bounding box.
		//   * img.offset_{x,y}: image_t draw offset. openBW's
		//     sprite_t owns a list of image_t entries (body,
		//     shadow, overlays); each image_t carries its own
		//     x/y offset from the sprite center in *SD* pixels
		//     (image.dat "Draw start" field). Shadows typically
		//     have offset_y>0 to sit below the body. Without
		//     applying this, the shadow lands on top of the body
		//     instead of under it.
		//
		// image_t offsets are authored in SD pixels; HD sprite pixels
		// are 4x SD (the same ratio that s = vp_scale/4 accounts for
		// for atlas geometry). Multiply img.offset by 4 first so its
		// units match hd->sprite_w / fr.offset. Then multiply the
		// combined offset by s to convert to widget pixels.
		int img_off_x_hd = (int)img.offset_x * 4;
		int img_off_y_hd = (int)img.offset_y * 4;
		double dx, dy;
		if (img.flipped) {
			dx = cx + s * ((int)hd->sprite_w / 2
			        - ((int)fr.offset_x + (int)fr.w)
			        - img_off_x_hd);
		} else {
			dx = cx + s * ((int)fr.offset_x
			        - (int)hd->sprite_w / 2
			        + img_off_x_hd);
		}
		dy = cy + s * ((int)fr.offset_y
		        - (int)hd->sprite_h / 2
		        + img_off_y_hd);
		QRectF dst(dx, dy, fr.w * s, fr.h * s);

		// Shadow blend: openBW's SD renderer uses modifier=10 with
		// a dark-palette LUT that yields translucent black.
		// Approximation for HD: draw a black silhouette at reduced
		// alpha keyed off the source alpha channel. Preserves the
		// sprite shape while darkening the underlying tiles rather
		// than showing the raw diffuse.
		if (img.is_shadow) {
			QImage silhouette(sub.size(),
				QImage::Format_RGBA8888);
			silhouette.fill(0x00000000);
			for (int y = 0; y < sub.height(); ++y) {
				const uint8_t* src = sub.scanLine(y);
				uint8_t* dst_scanline = silhouette.scanLine(y);
				for (int x = 0; x < sub.width(); ++x) {
					uint8_t a = src[x * 4 + 3];
					if (a == 0) continue;
					dst_scanline[x * 4 + 0] = 0;
					dst_scanline[x * 4 + 1] = 0;
					dst_scanline[x * 4 + 2] = 0;
					dst_scanline[x * 4 + 3] =
						(uint8_t)((int)a * 140 / 255);
				}
			}
			p.drawImage(dst, silhouette);
		} else {
			p.drawImage(dst, sub);
		}
		return true;
	}

	bool paint_hd(QPainter& p) {
		HdSprite* hd = owner ? owner->current_hd : nullptr;
		if (!hd || hd->frames.empty()) return false;

		// Ground + sprite share one viewport: a centered square
		// scaled to fit whichever side of the widget is shorter.
		// This keeps HD's aspect ratio 1:1 with SD (whose render
		// surface is 256x256). Without this, a non-square widget
		// stretches tiles + sprite differently (tiles by widget-
		// aspect, sprite by kHdScale) and the two drift apart.
		double vp_scale = 1.0;   // widget px per surface px
		double vp_ox = 0.0;      // widget-space viewport origin
		double vp_oy = 0.0;
		if (sim) {
			auto tile_pixels =
				sim->render_frame(/*draw_sprite=*/false);
			if (tile_pixels.width > 0 && tile_pixels.height > 0) {
				// Mirror SD's scale rule (SpriteCanvas paint at
				// mode_==SD): start at SCALE=3 and shrink down
				// (in integer steps) until the surface fits.
				// Locking to the same integer scale keeps HD
				// tiles the same size as SD tiles on the same
				// widget -- without it, HD widget-fill diverges
				// from SD's centered 768x768 box.
				int scale = SCALE;
				while (scale > 1
				       && (tile_pixels.width  * scale > width()
				        || tile_pixels.height * scale > height()))
				{
					--scale;
				}
				vp_scale = (double)scale;
				vp_ox = (width()  - tile_pixels.width  * vp_scale) / 2.0;
				vp_oy = (height() - tile_pixels.height * vp_scale) / 2.0;
			}

			HdAssetLoader* loader = owner
				? owner->hd_loader.get() : nullptr;
			bool used_hd_tiles = false;

			if (loader && loader->has_hd_tileset()) {
				auto cells = sim->visible_tiles();
				if (tile_pixels.width > 0 && tile_pixels.height > 0) {
					p.setRenderHint(
						QPainter::SmoothPixmapTransform, true);
					for (const auto& c : cells) {
						QImage mt = loader->hd_megatile(
							c.megatile_index);
						if (mt.isNull()) continue;
						QRectF dst(
							vp_ox + c.screen_x * vp_scale,
							vp_oy + c.screen_y * vp_scale,
							32 * vp_scale, 32 * vp_scale);
						p.drawImage(dst, mt);
					}
					used_hd_tiles = true;
				}
			}

			if (!used_hd_tiles) {
				if (tile_pixels.data && tile_pixels.width > 0
				    && tile_pixels.height > 0)
				{
					QImage tiles(
						reinterpret_cast<const uchar*>(
							tile_pixels.data),
						tile_pixels.width, tile_pixels.height,
						tile_pixels.pitch,
						QImage::Format_ARGB32);
					p.setRenderHint(
						QPainter::SmoothPixmapTransform, true);
					// Uniform-scale into the centered viewport
					// rect so the tile aspect stays 1:1 (matches
					// SD canvas).
					p.drawImage(
						QRectF(vp_ox, vp_oy,
						       tile_pixels.width  * vp_scale,
						       tile_pixels.height * vp_scale),
						tiles);
				}
			}
		}
		// Stash the viewport transform for the sprite paint below.
		hd_vp_scale_ = vp_scale;
		hd_vp_ox_    = vp_ox;
		hd_vp_oy_    = vp_oy;

		// Walk every image_t on the sprite in draw order (shadow
		// deepest, body next, overlays last). Match openBW's
		// draw_sprite iteration. Individual images that lack an
		// HD mapping are simply skipped; the SD tile background
		// already contains what they would have looked like in
		// classic mode.
		if (!sim) return false;
		auto images = sim->current_sprite_images();
		// One-shot per-unit dump: emit the sprite's image list the
		// first time we see each distinct (image_id) tuple so we can
		// tell whether the sim is emitting a shadow entry at all.
		{
			static int last_first_iid = -0x7fffffff;
			int first_iid = images.empty() ? -1 : images.front().image_id;
			if (first_iid != last_first_iid) {
				last_first_iid = first_iid;
				int shadow_count = 0;
				for (const auto& im : images) {
					if (im.is_shadow) ++shadow_count;
				}
				std::fprintf(stderr,
					"[hd-dbg] sprite image list changed: n=%zu "
					"shadow_entries=%d\n",
					images.size(), shadow_count);
				for (const auto& im : images) {
					std::fprintf(stderr,
						"    image_id=%d gfi=%d frame=%d shadow=%d "
						"hidden=%d off=(%d,%d) flip=%d\n",
						im.image_id, im.grp_filename_index,
						im.frame_index, (int)im.is_shadow,
						(int)im.hidden, im.offset_x, im.offset_y,
						(int)im.flipped);
				}
			}
		}
		for (const auto& img : images) paint_hd_image(p, img);
		return true;
	}

	void paintEvent(QPaintEvent*) override {
		QPainter p(this);
		p.fillRect(rect(), background_color);
		if (!sim) return;

		// HD-only canvas: attempt HD paint; if no HD sprite is
		// loaded for the current unit, leave the panel blank
		// (a placeholder gray square) rather than fall back to SD.
		// The SD canvas is right next door if the user needs SD.
		if (mode_ == CanvasMode::HD) {
			paint_hd(p);
			return;
		}

		// SD canvas below: skip HD entirely and always paint the
		// classic sprite. sim->render_frame() drives openBW's own
		// draw_sprite into an indexed surface, then converts to
		// RGBA -- exactly what the pre-split viewer did.
		auto pixels = sim->render_frame();
		if (!pixels.data || pixels.width <= 0 || pixels.height <= 0) {
			// Not ready yet (first-frame lazy init); Qt will call
			// paintEvent again on the next tick.
			return;
		}

		// Format_ARGB32 matches Qt's Format_ARGB32 (BGRA byte order
		// on little-endian) as documented in
		// qt_native_window.cpp:186-189. Wrap the raw window-surface
		// bytes into a QImage view -- no copy yet.
		QImage view(reinterpret_cast<const uchar*>(pixels.data),
		            pixels.width, pixels.height, pixels.pitch,
		            QImage::Format_ARGB32);
		// SimHarness::render_frame now paints the map tiles into
		// the surface before compositing the sprite, so palette
		// index 0 (pure black) no longer appears as a rectangular
		// backdrop and we can draw the surface as-is. Keep the
		// QImage as a read-only view over the surface bytes -- Qt
		// won't hold the pointer past the drawImage call below.
		QImage& snap = view;

		// Nearest-neighbor upscale via drawImage with an explicit
		// target rect. SmoothPixmapTransform=false forces nearest
		// (each source pixel becomes a NxN block of the same color).
		// Auto-fit the scale to whatever the widget can hold: prefer
		// SCALE (3x = pixel-clear) when there's room, otherwise
		// shrink so the sprite always fits. Enables small windows
		// on laptop screens without cropping the sprite.
		p.setRenderHint(QPainter::SmoothPixmapTransform, false);
		int scale = SCALE;
		while (scale > 1 && (pixels.width * scale > width()
		                     || pixels.height * scale > height()))
		{
			--scale;
		}
		int scaled_w = pixels.width * scale;
		int scaled_h = pixels.height * scale;
		int dx = (width() - scaled_w) / 2;
		int dy = (height() - scaled_h) / 2;
		p.drawImage(QRect(dx, dy, scaled_w, scaled_h), snap);
	}

private:
	ViewerWindow* owner;
	SimHarness* sim;
	CanvasMode mode_;
	QColor background_color;

	// HD viewport transform captured by paint_hd() and consumed
	// by paint_hd_image() so tiles + sprite share a single scale
	// + origin. Keeps HD's aspect ratio 1:1 regardless of the
	// widget's outer aspect. See paint_hd for the derivation.
	double hd_vp_scale_ = 1.0;
	double hd_vp_ox_    = 0.0;
	double hd_vp_oy_    = 0.0;
};

// -------------------------------------------------------------------
// ViewerWindow: main window.

// Names for iscript_anims (from data_types.h:446). Mirrored here so
// we don't pull sim_harness.cpp's copy through the header.
static const char* const ANIM_LABELS[28] = {
	"Init", "Death", "GndAttkInit", "AirAttkInit",
	"Unused1", "GndAttkRpt", "AirAttkRpt", "CastSpell",
	"GndAttkToIdle", "AirAttkToIdle", "Unused2", "Walking",
	"WalkingToIdle", "SpecialState1", "SpecialState2", "AlmostBuilt",
	"Built", "Landing", "LiftOff", "IsWorking",
	"WorkingToIdle", "WarpIn", "Unused3", "StarEditInit",
	"Disable", "Burrow", "UnBurrow", "Enable",
};

ViewerWindow::ViewerWindow(std::string data_path_,
                           std::string map_relpath_,
                           QString sc_version_,
                           QString sc_remastered_path_,
                           QWidget* parent)
	: QMainWindow(parent),
	  data_path(std::move(data_path_)),
	  map_relpath(std::move(map_relpath_)),
	  sc_version(std::move(sc_version_)),
	  sc_remastered_path(std::move(sc_remastered_path_)) {
	setWindowTitle("openBW sprite viewer");
	// Big enough to fit the 3x-upscaled 256x256 render surface
	// (= 768x768) plus the left control column (~220 px wide).
	// Startup size: left column ~220px + two canvases at ~480px
	// each + margins. Fits on a 1440-wide laptop screen. Both
	// canvases scale their content to whatever size they get
	// (SD auto-picks a 1x..3x nearest-neighbor scale; HD draws
	// 1:1 and lets the widget clip if too small), and the
	// window is freely resizable.
	resize(1280, 720);

	// --- unit registry ---
	// See bwenums.h for the openBW UnitTypes enum ordering. Units
	// here are drawn in whichever mode is active. In remastered
	// mode, unit_type -> flingy -> sprite -> image_id is looked up
	// via SimHarness::current_image_id() and then routed through
	// the HD mapping table -- so as long as tools/hd_mapping_table.json
	// has an entry for that image_id, the unit renders in HD.
	races.push_back({"Terran", {
		{"Marine",         /*Terran_Marine*/                0},
		{"Ghost",          /*Terran_Ghost*/                 1},
		{"Vulture",        /*Terran_Vulture*/               2},
		{"Goliath",        /*Terran_Goliath*/               3},
		{"Siege Tank",     /*Terran_Siege_Tank_Tank_Mode*/  5},
		{"SCV",            /*Terran_SCV*/                   7},
		{"Wraith",         /*Terran_Wraith*/                8},
		{"Science Vessel", /*Terran_Science_Vessel*/        9},
		{"Dropship",       /*Terran_Dropship*/             11},
		{"Battlecruiser",  /*Terran_Battlecruiser*/        12},
		{"Firebat",        /*Terran_Firebat*/              32},
		{"Medic",          /*Terran_Medic*/                34},
		{"Valkyrie",       /*Terran_Valkyrie*/             58},
		{"Command Center", /*Terran_Command_Center*/      106},
		{"Supply Depot",   /*Terran_Supply_Depot*/        109},
		{"Refinery",       /*Terran_Refinery*/            110},
		{"Barracks",       /*Terran_Barracks*/            111},
		{"Academy",        /*Terran_Academy*/             112},
		{"Factory",        /*Terran_Factory*/             113},
		{"Starport",       /*Terran_Starport*/            114},
		{"Science Facility", /*Terran_Science_Facility*/  116},
	}});

	// --- widgets ---
	// Two-tab layout: "Classic" hosts the original per-unit
	// anim browser; "HD Mapping" is the eyeball-driven
	// images.rel row -> openBW unit_name matcher. Both tabs share
	// the SimHarness + HdAssetLoader owned by this window.
	auto* tabs = new QTabWidget(this);
	setCentralWidget(tabs);

	auto* classic = new QWidget();
	auto* root = new QHBoxLayout(classic);

	auto* left = new QVBoxLayout();
	left->addWidget(new QLabel("Race"));
	race_cb = new QComboBox();
	for (const auto& r : races) race_cb->addItem(r.label);
	left->addWidget(race_cb);

	left->addWidget(new QLabel("Unit"));
	unit_cb = new QComboBox();
	left->addWidget(unit_cb);

	left->addWidget(new QLabel("Action"));
	anim_cb = new QComboBox();
	left->addWidget(anim_cb);

	left->addWidget(new QLabel("Direction (0=up, 4=right, 8=down, 12=left)"));
	dir_slider = new QSlider(Qt::Horizontal);
	dir_slider->setRange(0, 16);
	dir_slider->setValue(current_dir);
	left->addWidget(dir_slider);

	playpause_btn = new QPushButton("⏸ Pause");
	left->addWidget(playpause_btn);

	readout = new QLabel("");
	readout->setStyleSheet("font-family: monospace; color: #689;");
	readout->setWordWrap(true);
	left->addWidget(readout);
	left->addStretch(1);

	auto* info = new QLabel(
		"Uses openBW's real iscript.bin + rendering. Actions are\n"
		"driven by iscript_run_anim; direction adjusts the sprite\n"
		"heading which selects a directional frameset per frame.");
	info->setStyleSheet("color: #667; font-size: 11px;");
	info->setWordWrap(true);
	left->addWidget(info);

	root->addLayout(left, 0);

	// Boot the sim.
	sim = std::make_unique<SimHarness>();
	try {
		sim->boot(data_path, map_relpath);
	} catch (const std::exception& e) {
		qCritical() << "sim boot failed:" << e.what();
		readout->setText(QString("Boot failed: %1").arg(e.what()));
		sim.reset();
	}

	// HD mode: open the user's SC:R install via CascLib. Failure to
	// open is a soft error -- we fall back to classic rendering and
	// surface the reason in the readout label.
	if (sim && sc_version == "remastered") {
		hd_loader = std::make_unique<HdAssetLoader>();
		if (!hd_loader->open(sc_remastered_path)) {
			qWarning() << "HD open failed:"
			           << hd_loader->last_error();
			readout->setText(
				QString("HD mode disabled: %1")
					.arg(hd_loader->last_error()));
			hd_loader.reset();
		} else {
			// Optional runtime mapping table (bw_id -> sc_r_row)
			// produced by tools/validate_hd_mapping.py. Absence
			// falls back to the hardcoded 2-unit table below so
			// the Classic tab still shows Marine + SCV in HD.
			QString table_path = QCoreApplication::applicationDirPath()
				+ "/../../tools/hd_mapping_table.json";
			if (hd_loader->load_mapping_table(table_path)) {
				qInfo() << "HD mapping table loaded from"
				        << table_path;
			} else {
				qInfo() << "HD mapping table not loaded ("
				        << hd_loader->last_error()
				        << "); using hardcoded fallback";
			}
			// Open the HD tileset for whatever map SimHarness
			// booted with. Failure is soft -- HD canvas falls
			// back to the stretched SD tile surface as before.
			int ti = sim ? sim->tileset_index() : -1;
			if (ti >= 0 && hd_loader->open_hd_tileset(ti)) {
				qInfo() << "HD tileset loaded (index" << ti << ")";
			} else {
				qInfo() << "HD tileset not loaded:"
				        << hd_loader->last_error();
			}
		}
	}

	// Two canvases side-by-side: SD on the left (openBW's classic
	// GRP+palette blitter), HD on the right (SC:R diffuse atlas
	// via HdAssetLoader). Both share the same SimHarness so the
	// two views advance in lockstep -- easier to spot HD anim
	// bugs against the SD ground truth.
	{
		auto* pair = new QVBoxLayout();
		auto* labels = new QHBoxLayout();
		auto* sd_label = new QLabel("Classic (SD)");
		auto* hd_label = new QLabel("Remastered (HD)");
		sd_label->setAlignment(Qt::AlignCenter);
		hd_label->setAlignment(Qt::AlignCenter);
		sd_label->setStyleSheet("color: #667; font-weight: bold;");
		hd_label->setStyleSheet("color: #667; font-weight: bold;");
		labels->addWidget(sd_label);
		labels->addWidget(hd_label);
		pair->addLayout(labels);

		auto* canvases = new QHBoxLayout();
		sd_canvas = new SpriteCanvas(this, CanvasMode::SD, this);
		hd_canvas = new SpriteCanvas(this, CanvasMode::HD, this);
		canvases->addWidget(sd_canvas, 1);
		canvases->addWidget(hd_canvas, 1);
		pair->addLayout(canvases, 1);

		root->addLayout(pair, 1);
	}

	if (sim) {
		// Create the hidden native window that ui.h blits into.
		// Size = the source render surface that SpriteCanvas then
		// upscales by SCALE for display. 256x256 comfortably fits
		// any unit sprite (max retail GRP frame ~130x130 for
		// buildings; marine is 64x64).
		constexpr int W = 256, H = 256;
		sim->configure_viewport(W, H);

		connect(race_cb,
		        QOverload<int>::of(&QComboBox::currentIndexChanged),
		        this, &ViewerWindow::on_race_changed);
		connect(unit_cb,
		        QOverload<int>::of(&QComboBox::currentIndexChanged),
		        this, &ViewerWindow::on_unit_changed);
		connect(anim_cb,
		        QOverload<int>::of(&QComboBox::currentIndexChanged),
		        this, &ViewerWindow::on_anim_changed);
		connect(dir_slider, &QSlider::valueChanged,
		        this, &ViewerWindow::on_direction_changed);
		connect(playpause_btn, &QPushButton::clicked,
		        this, &ViewerWindow::on_playpause_clicked);
		connect(&tick_timer, &QTimer::timeout,
		        this, &ViewerWindow::on_tick);

		populate_units_for_race(0);
		booted = true;
		tick_timer.start(TICK_MS);
	}

	// Assemble the tabs. Classic first so the app opens on the
	// familiar view; HD Mapping is a tool-tab reachable via the
	// tab bar once the user is done browsing sprites.
	//
	// Autosave path: sibling of the SC:R install root. Chosen so
	// the mapping file lives next to the source it references and
	// survives repo checkouts / rebuilds. Empty when HD mode is
	// off, which disables autosave inside MappingTab.
	QString autosave_path;
	if (!sc_remastered_path.isEmpty()) {
		QDir d(sc_remastered_path);
		d.cdUp();
		autosave_path = d.filePath("hd_mapping.json");
	}
	// SD reference previews live under <data-path>/sd_previews/
	// (produced by tools/sd_sprite_dump). Passing the resolved
	// absolute path so the tab works even when Qt cwd differs.
	QString sd_previews_dir;
	if (!data_path.empty()) {
		QDir d(QString::fromStdString(data_path));
		sd_previews_dir = d.filePath("sd_previews");
	}
	tabs->addTab(classic, "Classic");
	tabs->addTab(
		new MappingTab(hd_loader.get(), autosave_path,
		               sd_previews_dir, tabs),
		"HD Mapping");
	frames_tab = new FramesTab(hd_loader.get(), tabs);
	tabs->addTab(frames_tab, "Frames");
}

ViewerWindow::~ViewerWindow() = default;

void ViewerWindow::populate_units_for_race(int race_index) {
	if (race_index < 0 || race_index >= (int)races.size()) return;
	unit_cb->blockSignals(true);
	unit_cb->clear();
	for (const auto& u : races[race_index].units) {
		unit_cb->addItem(u.label);
	}
	unit_cb->blockSignals(false);
	unit_cb->setCurrentIndex(0);
	on_unit_changed(0);
}

void ViewerWindow::populate_anims_for_current_unit() {
	anim_cb->blockSignals(true);
	anim_cb->clear();
	if (!sim) { anim_cb->blockSignals(false); return; }
	for (int i = 0; i < 28; ++i) {
		if (!sim->anim_available(i)) continue;
		anim_cb->addItem(QString("%1  %2").arg(i, 2).arg(ANIM_LABELS[i]),
		                 i);
	}
	anim_cb->blockSignals(false);
	if (anim_cb->count() > 0) {
		anim_cb->setCurrentIndex(0);
		on_anim_changed(0);
	}
}

void ViewerWindow::on_race_changed(int index) {
	populate_units_for_race(index);
}

// Fallback for when the runtime mapping table is unavailable.
// Long-term the answer comes from tools/hd_mapping_table.json
// (produced by validate_hd_mapping.py from the user's hand-mapped
// hd_mapping.json). This 2-entry table just keeps Marine + SCV
// working when developers haven't produced the table yet.
static int sc_r_image_id_for_unit_fallback(int unit_type_id) {
	switch (unit_type_id) {
		case 0:  return 244;   // Terran_Marine -- main_243.anim
		case 7:  return 248;   // Terran_SCV    -- main_247.anim
		default: return -1;
	}
}

void ViewerWindow::on_unit_changed(int index) {
	if (!sim || index < 0) return;
	int race_index = race_cb->currentIndex();
	if (race_index < 0 || race_index >= (int)races.size()) return;
	const auto& units = races[race_index].units;
	if (index >= (int)units.size()) return;
	int u_id = units[index].unit_type_id;
	if (!sim->spawn_unit(u_id)) {
		readout->setText("spawn_unit failed");
		return;
	}
	sim->set_heading_from_slider(current_dir);
	// Log what got spawned + how many anims iscript reports for it.
	// Helps diagnose "unit picker changed but anim list didn't"
	// symptoms; a proper unit swap always produces a distinct count.
	int n_anims = 0;
	for (int i = 0; i < 28; ++i) if (sim->anim_available(i)) ++n_anims;
	qInfo() << "spawn_unit type_id=" << u_id
	        << "-> anim_count=" << n_anims;
	populate_anims_for_current_unit();

	// HD path: resolve this unit's SC:R image_id, load (or reuse)
	// its HdSprite, and stash a non-owning pointer for the canvas.
	//
	// Preferred lookup: openBW bw_id (from the running sim, via
	// SimHarness::current_image_id()) -> hd_loader's mapping table
	// -> sc_r_row. Correctly handles every entry the user has
	// confirmed in the HD Mapping tab. Falls back to a 2-entry
	// hardcoded table (Marine + SCV) if the mapping table wasn't
	// loaded, so a fresh clone still sees SOMETHING in HD.
	current_hd = nullptr;
	if (hd_loader) {
		int sc_id = -1;
		int bw_id = -1;
		if (hd_loader->has_mapping_table()) {
			// Use grp_filename_index (arr/images.tbl string ordinal),
			// NOT image_type_t::id (ImageTypes enum ordinal). The
			// HD Mapping tab's combobox is keyed by images.tbl
			// filenames, so hd_mapping_table.json's bw_id column
			// is images.tbl ordinals -- distinct from ImageTypes.
			bw_id = sim->current_grp_filename_index();
			if (bw_id >= 0) sc_id = hd_loader->sc_r_row_for_bw_id(bw_id);
		}
		qInfo() << "on_unit_changed"
		        << units[index].label
		        << "u_id=" << u_id
		        << "bw_id(grp_idx)=" << bw_id
		        << "sc_r=" << sc_id;
		if (sc_id < 0) {
			sc_id = sc_r_image_id_for_unit_fallback(u_id);
		}
		if (sc_id >= 0) {
			auto it = hd_cache.find(u_id);
			if (it == hd_cache.end()) {
				auto sp = hd_loader->load_sprite(sc_id);
				if (sp) {
					current_hd = sp.get();
					hd_cache.emplace(u_id, std::move(sp));
				} else {
					qWarning() << "HD load failed for unit"
					           << u_id << "sc_id" << sc_id
					           << ":" << hd_loader->last_error();
				}
			} else {
				current_hd = it->second.get();
			}
		}
	}

	refresh_readout();
	if (sd_canvas) sd_canvas->request_repaint();
	if (hd_canvas) hd_canvas->request_repaint();

	// Tell the frames-browser tab which image_id to show. We use
	// the image_id of the CURRENT main image on the freshly-spawned
	// sprite (not the body-only image_id), because SC:R's images.rel
	// distinguishes body vs shadow vs overlay by row -- we want
	// whatever the sim is walking right now.
	if (frames_tab && sim) {
		auto images = sim->current_sprite_images();
		int primary_id = -1;
		// Prefer the body image (is_shadow=false, hidden=false).
		for (const auto& im : images) {
			if (!im.is_shadow && !im.hidden) {
				primary_id = im.image_id;
				break;
			}
		}
		if (primary_id < 0 && !images.empty()) {
			primary_id = images.front().image_id;
		}
		frames_tab->set_current_image_id(primary_id);
	}
}

void ViewerWindow::on_anim_changed(int index) {
	if (!sim || index < 0) return;
	int anim_id = anim_cb->itemData(index).toInt();
	if (anim_id < 0) return;
	// If the unit already died from a previous Death click, run_anim
	// will refuse. Respawn a fresh unit first, then play the anim.
	// User picking Death again on a corpse: respawn then re-die, which
	// is the visually-sensible interpretation.
	if (sim->unit_dying || !sim->run_anim(anim_id)) {
		int race_index = race_cb->currentIndex();
		int unit_index = unit_cb->currentIndex();
		if (race_index >= 0 && unit_index >= 0 &&
		    race_index < (int)races.size() &&
		    unit_index < (int)races[race_index].units.size()) {
			int type_id = races[race_index].units[unit_index].unit_type_id;
			if (sim->spawn_unit(type_id)) {
				sim->set_heading_from_slider(current_dir);
				sim->run_anim(anim_id);
			}
		}
	}
	refresh_readout();
	if (sd_canvas) sd_canvas->request_repaint();
	if (hd_canvas) hd_canvas->request_repaint();
}

void ViewerWindow::on_direction_changed(int value) {
	current_dir = value;
	if (sim) sim->set_heading_from_slider(current_dir);
	refresh_readout();
	if (sd_canvas) sd_canvas->request_repaint();
	if (hd_canvas) hd_canvas->request_repaint();
}

void ViewerWindow::on_playpause_clicked() {
	if (playing) {
		playing = false;
		tick_timer.stop();
		playpause_btn->setText("▶ Play");
	} else {
		playing = true;
		tick_timer.start(TICK_MS);
		playpause_btn->setText("⏸ Pause");
	}
}

void ViewerWindow::on_tick() {
	if (!sim) return;
	sim->tick();
	if (sd_canvas) sd_canvas->request_repaint();
	if (hd_canvas) hd_canvas->request_repaint();
}

void ViewerWindow::refresh_readout() {
	if (!sim) return;
	QString anim_name = "(none)";
	if (sim->current_anim >= 0 && sim->current_anim < 28) {
		anim_name = ANIM_LABELS[sim->current_anim];
	}
	auto info = sim->current_frame_info();
	readout->setText(
		QString("action: %1\ndir=%2  base=%3  offset=%4  frame=%5")
			.arg(anim_name).arg(current_dir)
			.arg(info.frame_base).arg(info.frame_offset)
			.arg(info.frame_index));
}

}   // namespace sprite_viewer
