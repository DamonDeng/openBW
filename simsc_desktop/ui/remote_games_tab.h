// Remote Games tab.
//
// Two-tier layout mirroring the local tab:
//   1. Games table -- one row per game visible to the current user
//                     (games they own OR are invited to).
//      Toolbar: [New game] [Refresh] [Accept] [Decline] [Cancel] [Delete].
//   2. Players box  -- for the selected game, one row per slot.
//                     Only the row for MY slot gets an [Observer] +
//                     [Copy agent URL] button pair (the remote server
//                     scopes observer perspective by API key, and I
//                     only have MY key). Other rows are informational.
//
// Data flow:
//   * On tab activation and every 10 s while visible, we GET /api/games
//     and refresh the model.
//   * Any mutation (create / accept / cancel / delete) auto-refreshes
//     the list on completion.

#pragma once

#include "../simsc_types.h"           // RemoteUser (for user cache)

#include <QtCore/QVector>
#include <QtWidgets/QWidget>

class QGroupBox;
class QLabel;
class QPushButton;
class QTableView;
class QTimer;
class QVBoxLayout;

namespace simsc_desktop {

class AgentCatalog;
class AgentSupervisor;
class AppPaths;
class MapCatalog;
class RemoteGamesModel;
class Settings;
class SimscApiClient;

class RemoteGamesTab : public QWidget {
	Q_OBJECT
public:
	RemoteGamesTab(const AppPaths* paths,
	               Settings* settings,
	               MapCatalog* catalog,
	               SimscApiClient* api,
	               AgentCatalog* agents,
	               AgentSupervisor* supervisor,
	               QWidget* parent = nullptr);

protected:
	void showEvent(QShowEvent* e) override;
	void hideEvent(QHideEvent* e) override;

private slots:
	void refresh();
	void onNewGame();
	void onAccept();
	void onDecline();
	void onCancel();
	void onDelete();

	void onUsersReceived(const QVector<RemoteUser>& users);
	void onGamesReceived(const QVector<RemoteGame>& games);
	void onGameCreated(const RemoteGame& game);
	void onError(const QString& op, int status, const QString& msg);

	void updateActionEnabled();
	void rebuildPlayersBox();

private:
	QString selectedGameId() const;
	QString myAlias() const;
	void openObserverForMe(const QString& game_id);
	void copyMyAgentUrl(const QString& game_id);
	void onAttachAgent(const QString& game_id, int slot,
	                   const QString& alias, const QString& race);
	void onDetachAgent(const QString& game_id, int slot);

	const AppPaths*   paths_      = nullptr;
	Settings*         settings_   = nullptr;
	MapCatalog*       catalog_    = nullptr;
	SimscApiClient*   api_        = nullptr;
	AgentCatalog*     agents_     = nullptr;
	AgentSupervisor*  supervisor_ = nullptr;

	RemoteGamesModel* model_    = nullptr;
	QTableView*       view_     = nullptr;

	QPushButton* new_btn_     = nullptr;
	QPushButton* refresh_btn_ = nullptr;
	QPushButton* accept_btn_  = nullptr;
	QPushButton* decline_btn_ = nullptr;
	QPushButton* cancel_btn_  = nullptr;
	QPushButton* delete_btn_  = nullptr;

	QLabel*      no_key_hint_ = nullptr;   // shown when api_key blank

	QGroupBox*   players_box_       = nullptr;
	QWidget*     players_container_ = nullptr;
	QVBoxLayout* players_layout_    = nullptr;

	QTimer*   poll_timer_ = nullptr;

	// Cache of last GET /api/users. Used by the New Remote Game
	// dialog. Refreshed lazily on-demand.
	QVector<RemoteUser> user_cache_;
	bool                users_pending_ = false;
	bool                open_dialog_when_users_ready_ = false;
};

}   // namespace simsc_desktop
