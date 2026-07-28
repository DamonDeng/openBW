#include "new_local_game_dialog.h"

#include "../local_user_roster.h"
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

// The four race options offered per slot. "random" is a
// map-provided default (server doesn't override; slot keeps the
// map's assignment). The three named races become --race flags.
const QStringList kRaceOptions = {"zerg", "terran", "protoss", "random"};

}   // namespace

NewLocalGameDialog::NewLocalGameDialog(MapCatalog* catalog,
                                       LocalUserRoster* roster,
                                       quint16 default_port,
                                       const QString& maps_dir_abspath,
                                       QWidget* parent)
	: QDialog(parent), catalog_(catalog), roster_(roster),
	  maps_dir_abspath_(maps_dir_abspath) {

	setWindowTitle(tr("New local game"));
	params_.preferred_port = default_port;

	auto* root = new QVBoxLayout(this);

	auto* top     = new QFormLayout;
	map_combo_    = new QComboBox(this);
	map_combo_->setModel(catalog_);
	top->addRow(tr("Map:"), map_combo_);
	root->addLayout(top);

	auto* slots_box = new QGroupBox(tr("Players"), this);
	slots_layout_   = new QVBoxLayout(slots_box);
	root->addWidget(slots_box, 1);

	auto* buttons = new QDialogButtonBox(
		QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	root->addWidget(buttons);
	connect(buttons, &QDialogButtonBox::accepted,
		this, &NewLocalGameDialog::onAccept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	connect(map_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
		this, &NewLocalGameDialog::onMapChanged);

	if (catalog_->rowCount() > 0) {
		map_combo_->setCurrentIndex(0);
		onMapChanged(0);
	}
}

void NewLocalGameDialog::rebuildSlotRows(int player_count) {
	// Blow the old rows away.
	for (auto* w : slot_widgets_) w->deleteLater();
	slot_widgets_.clear();
	slots_.clear();

	// Roster aliases -- same list for every slot dropdown.
	QStringList aliases;
	for (const auto& u : roster_->users()) aliases << u.alias;

	for (int slot = 0; slot < player_count; ++slot) {
		auto* row_w      = new QWidget;
		auto* row_layout = new QHBoxLayout(row_w);
		row_layout->setContentsMargins(0, 0, 0, 0);

		auto* label = new QLabel(tr("Slot %1:").arg(slot), row_w);
		label->setMinimumWidth(60);

		auto* alias = new QComboBox(row_w);
		alias->addItems(aliases);
		if (aliases.isEmpty()) {
			alias->addItem(tr("(no local users -- add some in Settings)"));
			alias->setEnabled(false);
		} else {
			// Default slot N to roster user N (wrapping). Saves a
			// click for the common 2-slot case and, when there
			// are fewer roster entries than slots, at least
			// spreads the duplication so the mistake is
			// obvious. The user still validates duplicates on OK.
			alias->setCurrentIndex(slot % aliases.size());
		}
		auto* race = new QComboBox(row_w);
		race->addItems(kRaceOptions);
		// Cycle default race so a 4-slot map defaults to
		// zerg/terran/protoss/random -- easier to spot a mistake at a
		// glance than four identical zergs.
		race->setCurrentIndex(slot % kRaceOptions.size());

		row_layout->addWidget(label);
		row_layout->addWidget(alias, 1);
		row_layout->addWidget(race);

		slots_layout_->addWidget(row_w);
		slot_widgets_.push_back(row_w);
		slots_.push_back({alias, race});
	}
}

void NewLocalGameDialog::onMapChanged(int row) {
	const auto* e = catalog_->entryAt(row);
	if (!e) return;
	rebuildSlotRows(e->player_count);
}

void NewLocalGameDialog::onAccept() {
	const auto* e = catalog_->entryAt(map_combo_->currentIndex());
	if (!e) {
		QMessageBox::warning(this, tr("New local game"),
			tr("Pick a map first."));
		return;
	}

	NewLocalGameParams p;
	p.preferred_port = params_.preferred_port;
	p.map_relpath    = e->filename;
	// Compose an absolute path from the maps dir + relative
	// filename. If no maps dir is bundled (empty), fall back to the
	// filename itself and let openbw_server resolve it against its
	// data-path.
	p.map_abspath    = maps_dir_abspath_.isEmpty()
		? e->filename
		: (maps_dir_abspath_ + "/" + e->filename);

	for (int i = 0; i < slots_.size(); ++i) {
		LocalPlayer lp;
		lp.slot  = i;
		lp.alias = slots_[i].alias_combo->currentText();
		lp.race  = slots_[i].race_combo->currentText();
		if (lp.alias.isEmpty()
		    || !slots_[i].alias_combo->isEnabled()) {
			QMessageBox::warning(this, tr("New local game"),
				tr("Assign a local user to slot %1.").arg(i));
			return;
		}
		p.players.push_back(lp);
	}

	// Duplicate-alias check: same alias in two slots would leave
	// openbw_server's user table with a colliding key. It handles
	// this at load time by rejecting the second entry, but the
	// resulting error is opaque -- guard here.
	for (int i = 0; i < p.players.size(); ++i) {
		for (int j = i + 1; j < p.players.size(); ++j) {
			if (p.players[i].alias == p.players[j].alias) {
				QMessageBox::warning(this, tr("New local game"),
					tr("Alias '%1' is used in more than one slot.")
						.arg(p.players[i].alias));
				return;
			}
		}
	}

	params_ = p;
	accept();
}

}   // namespace simsc_desktop
