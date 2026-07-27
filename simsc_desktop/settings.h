// Thin wrapper over QSettings for simsc_desktop.
//
// Keys are namespaced under [openbw / simsc_desktop] via the
// QCoreApplication org/app names set in main.cpp. On macOS this
// lands in ~/Library/Preferences/com.openbw.simsc_desktop.plist;
// on Linux under ~/.config/openbw/simsc_desktop.conf. First-run
// (nothing persisted yet) is detected via has_seen_first_run().
//
// This class is deliberately dumb: getters + setters + a couple
// of "resolved default" helpers. All UI-facing logic lives in the
// Settings tab widget. The runtime uses Settings by value; there
// is no singleton because QSettings itself already backs to the
// same file for every instance.

#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>

namespace simsc_desktop {

class Settings : public QObject {
	Q_OBJECT
public:
	explicit Settings(QObject* parent = nullptr);

	// Path to a directory containing StarDat.mpq / BrooDat.mpq /
	// Patch_rt.mpq. On first run this is unset; the app forces the
	// Settings tab open until the user picks a real path (or accepts
	// the bundled Resources/mpqs default when present).
	QString sc1_data_path() const;
	void    set_sc1_data_path(const QString&);

	// API key for the remote simsc backend. Empty until the user
	// enters one; Remote Games tab is disabled while empty.
	QString simsc_api_key() const;
	void    set_simsc_api_key(const QString&);

	// Base URL for the remote simsc backend. Default:
	// https://simsc.agentnumber47.com
	QString simsc_base_url() const;
	void    set_simsc_base_url(const QString&);

	// Starting port for the local openbw_server. If in use we
	// probe upward for a free one; this is only the FIRST-choice
	// port. Default 6040.
	int  default_local_port() const;
	void set_default_local_port(int);

	// Optional dev-only override: path to a custom openbw_server
	// binary. Empty = use the bundled one via AppPaths. Present
	// when a developer wants to test their own build without
	// re-bundling.
	QString server_binary_override() const;
	void    set_server_binary_override(const QString&);

	// True after the first successful save of any setting.
	// Distinguishes "we've never launched before" from "user
	// deliberately cleared their SC1 path".
	bool has_seen_first_run() const;
	void mark_first_run_seen();

signals:
	// Emitted whenever any setting changes. Kept coarse-grained
	// because the tab-side listeners refresh the whole form on
	// any change; no need for per-field signals.
	void changed();
};

}   // namespace simsc_desktop
