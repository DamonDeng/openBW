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

class SpriteCanvas : public QWidget {
public:
	// Nearest-neighbor upscale factor. 3x lets you see individual
	// pixels of the sprite without them being too small; higher
	// values just eat screen real estate for no additional detail.
	// Applied only in classic mode; HD sprites are already
	// high-resolution so we draw them 1:1.
	static constexpr int SCALE = 3;

	explicit SpriteCanvas(ViewerWindow* owner_,
	                      QWidget* parent = nullptr)
		: QWidget(parent), owner(owner_), sim(owner_->sim.get()) {
		setAttribute(Qt::WA_OpaquePaintEvent);
		setAttribute(Qt::WA_NoSystemBackground);
		setMinimumSize(256 * SCALE + 40, 256 * SCALE + 40);
		background_color = QColor(0x80, 0x80, 0x80);
	}

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

		// Source rect on the atlas.
		QImage sub = hd->diffuse.copy(
			fr.atlas_x, fr.atlas_y, fr.w, fr.h);

		// Place it so (fr.offset_x, fr.offset_y) inside the sub-
		// image aligns with the widget's center -- offset_x/y are
		// the "hotspot" of the sprite relative to its bounding box.
		int cx = width()  / 2;
		int cy = height() / 2;
		int dx = cx - (int)fr.offset_x;
		int dy = cy - (int)fr.offset_y;
		p.drawImage(dx, dy, sub);
		return true;
	}

	void paintEvent(QPaintEvent*) override {
		QPainter p(this);
		p.fillRect(rect(), background_color);
		if (!sim) return;

		// Try HD first; only fall through to classic when no HD
		// sprite is loaded (mode=classic, or HD mode with a unit
		// that has no HD anim).
		if (paint_hd(p)) return;

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

		// 3x nearest-neighbor upscale via drawImage with an explicit
		// target rect. SmoothPixmapTransform=false forces nearest
		// (each source pixel becomes a 3x3 block of the same color).
		p.setRenderHint(QPainter::SmoothPixmapTransform, false);
		int scaled_w = pixels.width * SCALE;
		int scaled_h = pixels.height * SCALE;
		int dx = (width() - scaled_w) / 2;
		int dy = (height() - scaled_h) / 2;
		p.drawImage(QRect(dx, dy, scaled_w, scaled_h), snap);
	}

private:
	ViewerWindow* owner;
	SimHarness* sim;
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
	resize(1050, 830);

	// --- unit registry ---
	// See bwenums.h for the openBW UnitTypes enum ordering.
	races.push_back({"Terran", {
		{"Marine", /*Terran_Marine*/ 0},
		{"SCV",    /*Terran_SCV*/    7},
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
		}
	}

	canvas = new SpriteCanvas(this, this);
	root->addWidget(canvas, 1);

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

// First-cut mapping from openBW UnitTypes -> SC:R images.rel row.
// Values verified by grepping arr/images.tbl for the unit's main
// GRP path, then locating the corresponding HD row (flag=8) in
// images.rel. Long-term this should be data-driven off images.tbl
// rather than hand-maintained.
//
// The mapping to images.rel row is NOT the same as the images.tbl
// ordinal: for Marine, images.tbl row 244 = "terran\\marine.grp"
// and images.rel row 244 (flag=8) points at main_243.anim. That's
// a lucky 1:1 for these two units, but the general case may need a
// small ordinal-shift table -- we'll pin down when we add more units.
static int sc_r_image_id_for_unit(int unit_type_id) {
	switch (unit_type_id) {
		case 0:  return 244;   // Terran_Marine -- main_243.anim
		case 7:  return 248;   // Terran_SCV    -- main_247.anim
		default: return -1;    // no HD mapping known -> fall back
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
	current_hd = nullptr;
	if (hd_loader) {
		int u_id = units[index].unit_type_id;
		int sc_id = sc_r_image_id_for_unit(u_id);
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
	canvas->request_repaint();
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
	canvas->request_repaint();
}

void ViewerWindow::on_direction_changed(int value) {
	current_dir = value;
	if (sim) sim->set_heading_from_slider(current_dir);
	refresh_readout();
	canvas->request_repaint();
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
	canvas->request_repaint();
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
