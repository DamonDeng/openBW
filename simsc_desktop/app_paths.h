// Resolves the paths to bundled runtime resources.
//
// Two build modes to cover:
//
//   * Bundled  -- macOS .app produced by our CMake install step.
//     Layout:
//         simsc_desktop.app/Contents/MacOS/simsc_desktop      (this binary)
//         simsc_desktop.app/Contents/MacOS/openbw_server      (child)
//         simsc_desktop.app/Contents/Resources/mpqs/*.mpq
//         simsc_desktop.app/Contents/Resources/maps.json
//         simsc_desktop.app/Contents/Resources/maps/*.scm
//
//   * Dev      -- running the bare binary out of build_qt/simsc_desktop/.
//     The bundled paths above do not exist. We fall back to
//     next-to-binary probing, and finally to environment variables:
//         SIMSC_DESKTOP_SERVER_BIN
//         SIMSC_DESKTOP_MPQ_DIR
//         SIMSC_DESKTOP_MAPS_DIR
//     for developers who want to point at a custom build tree.
//
// Callers should treat AppPaths as read-only + immutable for the
// lifetime of the app. It's not a QObject; construct once in main().

#pragma once

#include <QtCore/QString>

namespace simsc_desktop {

class AppPaths {
public:
	AppPaths();

	// Absolute path to the openbw_server executable this app owns.
	// Empty QString when we couldn't find one -- callers must check
	// before spawning.
	QString server_binary() const { return server_binary_; }

	// Directory that holds StarDat.mpq / BrooDat.mpq / Patch_rt.mpq
	// bundled next to the app. Empty when no bundled MPQs are
	// present (the user must set sc1_data_path themselves in
	// Settings). This is a *default*, not a hard requirement -- if
	// the user has SC1 installed somewhere else, Settings wins.
	QString bundled_mpq_dir() const { return bundled_mpq_dir_; }

	// Directory holding bundled .scm maps. Feeds the initial map
	// picker's "known maps" list. Empty if not bundled.
	QString bundled_maps_dir() const { return bundled_maps_dir_; }

	// Path to maps.json (mirror of simsc/app/static/maps.json).
	// Used to populate the map dropdown's display names + player
	// counts. Empty when not bundled -- the app falls back to
	// scanning bundled_maps_dir_ for filenames.
	QString maps_json_path() const { return maps_json_path_; }

private:
	QString server_binary_;
	QString bundled_mpq_dir_;
	QString bundled_maps_dir_;
	QString maps_json_path_;
};

}   // namespace simsc_desktop
