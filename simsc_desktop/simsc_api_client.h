// Thin QNetworkAccessManager wrapper for the simsc REST API.
//
// Every call attaches `X-API-Key: <settings.simsc_api_key>` from
// Settings. Responses are surfaced as signals -- no futures /
// promises / callbacks-with-context to keep the API surface Qt-
// idiomatic and avoid manual lifetime bookkeeping.
//
// The client is intentionally dumb: no retry, no exponential
// backoff, no caching. The tab polls list_games on a 10s QTimer
// and re-issues on user actions. Anything more sophisticated is
// Phase 1d material.

#pragma once

#include "simsc_types.h"

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtNetwork/QNetworkRequest>   // used by inline attachAuth() typing

class QNetworkAccessManager;
class QNetworkReply;

namespace simsc_desktop {

class Settings;

class SimscApiClient : public QObject {
	Q_OBJECT
public:
	explicit SimscApiClient(Settings* settings, QObject* parent = nullptr);

	// Read.  Emit `usersReceived` on success or `errorOccurred(op, ...)`
	// on failure. Same pattern for the rest.
	void listUsers();
	void listGames();
	void getGame(const QString& game_id);

	// Write.
	void createGame(const CreateRemoteGameReq& req);
	void acceptGame(const QString& game_id);
	void declineGame(const QString& game_id);
	void cancelGame(const QString& game_id);
	void deleteGame(const QString& game_id);

signals:
	void usersReceived(const QVector<RemoteUser>& users);
	void gamesReceived(const QVector<RemoteGame>& games);
	void gameReceived(const RemoteGame& game);
	void gameCreated(const RemoteGame& game);
	void gameAccepted(const RemoteGame& game);
	void gameDeclined(const QString& game_id);
	void gameCancelled(const QString& game_id);
	void gameDeleted(const QString& game_id);

	// op is one of "list_users", "list_games", "get_game",
	// "create_game", "accept_game", "decline_game", "cancel_game",
	// "delete_game" -- keep it stable, the tab uses it to render
	// contextual error text.
	void errorOccurred(const QString& op,
	                   int http_status,
	                   const QString& message);

private:
	QNetworkReply* get(const QString& path);
	QNetworkReply* post(const QString& path, const QByteArray& body);
	QNetworkReply* del(const QString& path);
	void attachAuth(QNetworkRequest& req) const;

	Settings*                settings_ = nullptr;
	QNetworkAccessManager*   nam_      = nullptr;
};

}   // namespace simsc_desktop
