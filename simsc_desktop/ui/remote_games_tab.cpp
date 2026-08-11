#include "remote_games_tab.h"

#include "../agent_catalog.h"
#include "../agent_supervisor.h"
#include "../app_paths.h"
#include "../map_catalog.h"
#include "../remote_games_model.h"
#include "../settings.h"
#include "../simsc_api_client.h"
#include "attach_agent_dialog.h"
#include "new_remote_game_dialog.h"
#include "observer_window.h"

#include <QtCore/QTimer>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QFrame>
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

namespace {

// Poll interval for GET /api/games while the tab is visible.
// 10 s is a balance between "state changes are visible fast" and
// "the server sees N clients per user rather than N * 10 in a
// stampede". Consider making this a Setting later.
constexpr int kPollIntervalMs = 10 * 1000;

}   // namespace

RemoteGamesTab::RemoteGamesTab(const AppPaths* paths,
                               Settings* settings,
                               MapCatalog* catalog,
                               SimscApiClient* api,
                               AgentCatalog* agents,
                               AgentSupervisor* supervisor,
                               QWidget* parent)
	: QWidget(parent), paths_(paths), settings_(settings),
	  catalog_(catalog), api_(api),
	  agents_(agents), supervisor_(supervisor) {

	auto* root = new QVBoxLayout(this);

	// Reveal-only banner when the API key isn't set. Everything
	// else is disabled in that state.
	no_key_hint_ = new QLabel(
		tr("No simsc API key set. Enter one in Settings to use "
		   "the Remote Games tab."), this);
	no_key_hint_->setWordWrap(true);
	no_key_hint_->setStyleSheet(
		"QLabel { color: #b58900; background: #fdf6e3; padding: 6px; }");
	root->addWidget(no_key_hint_);

	// Toolbar.
	auto* toolbar = new QHBoxLayout;
	new_btn_     = new QPushButton(tr("New game..."), this);
	refresh_btn_ = new QPushButton(tr("Refresh"), this);
	accept_btn_  = new QPushButton(tr("Accept"), this);
	decline_btn_ = new QPushButton(tr("Decline"), this);
	cancel_btn_  = new QPushButton(tr("Cancel"), this);
	delete_btn_  = new QPushButton(tr("Delete"), this);
	toolbar->addWidget(new_btn_);
	toolbar->addWidget(refresh_btn_);
	toolbar->addSpacing(12);
	toolbar->addWidget(accept_btn_);
	toolbar->addWidget(decline_btn_);
	toolbar->addSpacing(12);
	toolbar->addWidget(cancel_btn_);
	toolbar->addWidget(delete_btn_);
	toolbar->addStretch();
	root->addLayout(toolbar);

	// Games table.
	model_ = new RemoteGamesModel(this);
	view_  = new QTableView(this);
	view_->setModel(model_);
	view_->setSelectionBehavior(QAbstractItemView::SelectRows);
	view_->setSelectionMode(QAbstractItemView::SingleSelection);
	view_->verticalHeader()->setVisible(false);
	view_->horizontalHeader()->setStretchLastSection(true);
	root->addWidget(view_, 1);

	// Per-player tier.
	players_box_ = new QGroupBox(tr("Players"), this);
	auto* box_layout = new QVBoxLayout(players_box_);
	auto* scroll = new QScrollArea(players_box_);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setMinimumHeight(120);
	scroll->setMaximumHeight(220);
	players_container_ = new QWidget(scroll);
	players_layout_    = new QVBoxLayout(players_container_);
	players_layout_->setContentsMargins(0, 0, 0, 0);
	players_layout_->addStretch();
	scroll->setWidget(players_container_);
	box_layout->addWidget(scroll);
	root->addWidget(players_box_);

	// Wiring.
	connect(new_btn_,     &QPushButton::clicked, this, &RemoteGamesTab::onNewGame);
	connect(refresh_btn_, &QPushButton::clicked, this, &RemoteGamesTab::refresh);
	connect(accept_btn_,  &QPushButton::clicked, this, &RemoteGamesTab::onAccept);
	connect(decline_btn_, &QPushButton::clicked, this, &RemoteGamesTab::onDecline);
	connect(cancel_btn_,  &QPushButton::clicked, this, &RemoteGamesTab::onCancel);
	connect(delete_btn_,  &QPushButton::clicked, this, &RemoteGamesTab::onDelete);

	connect(view_->selectionModel(),
		&QItemSelectionModel::selectionChanged, this,
		[this] { updateActionEnabled(); rebuildPlayersBox(); });

	connect(api_, &SimscApiClient::usersReceived,
		this, &RemoteGamesTab::onUsersReceived);
	connect(api_, &SimscApiClient::gamesReceived,
		this, &RemoteGamesTab::onGamesReceived);
	connect(api_, &SimscApiClient::gameCreated,
		this, &RemoteGamesTab::onGameCreated);
	// Any successful mutation triggers a full refresh -- cheap
	// enough at 10-50 games and keeps the model consistent even
	// when the server-side state machine reshapes things (e.g.
	// last decline auto-cancels the game).
	connect(api_, &SimscApiClient::gameAccepted,
		this, [this](const RemoteGame&){ refresh(); });
	connect(api_, &SimscApiClient::gameDeclined,
		this, [this](const QString&){ refresh(); });
	connect(api_, &SimscApiClient::gameCancelled,
		this, [this](const QString&){ refresh(); });
	connect(api_, &SimscApiClient::gameDeleted,
		this, [this](const QString&){ refresh(); });

	connect(api_, &SimscApiClient::errorOccurred,
		this, &RemoteGamesTab::onError);

	// React to the api key being set/cleared while the tab is open
	// (e.g. user pasted a key in Settings). Cheap to just re-eval.
	connect(settings_, &Settings::changed,
		this, [this] { updateActionEnabled(); });

	// Rebuild the players box when an agent attaches / exits so the
	// button label toggles.
	if (supervisor_) {
		connect(supervisor_, &AgentSupervisor::agentAttached, this,
			[this](const QString& game_id, int, const QString&) {
				if (game_id == selectedGameId()) rebuildPlayersBox();
			});
		connect(supervisor_, &AgentSupervisor::agentDetached, this,
			[this](const QString& game_id, int, int) {
				if (game_id == selectedGameId()) rebuildPlayersBox();
			});
	}

	poll_timer_ = new QTimer(this);
	poll_timer_->setInterval(kPollIntervalMs);
	connect(poll_timer_, &QTimer::timeout, this, &RemoteGamesTab::refresh);

	updateActionEnabled();
	rebuildPlayersBox();
}

