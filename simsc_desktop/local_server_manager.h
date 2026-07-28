// Owns every running openbw_server child process and exposes them
// as a QAbstractListModel for the Local Games tab to bind against.
//
// One LocalGame == one QProcess. Process ownership follows the
// LocalGame's status:
//   Starting/Running:  QProcess is alive, will be terminated on
//                      stopGame() or on the manager's destruction.
//   Exited/Failed:     QProcess has been reaped; row survives until
//                      the user removes it (so they can inspect the
//                      stderr tail).
//
// URL policy for local games:
//   agent:     ws://localhost:<port>/agent?key=<key>
//   observer:  ws://localhost:<port+1>/observer?key=<key>
//
// Under a REMOTE (EKS/ALB) game the URL is
//   wss://simsc.agentnumber47.com/game/<id>/{agent,observer}?key=<key>
// because the load balancer path-routes to a per-game pod. We
// deliberately do NOT emulate that path shape locally: locally
// the game_id maps to a port, not a path, so pretending
// otherwise would mislead any developer inspecting traffic. The
// agent code stays URL-agnostic; only the URL string differs.
//
// Port allocation: `preferred_port` is the FIRST port we try
// (default 6040 from Settings). If either <p> or <p+1> is in
// use, we step by 2 (6040, 6042, 6044, ...) up to 10 pairs
// ahead. Predictable port numbers matter for pasting agent URLs
// during dev.

#pragma once

#include "local_game.h"

#include <QtCore/QAbstractListModel>
#include <QtCore/QMap>
#include <QtCore/QObject>
#include <QtCore/QVector>

class QProcess;

namespace simsc_desktop {

class Settings;
class LocalUserRoster;
class AppPaths;

// Parameters passed into LocalServerManager::startGame. Bundled
// as a POD so the New Local Game dialog can construct+validate
// one object and hand it over without a long parameter list.
struct NewLocalGameParams {
	QString map_relpath;
	QString map_abspath;
	QVector<LocalPlayer> players;
	quint16 preferred_port = 6040;   // may be bumped if in use
};

class LocalServerManager : public QAbstractListModel {
	Q_OBJECT
public:
	enum Column {
		GameIdCol   = 0,
		MapCol      = 1,
		PlayersCol  = 2,
		PortCol     = 3,
		StatusCol   = 4,
		ColumnCount
	};

	LocalServerManager(const AppPaths* paths,
	                   Settings* settings,
	                   LocalUserRoster* roster,
	                   QObject* parent = nullptr);
	~LocalServerManager() override;

	// Launch a new server. On success returns the game_id; on
	// failure returns an empty string and emits errorOccurred().
	// "Success" here means QProcess::start() succeeded -- the game
	// might still fail seconds later; watch for gameStatusChanged.
	QString startGame(const NewLocalGameParams& p);

	// Terminate a running server (SIGTERM, then SIGKILL after
	// 3s). Idempotent.
	void stopGame(const QString& game_id);

	// Terminate every running server. Called from MainWindow's
	// aboutToQuit path.
	void stopAll();

	// Read access for the tab's toolbar handlers.
	const LocalGame* gameById(const QString& game_id) const;
	const LocalGame* gameAtRow(int row) const;

	// URL helpers -- centralized so the tab can't accidentally
	// build a slightly different URL from the observer window.
	QString agentUrl(const QString& game_id) const;
	QString observerUrl(const QString& game_id) const;
	// The current user's API key for observing a particular local
	// game. Local server auth is by hash of any known api_key, so
	// we can pick any local user's plaintext key -- we pick the
	// alias assigned to slot 0 by default; if the caller wants a
	// specific player perspective they can pass that alias.
	QString apiKeyForObserver(const QString& game_id,
	                          const QString& alias = {}) const;

	// QAbstractListModel is column-less by default; we lie about
	// column count so the tab can render as a QTreeView / table.
	int rowCount(const QModelIndex& parent = {}) const override;
	int columnCount(const QModelIndex& parent = {}) const override;
	QVariant data(const QModelIndex& idx, int role) const override;
	QVariant headerData(int section, Qt::Orientation, int) const override;

signals:
	void gameStatusChanged(const QString& game_id);
	void errorOccurred(const QString& game_id, const QString& message);

private:
	struct Entry {
		LocalGame game;
		QProcess* proc = nullptr;
	};

	int rowOfGame(const QString& game_id) const;
	void appendStderr(const QString& game_id, const QByteArray& chunk);
	void onProcessFinished(const QString& game_id, int code, int exitStatus);
	void onProcessErrorOccurred(const QString& game_id, int qprocess_error);
	void markStatus(const QString& game_id, LocalGame::Status s);
	quint16 pickFreePort(quint16 preferred) const;

	const AppPaths*  paths_    = nullptr;
	Settings*        settings_ = nullptr;
	LocalUserRoster* roster_   = nullptr;

	QVector<Entry> entries_;
};

}   // namespace simsc_desktop
