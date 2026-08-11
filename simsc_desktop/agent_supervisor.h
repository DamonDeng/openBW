// AgentSupervisor — the "keep track of agent QProcesses" runtime.
//
// Owns one QProcess per (game_id, slot) attachment. Public API is
// intentionally narrow:
//
//   attach(game_id, slot, agent_path, url, api_key, race) -> bool
//   detach(game_id, slot)
//   is_attached(game_id, slot) const -> bool
//   attachedAt(game_id, slot) const -> QString    // agent display name
//
// Emits signals for the UI to react:
//
//   agentAttached  (game_id, slot, agent_display_name)
//   agentDetached  (game_id, slot, exit_code)      // any exit path
//
// The (game_id, slot) key is what per-slot Attach buttons key on. It
// stays intact for a game's lifetime; detaching just clears the entry
// so the button re-enables.
//
// Shutdown ordering (called from MainWindow's aboutToQuit): stopAll()
// sends SIGTERM to every child, waits up to 3s, then SIGKILLs
// stragglers. Same shape as LocalServerManager::stopAll -- we run
// this BEFORE that one so agents drop cleanly before their server
// disappears out from under them.

#pragma once

#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QPair>
#include <QtCore/QString>

class QProcess;

namespace simsc_desktop {

class AgentSupervisor : public QObject {
	Q_OBJECT
public:
	explicit AgentSupervisor(QObject* parent = nullptr);
	~AgentSupervisor() override;

	// Spawn `agent_path` with the standard launch-contract argv:
	//   agent_path --url <url> --api-key <api_key> --race <race>
	// Returns true if the process started; false if it failed to
	// launch (missing binary, permission denied, etc.). On failure
	// the caller sees an agentDetached emission on the same
	// event-loop turn.
	//
	// Guard against double-attach: if a QProcess is already tracked
	// for (game_id, slot), returns false without touching anything.
	bool attach(const QString& game_id, int slot,
	            const QString& agent_path,
	            const QString& agent_display_name,
	            const QString& url,
	            const QString& api_key,
	            const QString& race);

	// Terminate the agent for (game_id, slot). No-op if nothing is
	// attached there. Emits agentDetached with the exit code
	// asynchronously once the child actually exits.
	void detach(const QString& game_id, int slot);

	// True while a live QProcess exists for (game_id, slot).
	bool is_attached(const QString& game_id, int slot) const;

	// Display name (as passed to attach()) of the agent currently
	// attached at (game_id, slot). Empty when nothing's attached.
	QString attached_at(const QString& game_id, int slot) const;

	// Terminate every tracked child. Called from MainWindow's
	// aboutToQuit hook. Blocks up to 3s per child before SIGKILL.
	void stop_all();

signals:
	void agentAttached(const QString& game_id, int slot,
	                   const QString& agent_display_name);
	// exit_code is QProcess::exitCode() when the child exited
	// cleanly, or -1 for launch failures / kills.
	void agentDetached(const QString& game_id, int slot,
	                   int exit_code);

private:
	using Key = QPair<QString, int>;   // (game_id, slot)

	struct Entry {
		QProcess* proc = nullptr;
		QString display_name;
	};

	QHash<Key, Entry> attachments_;

	void on_finished(const Key& key, int exit_code);
};

}   // namespace simsc_desktop
