// See playground_tab.h for the design.

#include "playground_tab.h"

#include "sim_harness.h"

#include <QtCore/QTimer>
#include <QtGui/QImage>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPaintEvent>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

#include <cstdio>

namespace sprite_viewer {

// -------------------------------------------------------------------
// PlaygroundCanvas: renders the sim's SD surface + emits click/hover
// events with world coordinates. Free-camera: caller drives screen
// position via sim's set_screen_pos, not centered on any single unit.

// Wider than the animation-browser canvas (256x256) so we get a
// scenario-scale view. Sized to fill a 1280-wide viewer window
// once the left control panel takes its ~220 px. The 4:3 aspect
// matches the retail SC:BW map view proportions so units look
// natural at their authored scale.
constexpr int kCanvasW = 853;
constexpr int kCanvasH = 640;

class PlaygroundCanvas : public QWidget {
	Q_OBJECT
public:
	explicit PlaygroundCanvas(SimHarness* sim, QWidget* parent = nullptr)
		: QWidget(parent), sim_(sim) {
		setMinimumSize(kCanvasW, kCanvasH);
		setMouseTracking(true);
		setFocusPolicy(Qt::StrongFocus);
	}

	void set_sim(SimHarness* sim) {
		sim_ = sim;
		update();
	}

	QSize sizeHint() const override {
		return QSize(kCanvasW, kCanvasH);
	}

signals:
	// world_x/world_y are in SD-map pixels, computed by adding the
	// current sim screen_pos to the widget-local click position. The
	// widget doesn't know about the sim; the tab connects these
	// signals to whatever it wants to do with a click.
	void clicked(int world_x, int world_y);
	void hovered(int world_x, int world_y);

protected:
	void paintEvent(QPaintEvent*) override {
		QPainter p(this);
		p.fillRect(rect(), QColor(0, 0, 0));
		if (!sim_) return;
		auto pixels = sim_->render_frame();
		if (!pixels.data || pixels.width <= 0 || pixels.height <= 0) {
			return;
		}
		// Same wrapping trick SpriteCanvas uses: read-only view
		// over the RGBA surface. Format_ARGB32 matches Qt's byte
		// order on little-endian (BGRA in memory).
		QImage view(reinterpret_cast<const uchar*>(pixels.data),
		            pixels.width, pixels.height, pixels.pitch,
		            QImage::Format_ARGB32);
		p.drawImage(0, 0, view);
	}

	void mouseMoveEvent(QMouseEvent* e) override {
		if (!sim_) return;
		emit hovered(sim_->screen_pos_x() + (int)e->position().x(),
		             sim_->screen_pos_y() + (int)e->position().y());
	}

