#include "main_window.h"

#include "../agent_catalog.h"
#include "../agent_supervisor.h"
#include "../app_paths.h"
#include "../local_server_manager.h"
#include "../local_user_roster.h"
#include "../map_catalog.h"
#include "../settings.h"
#include "../simsc_api_client.h"
#include "local_games_tab.h"
#include "remote_games_tab.h"
#include "settings_tab.h"

#include <QtCore/QCoreApplication>
#include <QtWidgets/QTabWidget>

namespace simsc_desktop {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
	setWindowTitle(tr("simsc_desktop"));
	resize(960, 640);

	paths_    = new AppPaths();
	settings_ = new Settings(this);
	roster_   = new LocalUserRoster(this);
	roster_->load();

	// If the user hasn't picked an SC1 data path yet, seed it with
	// the bundled MPQ dir (if present). Makes the fresh-install
	// experience one click shorter.
	if (settings_->sc1_data_path().isEmpty()
	    && !paths_->bundled_mpq_dir().isEmpty()) {
		settings_->set_sc1_data_path(paths_->bundled_mpq_dir());
	}

	catalog_  = new MapCatalog(this);
	catalog_->loadFromFile(paths_->maps_json_path());

	manager_    = new LocalServerManager(paths_, settings_, roster_, this);
	api_        = new SimscApiClient(settings_, this);
	agents_     = new AgentCatalog(this);
	agents_->set_agents_dir(settings_->agents_dir());
	supervisor_ = new AgentSupervisor(this);

	// If the user updates the agents directory later, keep the
	// catalog in sync -- picker rescans automatically the next time
	// it opens.
	connect(settings_, &Settings::changed, agents_, [this] {
		agents_->set_agents_dir(settings_->agents_dir());
	});

	// Kill every local server before we drop off the event loop.
	// aboutToQuit fires after the last widget closes; window
	// closeEvents have already surfaced through the QApplication
	// event loop by this point.
	//
	// Order matters: kill agents FIRST so they drop their WS to
	// openbw_server cleanly, THEN kill the server itself. Otherwise
	// dying servers can trip agents' error paths and leave orphan
	// python processes.
	connect(qApp, &QCoreApplication::aboutToQuit,
		supervisor_, &AgentSupervisor::stop_all);
	connect(qApp, &QCoreApplication::aboutToQuit,
		manager_, &LocalServerManager::stopAll);

	tabs_         = new QTabWidget(this);
	local_tab_    = new LocalGamesTab(
		paths_, settings_, roster_, catalog_, manager_,
		agents_, supervisor_, tabs_);
	remote_tab_   = new RemoteGamesTab(
		paths_, settings_, catalog_, api_,
		agents_, supervisor_, tabs_);
	settings_tab_ = new SettingsTab(settings_, roster_, tabs_);

	tabs_->addTab(local_tab_,    tr("Local games"));
	tabs_->addTab(remote_tab_,   tr("Remote games"));
	tabs_->addTab(settings_tab_, tr("Settings"));

	setCentralWidget(tabs_);
}

MainWindow::~MainWindow() {
	delete paths_;
	paths_ = nullptr;
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
