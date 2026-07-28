#include "new_remote_game_dialog.h"

#include "../map_catalog.h"

#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QVBoxLayout>

namespace simsc_desktop {

namespace {

const QStringList kRaceOptions = {"zerg", "terran", "protoss", "random"};

// Same closed vocabulary as simsc/app/services/games.py::GAME_SPEEDS.
// Order is fastest -> slowest so the dropdown reads sensibly.
const QStringList kSpeedOptions = {
	"turbosuper", "superfast", "fastest",
	"faster", "fast", "normal",
	"slow", "slower", "slowest",
};

// Synthetic "empty" and "AIBot" slot options prefixed with a
// bracket so they sort separately from real aliases and can't
// collide with a hypothetical user literally named "AIBot".
const auto kEmptySlot = QStringLiteral("(empty)");
const auto kAIBot     = QStringLiteral("AIBot");

}   // namespace

NewRemoteGameDialog::NewRemoteGameDialog(MapCatalog* catalog,
                                         const QVector<RemoteUser>& roster,
                                         const QString& current_user_alias,
                                         const QString& default_game_speed,
                                         QWidget* parent)
	: QDialog(parent), catalog_(catalog), roster_(roster),
	  current_user_alias_(current_user_alias) {

	setWindowTitle(tr("New remote game"));

	auto* root = new QVBoxLayout(this);

	auto* top    = new QFormLayout;
	map_combo_   = new QComboBox(this);
	map_combo_->setModel(catalog_);
	speed_combo_ = new QComboBox(this);
	for (const auto& s : kSpeedOptions) speed_combo_->addItem(s);
	{
		int idx = speed_combo_->findText(default_game_speed);
		if (idx < 0) idx = speed_combo_->findText("fastest");
		if (idx < 0) idx = 0;
		speed_combo_->setCurrentIndex(idx);
	}
	top->addRow(tr("Map:"),        map_combo_);
	top->addRow(tr("Game speed:"), speed_combo_);
	root->addLayout(top);

	auto* slots_box = new QGroupBox(tr("Players"), this);
	slots_layout_   = new QVBoxLayout(slots_box);
	root->addWidget(slots_box, 1);

	auto* buttons = new QDialogButtonBox(
		QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	root->addWidget(buttons);
	connect(buttons, &QDialogButtonBox::accepted,
		this, &NewRemoteGameDialog::onAccept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	connect(map_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
		this, &NewRemoteGameDialog::onMapChanged);

	if (catalog_->rowCount() > 0) {
		map_combo_->setCurrentIndex(0);
		onMapChanged(0);
	}
}

void NewRemoteGameDialog::rebuildSlotRows(int player_count) {
	for (auto* w : slot_widgets_) w->deleteLater();
	slot_widgets_.clear();
	slots_.clear();

	// Aliases dropdown: current user first (so slot 0 defaults to
	// them -- the natural "one of the players is me" shape), then
	// every other roster alias, then the two synthetic options.
	QStringList aliases;
	if (!current_user_alias_.isEmpty()) aliases << current_user_alias_;
	for (const auto& u : roster_) {
		if (u.alias != current_user_alias_) aliases << u.alias;
	}
	aliases << kAIBot << kEmptySlot;

	for (int slot = 0; slot < player_count; ++slot) {
		auto* row_w      = new QWidget;
		auto* row_layout = new QHBoxLayout(row_w);
		row_layout->setContentsMargins(0, 0, 0, 0);

		auto* label = new QLabel(tr("Slot %1:").arg(slot), row_w);
		label->setMinimumWidth(60);

		auto* alias = new QComboBox(row_w);
		alias->addItems(aliases);
		// Slot 0 -> me, other slots -> AIBot by default so the
		// game is playable immediately with a single click. User
		// can still assign real invitees.
		alias->setCurrentIndex(slot == 0 ? 0 : aliases.indexOf(kAIBot));

		auto* race = new QComboBox(row_w);
		race->addItems(kRaceOptions);
		race->setCurrentIndex(slot % kRaceOptions.size());

		row_layout->addWidget(label);
		row_layout->addWidget(alias, 1);
		row_layout->addWidget(race);

		slots_layout_->addWidget(row_w);
		slot_widgets_.push_back(row_w);
		slots_.push_back({alias, race});
	}
}

void NewRemoteGameDialog::onMapChanged(int row) {
	const auto* e = catalog_->entryAt(row);
	if (!e) return;
	rebuildSlotRows(e->player_count);
}

void NewRemoteGameDialog::onAccept() {
	const auto* e = catalog_->entryAt(map_combo_->currentIndex());
	if (!e) {
		QMessageBox::warning(this, tr("New remote game"),
			tr("Pick a map first."));
		return;
	}

	CreateRemoteGameReq r;
	r.map        = e->filename;
	r.game_speed = speed_combo_->currentText();

	for (int i = 0; i < slots_.size(); ++i) {
		const auto alias = slots_[i].alias_combo->currentText();
		const auto race  = slots_[i].race_combo->currentText();
		r.races << race;
		if (alias == kEmptySlot) r.player_aliases << QString();
		else                     r.player_aliases << alias;
	}

	// Same duplicate-alias check we do locally. Server would
	// reject it too, but a client-side message is faster.
	for (int i = 0; i < r.player_aliases.size(); ++i) {
		if (r.player_aliases[i].isEmpty()) continue;
		if (r.player_aliases[i] == kAIBot) continue;   // AIBot may repeat
		for (int j = i + 1; j < r.player_aliases.size(); ++j) {
			if (r.player_aliases[i] == r.player_aliases[j]) {
				QMessageBox::warning(this, tr("New remote game"),
					tr("Alias '%1' is used in more than one slot.")
						.arg(r.player_aliases[i]));
				return;
			}
		}
	}

	params_ = r;
	accept();
}

}   // namespace simsc_desktop
