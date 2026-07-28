// See simsc_api_client.h.
//
// Parsing philosophy: on unexpected shape (missing field, wrong
// type) we accept what we can and default the rest. The remote
// simsc is authoritative; if it evolves faster than we do, we'd
// rather show a game row with a blank state than throw an error
// and hide the row entirely.

#include "simsc_api_client.h"

#include "settings.h"

#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace simsc_desktop {

namespace {

// Parse an ISO-8601 timestamp. simsc emits Python-style
// "2026-07-28T14:22:00.123456+00:00". QDateTime::fromString with
// Qt::ISODateWithMs handles fractional seconds up to ms; anything
// finer is truncated silently, which is fine for UI display.
QDateTime parseIso(const QJsonValue& v) {
	if (!v.isString()) return {};
	auto s = v.toString();
	if (s.isEmpty()) return {};
	auto d = QDateTime::fromString(s, Qt::ISODateWithMs);
	if (!d.isValid()) d = QDateTime::fromString(s, Qt::ISODate);
	return d;
}

QString jsonStr(const QJsonValue& v) {
	return v.isString() ? v.toString() : QString();
}

RemoteUser parseUser(const QJsonObject& o) {
	RemoteUser u;
	u.alias        = jsonStr(o.value("alias"));
	u.display_name = jsonStr(o.value("display_name"));
	return u;
}

RemoteInvitation parseInvitation(const QJsonObject& o) {
	RemoteInvitation i;
	i.alias         = jsonStr(o.value("alias"));
	i.status        = jsonStr(o.value("status"));
	i.invited_at    = parseIso(o.value("invited_at"));
	i.responded_at  = parseIso(o.value("responded_at"));
	return i;
}

RemoteGame parseGame(const QJsonObject& o) {
	RemoteGame g;
	g.game_id     = jsonStr(o.value("game_id"));
	g.owner_alias = jsonStr(o.value("owner_alias"));
	g.map         = jsonStr(o.value("map"));
	for (const auto& v : o.value("races").toArray()) {
		g.races << jsonStr(v);
	}
	for (const auto& v : o.value("player_aliases").toArray()) {
		// simsc encodes empty slots as JSON null; store as "" so
		// downstream Qt code doesn't have to special-case null.
		g.player_aliases << (v.isNull() ? QString() : jsonStr(v));
	}
	g.game_speed = jsonStr(o.value("game_speed"));
	g.state      = jsonStr(o.value("state"));
	for (const auto& v : o.value("invitations").toArray()) {
		g.invitations.push_back(parseInvitation(v.toObject()));
	}
	g.pod_phase   = jsonStr(o.value("pod_phase"));
	g.created_at  = parseIso(o.value("created_at"));
	g.started_at  = parseIso(o.value("started_at"));
	g.ended_at    = parseIso(o.value("ended_at"));
	g.agent_url   = jsonStr(o.value("agent_url"));
	g.observer_url = jsonStr(o.value("observer_url"));
	g.my_invitation_status = jsonStr(o.value("my_invitation_status"));
	return g;
}

// Read body, return parsed JSON. Reply is deleteLater()'d by the
// caller after this returns; do NOT retain it.
QJsonDocument readJson(QNetworkReply* rep) {
	const auto bytes = rep->readAll();
	return QJsonDocument::fromJson(bytes);
}

// Format a network error into human-readable text for the tab.
QString errorMessage(QNetworkReply* rep, int http_status) {
	// Server may return {"detail": "..."} on 4xx/5xx; surface that
	// if present, else fall back to Qt's raw reason.
	const auto doc = QJsonDocument::fromJson(rep->readAll());
	if (doc.isObject()) {
		const auto detail = doc.object().value("detail");
		if (detail.isString()) return detail.toString();
	}
	if (http_status > 0) return QStringLiteral("HTTP %1").arg(http_status);
	return rep->errorString();
}

// Read HTTP status even on network-level errors (Qt sometimes has
// both). Returns 0 if unavailable.
int httpStatus(QNetworkReply* rep) {
	const auto v = rep->attribute(QNetworkRequest::HttpStatusCodeAttribute);
	return v.isValid() ? v.toInt() : 0;
}

}   // namespace

SimscApiClient::SimscApiClient(Settings* settings, QObject* parent)
	: QObject(parent), settings_(settings),
	  nam_(new QNetworkAccessManager(this)) {}

void SimscApiClient::attachAuth(QNetworkRequest& req) const {
	// simsc reads api key from the X-API-Key header. See
	// simsc/app/auth/deps.py::require_user.
	const auto key = settings_->simsc_api_key();
	if (!key.isEmpty()) {
		req.setRawHeader("X-API-Key", key.toUtf8());
	}
}

QNetworkReply* SimscApiClient::get(const QString& path) {
	QNetworkRequest req(QUrl(settings_->simsc_base_url() + path));
	attachAuth(req);
	return nam_->get(req);
}

QNetworkReply* SimscApiClient::post(const QString& path,
                                    const QByteArray& body) {
	QNetworkRequest req(QUrl(settings_->simsc_base_url() + path));
	attachAuth(req);
	req.setHeader(QNetworkRequest::ContentTypeHeader,
	              QStringLiteral("application/json"));
	return nam_->post(req, body);
}

QNetworkReply* SimscApiClient::del(const QString& path) {
	QNetworkRequest req(QUrl(settings_->simsc_base_url() + path));
	attachAuth(req);
	return nam_->deleteResource(req);
}

