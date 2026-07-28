// See map_catalog.h.

#include "map_catalog.h"

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QVariant>

namespace simsc_desktop {

MapCatalog::MapCatalog(QObject* parent) : QAbstractListModel(parent) {}

void MapCatalog::loadFromFile(const QString& json_path) {
	beginResetModel();
	entries_.clear();
	if (!json_path.isEmpty()) {
		QFile f(json_path);
		if (f.open(QIODevice::ReadOnly)) {
			const auto doc = QJsonDocument::fromJson(f.readAll());
			if (doc.isArray()) {
				for (const auto& v : doc.array()) {
					const auto o = v.toObject();
					MapEntry e;
					e.filename     = o.value("filename").toString();
					e.display_name = o.value("display_name").toString();
					e.player_count = o.value("player_count").toInt();
					if (!e.filename.isEmpty()) entries_.push_back(e);
				}
			}
		}
	}
	endResetModel();
}

const MapEntry* MapCatalog::entryAt(int row) const {
	if (row < 0 || row >= entries_.size()) return nullptr;
	return &entries_.at(row);
}

int MapCatalog::rowCount(const QModelIndex&) const {
	return entries_.size();
}

QVariant MapCatalog::data(const QModelIndex& idx, int role) const {
	if (!idx.isValid() || idx.row() < 0 || idx.row() >= entries_.size()) {
		return {};
	}
	const auto& e = entries_.at(idx.row());
	switch (role) {
		case Qt::DisplayRole:
			// "Bottleneck (2p)" -- readable in a combobox.
			return QStringLiteral("%1 (%2p)").arg(e.display_name)
			                                 .arg(e.player_count);
		case FilenameRole:    return e.filename;
		case DisplayNameRole: return e.display_name;
		case PlayerCountRole: return e.player_count;
		case EntryRole:       return QVariant::fromValue(e);
	}
	return {};
}

QHash<int, QByteArray> MapCatalog::roleNames() const {
	auto r = QAbstractListModel::roleNames();
	r.insert(FilenameRole,    "filename");
	r.insert(DisplayNameRole, "display_name");
	r.insert(PlayerCountRole, "player_count");
	return r;
}

}   // namespace simsc_desktop
