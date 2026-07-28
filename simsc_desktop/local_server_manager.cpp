// See local_server_manager.h.

#include "local_server_manager.h"

#include "app_paths.h"
#include "local_user_roster.h"
#include "settings.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QUuid>
#include <QtNetwork/QTcpServer>

namespace simsc_desktop {

namespace {

// Cap stderr tail so a chatty server can't blow the model up.
constexpr int kStderrTailLimit = 64 * 1024;

QString sha256Hex(const QString& s) {
	QCryptographicHash h(QCryptographicHash::Sha256);
	h.addData(s.toUtf8());
	return QString::fromLatin1(h.result().toHex());
}

QString statusName(LocalGame::Status s) {
	switch (s) {
		case LocalGame::Status::Starting: return QStringLiteral("Starting");
		case LocalGame::Status::Running:  return QStringLiteral("Running");
		case LocalGame::Status::Exited:   return QStringLiteral("Exited");
		case LocalGame::Status::Failed:   return QStringLiteral("Failed");
	}
	return {};
}

}   // namespace

LocalServerManager::LocalServerManager(const AppPaths* paths,
                                       Settings* settings,
                                       LocalUserRoster* roster,
                                       QObject* parent)
	: QAbstractListModel(parent)
	, paths_(paths)
	, settings_(settings)
	, roster_(roster) {}

LocalServerManager::~LocalServerManager() {
	stopAll();
}

int LocalServerManager::rowOfGame(const QString& game_id) const {
	for (int i = 0; i < entries_.size(); ++i) {
		if (entries_[i].game.game_id == game_id) return i;
	}
	return -1;
}

const LocalGame* LocalServerManager::gameById(const QString& game_id) const {
	const int r = rowOfGame(game_id);
	return r < 0 ? nullptr : &entries_[r].game;
}

const LocalGame* LocalServerManager::gameAtRow(int row) const {
	if (row < 0 || row >= entries_.size()) return nullptr;
	return &entries_[row].game;
}

quint16 LocalServerManager::pickFreePort(quint16 preferred) const {
	// Step by 2 so each game gets a clean (even, odd) pair -- the
	// server takes `p` for agents and `p + 1` for observers, so
	// after game N we've used p..p+1, p+2..p+3, etc. Probing by 1
	// would waste every other iteration because p+1 is always in
	// use by the previous game.
	//
	// Bind address: MUST match what openbw_server binds (0.0.0.0),
	// not 127.0.0.1. Under macOS + SO_REUSEADDR (which the server
	// sets), a fresh socket can bind a more specific address like
	// 127.0.0.1:P even when 0.0.0.0:P is already owned by another
	// process -- so probing on LocalHost would falsely report the
	// port as free and then the server would fail to bind.
	//
	// TOCTOU note: QTcpServer::listen briefly binds and immediately
	// releases each candidate; another process could grab the port
	// between here and the server's own bind. Not a real hazard on
	// a personal machine; if we hit it the server surfaces the bind
	// failure via its own error path.
	for (quint16 p = preferred; p < preferred + 20 && p >= preferred; p += 2) {
		QTcpServer a, b;
		if (a.listen(QHostAddress::AnyIPv4, p)
		    && b.listen(QHostAddress::AnyIPv4, quint16(p + 1))) {
			return p;
		}
	}
	// Nothing free in the 10-pair window; return preferred and let
	// openbw_server's bind() surface the collision to the UI.
	return preferred;
}

QString LocalServerManager::startGame(const NewLocalGameParams& p) {
	// ------------------------------------------------------------
	// Preconditions: server binary must exist and every named
	// player must resolve to a plaintext api_key in the roster.
	// ------------------------------------------------------------
	auto server_bin = settings_->server_binary_override();
	if (server_bin.isEmpty()) server_bin = paths_->server_binary();
	if (server_bin.isEmpty() || !QFileInfo::exists(server_bin)) {
		emit errorOccurred({},
			tr("openbw_server binary not found. Set an override in Settings."));
		return {};
	}

	auto data_dir = settings_->sc1_data_path();
	if (data_dir.isEmpty() || !QFileInfo::exists(data_dir)) {
		emit errorOccurred({},
			tr("SC1 data path is not set. Set it in Settings."));
		return {};
	}

	for (const auto& lp : p.players) {
		if (lp.alias.isEmpty()) {
			emit errorOccurred({}, tr("A slot has no alias assigned."));
			return {};
		}
		if (roster_->apiKeyFor(lp.alias).isEmpty()) {
			emit errorOccurred({},
				tr("Local user '%1' has no API key.").arg(lp.alias));
			return {};
		}
	}

	// ------------------------------------------------------------
	// Compose the row + argv.
	// ------------------------------------------------------------
	Entry e;
	e.game.game_id     = QUuid::createUuid()
		.toString(QUuid::WithoutBraces).left(8);
	e.game.port        = pickFreePort(p.preferred_port);
	e.game.map_relpath = p.map_relpath;
	e.game.map_abspath = p.map_abspath;
	e.game.players     = p.players;
	e.game.started_at  = QDateTime::currentDateTime();
	e.game.status      = LocalGame::Status::Starting;

	QStringList argv;
	argv << "--map"        << e.game.map_abspath;
	argv << "--data-path"  << data_dir;
	argv << "--ws-port"    << QString::number(e.game.port);
	argv << "--obs-port"   << QString::number(e.game.port + 1);
	// No --any-ws-path: the local observer connects to /observer
	// (server's default) directly. The agent WS server already
	// accepts any path -- see server/ws_server.h:extract_key.
	argv << "--game-speed"
	     << QString::number(settings_->default_game_speed_ms());

	for (const auto& lp : p.players) {
		const auto key    = roster_->apiKeyFor(lp.alias);
		const auto hashed = sha256Hex(key);
		argv << "--user-hash"
		     << QStringLiteral("%1:%2:player:%3")
		            .arg(lp.alias).arg(hashed).arg(lp.slot);
		if (lp.race == "zerg" || lp.race == "terran"
		    || lp.race == "protoss") {
			argv << "--race"
			     << QStringLiteral("%1=%2").arg(lp.slot).arg(lp.race);
		}
	}

	// ------------------------------------------------------------
	// Wire QProcess. lambda captures the game_id (not a raw
	// pointer) so removals/reordering don't invalidate the target.
	// ------------------------------------------------------------
	auto* proc = new QProcess(this);
	e.proc = proc;
	proc->setProcessChannelMode(QProcess::SeparateChannels);

	const auto game_id = e.game.game_id;
	QObject::connect(proc, &QProcess::readyReadStandardError,
		this, [this, proc, game_id] {
			appendStderr(game_id, proc->readAllStandardError());
		});
	QObject::connect(proc, &QProcess::readyReadStandardOutput,
		this, [this, proc, game_id] {
			// Forward stdout to our own stderr so `open .../simsc_desktop.app`
			// users see server progress in the launching terminal. This
			// isn't shown in the UI; it's just for debugging.
			const auto b = proc->readAllStandardOutput();
			fwrite(b.constData(), 1, b.size(), stderr);
			// Peek for the ready signal. Server prints
			// "[srv] starting: ..." early; we treat that as
			// running (it can still fail later, in which case
			// finished() will flip us to Failed).
			if (b.contains("[srv] starting:")) {
				markStatus(game_id, LocalGame::Status::Running);
			}
		});
	QObject::connect(proc,
		QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
		this, [this, game_id](int code, QProcess::ExitStatus st) {
			onProcessFinished(game_id, code, (int)st);
		});
	QObject::connect(proc, &QProcess::errorOccurred,
		this, [this, game_id](QProcess::ProcessError err) {
			onProcessErrorOccurred(game_id, (int)err);
		});

	// Insert into model. Must happen before start() so the entry
	// exists by the time signals fire.
	beginInsertRows({}, entries_.size(), entries_.size());
	entries_.push_back(std::move(e));
	endInsertRows();

	proc->start(server_bin, argv);
	if (!proc->waitForStarted(3000)) {
		markStatus(game_id, LocalGame::Status::Failed);
		emit errorOccurred(game_id,
			tr("openbw_server failed to start: %1").arg(proc->errorString()));
		return game_id;
	}

	return game_id;
}

void LocalServerManager::stopGame(const QString& game_id) {
	const int r = rowOfGame(game_id);
	if (r < 0) return;
	auto* proc = entries_[r].proc;
	if (!proc) return;
	if (proc->state() == QProcess::NotRunning) return;
	proc->terminate();
	if (!proc->waitForFinished(3000)) {
		proc->kill();
		proc->waitForFinished(1000);
	}
}

void LocalServerManager::stopAll() {
	// Terminate every still-running server. Called from destructor
	// and from MainWindow's aboutToQuit handler. Safe to call
	// multiple times.
	for (auto& e : entries_) {
		if (e.proc && e.proc->state() != QProcess::NotRunning) {
			e.proc->terminate();
		}
	}
	// Second pass: wait, then kill stragglers.
	for (auto& e : entries_) {
		if (!e.proc) continue;
		if (e.proc->state() == QProcess::NotRunning) continue;
		if (!e.proc->waitForFinished(3000)) {
			e.proc->kill();
			e.proc->waitForFinished(1000);
		}
	}
}

QString LocalServerManager::agentUrl(const QString& game_id) const {
	const auto* g = gameById(game_id);
	if (!g) return {};
	// Local games route by port, not by path. See header comment.
	return QStringLiteral("ws://localhost:%1/agent").arg(g->port);
}

QString LocalServerManager::observerUrl(const QString& game_id) const {
	const auto* g = gameById(game_id);
	if (!g) return {};
	return QStringLiteral("ws://localhost:%1/observer")
		.arg(g->port + 1);
}

QString LocalServerManager::apiKeyForObserver(const QString& game_id,
                                              const QString& alias) const {
	const auto* g = gameById(game_id);
	if (!g) return {};
	// If caller asked for a specific perspective, use that user's key.
	if (!alias.isEmpty()) return roster_->apiKeyFor(alias);
	// Otherwise fall back to whichever player is in slot 0.
	for (const auto& lp : g->players) {
		if (lp.slot == 0) return roster_->apiKeyFor(lp.alias);
	}
	// Last resort: any player's key.
	if (!g->players.isEmpty()) return roster_->apiKeyFor(g->players[0].alias);
	return {};
}

void LocalServerManager::appendStderr(const QString& game_id,
                                      const QByteArray& chunk) {
	const int r = rowOfGame(game_id);
	if (r < 0) return;
	// Also forward to our own stderr so it shows up in the launch
	// terminal for debugging.
	fwrite(chunk.constData(), 1, chunk.size(), stderr);
	auto& g = entries_[r].game;
	g.stderr_tail += QString::fromUtf8(chunk);
	if (g.stderr_tail.size() > kStderrTailLimit) {
		g.stderr_tail = g.stderr_tail.right(kStderrTailLimit);
	}
	// Server prints "[srv] starting:" on stderr in the current
	// build (see server/main.cpp:411) -- treat that as ready.
	if (g.status == LocalGame::Status::Starting
	    && chunk.contains("[srv] starting:")) {
		markStatus(game_id, LocalGame::Status::Running);
	}
}

void LocalServerManager::onProcessFinished(const QString& game_id,
                                           int code, int exitStatus) {
	const int r = rowOfGame(game_id);
	if (r < 0) return;
	const bool ok = (exitStatus == QProcess::NormalExit && code == 0);
	markStatus(game_id, ok ? LocalGame::Status::Exited
	                       : LocalGame::Status::Failed);
	if (!ok) {
		emit errorOccurred(game_id,
			tr("openbw_server exited with status=%1 code=%2")
			    .arg(exitStatus).arg(code));
	}
}

void LocalServerManager::onProcessErrorOccurred(const QString& game_id,
                                                int qprocess_error) {
	const int r = rowOfGame(game_id);
	if (r < 0) return;
	// A FailedToStart-class error means the process never ran.
	// Crashed / Timeout / WriteError post-start are handled by
	// onProcessFinished; we only surface start failures here.
	if (qprocess_error == QProcess::FailedToStart) {
		markStatus(game_id, LocalGame::Status::Failed);
		emit errorOccurred(game_id,
			tr("QProcess error %1").arg(qprocess_error));
	}
}

void LocalServerManager::markStatus(const QString& game_id,
                                    LocalGame::Status s) {
	const int r = rowOfGame(game_id);
	if (r < 0) return;
	if (entries_[r].game.status == s) return;
	entries_[r].game.status = s;
	const auto top    = index(r, 0);
	const auto bottom = index(r, ColumnCount - 1);
	emit dataChanged(top, bottom);
	emit gameStatusChanged(game_id);
}

int LocalServerManager::rowCount(const QModelIndex&) const {
	return entries_.size();
}

int LocalServerManager::columnCount(const QModelIndex&) const {
	return ColumnCount;
}

QVariant LocalServerManager::data(const QModelIndex& idx, int role) const {
	if (!idx.isValid() || idx.row() < 0 || idx.row() >= entries_.size()) {
		return {};
	}
	const auto& g = entries_[idx.row()].game;
	if (role != Qt::DisplayRole) return {};
	switch (idx.column()) {
		case GameIdCol:  return g.game_id;
		case MapCol:     return g.map_relpath;
		case PlayersCol: {
			QStringList parts;
			for (const auto& lp : g.players) {
				parts << QStringLiteral("%1(%2)").arg(lp.alias, lp.race);
			}
			return parts.join(", ");
		}
		case PortCol:    return g.port;
		case StatusCol:  return statusName(g.status);
	}
	return {};
}

QVariant LocalServerManager::headerData(int section, Qt::Orientation o,
                                        int role) const {
	if (o != Qt::Horizontal || role != Qt::DisplayRole) return {};
	switch (section) {
		case GameIdCol:  return QStringLiteral("Game");
		case MapCol:     return QStringLiteral("Map");
		case PlayersCol: return QStringLiteral("Players");
		case PortCol:    return QStringLiteral("Port");
		case StatusCol:  return QStringLiteral("Status");
	}
	return {};
}

}   // namespace simsc_desktop
