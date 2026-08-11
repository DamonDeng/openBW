#include "local_games_tab.h"

#include "../agent_catalog.h"
#include "../agent_supervisor.h"
#include "../app_paths.h"
#include "../local_server_manager.h"
#include "../local_user_roster.h"
#include "../map_catalog.h"
#include "../settings.h"
#include "attach_agent_dialog.h"
#include "new_local_game_dialog.h"
#include "observer_window.h"

#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QTableView>
#include <QtWidgets/QVBoxLayout>

namespace simsc_desktop {

LocalGamesTab::LocalGamesTab(const AppPaths* paths,
                             Settings* settings,
                             LocalUserRoster* roster,
                             MapCatalog* catalog,
                             LocalServerManager* manager,
                             AgentCatalog* agents,
                             AgentSupervisor* supervisor,
                             QWidget* parent)
	: QWidget(parent), paths_(paths), settings_(settings),
	  roster_(roster), catalog_(catalog), manager_(manager),
	  agents_(agents), supervisor_(supervisor) {

	auto* root = new QVBoxLayout(this);

	// ------------------------------------------------------------
	// Tier 1: games table + game-level toolbar (New / Stop).
	// ------------------------------------------------------------
	auto* toolbar = new QHBoxLayout;
	new_btn_  = new QPushButton(tr("New game..."), this);
	stop_btn_ = new QPushButton(tr("Stop"), this);
	toolbar->addWidget(new_btn_);
	toolbar->addWidget(stop_btn_);
	toolbar->addStretch();
	root->addLayout(toolbar);

	view_ = new QTableView(this);
	view_->setModel(manager_);
	view_->setSelectionBehavior(QAbstractItemView::SelectRows);
	view_->setSelectionMode(QAbstractItemView::SingleSelection);
	view_->verticalHeader()->setVisible(false);
	view_->horizontalHeader()->setStretchLastSection(true);
	root->addWidget(view_, 1);

	// ------------------------------------------------------------
	// Tier 2: per-player rows for the selected game.
	// ------------------------------------------------------------
	players_box_ = new QGroupBox(tr("Players"), this);
	auto* box_layout = new QVBoxLayout(players_box_);

	auto* scroll = new QScrollArea(players_box_);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	// Cap the players-tier height so a 4-8 player game doesn't
	// squeeze the games table down to a couple of rows.
	scroll->setMinimumHeight(120);
	scroll->setMaximumHeight(220);

	players_container_ = new QWidget(scroll);
	players_layout_    = new QVBoxLayout(players_container_);
	players_layout_->setContentsMargins(0, 0, 0, 0);
	players_layout_->addStretch();
	scroll->setWidget(players_container_);

	box_layout->addWidget(scroll);
	root->addWidget(players_box_);

	// ------------------------------------------------------------
	// Signal wiring.
	// ------------------------------------------------------------
	connect(new_btn_,  &QPushButton::clicked, this, &LocalGamesTab::onNewGame);
	connect(stop_btn_, &QPushButton::clicked, this, &LocalGamesTab::onStopGame);

	connect(manager_, &LocalServerManager::errorOccurred, this,
		[this](const QString& game_id, const QString& msg) {
			QMessageBox::warning(this, tr("Local game"),
				game_id.isEmpty() ? msg
				                  : tr("Game %1: %2").arg(game_id, msg));
		});

	connect(view_->selectionModel(),
		&QItemSelectionModel::selectionChanged, this,
		[this] {
			updateActionEnabled();
			rebuildPlayersBox();
		});
	connect(manager_, &LocalServerManager::gameStatusChanged, this,
		[this](const QString& game_id) {
			updateActionEnabled();
			// Only rebuild the players box if the game whose status
			// changed is the one currently selected -- otherwise a
			// background game flipping to Exited would wipe the
			// user's current player buttons.
			if (game_id == selectedGameId()) rebuildPlayersBox();
		});

	// Rebuild the players box whenever an agent attaches or exits so
	// the button label toggles between Attach... and Detach.
	if (supervisor_) {
		connect(supervisor_, &AgentSupervisor::agentAttached, this,
			[this](const QString& game_id, int /*slot*/, const QString&) {
				if (game_id == selectedGameId()) rebuildPlayersBox();
			});
		connect(supervisor_, &AgentSupervisor::agentDetached, this,
			[this](const QString& game_id, int /*slot*/, int /*rc*/) {
				if (game_id == selectedGameId()) rebuildPlayersBox();
			});
	}

	updateActionEnabled();
	rebuildPlayersBox();
}

QString LocalGamesTab::selectedGameId() const {
	const auto sel = view_->selectionModel();
	if (!sel || !sel->hasSelection()) return {};
	const int row = sel->currentIndex().row();
	const auto* g = manager_->gameAtRow(row);
	return g ? g->game_id : QString();
}

void LocalGamesTab::updateActionEnabled() {
	const auto id = selectedGameId();
	const auto* g = id.isEmpty() ? nullptr : manager_->gameById(id);
	const bool is_active = g
		&& (g->status == LocalGame::Status::Starting
		    || g->status == LocalGame::Status::Running);
	stop_btn_->setEnabled(is_active);
}

void LocalGamesTab::rebuildPlayersBox() {
	// Blow the old rows away. We skip the trailing stretch item so
	// the container doesn't collapse to the top.
	while (players_layout_->count() > 1) {
		auto* item = players_layout_->takeAt(0);
		if (auto* w = item->widget()) w->deleteLater();
		delete item;
	}

	const auto id = selectedGameId();
	const auto* g = id.isEmpty() ? nullptr : manager_->gameById(id);
	if (!g) {
		players_box_->setTitle(tr("Players"));
		auto* hint = new QLabel(
			tr("Select a game above to see its players."),
			players_container_);
		hint->setEnabled(false);
		players_layout_->insertWidget(0, hint);
		return;
	}

	players_box_->setTitle(tr("Players in game %1").arg(id));

	const bool is_active = (g->status == LocalGame::Status::Starting
	                        || g->status == LocalGame::Status::Running);

	for (const auto& lp : g->players) {
		auto* row_w = new QWidget(players_container_);
		auto* h     = new QHBoxLayout(row_w);
		h->setContentsMargins(0, 0, 0, 0);

		auto* label = new QLabel(
			tr("Slot %1 -- %2 (%3)")
				.arg(lp.slot).arg(lp.alias, lp.race),
			row_w);
		label->setMinimumWidth(220);

		auto* observe_btn = new QPushButton(tr("Observer"), row_w);
		auto* copy_btn    = new QPushButton(tr("Copy agent URL"), row_w);
		observe_btn->setEnabled(is_active);

		const auto alias = lp.alias;
		const auto race  = lp.race;
		const auto slot  = lp.slot;
		connect(observe_btn, &QPushButton::clicked, this,
			[this, id, alias] { openObserverAs(id, alias); });
		connect(copy_btn, &QPushButton::clicked, this,
			[this, id, alias] { copyAgentUrlFor(id, alias); });

		// Attach / Detach agent button. For local games the lobby
		// holds every slot's api-key via LocalUserRoster, so the
		// button is visible on every slot regardless of whose
		// desktop this is. The button label toggles based on the
		// supervisor's current state.
		QPushButton* attach_btn = nullptr;
		if (agents_ && supervisor_ && !settings_->agents_dir().isEmpty()) {
			const bool attached = supervisor_->is_attached(id, slot);
			attach_btn = new QPushButton(
				attached
					? tr("Detach agent")
					: tr("Attach agent..."),
				row_w);
			attach_btn->setEnabled(is_active);
			if (attached) {
				attach_btn->setToolTip(
					tr("Currently attached: %1")
						.arg(supervisor_->attached_at(id, slot)));
			}
			connect(attach_btn, &QPushButton::clicked, this,
				[this, id, slot, alias, race, attached] {
					if (attached) {
						onDetachAgent(id, slot);
					} else {
						onAttachAgent(id, slot, alias, race);
					}
				});
		}

		h->addWidget(label);
		h->addStretch();
		h->addWidget(observe_btn);
		h->addWidget(copy_btn);
		if (attach_btn) h->addWidget(attach_btn);

		players_layout_->insertWidget(players_layout_->count() - 1, row_w);
	}
}

void LocalGamesTab::onNewGame() {
	if (roster_->users().isEmpty()) {
		QMessageBox::information(this, tr("New local game"),
			tr("Add at least one local user in Settings before "
			   "starting a game."));
		return;
	}
	if (catalog_->rowCount() == 0) {
		QMessageBox::information(this, tr("New local game"),
			tr("No maps found in the bundled maps directory. Set "
			   "SIMSC_DESKTOP_MAPS_SOURCE_DIR at build time or "
			   "point AppPaths at a maps directory."));
		return;
	}

	NewLocalGameDialog dlg(catalog_, roster_,
	                       (quint16)settings_->default_local_port(),
	                       paths_->bundled_maps_dir(), this);
	if (dlg.exec() != QDialog::Accepted) return;

	const auto game_id = manager_->startGame(dlg.getParams());
	if (!game_id.isEmpty()) {
		const int r = manager_->rowCount() - 1;
		view_->selectRow(r);
	}
}

void LocalGamesTab::onStopGame() {
	const auto id = selectedGameId();
	if (id.isEmpty()) return;
	manager_->stopGame(id);
}

void LocalGamesTab::openObserverAs(const QString& game_id,
                                   const QString& alias) {
	const auto* g = manager_->gameById(game_id);
	if (!g) return;

	ObserverParams p;
	p.title     = tr("simsc_desktop observer: %1 (%2)").arg(game_id, alias);
	p.url       = manager_->observerUrl(game_id);
	p.data_path = settings_->sc1_data_path();
	p.map_path  = g->map_abspath;
	// The `alias` overload picks that specific user's key so the
	// server routes the observer to that player's perspective
	// (fog, minimap fill, viewing_slot).
	p.api_key   = manager_->apiKeyForObserver(game_id, alias);

	// Race integers MUST match bwgame::race_t (game_types.h:108):
	//   zerg=0, terran=1, protoss=2. Getting this wrong silently
	//   spawns starting units of the wrong race and desyncs the
	//   observer at frame 0.
	for (const auto& lp : g->players) {
		if (lp.slot < 0 || lp.slot >= 8) continue;
		int r = -1;
		if      (lp.race == "zerg")    r = 0;
		else if (lp.race == "terran")  r = 1;
		else if (lp.race == "protoss") r = 2;
		p.race_overrides[lp.slot] = r;
	}

	new ObserverWindow(p, this);
}

void LocalGamesTab::copyAgentUrlFor(const QString& game_id,
                                    const QString& alias) {
	const auto url = manager_->agentUrl(game_id);
	const auto key = manager_->apiKeyForObserver(game_id, alias);
	const auto full = key.isEmpty()
		? url
		: QStringLiteral("%1?key=%2").arg(url, key);
	QGuiApplication::clipboard()->setText(full);
}

void LocalGamesTab::onAttachAgent(const QString& game_id, int slot,
                                  const QString& alias,
                                  const QString& race) {
	if (!agents_ || !supervisor_) return;
	AttachAgentDialog dlg(agents_, alias, race, this);
	if (dlg.exec() != QDialog::Accepted) return;
	const auto path = dlg.picked_path();
	if (path.isEmpty()) return;
	// The agent URL for local games is what agentUrl(game_id) returns
	// (base ws:// with no query string). Its api-key is the roster
	// entry for this slot's alias.
	const auto url = manager_->agentUrl(game_id);
	const auto key = manager_->apiKeyForObserver(game_id, alias);
	if (url.isEmpty() || key.isEmpty()) {
		QMessageBox::warning(this, tr("Attach agent"),
			tr("Cannot resolve URL or API key for slot %1 (%2). "
			   "Is the game running?").arg(slot).arg(alias));
		return;
	}
	if (!supervisor_->attach(game_id, slot, path,
	                         dlg.picked_display_name(),
	                         url, key, race)) {
		QMessageBox::warning(this, tr("Attach agent"),
			tr("Failed to launch %1 -- another agent is already "
			   "attached to slot %2, or the file couldn't be "
			   "executed.").arg(dlg.picked_display_name()).arg(slot));
	}
}

void LocalGamesTab::onDetachAgent(const QString& game_id, int slot) {
	if (!supervisor_) return;
	supervisor_->detach(game_id, slot);
}

}   // namespace simsc_desktop
