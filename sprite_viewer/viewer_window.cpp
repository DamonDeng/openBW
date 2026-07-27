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
		// laptop screen. Startup uses sizeHint() below (~480x400
		// per canvas); user can resize freely.
		setMinimumSize(320, 320);
		background_color = QColor(0x80, 0x80, 0x80);
	}

	QSize sizeHint()    const override { return QSize(480, 400); }
	QSize minimumSizeHint() const override { return QSize(320, 320); }

	void request_repaint() { update(); }

protected:
	// HD paint path: draw the currently-selected frame from the
	// HdSprite's diffuse atlas, centered on the widget. `frame_idx`
	// comes from iscript state via SimHarness (image->frame_index).
	// Returns true iff HD rendering was attempted (so classic can
	// be skipped this tick).
	bool paint_hd(QPainter& p) {
		HdSprite* hd = owner ? owner->current_hd : nullptr;
		if (!hd || hd->frames.empty()) return false;

		// Which frame? Read from the iscript state. SimHarness owns
		// the unit and its main_image; grab the frame_index there.
		int f = 0;
		if (sim) {
			auto fi = sim->current_frame_info();
			if (fi.frame_index >= 0) f = fi.frame_index;
		}
		if (f < 0 || f >= (int)hd->frames.size()) f = 0;
		const auto& fr = hd->frames[f];

		// Prefer the composited (diffuse + teamcolor*player_color)
		// atlas when available so the sprite shows its owner's tint;
		// falls back to raw diffuse if teamcolor was absent for
		// this unit (e.g. neutral doodads).
		const QImage& src_atlas =
			hd->composited.isNull() ? hd->diffuse : hd->composited;
		QImage sub = src_atlas.copy(
			fr.atlas_x, fr.atlas_y, fr.w, fr.h);

		// Place the frame the same way openBW's SD renderer does
		// (bwgame.h:13334 get_image_map_position):
		//
		//   NORMAL : screen = sprite_center + frame.offset - bbox_center
		//   FLIPPED: screen = sprite_center + bbox_center - (frame.offset + frame.size)
		//
		// The per-frame offset is measured from the top-left of the
		// sprite's bounding box (sprite_w * sprite_h). The flipped
		// variant mirrors the anchor point across the bbox center so
		// h-flipped left facings line up correctly. iscript only
		// stores facings 0..16 in the frame table -- facings 17..31
		// draw one of those 17 frames with flipped=true.
		bool flipped = sim ? sim->current_flipped() : false;
		if (flipped) sub = sub.mirrored(/*h=*/true, /*v=*/false);

		int cx = width()  / 2;
		int cy = height() / 2;
		int dx, dy;
		if (flipped) {
			dx = cx + (int)hd->sprite_w / 2
			        - ((int)fr.offset_x + (int)fr.w);
		} else {
			dx = cx + (int)fr.offset_x - (int)hd->sprite_w / 2;
		}
		dy = cy + (int)fr.offset_y - (int)hd->sprite_h / 2;
		p.drawImage(dx, dy, sub);
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
		// openBW's render surface is filled with palette index 0
		// (= RGB 0,0,0 alpha 255 on all tilesets) before draw_sprite
		// writes into it. The indexed->rgba blit copies index 0 as
		// opaque black, so the widget sees a black square around
		// the sprite. Detach a mutable copy and swap those pure-
		// black pixels for the widget's mid-gray background so the
		// marine sits on gray instead. Legitimate sprite pixels use
		// palette shades like 0x08/0x10; NONE map exactly to
		// (0,0,0), so the filter is unambiguous.
		QImage snap = view.copy();
		const QRgb gray = qRgb(0x80, 0x80, 0x80);
		for (int y = 0; y < snap.height(); ++y) {
			QRgb* row = reinterpret_cast<QRgb*>(snap.scanLine(y));
			for (int x = 0; x < snap.width(); ++x) {
				if ((row[x] & 0x00FFFFFF) == 0) row[x] = gray;
			}
		}

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
