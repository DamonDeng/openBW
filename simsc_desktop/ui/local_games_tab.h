// Local Games tab.
//
// Two tiers, top-to-bottom in one column:
//   1. Games table  -- one row per (running / exited) local game,
//                      bound to LocalServerManager's model.
//                      Toolbar: [New game] [Stop].
//   2. Players box  -- for the selected game, one row per player
//                      with per-player [Observer] and [Copy agent URL]
//                      buttons. The Observer button attaches with that
//                      player's api_key so the perspective matches.
//
// The players box is rebuilt whenever the games-table selection
// changes OR the selected game's status changes.

#pragma once

#include <QtWidgets/QWidget>

class QGroupBox;
class QPushButton;
class QTableView;
class QVBoxLayout;

namespace simsc_desktop {

class LocalServerManager;
class LocalUserRoster;
class MapCatalog;
class Settings;
class AppPaths;

class LocalGamesTab : public QWidget {
	Q_OBJECT
public:
	LocalGamesTab(const AppPaths* paths,
	              Settings* settings,
	              LocalUserRoster* roster,
	              MapCatalog* catalog,
	              LocalServerManager* manager,
	              QWidget* parent = nullptr);

private slots:
	void onNewGame();
	void onStopGame();
	void updateActionEnabled();
	void rebuildPlayersBox();

private:
	QString selectedGameId() const;
	void openObserverAs(const QString& game_id, const QString& alias);
	void copyAgentUrlFor(const QString& game_id, const QString& alias);

	const AppPaths*      paths_    = nullptr;
	Settings*            settings_ = nullptr;
	LocalUserRoster*     roster_   = nullptr;
	MapCatalog*          catalog_  = nullptr;
	LocalServerManager*  manager_  = nullptr;

	QTableView*  view_        = nullptr;
	QPushButton* new_btn_     = nullptr;
	QPushButton* stop_btn_    = nullptr;

	// Player-tier UI. `players_container_` holds one row per player
	// of the selected game; rebuilt in rebuildPlayersBox().
	QGroupBox*   players_box_       = nullptr;
	QWidget*     players_container_ = nullptr;
	QVBoxLayout* players_layout_    = nullptr;
};

}   // namespace simsc_desktop
