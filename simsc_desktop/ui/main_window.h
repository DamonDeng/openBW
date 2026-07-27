// Top-level app window: title bar + three tabs.
//
// Phase 1a wiring:
//   * owns Settings + LocalUserRoster (as children).
//   * hosts LocalGamesTab (stub), RemoteGamesTab (stub), SettingsTab.
//   * enforces first-run: if the roster is empty AND
//     settings.has_seen_first_run() is false, switch to the Settings
//     tab on show. Setting sc1_data_path or adding a local user
//     marks the flag.
//
// Later phases attach LocalServerManager + SimscApiClient. Those
// members are placeholders now (nullptr) so the header doesn't
// need churn.

#pragma once

#include <QtWidgets/QMainWindow>

class QTabWidget;

namespace simsc_desktop {

class Settings;
class LocalUserRoster;
class LocalGamesTab;
class RemoteGamesTab;
class SettingsTab;

class MainWindow : public QMainWindow {
	Q_OBJECT
public:
	explicit MainWindow(QWidget* parent = nullptr);

protected:
	void showEvent(QShowEvent* e) override;

private:
	Settings*        settings_ = nullptr;
	LocalUserRoster* roster_   = nullptr;

	QTabWidget*     tabs_          = nullptr;
	LocalGamesTab*  local_tab_     = nullptr;
	RemoteGamesTab* remote_tab_    = nullptr;
	SettingsTab*    settings_tab_  = nullptr;
};

}   // namespace simsc_desktop