void RemoteGamesTab::showEvent(QShowEvent* e) {
	QWidget::showEvent(e);
	if (!settings_->simsc_api_key().isEmpty()) {
		refresh();
		poll_timer_->start();
	}
}

void RemoteGamesTab::hideEvent(QHideEvent* e) {
	QWidget::hideEvent(e);
	poll_timer_->stop();
}

QString RemoteGamesTab::selectedGameId() const {
	const auto sel = view_->selectionModel();
	if (!sel || !sel->hasSelection()) return {};
	const auto* g = model_->gameAtRow(sel->currentIndex().row());
	return g ? g->game_id : QString();
}

QString RemoteGamesTab::myAlias() const {
	// simsc doesn't have a "who am I" endpoint separate from the
	// user roster; we key off the API key and find the alias
	// whose row we can see. Simplest heuristic: any game we own
	// carries owner_alias == me. If we're not the owner of any
	// listed game, we fall back to the first game where we're an
	// invitee (my_invitation_status != ""). Ties are rare; if the
	// user has never appeared in any listed game, alias stays
	// empty and per-player Observer stays disabled.
	for (int r = 0; r < model_->rowCount(); ++r) {
		const auto* g = model_->gameAtRow(r);
		if (!g) continue;
		if (!g->my_invitation_status.isEmpty()) {
			for (const auto& inv : g->invitations) {
				if (inv.status == g->my_invitation_status) return inv.alias;
			}
		}
	}
	// Second pass: games I own.
	for (int r = 0; r < model_->rowCount(); ++r) {
		const auto* g = model_->gameAtRow(r);
		if (g && !g->owner_alias.isEmpty()) return g->owner_alias;
	}
	return {};
}

