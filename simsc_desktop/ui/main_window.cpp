#include "main_window.h"

#include "../local_user_roster.h"
#include "../settings.h"
#include "local_games_tab.h"
#include "remote_games_tab.h"
#include "settings_tab.h"

#include <QtWidgets/QTabWidget>

namespace simsc_desktop {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
	setWindowTitle(tr("simsc_desktop"));
	resize(960, 640);

	settings_ = new Settings(this);
	roster_   = new LocalUserRoster(this);
	roster_->load();

	tabs_         = new QTabWidget(this);
	local_tab_    = new LocalGamesTab(tabs_);
	remote_tab_   = new RemoteGamesTab(tabs_);
	settings_tab_ = new SettingsTab(settings_, roster_, tabs_);

	tabs_->addTab(local_tab_,    tr("Local games"));
	tabs_->addTab(remote_tab_,   tr("Remote games"));
	tabs_->addTab(settings_tab_, tr("Settings"));

	setCentralWidget(tabs_);
}

void MainWindow::showEvent(QShowEvent* e) {
	QMainWindow::showEvent(e);

	// One-shot first-run: if the user has never saved a setting AND
	// the roster is empty, drop them straight into Settings so they
	// know where to look. mark_first_run_seen() so we don't nag on
	// subsequent launches even if they left everything blank.
	if (!settings_->has_seen_first_run()
	    && roster_->users().isEmpty()
	    && settings_->sc1_data_path().isEmpty()) {
		tabs_->setCurrentWidget(settings_tab_);
		settings_->mark_first_run_seen();
	}
}

}   // namespace simsc_desktop
