// See agent_supervisor.h.
//
// One QProcess per (game_id, slot) attachment. We wire the child's
// stdout/stderr to be discarded (nullptr channel) rather than piping
// into simsc_desktop -- agents can be chatty and a long-running one
// would grow a bounded Qt IPC buffer forever. Users who want to see
// agent output run the agent from a shell instead of via the lobby.
//
// Exit code semantics: QProcess::finished delivers int exitCode +
// QProcess::ExitStatus. We collapse both into a single "exit_code"
// int for the agentDetached signal: on NormalExit it's the child's
// exit(), on CrashExit / launch failure it's -1. The UI just wants
// "did it succeed?" and shows the number as-is next to the row.

#include "agent_supervisor.h"

#include <QtCore/QProcess>
#include <QtCore/QStringList>
#include <QtCore/QTimer>

namespace simsc_desktop {

AgentSupervisor::AgentSupervisor(QObject* parent) : QObject(parent) {}

AgentSupervisor::~AgentSupervisor() {
	// Belt-and-braces: MainWindow's aboutToQuit is expected to call
	// stop_all(). If it didn't (e.g. during a test), still tear down
	// so we don't leak child processes.
	stop_all();
}

bool AgentSupervisor::attach(const QString& game_id, int slot,
                             const QString& agent_path,
                             const QString& agent_display_name,
                             const QString& url,
                             const QString& api_key,
                             const QString& race) {
	Key key{game_id, slot};
	if (attachments_.contains(key)) return false;

	auto* proc = new QProcess(this);

	// Discard child stdout/stderr. See file-top comment for rationale.
	proc->setStandardOutputFile(QProcess::nullDevice());
	proc->setStandardErrorFile(QProcess::nullDevice());

	QStringList argv;
	argv << QStringLiteral("--url") << url
	     << QStringLiteral("--api-key") << api_key
	     << QStringLiteral("--race") << race;

	// finished(int, QProcess::ExitStatus) -> unified path via
	// on_finished. Capture key by value so a later detach() doesn't
	// invalidate what the lambda reads.
	connect(proc, &QProcess::finished, this,
		[this, key](int exit_code, QProcess::ExitStatus status) {
			int reported = (status == QProcess::NormalExit)
				? exit_code : -1;
			on_finished(key, reported);
		});

	// errorOccurred fires on launch failure (FailedToStart) before
	// finished; catch it so the UI hears "detached" and re-enables
	// the Attach button on the same event-loop turn.
	connect(proc, &QProcess::errorOccurred, this,
		[this, key](QProcess::ProcessError err) {
			if (err == QProcess::FailedToStart) {
				on_finished(key, -1);
			}
			// Other errors (Crashed, Timedout, WriteError, ReadError,
			// UnknownError) all end with finished() as well, so no
			// double-emit risk.
		});

	proc->start(agent_path, argv);
	if (proc->state() == QProcess::NotRunning
	    && proc->error() == QProcess::FailedToStart) {
		// Started synchronously-failed. errorOccurred will fire on
		// the next event-loop turn; leave the entry in the map so
		// on_finished's cleanup path removes it.
	}

	attachments_.insert(key, Entry{proc, agent_display_name});
	emit agentAttached(game_id, slot, agent_display_name);
	return true;
}

void AgentSupervisor::detach(const QString& game_id, int slot) {
	Key key{game_id, slot};
	auto it = attachments_.find(key);
	if (it == attachments_.end()) return;
	QProcess* proc = it.value().proc;
	if (proc && proc->state() != QProcess::NotRunning) {
		// SIGTERM; on_finished handles the eventual removal + signal
		// emission. If the child ignores TERM we escalate in
		// stop_all(); for a one-off detach we let it settle.
		proc->terminate();
	} else {
		// Already dead but map entry lingered -- clean up now.
		on_finished(key, proc ? proc->exitCode() : -1);
	}
}

bool AgentSupervisor::is_attached(const QString& game_id, int slot) const {
	return attachments_.contains({game_id, slot});
}

QString AgentSupervisor::attached_at(const QString& game_id, int slot) const {
	auto it = attachments_.constFind({game_id, slot});
	if (it == attachments_.constEnd()) return {};
	return it.value().display_name;
}

void AgentSupervisor::stop_all() {
	// Snapshot keys so we can iterate while on_finished mutates the
	// map underneath us.
	const auto keys = attachments_.keys();
	for (const auto& key : keys) {
		auto it = attachments_.find(key);
		if (it == attachments_.end()) continue;
		QProcess* proc = it.value().proc;
		if (!proc) continue;
		if (proc->state() == QProcess::NotRunning) continue;
		proc->terminate();
	}
	// Give children 3s to exit on TERM, then SIGKILL stragglers. This
	// is a synchronous wait -- fine at shutdown; the alternative is
	// leaving orphaned python processes hanging on the WS.
	for (const auto& key : keys) {
		auto it = attachments_.find(key);
		if (it == attachments_.end()) continue;
		QProcess* proc = it.value().proc;
		if (!proc) continue;
		if (proc->state() == QProcess::NotRunning) continue;
		if (!proc->waitForFinished(3000)) {
			proc->kill();
			proc->waitForFinished(1000);
		}
	}
}

void AgentSupervisor::on_finished(const Key& key, int exit_code) {
	auto it = attachments_.find(key);
	if (it == attachments_.end()) return;   // idempotent
	QProcess* proc = it.value().proc;
	attachments_.erase(it);
	// Delete via deleteLater so any pending queued QProcess signals
	// have a chance to fire before the object goes away.
	if (proc) proc->deleteLater();
	emit agentDetached(key.first, key.second, exit_code);
}

}   // namespace simsc_desktop
