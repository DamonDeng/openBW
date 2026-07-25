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

#include <QtCore/QDebug>
#include <QtGui/QPainter>
#include <QtGui/QPaintEvent>
#include <QtGui/QImage>
#include <QtWidgets/QApplication>
#include <QtWidgets/QHBoxLayout>
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
	static constexpr int SCALE = 3;

	explicit SpriteCanvas(SimHarness* sim, QWidget* parent = nullptr)
		: QWidget(parent), sim(sim) {
		setAttribute(Qt::WA_OpaquePaintEvent);
		setAttribute(Qt::WA_NoSystemBackground);
		// Canvas needs to fit the openBW render surface at SCALE
		// with a little margin. render_frame produces 256x256, so
		// 3x = 768x768 + ~40 px of padding on each side.
		setMinimumSize(256 * SCALE + 40, 256 * SCALE + 40);
		// Mid-gray background makes the marine sprite legible
		// against both dark (armor, muzzle) and light (skin, HUD
		// numbers) pixels. Distinct from openBW's black-for-
		// unexplored so players don't confuse it with fog.
		background_color = QColor(0x80, 0x80, 0x80);
	}

	void request_repaint() { update(); }

protected:
	void paintEvent(QPaintEvent*) override {
		QPainter p(this);
		p.fillRect(rect(), background_color);
		if (!sim) return;

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
                           QWidget* parent)
	: QMainWindow(parent),
	  data_path(std::move(data_path_)),
	  map_relpath(std::move(map_relpath_)) {
	setWindowTitle("openBW sprite viewer");
	// Big enough to fit the 3x-upscaled 256x256 render surface
	// (= 768x768) plus the left control column (~220 px wide).
	resize(1050, 830);

	// --- unit registry (MVP: Terran + Marine only) ---
	// bwgame::UnitTypes::Terran_Marine == 0. See bwenums.h.
	races.push_back({"Terran", {
		{"Marine", /*Terran_Marine*/ 0},
	}});

	// --- widgets ---
	auto* central = new QWidget(this);
	setCentralWidget(central);
	auto* root = new QHBoxLayout(central);

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

	canvas = new SpriteCanvas(sim.get(), this);
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

void ViewerWindow::on_unit_changed(int index) {
	if (!sim || index < 0) return;
	int race_index = race_cb->currentIndex();
	if (race_index < 0 || race_index >= (int)races.size()) return;
	const auto& units = races[race_index].units;
	if (index >= (int)units.size()) return;
	if (!sim->spawn_unit(units[index].unit_type_id)) {
		readout->setText("spawn_unit failed");
		return;
	}
	sim->set_heading_from_slider(current_dir);
	populate_anims_for_current_unit();
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
