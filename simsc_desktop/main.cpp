// simsc_desktop entry point.
//
// The org+app names below are picked up by QSettings so all our
// getters land in the same platform-native config file. Keep them
// stable -- changing either would move every user's persisted
// settings out from under them.

#include "ui/main_window.h"

#include "../bwgame.h"          // for bwgame::a_string used by ui:: log/fatal
#include "ui.h"                 // pulls in the ui::log/fatal declarations

#include <QtCore/QCoreApplication>
#include <QtWidgets/QApplication>

#include <cstdio>
#include <cstdlib>

// Header-only ui.h declares these two hook symbols and expects the
// executable to define them. simsc_app's main.cpp does the same;
// duplicated verbatim rather than shared so the ObserverWindow's
// log output stays independent of any future simsc_app choices.
namespace bwgame {
namespace ui {
void log_str(bwgame::a_string str) {
	std::fwrite(str.data(), str.size(), 1, stdout);
	std::fflush(stdout);
}
void fatal_error_str(bwgame::a_string str) {
	log("fatal error: %s\n", str);
	std::fflush(stdout);
	std::abort();
}
}   // namespace ui
}   // namespace bwgame

int main(int argc, char** argv) {
	QCoreApplication::setOrganizationName(QStringLiteral("openbw"));
	QCoreApplication::setOrganizationDomain(QStringLiteral("openbw.org"));
	QCoreApplication::setApplicationName(QStringLiteral("simsc_desktop"));

	QApplication app(argc, argv);

	simsc_desktop::MainWindow w;
	w.show();

	return app.exec();
}