// ------------------------------------------------------------------
// GET /api/users
// ------------------------------------------------------------------
void SimscApiClient::listUsers() {
	auto* rep = get(QStringLiteral("/api/users"));
	connect(rep, &QNetworkReply::finished, this, [this, rep] {
		rep->deleteLater();
		const int status = httpStatus(rep);
		if (rep->error() != QNetworkReply::NoError || status >= 400) {
			emit errorOccurred(QStringLiteral("list_users"),
				status, errorMessage(rep, status));
			return;
		}
		QVector<RemoteUser> users;
		const auto arr = QJsonDocument::fromJson(rep->readAll()).array();
		for (const auto& v : arr) users.push_back(parseUser(v.toObject()));
		emit usersReceived(users);
	});
}

// ------------------------------------------------------------------
// GET /api/games
// ------------------------------------------------------------------
void SimscApiClient::listGames() {
	auto* rep = get(QStringLiteral("/api/games"));
	connect(rep, &QNetworkReply::finished, this, [this, rep] {
		rep->deleteLater();
		const int status = httpStatus(rep);
		if (rep->error() != QNetworkReply::NoError || status >= 400) {
			emit errorOccurred(QStringLiteral("list_games"),
				status, errorMessage(rep, status));
			return;
		}
		QVector<RemoteGame> games;
		const auto arr = QJsonDocument::fromJson(rep->readAll()).array();
		for (const auto& v : arr) games.push_back(parseGame(v.toObject()));
		emit gamesReceived(games);
	});
}

// ------------------------------------------------------------------
// GET /api/games/{id}
// ------------------------------------------------------------------
void SimscApiClient::getGame(const QString& game_id) {
	auto* rep = get(QStringLiteral("/api/games/%1").arg(game_id));
	connect(rep, &QNetworkReply::finished, this, [this, rep] {
		rep->deleteLater();
		const int status = httpStatus(rep);
		if (rep->error() != QNetworkReply::NoError || status >= 400) {
			emit errorOccurred(QStringLiteral("get_game"),
				status, errorMessage(rep, status));
			return;
		}
		emit gameReceived(parseGame(
			QJsonDocument::fromJson(rep->readAll()).object()));
	});
}

// ------------------------------------------------------------------
// POST /api/games
// ------------------------------------------------------------------
void SimscApiClient::createGame(const CreateRemoteGameReq& r) {
	QJsonArray races, aliases;
	for (const auto& s : r.races)   races.push_back(s);
	// Empty string in our type -> JSON null on the wire (empty slot).
	for (const auto& a : r.player_aliases) {
		aliases.push_back(a.isEmpty() ? QJsonValue(QJsonValue::Null)
		                              : QJsonValue(a));
	}
	QJsonObject body{
		{"map",             r.map},
		{"races",           races},
		{"player_aliases",  aliases},
		{"game_speed",      r.game_speed},
	};
	auto* rep = post(QStringLiteral("/api/games"),
	                 QJsonDocument(body).toJson(QJsonDocument::Compact));
	connect(rep, &QNetworkReply::finished, this, [this, rep] {
		rep->deleteLater();
		const int status = httpStatus(rep);
		if (rep->error() != QNetworkReply::NoError || status >= 400) {
			emit errorOccurred(QStringLiteral("create_game"),
				status, errorMessage(rep, status));
			return;
		}
		emit gameCreated(parseGame(
			QJsonDocument::fromJson(rep->readAll()).object()));
	});
}

// ------------------------------------------------------------------
// POST /api/games/{id}/{accept|decline|cancel}
// ------------------------------------------------------------------
void SimscApiClient::acceptGame(const QString& game_id) {
	auto* rep = post(QStringLiteral("/api/games/%1/accept").arg(game_id),
	                 QByteArray());
	connect(rep, &QNetworkReply::finished, this, [this, rep] {
		rep->deleteLater();
		const int status = httpStatus(rep);
		if (rep->error() != QNetworkReply::NoError || status >= 400) {
			emit errorOccurred(QStringLiteral("accept_game"),
				status, errorMessage(rep, status));
			return;
		}
		emit gameAccepted(parseGame(
			QJsonDocument::fromJson(rep->readAll()).object()));
	});
}

void SimscApiClient::declineGame(const QString& game_id) {
	auto* rep = post(QStringLiteral("/api/games/%1/decline").arg(game_id),
	                 QByteArray());
	connect(rep, &QNetworkReply::finished, this, [this, rep, game_id] {
		rep->deleteLater();
		const int status = httpStatus(rep);
		if (rep->error() != QNetworkReply::NoError || status >= 400) {
			emit errorOccurred(QStringLiteral("decline_game"),
				status, errorMessage(rep, status));
			return;
		}
		emit gameDeclined(game_id);
	});
}

void SimscApiClient::cancelGame(const QString& game_id) {
	auto* rep = post(QStringLiteral("/api/games/%1/cancel").arg(game_id),
	                 QByteArray());
	connect(rep, &QNetworkReply::finished, this, [this, rep, game_id] {
		rep->deleteLater();
		const int status = httpStatus(rep);
		if (rep->error() != QNetworkReply::NoError || status >= 400) {
			emit errorOccurred(QStringLiteral("cancel_game"),
				status, errorMessage(rep, status));
			return;
		}
		emit gameCancelled(game_id);
	});
}

// ------------------------------------------------------------------
// DELETE /api/games/{id}
// ------------------------------------------------------------------
void SimscApiClient::deleteGame(const QString& game_id) {
	auto* rep = del(QStringLiteral("/api/games/%1").arg(game_id));
	connect(rep, &QNetworkReply::finished, this, [this, rep, game_id] {
		rep->deleteLater();
		const int status = httpStatus(rep);
		if (rep->error() != QNetworkReply::NoError || status >= 400) {
			emit errorOccurred(QStringLiteral("delete_game"),
				status, errorMessage(rep, status));
			return;
		}
		emit gameDeleted(game_id);
	});
}

}   // namespace simsc_desktop
