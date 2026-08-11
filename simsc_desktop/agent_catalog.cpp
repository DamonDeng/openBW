// See agent_catalog.h.
//
// Filesystem walk: QDirIterator recursive, filter to Files, skip
// hidden. For each hit we check QFileInfo::isExecutable() -- on
// macOS/Linux that's the +x bit; on Windows it's an .exe/.bat/.cmd
// suffix, which is what we want for cross-platform "run me" semantics.
//
// Sort by display_name (ASCII, case-insensitive) so the picker order
// is stable and predictable. If a user renames a file the position
// changes but that's fine -- the picker is a list of names, not a
// numbered menu.

#include "agent_catalog.h"

#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFileInfo>

#include <algorithm>

namespace simsc_desktop {

namespace {

// Extract "z_agent_v5" from "/path/to/z_agent_v5.py" -- strip the
// directory + trailing extension. Keeps embedded dots (a shell script
// literally named "z.agent.sh" becomes "z.agent"), which is fine for
// display purposes.
QString display_name_for(const QFileInfo& fi) {
	QString base = fi.completeBaseName();
	if (base.isEmpty()) base = fi.fileName();
	return base;
}

}   // namespace

AgentCatalog::AgentCatalog(QObject* parent)
	: QAbstractListModel(parent) {}

void AgentCatalog::set_agents_dir(const QString& path) {
	if (path == agents_dir_) return;
	agents_dir_ = path;
	rescan();
}

void AgentCatalog::rescan() {
	beginResetModel();
	entries_.clear();

	if (!agents_dir_.isEmpty()) {
		QDir root(agents_dir_);
		if (root.exists()) {
			QDirIterator it(
				agents_dir_,
				QDir::Files | QDir::NoSymLinks | QDir::NoDotAndDotDot,
				QDirIterator::Subdirectories);
			while (it.hasNext()) {
				it.next();
				QFileInfo fi = it.fileInfo();
				// Skip hidden (leading dot) so editor swap files and
				// .DS_Store don't clutter the picker.
				if (fi.fileName().startsWith('.')) continue;
				// The whole point of the standard: only entries the
				// OS will actually execute count. On Unix that's the
				// +x bit; on Windows QFileInfo::isExecutable() picks
				// up known suffixes.
				if (!fi.isExecutable()) continue;
				entries_.push_back({
					display_name_for(fi),
					fi.absoluteFilePath(),
				});
			}
		}
	}

	std::sort(entries_.begin(), entries_.end(),
		[](const AgentEntry& a, const AgentEntry& b) {
			return QString::compare(
				a.display_name, b.display_name,
				Qt::CaseInsensitive) < 0;
		});

	endResetModel();
}

int AgentCatalog::rowCount(const QModelIndex& parent) const {
	if (parent.isValid()) return 0;
	return (int)entries_.size();
}

QVariant AgentCatalog::data(const QModelIndex& index, int role) const {
	if (!index.isValid() || index.row() < 0
	    || index.row() >= entries_.size()) return {};
	const auto& e = entries_.at(index.row());
	if (role == Qt::DisplayRole) return e.display_name;
	// Second column of the QComboBox popup: absolute path as tooltip
	// so an attendee can spot which of two like-named files they're
	// about to launch.
	if (role == Qt::ToolTipRole) return e.path;
	return {};
}

}   // namespace simsc_desktop
