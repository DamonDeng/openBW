// sprite_viewer entry point.
//
// A minimal Qt6 desktop app that shows one openBW unit in isolation
// and drives its iscript.bin animations. Reuses:
//   * bwgame.h / actions.h / iscript_run_anim -- authoritative
//     animation state machine
//   * ui/ui.h::draw_sprite -- rendering
//   * simsc_app/qt_native_window.cpp -- Qt drawing backend
//     (referenced directly from CMakeLists as a source file so we
//     don't duplicate the ~750 LOC of window plumbing).

#include "viewer_window.h"

#include <QtCore/QCommandLineOption>
#include <QtCore/QCommandLineParser>
#include <QtWidgets/QApplication>

#include <cstdio>
#include <cstdlib>
#include <string>

// The ui:: log-sink hooks that bwgame + ui.h reference.
// Same shape as simsc_app/main.cpp lines 43-55 and observer.cpp.
#include "common.h"
namespace bwgame {
namespace ui {
void log_str(a_string str) {
	std::fwrite(str.data(), str.size(), 1, stdout);
	std::fflush(stdout);
}
void fatal_error_str(a_string str) {
	log("fatal error: %s\n", str);
	std::fflush(stdout);
	std::abort();
}
}
}

int main(int argc, char** argv) {
	QApplication app(argc, argv);
	QCoreApplication::setApplicationName("sprite_viewer");

	QCommandLineParser parser;
	parser.setApplicationDescription(
		"openBW sprite viewer -- browse unit animations via real iscript.bin");
	parser.addHelpOption();
	QCommandLineOption data_opt(
		{"d", "data-path"},
		"Directory containing StarDat.mpq / BrooDat.mpq / Patch_rt.mpq "
		"and the .scm map file. Default: 'original_resources'.",
		"dir", "original_resources");
	QCommandLineOption map_opt(
		{"m", "map"},
		"Path (relative to --data-path) to a .scm/.scx map used as the "
		"backing stage. Default: '(4)Blood Bath.scm'.",
		"file", "(4)Blood Bath.scm");
	// HD mode. --sc-version=remastered switches the render path from
	// the classic GRP/palette blitter to a CascLib-backed HD loader
	// that reads a user's SC:R install directly. Assets are NEVER
	// bundled with this binary; the user brings their own SC:R.
	// The path should point at the folder containing Data/, Maps/,
	// x86_64/... (same shape as ../casc_space/StarCraft).
	QCommandLineOption sc_version_opt(
		"sc-version",
		"'classic' (default) uses the GRP+palette classic renderer. "
		"'remastered' reads HD sprites from --sc-remastered-path.",
		"mode", "classic");
	QCommandLineOption sc_path_opt(
		"sc-remastered-path",
		"Path to a StarCraft: Remastered install root (the folder "
		"containing Data/, Maps/, x86_64/...). Required when "
		"--sc-version=remastered.",
		"path", "");
	parser.addOption(data_opt);
	parser.addOption(map_opt);
	parser.addOption(sc_version_opt);
	parser.addOption(sc_path_opt);
	parser.process(app);

	std::string data_path = parser.value(data_opt).toStdString();
	std::string map_relpath = parser.value(map_opt).toStdString();
	QString sc_version = parser.value(sc_version_opt);
	QString sc_remastered_path = parser.value(sc_path_opt);

	if (sc_version == "remastered" && sc_remastered_path.isEmpty()) {
		std::fprintf(stderr,
			"--sc-version=remastered requires --sc-remastered-path\n");
		return 1;
	}

	sprite_viewer::ViewerWindow window(
		data_path, map_relpath, sc_version, sc_remastered_path);
	window.show();
	return app.exec();
}
