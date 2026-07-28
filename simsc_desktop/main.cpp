// simsc_desktop entry point.
//
// The org+app names below are picked up by QSettings so all our
// getters land in the same platform-native config file. Keep them
// stable -- changing either would move every user's persisted
// settings out from under them.
//
// Note: bwgame.h / ui.h are deliberately NOT included from this
// file. Those headers define global tables (see korean.h) and
// must be pulled into exactly ONE translation unit -- which for
// simsc_desktop is observer_window.cpp. The ui::log_str /
// fatal_error_str hooks the game code needs at link time live
// there too.

#include "ui/main_window.h"

#include <QtCore/QCoreApplication>
#include <QtWidgets/QApplication>

int main(int argc, char** argv) {
	QCoreApplication::setOrganizationName(QStringLiteral("openbw"));
	QCoreApplication::setOrganizationDomain(QStringLiteral("openbw.org"));
	QCoreApplication::setApplicationName(QStringLiteral("simsc_desktop"));

	QApplication app(argc, argv);

	simsc_desktop::MainWindow w;
	w.show();

	return app.exec();
}