void RemoteGamesTab::updateActionEnabled() {
	const bool have_key = !settings_->simsc_api_key().isEmpty();
	no_key_hint_->setVisible(!have_key);
	new_btn_->setEnabled(have_key);
	refresh_btn_->setEnabled(have_key);
	view_->setEnabled(have_key);

	if (!have_key) {
		accept_btn_->setEnabled(false);
		decline_btn_->setEnabled(false);
		cancel_btn_->setEnabled(false);
		delete_btn_->setEnabled(false);
		return;
	}

	const auto id = selectedGameId();
	const auto* g = id.isEmpty() ? nullptr : model_->gameById(id);
	const bool have_sel = g != nullptr;

	const bool pending_for_me = g && g->my_invitation_status == "pending";
	const bool i_own = g && g->owner_alias == myAlias();
	const bool is_pending_state = g && g->state == "pending_invitations";

	accept_btn_->setEnabled(pending_for_me);
	decline_btn_->setEnabled(pending_for_me);
	cancel_btn_->setEnabled(i_own && is_pending_state);
	delete_btn_->setEnabled(have_sel && (i_own || false));   // owner only
}

void RemoteGamesTab::rebuildPlayersBox() {
	while (players_layout_->count() > 1) {
		auto* item = players_layout_->takeAt(0);
		if (auto* w = item->widget()) w->deleteLater();
		delete item;
	}

	const auto id = selectedGameId();
	const auto* g = id.isEmpty() ? nullptr : model_->gameById(id);
	if (!g) {
		players_box_->setTitle(tr("Players"));
		auto* hint = new QLabel(
			tr("Select a game above to see its players."),
			players_container_);
		hint->setEnabled(false);
		players_layout_->insertWidget(0, hint);
		return;
	}

	players_box_->setTitle(tr("Players in game %1").arg(g->game_id.left(8)));

	const bool is_running = (g->state == "running");
	const auto me = myAlias();

	for (int i = 0; i < g->player_aliases.size(); ++i) {
		auto* row_w = new QWidget(players_container_);
		auto* h     = new QHBoxLayout(row_w);
		h->setContentsMargins(0, 0, 0, 0);

		const auto ali  = g->player_aliases[i];
		const auto race = i < g->races.size() ? g->races[i] : QString();
		auto* label = new QLabel(
			tr("Slot %1 -- %2 (%3)")
				.arg(i)
				.arg(ali.isEmpty() ? QStringLiteral("(empty)") : ali)
				.arg(race),
			row_w);
		label->setMinimumWidth(220);
		h->addWidget(label);
		h->addStretch();

		// Only surface Observer/Copy/Attach for the row that is ME.
		// The remote server only issues me an api key for my own
		// perspective; other players' rows are informational.
		if (!me.isEmpty() && ali == me) {
			auto* observe_btn = new QPushButton(tr("Observer"), row_w);
			auto* copy_btn    = new QPushButton(tr("Copy agent URL"), row_w);
			observe_btn->setEnabled(is_running && !g->observer_url.isEmpty());
			copy_btn->setEnabled(!g->agent_url.isEmpty());
			const auto id_copy = g->game_id;
			connect(observe_btn, &QPushButton::clicked, this,
				[this, id_copy] { openObserverForMe(id_copy); });
			connect(copy_btn, &QPushButton::clicked, this,
				[this, id_copy] { copyMyAgentUrl(id_copy); });
			h->addWidget(observe_btn);
			h->addWidget(copy_btn);

			// Attach / Detach agent for MY slot.
			if (agents_ && supervisor_
			    && !settings_->agents_dir().isEmpty()) {
				const int slot = i;
				const auto race_copy = race;
				const bool attached = supervisor_->is_attached(
					id_copy, slot);
				auto* attach_btn = new QPushButton(
					attached
						? tr("Detach agent")
						: tr("Attach agent..."),
					row_w);
				attach_btn->setEnabled(is_running
				                       && !g->agent_url.isEmpty());
				if (attached) {
					attach_btn->setToolTip(
						tr("Currently attached: %1").arg(
							supervisor_->attached_at(
								id_copy, slot)));
				}
				const auto ali_copy = ali;
				connect(attach_btn, &QPushButton::clicked, this,
					[this, id_copy, slot, ali_copy, race_copy, attached] {
						if (attached) onDetachAgent(id_copy, slot);
						else onAttachAgent(id_copy, slot,
						                   ali_copy, race_copy);
					});
				h->addWidget(attach_btn);
			}
		}

		players_layout_->insertWidget(players_layout_->count() - 1, row_w);
	}
}

