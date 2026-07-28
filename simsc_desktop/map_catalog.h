// Loads the shared maps.json catalog and exposes it as a
// QAbstractListModel so the new-game dialogs can bind directly.
//
// Format on disk (mirrors simsc/app/static/maps.json):
//   [ { "filename": "...", "display_name": "...", "player_count": N }, ... ]
//
// The filename here is a RELATIVE path resolved against the
// bundled maps dir (AppPaths::bundled_maps_dir()); the tab hands
// the absolute path to LocalServerManager when starting a game.

#pragma once

#include <QtCore/QAbstractListModel>
#include <QtCore/QString>
#include <QtCore/QVector>

namespace simsc_desktop {

struct MapEntry {
	QString filename;      // relative to maps dir, e.g. "(2)Bottleneck.scm"
	QString display_name;
	int     player_count = 0;
};

class MapCatalog : public QAbstractListModel {
	Q_OBJECT
public:
	// Custom roles so the dialog can pull structured data without
	// re-parsing the display text. Values chosen above the standard
	// Qt::UserRole so they don't collide with anything Qt owns.
	enum Roles {
		FilenameRole    = Qt::UserRole + 1,
		DisplayNameRole,
		PlayerCountRole,
		EntryRole,          // whole struct, for callers that want it
	};

	explicit MapCatalog(QObject* parent = nullptr);

	// Load from disk. `json_path` may be empty (roster stays empty).
	// Missing or malformed file is not fatal -- the model stays
	// empty and callers see an empty picker.
	void loadFromFile(const QString& json_path);

	const QVector<MapEntry>& entries() const { return entries_; }
	const MapEntry* entryAt(int row) const;

	// QAbstractListModel.
	int rowCount(const QModelIndex& parent = {}) const override;
	QVariant data(const QModelIndex& idx, int role) const override;
	QHash<int, QByteArray> roleNames() const override;

private:
	QVector<MapEntry> entries_;
};

}   // namespace simsc_desktop
