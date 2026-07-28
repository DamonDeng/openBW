// Value struct for one running (or previously-run) local game.
//
// Kept out of local_server_manager.h so UI code can #include just
// this header for read-only display purposes without pulling in
// QProcess. LocalServerManager owns the actual QProcess and hands
// out const LocalGame& via its model API.

#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QString>
#include <QtCore/QVector>

namespace simsc_desktop {

// One row per slot in the New Local Game dialog.
struct LocalPlayer {
	QString alias;   // "" = empty slot / spectator; must be non-empty in Phase 1b
	int     slot = 0;
	QString race;    // "zerg" | "terran" | "protoss" | "random"
};

struct LocalGame {
	QString game_id;                // UUID; goes into agent URL as /game/<id>
	quint16 port = 0;               // agent WS port (--ws-port). Observer =
	                                // port + 1 (--obs-port).
	QString map_relpath;            // relative to bundled maps dir
	QString map_abspath;            // absolute path handed to openbw_server
	QVector<LocalPlayer> players;
	QDateTime started_at;

	enum class Status {
		Starting,   // process launched, no ready signal yet
		Running,    // process still alive
		Exited,     // process left with a zero exit code
		Failed,     // process didn't start or exited non-zero
	} status = Status::Starting;

	// stderr tail from openbw_server. Only meaningful when status
	// is Failed or Exited -- otherwise it's whatever the last N
	// KB of stderr happened to contain.
	QString stderr_tail;
};

}   // namespace simsc_desktop