void RemoteGamesTab::refresh() {
	if (settings_->simsc_api_key().isEmpty()) return;
	api_->listGames();
}

void RemoteGamesTab::onNewGame() {
	if (settings_->simsc_api_key().isEmpty()) {
		QMessageBox::information(this, tr("New remote game"),
			tr("Set the simsc API key in Settings first."));
		return;
	}
	if (catalog_->rowCount() == 0) {
		QMessageBox::information(this, tr("New remote game"),
			tr("No maps found."));
		return;
	}

	// Roster is fetched lazily. If we don't have it yet, ask for
	// it and flag ourselves so the dialog opens when it arrives.
	if (user_cache_.isEmpty() && !users_pending_) {
		users_pending_ = true;
		open_dialog_when_users_ready_ = true;
		api_->listUsers();
		return;
	}
	if (user_cache_.isEmpty()) {
		// Already pending, just wait.
		open_dialog_when_users_ready_ = true;
		return;
	}

	NewRemoteGameDialog dlg(catalog_, user_cache_, myAlias(),
	                        QStringLiteral("fastest"), this);
	if (dlg.exec() != QDialog::Accepted) return;
	api_->createGame(dlg.getParams());
}

void RemoteGamesTab::onAccept() {
	const auto id = selectedGameId();
	if (!id.isEmpty()) api_->acceptGame(id);
}

void RemoteGamesTab::onDecline() {
	const auto id = selectedGameId();
	if (!id.isEmpty()) api_->declineGame(id);
}

void RemoteGamesTab::onCancel() {
	const auto id = selectedGameId();
	if (id.isEmpty()) return;
	if (QMessageBox::question(this, tr("Cancel game"),
		tr("Cancel game %1?").arg(id.left(8)))
		!= QMessageBox::Yes) return;
	api_->cancelGame(id);
}

void RemoteGamesTab::onDelete() {
	const auto id = selectedGameId();
	if (id.isEmpty()) return;
	if (QMessageBox::question(this, tr("Delete game"),
		tr("Delete game %1? This tears down the running pod if any.")
			.arg(id.left(8)))
		!= QMessageBox::Yes) return;
	api_->deleteGame(id);
}

void RemoteGamesTab::onUsersReceived(const QVector<RemoteUser>& users) {
	user_cache_    = users;
	users_pending_ = false;
	if (open_dialog_when_users_ready_) {
		open_dialog_when_users_ready_ = false;
		onNewGame();
	}
}

void RemoteGamesTab::onGamesReceived(const QVector<RemoteGame>& games) {
	// Preserve current selection across the reset if we can.
	const auto sel_id = selectedGameId();
	model_->setGames(games);
	if (!sel_id.isEmpty()) {
		const int r = model_->rowOfGame(sel_id);
		if (r >= 0) view_->selectRow(r);
	}
	updateActionEnabled();
	rebuildPlayersBox();
}

void RemoteGamesTab::onGameCreated(const RemoteGame&) {
	refresh();
}

