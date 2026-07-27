// See app_paths.h. Path resolution order for each field:
//
//   server_binary_:
//     1. $SIMSC_DESKTOP_SERVER_BIN                      (dev override)
//     2. <exeDir>/openbw_server                         (bundled or dev-adjacent)
//     3. <exeDir>/../server/openbw_server               (in-tree build_qt layout)
//     4. empty                                          (caller must surface error)
//
//   bundled_mpq_dir_ / bundled_maps_dir_ / maps_json_path_:
//     1. $SIMSC_DESKTOP_MPQ_DIR / $SIMSC_DESKTOP_MAPS_DIR
//     2. macOS bundle Contents/Resources/{mpqs,maps,maps.json}
//     3. <exeDir>/../Resources/{mpqs,maps,maps.json}    (flat-layout install)
//     4. empty                                          (Settings must supply)

#include "app_paths.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QProcessEnvironment>

namespace simsc_desktop {

namespace {

QString first_existing(std::initializer_list<QString> candidates) {
	for (const auto& c : candidates) {
		if (c.isEmpty()) continue;
		if (QFileInfo::exists(c)) return c;
	}
	return {};
}

}   // namespace

AppPaths::AppPaths() {
	const auto env = QProcessEnvironment::systemEnvironment();
	const QString exe_dir = QCoreApplication::applicationDirPath();

	// The macOS bundle layout: applicationDirPath() = .app/Contents/MacOS.
	// Contents/Resources is one directory up + "/Resources".
	const QString bundle_resources = QDir(exe_dir + "/../Resources")
		.absolutePath();

#ifdef Q_OS_MACOS
	constexpr bool is_bundle_layout = true;
#else
	constexpr bool is_bundle_layout = false;
#endif

	server_binary_ = first_existing({
		env.value("SIMSC_DESKTOP_SERVER_BIN"),
		exe_dir + "/openbw_server",
		exe_dir + "/../server/openbw_server",
	});

	bundled_mpq_dir_ = first_existing({
		env.value("SIMSC_DESKTOP_MPQ_DIR"),
		is_bundle_layout ? bundle_resources + "/mpqs" : QString(),
		exe_dir + "/../Resources/mpqs",
	});

	bundled_maps_dir_ = first_existing({
		env.value("SIMSC_DESKTOP_MAPS_DIR"),
		is_bundle_layout ? bundle_resources + "/maps" : QString(),
		exe_dir + "/../Resources/maps",
	});

	maps_json_path_ = first_existing({
		env.value("SIMSC_DESKTOP_MAPS_JSON"),
		is_bundle_layout ? bundle_resources + "/maps.json" : QString(),
		exe_dir + "/../Resources/maps.json",
	});
}

}   // namespace simsc_desktop
