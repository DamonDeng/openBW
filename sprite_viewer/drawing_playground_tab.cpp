// See drawing_playground_tab.h.

#include "drawing_playground_tab.h"

#include "sim_harness.h"

#include <QtCore/QTimer>
#include <QtGui/QImage>
#include <QtGui/QKeyEvent>
#include <QtGui/QPainter>
#include <QtGui/QPaintEvent>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QSlider>
#include <QtWidgets/QVBoxLayout>

namespace sprite_viewer {

// -------------------------------------------------------------------
// DrawingCanvas: shows the sim's SD render. Same pattern as the
// PlaygroundCanvas but without click interactions -- calibration is
// keyboard-driven.

constexpr int kCanvasW = 640;
constexpr int kCanvasH = 480;

class DrawingCanvas : public QWidget {
	Q_OBJECT
public:
	explicit DrawingCanvas(SimHarness* sim, QWidget* parent = nullptr)
		: QWidget(parent), sim_(sim) {
		setMinimumSize(kCanvasW, kCanvasH);
	}
	void set_sim(SimHarness* sim) { sim_ = sim; update(); }
	QSize sizeHint() const override { return QSize(kCanvasW, kCanvasH); }

protected:
	void paintEvent(QPaintEvent*) override {
		QPainter p(this);
		p.fillRect(rect(), QColor(0x40, 0x40, 0x40));
		if (!sim_) return;
		auto pixels = sim_->render_frame();
		if (!pixels.data || pixels.width <= 0 || pixels.height <= 0) {
			return;
		}
		QImage view(reinterpret_cast<const uchar*>(pixels.data),
		            pixels.width, pixels.height, pixels.pitch,
		            QImage::Format_ARGB32);
		p.drawImage(0, 0, view);
	}

private:
	SimHarness* sim_;
};

// -------------------------------------------------------------------
// DrawingPlaygroundTab

DrawingPlaygroundTab::DrawingPlaygroundTab(std::string data_path,
                                            std::string map_relpath,
                                            QWidget* parent)
	: QWidget(parent),
	  data_path_(std::move(data_path)),
	  map_relpath_(std::move(map_relpath))
{
	auto* root = new QHBoxLayout(this);

	// Sidebar.
	auto* left = new QVBoxLayout();
	left->addWidget(new QLabel("<b>Calibration</b>"));

	readout_ = new QLabel("");
	readout_->setStyleSheet(
		"font-family: monospace; font-size: 13px; color: #222;"
		"padding: 6px; background: #eef; border: 1px solid #99a;");
	readout_->setWordWrap(true);
	left->addWidget(readout_);

	left->addWidget(new QLabel("Adjust layer:"));
	pick_turret_ = new QRadioButton("Turret");
	pick_flame_  = new QRadioButton("Flame (overlay)");
	pick_flame_->setChecked(true);
	left->addWidget(pick_turret_);
	left->addWidget(pick_flame_);

	left->addWidget(new QLabel("Facing (0..16):"));
	facing_slider_ = new QSlider(Qt::Horizontal);
	facing_slider_->setRange(0, 16);
	facing_slider_->setValue(0);
	left->addWidget(facing_slider_);

	help_label_ = new QLabel(
		"Arrow keys: nudge active layer by 1 SD pixel.\n"
		"Shift+arrow: nudge by 10 pixels.\n"
		"R: reset active layer to (0, 0).\n"
		"Space: cycle facing.\n"
		"\nClick the canvas first to give it keyboard focus.");
	help_label_->setStyleSheet("color: #556; font-size: 11px;");
	help_label_->setWordWrap(true);
	left->addWidget(help_label_);
	left->addStretch(1);

	root->addLayout(left, 0);

	// Canvas.
	canvas_ = new DrawingCanvas(nullptr, this);
	canvas_->setFocusPolicy(Qt::StrongFocus);
	root->addWidget(canvas_, 1);

	connect(facing_slider_, &QSlider::valueChanged,
	        this, &DrawingPlaygroundTab::on_facing_changed);

	// Take focus so arrow keys work as soon as the tab shows.
	setFocusPolicy(Qt::StrongFocus);

	refresh_readout();
}

DrawingPlaygroundTab::~DrawingPlaygroundTab() = default;

void DrawingPlaygroundTab::showEvent(QShowEvent* e) {
	QWidget::showEvent(e);
	ensure_booted();
	canvas_->setFocus();
}

void DrawingPlaygroundTab::ensure_booted() {
	if (sim_ || boot_failed_) return;
	auto sim = std::make_unique<SimHarness>();
	try {
		sim->boot(data_path_, map_relpath_);
	} catch (const std::exception& e) {
		boot_failed_ = true;
		boot_error_ = QString::fromUtf8(e.what());
		if (readout_) {
			readout_->setText(
				QStringLiteral("Boot failed: %1").arg(boot_error_));
		}
		return;
	}
	sim->configure_viewport(kCanvasW, kCanvasH);
	// Show the raw retail-authored drift for calibration -- if the
	// production compensation table is on, the drift would already be
	// corrected and the tab would show nothing to tune.
	sim->set_overlay_compensation_enabled(false);

	// Spawn a sieged tank at map center. spawn_unit auto-centers
	// the camera on the unit. Type_id 30 = Terran_Siege_Tank_Siege_Mode
	// (verified via the runtime scan we did earlier).
	sim->spawn_unit(30);
	// Start firing so the flame overlay is visible.
	// GndAttkRpt (5) loops via our auto-refire logic.
	sim->run_anim(5);

	sim_ = std::move(sim);
	canvas_->set_sim(sim_.get());

	tick_timer_ = new QTimer(this);
	tick_timer_->setInterval(1000 / 24);
	connect(tick_timer_, &QTimer::timeout,
	        this, &DrawingPlaygroundTab::on_tick);
	tick_timer_->start();

	apply_deltas();
	refresh_readout();
}

void DrawingPlaygroundTab::on_tick() {
	if (!sim_) return;
	sim_->tick();
	canvas_->update();
}

void DrawingPlaygroundTab::on_facing_changed(int slider_value) {
	if (!sim_) return;
	sim_->set_turret_heading_from_slider(slider_value);
}

void DrawingPlaygroundTab::keyPressEvent(QKeyEvent* e) {
	int step = (e->modifiers() & Qt::ShiftModifier) ? 10 : 1;
	int* dx = nullptr;
	int* dy = nullptr;
	if (pick_turret_ && pick_turret_->isChecked()) {
		dx = &turret_dx_; dy = &turret_dy_;
	} else if (pick_flame_ && pick_flame_->isChecked()) {
		dx = &flame_dx_; dy = &flame_dy_;
	}
	if (!dx || !dy) {
		QWidget::keyPressEvent(e);
		return;
	}
	bool handled = true;
	switch (e->key()) {
	case Qt::Key_Left:  *dx -= step; break;
	case Qt::Key_Right: *dx += step; break;
	case Qt::Key_Up:    *dy -= step; break;
	case Qt::Key_Down:  *dy += step; break;
	case Qt::Key_R:     *dx = 0; *dy = 0; break;
	case Qt::Key_Space: {
		if (facing_slider_) {
			int v = (facing_slider_->value() + 1) % 17;
			facing_slider_->setValue(v);
		}
		break;
	}
	default: handled = false; break;
	}
	if (handled) {
		apply_deltas();
		refresh_readout();
		e->accept();
	} else {
		QWidget::keyPressEvent(e);
	}
}

void DrawingPlaygroundTab::apply_deltas() {
	if (!sim_) return;
	// base = 0, we only tune turret + flame (overlay). Sieged tank
	// base doesn't visually rotate; its placement is authoritative.
	sim_->set_debug_offset_delta(0, 0,
	                              turret_dx_, turret_dy_,
	                              flame_dx_,  flame_dy_);
}

void DrawingPlaygroundTab::refresh_readout() {
	if (!readout_) return;
	readout_->setText(
		QStringLiteral(
			"Turret delta: dx=%1, dy=%2\n"
			"Flame  delta: dx=%3, dy=%4")
		.arg(turret_dx_).arg(turret_dy_)
		.arg(flame_dx_).arg(flame_dy_));
}

}   // namespace sprite_viewer

#include "drawing_playground_tab.moc"