	void mousePressEvent(QMouseEvent* e) override {
		if (!sim_) return;
		if (e->button() == Qt::LeftButton) {
			emit clicked(
				sim_->screen_pos_x() + (int)e->position().x(),
				sim_->screen_pos_y() + (int)e->position().y());
		}
	}

private:
	SimHarness* sim_;   // non-owning
};

// -------------------------------------------------------------------
// PlaygroundTab: sidebar controls + right-side canvas.

// Terran unit registry (mirrors the animation-browser tab's list).
// Phase 1 exposes only Terran; Zerg + Protoss can be added later
// without other code changes since spawn_at takes any unit_type_id.
struct RaceUnits {
	const char* race;
	std::vector<std::pair<const char*, int>> units;
};
static const std::vector<RaceUnits>& races() {
	static const std::vector<RaceUnits> table = {
		{"Terran", {
			{"Marine",             0},
			{"Ghost",               1},
			{"Vulture",             2},
			{"Goliath",             3},
			{"Siege Tank",          5},
			{"SCV",                 7},
			{"Wraith",              8},
			{"Science Vessel",      9},
			{"Dropship",           11},
			{"Battlecruiser",      12},
			{"Firebat",            32},
			{"Medic",              34},
			{"Valkyrie",           58},
			{"Siege Tank (Sieged)", 30},
		}},
	};
	return table;
}

PlaygroundTab::PlaygroundTab(std::string data_path,
                             std::string map_relpath,
                             QWidget* parent)
	: QWidget(parent),
	  data_path_(std::move(data_path)),
	  map_relpath_(std::move(map_relpath))
{
	auto* root = new QHBoxLayout(this);

	// Left control panel.
	auto* left = new QVBoxLayout();
	left->setSpacing(6);

	left->addWidget(new QLabel("<b>Spawn</b>"));
	race_cb_ = new QComboBox();
	for (const auto& r : races()) race_cb_->addItem(r.race);
	left->addWidget(race_cb_);
	unit_cb_ = new QComboBox();
	left->addWidget(unit_cb_);
	owner_cb_ = new QComboBox();
	for (int i = 0; i < 8; ++i) {
		owner_cb_->addItem(QStringLiteral("Player %1").arg(i));
	}
	left->addWidget(owner_cb_);

	spawn_btn_ = new QPushButton("Spawn at map center");
	left->addWidget(spawn_btn_);
	center_btn_ = new QPushButton("Center camera on map");
	left->addWidget(center_btn_);

	hover_label_ = new QLabel("world: ---, ---");
	hover_label_->setStyleSheet("font-family: monospace; color: #666;");
	left->addWidget(hover_label_);

	status_label_ = new QLabel("");
	status_label_->setWordWrap(true);
	status_label_->setStyleSheet("color: #a44;");
	left->addWidget(status_label_);

	left->addWidget(new QLabel("<b>Alive units</b>"));
	units_list_ = new QListWidget();
	units_list_->setMinimumHeight(200);
	left->addWidget(units_list_, /*stretch*/ 1);

	// Action buttons for the selected unit. Move/Attack are two-
	// step: pressing the button arms a click-mode that the next
	// canvas click consumes. Stop/HoldPosition/Siege/Unsiege/Kill
	// fire immediately.
	left->addWidget(new QLabel("<b>Actions</b>"));
	move_btn_     = new QPushButton("Move");
	move_btn_->setCheckable(true);
	attack_btn_   = new QPushButton("Attack");
	attack_btn_->setCheckable(true);
	stop_btn_     = new QPushButton("Stop");
	hold_btn_     = new QPushButton("Hold Position");
	siege_btn_    = new QPushButton("Siege");
	unsiege_btn_  = new QPushButton("Unsiege");
	kill_btn_     = new QPushButton("Kill");
	for (auto* b : { move_btn_, attack_btn_, stop_btn_, hold_btn_,
	                 siege_btn_, unsiege_btn_, kill_btn_ }) {
		left->addWidget(b);
		b->setEnabled(false);
	}

	root->addLayout(left, 0);

	// Right canvas.
	canvas_ = new PlaygroundCanvas(nullptr, this);
	root->addWidget(canvas_, /*stretch*/ 1);

	// Wiring.
	connect(race_cb_, QOverload<int>::of(&QComboBox::currentIndexChanged),
	        this, &PlaygroundTab::on_race_changed);
	connect(spawn_btn_, &QPushButton::clicked,
	        this, &PlaygroundTab::on_spawn_clicked);
	connect(center_btn_, &QPushButton::clicked,
	        this, &PlaygroundTab::on_center_clicked);
	connect(canvas_, &PlaygroundCanvas::hovered,
	        this, [this](int x, int y) {
		        hover_label_->setText(
		            QStringLiteral("world: %1, %2").arg(x).arg(y));
	        });
	connect(canvas_, &PlaygroundCanvas::clicked,
	        this, &PlaygroundTab::on_canvas_clicked);
	connect(units_list_, &QListWidget::itemSelectionChanged,
	        this, &PlaygroundTab::on_units_selection_changed);
	connect(move_btn_,    &QPushButton::clicked,
	        this, &PlaygroundTab::on_move_clicked);
	connect(attack_btn_,  &QPushButton::clicked,
	        this, &PlaygroundTab::on_attack_clicked);
	connect(stop_btn_,    &QPushButton::clicked,
	        this, &PlaygroundTab::on_stop_clicked);
	connect(hold_btn_,    &QPushButton::clicked,
	        this, &PlaygroundTab::on_hold_clicked);
	connect(siege_btn_,   &QPushButton::clicked,
	        this, &PlaygroundTab::on_siege_clicked);
	connect(unsiege_btn_, &QPushButton::clicked,
	        this, &PlaygroundTab::on_unsiege_clicked);
	connect(kill_btn_,    &QPushButton::clicked,
	        this, &PlaygroundTab::on_kill_clicked);

	on_race_changed(0);
}

PlaygroundTab::~PlaygroundTab() = default;

void PlaygroundTab::showEvent(QShowEvent* e) {
	QWidget::showEvent(e);
	ensure_booted();
}

void PlaygroundTab::ensure_booted() {
	if (sim_ || boot_failed_) return;
	// Deferred boot: a second SimHarness dedicated to the playground
	// so we can size its render surface differently (kCanvasW x H)
	// without disturbing the animation-browser tab's 256x256 canvas.
	// Uses the same MPQs + map file as ViewerWindow.
	auto sim = std::make_unique<SimHarness>();
	try {
		sim->boot(data_path_, map_relpath_);
	} catch (const std::exception& e) {
		boot_failed_ = true;
		boot_error_ = QString::fromUtf8(e.what());
		status_label_->setText(
			QStringLiteral("Playground boot failed: %1").arg(boot_error_));
		return;
	}
	sim->configure_viewport(kCanvasW, kCanvasH);
	sim->set_playground_mode(true);
	// Center camera on the middle of the map so there's something
	// to see even before the user spawns a unit.
	sim->set_screen_pos(sim->map_width_px()  / 2 - kCanvasW / 2,
	                    sim->map_height_px() / 2 - kCanvasH / 2);
	sim_ = std::move(sim);
	canvas_->set_sim(sim_.get());

	// Tick the sim at 24 Hz -- same rate as the animation-browser
	// tab. openBW's own tick advances iscript for every unit; the
	// canvas repaint pulls the new frame.
	tick_timer_ = new QTimer(this);
	tick_timer_->setInterval(1000 / 24);
	connect(tick_timer_, &QTimer::timeout,
	        this, &PlaygroundTab::on_tick);
	tick_timer_->start();
	status_label_->setText("");
}

void PlaygroundTab::on_race_changed(int race_index) {
	populate_units_for_race(race_index);
}

void PlaygroundTab::populate_units_for_race(int race_index) {
	if (race_index < 0 || race_index >= (int)races().size()) return;
	unit_cb_->clear();
	for (const auto& u : races()[race_index].units) {
		unit_cb_->addItem(u.first);
	}
}

void PlaygroundTab::on_spawn_clicked() {
	if (!sim_) return;
	int race_index = race_cb_->currentIndex();
	int unit_index = unit_cb_->currentIndex();
	if (race_index < 0 || unit_index < 0) return;
	const auto& r = races()[race_index];
	if (unit_index >= (int)r.units.size()) return;
	int unit_type_id = r.units[unit_index].second;
	int owner = owner_cb_->currentIndex();
	if (owner < 0) owner = 0;
	// Spawn at map center. Phase 2 will support "spawn where you
	// clicked" by connecting the canvas's clicked() signal.
	int wx = sim_->map_width_px()  / 2;
	int wy = sim_->map_height_px() / 2;
	int id = sim_->spawn_at(unit_type_id, wx, wy, owner);
	if (id < 0) {
		status_label_->setText(
			QStringLiteral("spawn_at failed for unit_type=%1")
				.arg(unit_type_id));
	} else {
		status_label_->setText("");
		refresh_unit_list();
	}
}

void PlaygroundTab::on_center_clicked() {
	if (!sim_) return;
	sim_->set_screen_pos(sim_->map_width_px()  / 2 - kCanvasW / 2,
	                     sim_->map_height_px() / 2 - kCanvasH / 2);
	canvas_->update();
}

void PlaygroundTab::on_tick() {
	if (!sim_) return;
	sim_->tick();
	canvas_->update();
	// Refresh the unit list at a lower cadence so we're not
	// rebuilding the QListWidget 24 times a second. Once per second
	// is fine for status.
	static int tick_ctr = 0;
	if ((++tick_ctr % 24) == 0) refresh_unit_list();
}

void PlaygroundTab::refresh_unit_list() {
	if (!sim_ || !units_list_) return;
	auto units = sim_->list_units();
	// Remember the currently-selected id so it survives the rebuild.
	int selected_id = -1;
	if (auto* item = units_list_->currentItem()) {
		selected_id = item->data(Qt::UserRole).toInt();
	}
	// Rebuilding the QListWidget's rows clears then repopulates
	// its selection, which would fire itemSelectionChanged twice
	// (once on clear, once on the re-select). Our handler would
	// then cancel any armed Move/Attack targeting mode -- which
	// makes the toggled Move button auto-untoggle after ~1s.
	// Block signals for the rebuild so only real user selection
	// changes trigger the handler.
	units_list_->blockSignals(true);
	units_list_->clear();
	for (const auto& u : units) {
		auto* item = new QListWidgetItem(
			QStringLiteral("#%1 %2 (p%3) hp=%4  @(%5,%6)")
				.arg(u.id)
				.arg(QString::fromStdString(u.type_name))
				.arg(u.owner)
				.arg(u.hp_int)
				.arg(u.world_x).arg(u.world_y));
		item->setData(Qt::UserRole, u.id);
		units_list_->addItem(item);
		if (u.id == selected_id) {
			units_list_->setCurrentItem(item);
		}
	}
	units_list_->blockSignals(false);
	update_action_buttons();
}

int PlaygroundTab::selected_unit_id() const {
	auto* item = units_list_ ? units_list_->currentItem() : nullptr;
	if (!item) return -1;
	return item->data(Qt::UserRole).toInt();
}

void PlaygroundTab::update_action_buttons() {
	int id = selected_unit_id();
	bool has = (id >= 0 && sim_);
	int type_id = has ? sim_->unit_type_id_of(id) : -1;
	if (type_id < 0) has = false;   // stale selection (unit died)

	// While a target-picking mode is armed, keep the toggled button
	// enabled (so the user can un-toggle it) and disable every other
	// action. This makes it visually obvious what click will do
	// next, and prevents accidentally firing Kill/Siege while
	// aiming a Move/Attack.
	bool targeting = (click_mode_ != ClickMode::None);
	auto set_common = [&](QPushButton* b, bool ok) {
		if (b) b->setEnabled(ok && !targeting);
	};
	set_common(stop_btn_, has);
	set_common(hold_btn_, has);
	set_common(kill_btn_, has);
	// Siege/Unsiege only for the tank chassis in the right mode.
	// Numbers match the runtime scan: Tank_Mode base=5,
	// Siege_Mode base=30 (Hero Duke's are 25/26).
	set_common(siege_btn_,
		has && (type_id == 5  || type_id == 25));
	set_common(unsiege_btn_,
		has && (type_id == 30 || type_id == 26));

	// Move/Attack: enabled when a unit is selected, OR when they
	// are the currently-toggled targeting button (so the user can
	// cancel by clicking again). Never both toggled at once.
	if (move_btn_) {
		move_btn_->setEnabled(
			has || click_mode_ == ClickMode::Move);
		move_btn_->setChecked(click_mode_ == ClickMode::Move);
	}
	if (attack_btn_) {
		attack_btn_->setEnabled(
			has || click_mode_ == ClickMode::Attack);
		attack_btn_->setChecked(click_mode_ == ClickMode::Attack);
	}
}

void PlaygroundTab::set_click_mode(ClickMode m, const char* status) {
	click_mode_ = m;
	if (status_label_) status_label_->setText(QString::fromUtf8(status));
	update_action_buttons();
}

void PlaygroundTab::on_units_selection_changed() {
	// Only reset a targeting click-mode if the selection actually
	// moved to a different unit -- rebuilding the list every second
	// can nudge the "current item" briefly, and we don't want that
	// to yank the toggled Move/Attack button off mid-aim.
	int now = selected_unit_id();
	if (now != last_seen_selection_ && click_mode_ != ClickMode::None) {
		set_click_mode(ClickMode::None, "");
	}
	last_seen_selection_ = now;
	update_action_buttons();
}

void PlaygroundTab::on_move_clicked() {
	// Toggle: clicking Move again while already in Move-mode cancels.
	if (click_mode_ == ClickMode::Move) {
		set_click_mode(ClickMode::None, "");
	} else {
		set_click_mode(ClickMode::Move,
			"Click on the map to set the move destination. "
			"Click Move again to cancel.");
	}
}

void PlaygroundTab::on_attack_clicked() {
	if (click_mode_ == ClickMode::Attack) {
		set_click_mode(ClickMode::None, "");
	} else {
		set_click_mode(ClickMode::Attack,
			"Click on the map (or on a unit) to attack. "
			"Click Attack again to cancel.");
	}
}

void PlaygroundTab::on_stop_clicked() {
	if (sim_) sim_->order_stop(selected_unit_id());
}

void PlaygroundTab::on_hold_clicked() {
	if (sim_) sim_->order_hold_position(selected_unit_id());
}

void PlaygroundTab::on_siege_clicked() {
	if (!sim_) return;
	if (!sim_->order_siege(selected_unit_id())) {
		status_label_->setText("Siege not allowed for this unit.");
	}
}

void PlaygroundTab::on_unsiege_clicked() {
	if (!sim_) return;
	if (!sim_->order_unsiege(selected_unit_id())) {
		status_label_->setText("Unsiege not allowed for this unit.");
	}
}

void PlaygroundTab::on_kill_clicked() {
	if (!sim_) return;
	int id = selected_unit_id();
	if (sim_->kill_unit_id(id)) {
		refresh_unit_list();
	}
}

void PlaygroundTab::on_canvas_clicked(int world_x, int world_y) {
	if (!sim_) return;
	int uid = selected_unit_id();
	switch (click_mode_) {
	case ClickMode::Move: {
		if (uid >= 0) sim_->order_move(uid, world_x, world_y);
		set_click_mode(ClickMode::None, "");
		break;
	}
	case ClickMode::Attack: {
		if (uid >= 0) {
			// Prefer AttackUnit when the click landed on a unit
			// (24 SD-pixel tolerance ≈ 3/4 tile), else AttackMove
			// on the ground.
			int target = sim_->unit_near(world_x, world_y, 24);
			if (target >= 0 && target != uid) {
				sim_->order_attack_unit(uid, target);
			} else {
				sim_->order_attack_move(uid, world_x, world_y);
			}
		}
		set_click_mode(ClickMode::None, "");
		break;
	}
	case ClickMode::None:
	default: {
		// No armed action: clicking on a unit selects it.
		int hit = sim_->unit_near(world_x, world_y, 24);
		if (hit >= 0 && units_list_) {
			for (int i = 0; i < units_list_->count(); ++i) {
				auto* item = units_list_->item(i);
				if (item && item->data(Qt::UserRole).toInt() == hit) {
					units_list_->setCurrentItem(item);
					break;
				}
			}
		}
		break;
	}
	}
}

}   // namespace sprite_viewer

// Qt's moc needs the Q_OBJECT definitions available. Since
// PlaygroundCanvas is defined in this .cpp (not in a header), we
// need its moc output too -- CMake's AUTOMOC will run over this
// file and produce the .moc include below.
#include "playground_tab.moc"
