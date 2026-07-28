// Top-level app window: title bar + three tabs.
//
// Owns everything the app needs at process-lifetime scope:
//   Settings, LocalUserRoster, AppPaths, MapCatalog, and
//   LocalServerManager. Tabs hold non-owning pointers.
//
// Shutdown ordering (aboutToQuit):
//   1. Tab widgets close first (Qt does that automatically as
//      children of the QMainWindow).
//   2. LocalServerManager::stopAll() sends SIGTERM to every
//      running openbw_server, waits, then SIGKILLs stragglers.
//      Its destructor does the same as a safety net.

#pragma once

#include <QtWidgets/QMainWindow>

class QTabWidget;

namespace simsc_desktop {

class AppPaths;
class LocalGamesTab;
class LocalServerManager;
class LocalUserRoster;
class MapCatalog;
class RemoteGamesTab;
class SettingsTab;
class Settings;

class MainWindow : public QMainWindow {
	Q_OBJECT
public:
	explicit MainWindow(QWidget* parent = nullptr);
	~MainWindow() override;

protected:
	void showEvent(QShowEvent* e) override;

private:
	AppPaths*           paths_    = nullptr;
	Settings*           settings_ = nullptr;
	LocalUserRoster*    roster_   = nullptr;
	MapCatalog*         catalog_  = nullptr;
	LocalServerManager* manager_  = nullptr;

	QTabWidget*     tabs_          = nullptr;
	LocalGamesTab*  local_tab_     = nullptr;
	RemoteGamesTab* remote_tab_    = nullptr;
	SettingsTab*    settings_tab_  = nullptr;
};

}   // namespace simsc_desktop
