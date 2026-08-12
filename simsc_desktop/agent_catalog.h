// AgentCatalog — the "which agents are launchable?" model.
//
// Scans Settings::agents_dir() for executable files. Every result is
// a picker entry in the Attach-Agent dialog. Kept deliberately simple:
// no manifest, no per-agent metadata, no cache — filename is the label
// the user sees.
//
// The launch contract every listed wrapper MUST honor is documented in
// simsc_agents/README.md (and its Python-side shared helper is in
// python_agent/agent_cli.py). The runtime side that actually spawns
// them is AgentSupervisor. AgentCatalog only enumerates.
//
// Rescan semantics: rescan() re-reads the directory synchronously.
// The picker calls it every time it opens; there is no filesystem
// watcher because (a) 33-ish agents stat in single-digit milliseconds
// even on cold cache, and (b) a watcher would need cross-platform
// glue we don't yet want.

#pragma once

#include <QtCore/QAbstractListModel>
#include <QtCore/QString>
#include <QtCore/QVector>

namespace simsc_desktop {

struct AgentEntry {
	// Human-readable label shown in the picker. Currently the file's
	// basename with any extension stripped ("z_agent_v5"), which the
	// workshop story finds friendly.
	QString display_name;
	// Absolute path to the executable. Handed to QProcess as-is.
	QString path;
};

// A QAbstractListModel so the Attach-Agent dialog can bind a QListView
// / QComboBox directly. Row order is the sorted display_name so the
// picker is stable across rescans.
class AgentCatalog : public QAbstractListModel {
	Q_OBJECT
public:
	explicit AgentCatalog(QObject* parent = nullptr);

	// Point the catalog at a new agents directory. Empty string is
	// allowed and results in a zero-row model (feature disabled).
	// Triggers a synchronous rescan.
	void set_agents_dir(const QString& path);
	QString agents_dir() const { return agents_dir_; }

	// Re-read the current agents_dir(). Callers (mainly the dialog's
	// showEvent) invoke this so the picker reflects filesystem
	// changes since last open. Cheap enough to call unconditionally.
	void rescan();

	// Rows correspond to `entries_` order (sorted by display_name).
	int rowCount(const QModelIndex& parent = {}) const override;
	QVariant data(const QModelIndex& index,
	              int role = Qt::DisplayRole) const override;

	// Direct access for callers that want the full struct.
	const AgentEntry& at(int row) const { return entries_.at(row); }

private:
	QString agents_dir_;
	QVector<AgentEntry> entries_;
};

}   // namespace simsc_desktop