void RemoteGamesTab::onError(const QString& op, int status, const QString& msg) {
	// Reset the pending-users flag if the failure was on the
	// roster fetch itself. Otherwise the tab gets stuck: New Game
	// waits forever on user_cache_ that will never populate, and
	// clicking the button becomes a silent no-op.
	if (op == QStringLiteral("list_users")) {
		users_pending_ = false;
		open_dialog_when_users_ready_ = false;
	}

	// Non-blocking for list_games so a transient network failure
	// during a background poll doesn't spam a modal. All other
	// ops are user-initiated -> surface immediately.
	if (op == QStringLiteral("list_games")) {
		qWarning("simsc list_games failed: [%d] %s",
			status, msg.toUtf8().constData());
		return;
	}
	QMessageBox::warning(this, tr("Remote games"),
		tr("%1 failed (HTTP %2): %3").arg(op).arg(status).arg(msg));
}

void RemoteGamesTab::openObserverForMe(const QString& game_id) {
	const auto* g = model_->gameById(game_id);
	if (!g || g->observer_url.isEmpty()) return;

	ObserverParams p;
	p.title     = tr("simsc_desktop remote observer: %1 (%2)")
		.arg(game_id.left(8), myAlias());
	p.url       = g->observer_url;
	p.data_path = settings_->sc1_data_path();
	p.api_key   = settings_->simsc_api_key();

	// Remote map is a filename inside the server-side bundle. Our
	// local observer needs an absolute path to the same .scm. Use
	// the bundled maps dir; if the map isn't present locally, load
	// will fail and the observer window will surface it.
	p.map_path = paths_->bundled_maps_dir().isEmpty()
		? g->map
		: paths_->bundled_maps_dir() + "/" + g->map;

	// Races: same integer mapping as local (game_types.h:108,
	// zerg=0/terran=1/protoss=2).
	for (int i = 0; i < g->races.size() && i < 8; ++i) {
		int r = -1;
		if      (g->races[i] == "zerg")    r = 0;
		else if (g->races[i] == "terran")  r = 1;
		else if (g->races[i] == "protoss") r = 2;
		p.race_overrides[i] = r;
	}

	new ObserverWindow(p, this);
}

void RemoteGamesTab::copyMyAgentUrl(const QString& game_id) {
	const auto* g = model_->gameById(game_id);
	if (!g || g->agent_url.isEmpty()) return;
	const auto key = settings_->simsc_api_key();
	const auto full = key.isEmpty()
		? g->agent_url
		: QStringLiteral("%1?key=%2").arg(g->agent_url, key);
	QGuiApplication::clipboard()->setText(full);
}

void RemoteGamesTab::onAttachAgent(const QString& game_id, int slot,
                                   const QString& alias,
                                   const QString& race) {
	if (!agents_ || !supervisor_) return;
	const auto* g = model_->gameById(game_id);
	if (!g || g->agent_url.isEmpty()) return;
	AttachAgentDialog dlg(agents_, alias, race, this);
	if (dlg.exec() != QDialog::Accepted) return;
	const auto path = dlg.picked_path();
	if (path.isEmpty()) return;
	const auto key = settings_->simsc_api_key();
	if (key.isEmpty()) {
		QMessageBox::warning(this, tr("Attach agent"),
			tr("No simsc API key set. Enter one in Settings."));
		return;
	}
	if (!supervisor_->attach(game_id, slot, path,
	                         dlg.picked_display_name(),
	                         g->agent_url, key, race)) {
		QMessageBox::warning(this, tr("Attach agent"),
			tr("Failed to launch %1 -- another agent is already "
			   "attached to slot %2, or the file couldn't be "
			   "executed.").arg(dlg.picked_display_name()).arg(slot));
	}
}

void RemoteGamesTab::onDetachAgent(const QString& game_id, int slot) {
	if (!supervisor_) return;
	supervisor_->detach(game_id, slot);
}

}   // namespace simsc_desktop
