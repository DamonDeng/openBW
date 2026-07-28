// Value types mirroring simsc's REST responses.
//
// Field names + shapes match simsc/app/routes/games.py exactly.
// If the server-side Pydantic changes, this header must change
// with it; do NOT let the two drift silently.

#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>

namespace simsc_desktop {

// GET /api/users -- UserRoster in games.py:263
struct RemoteUser {
	QString alias;
	QString display_name;   // optional; may be empty
};

// InvitationOut in games.py:47
struct RemoteInvitation {
	QString   alias;
	QString   status;         // "pending" | "accepted" | "declined" | "expired"
	QDateTime invited_at;
	QDateTime responded_at;   // null-ish if not set
};

// GameOut in games.py:54
struct RemoteGame {
	QString game_id;
	QString owner_alias;
	QString map;
	QStringList races;              // parallel to player_aliases
	QStringList player_aliases;     // "" for empty slot (JSON null)
	QString game_speed;             // named speed, e.g. "fastest"
	QString state;                  // "pending_invitations" | "starting" |
	                                //   "running" | "ended" | "cancelled" | ...
	QVector<RemoteInvitation> invitations;
	QString pod_phase;              // "" if not running or not fetched
	QDateTime created_at;
	QDateTime started_at;
	QDateTime ended_at;
	QString agent_url;              // wss://.../game/<id>/agent
	QString observer_url;           // wss://.../game/<id>/observer
	QString my_invitation_status;   // "" if none applies
};

// POST /api/games body -- CreateGameIn in games.py:117
struct CreateRemoteGameReq {
	QString map;
	QStringList races;
	QStringList player_aliases;     // "" -> null on the wire (empty slot)
	QString game_speed = QStringLiteral("fastest");
};

}   // namespace simsc_desktop
