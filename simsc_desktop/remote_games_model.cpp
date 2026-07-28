#include "remote_games_model.h"

#include <QtCore/QStringList>

namespace simsc_desktop {

RemoteGamesModel::RemoteGamesModel(QObject* parent)
	: QAbstractTableModel(parent) {}

void RemoteGamesModel::setGames(QVector<RemoteGame> games) {
	beginResetModel();
	games_ = std::move(games);
	endResetModel();
}

const RemoteGame* RemoteGamesModel::gameAtRow(int row) const {
	if (row < 0 || row >= games_.size()) return nullptr;
	return &games_.at(row);
}

int RemoteGamesModel::rowOfGame(const QString& game_id) const {
	for (int i = 0; i < games_.size(); ++i) {
		if (games_[i].game_id == game_id) return i;
	}
	return -1;
}

const RemoteGame* RemoteGamesModel::gameById(const QString& game_id) const {
	const int r = rowOfGame(game_id);
	return r < 0 ? nullptr : &games_.at(r);
}

int RemoteGamesModel::rowCount(const QModelIndex&) const {
	return games_.size();
}

int RemoteGamesModel::columnCount(const QModelIndex&) const {
	return ColumnCount;
}

QVariant RemoteGamesModel::data(const QModelIndex& idx, int role) const {
	if (!idx.isValid() || idx.row() < 0 || idx.row() >= games_.size()) {
		return {};
	}
	if (role != Qt::DisplayRole) return {};
	const auto& g = games_.at(idx.row());
	switch (idx.column()) {
		case GameIdCol: return g.game_id.left(8);
		case MapCol:    return g.map;
		case OwnerCol:  return g.owner_alias;
		case PlayersCol: {
			QStringList parts;
			for (int i = 0; i < g.player_aliases.size(); ++i) {
				const auto race = i < g.races.size() ? g.races[i] : QString();
				const auto ali  = g.player_aliases[i];
				parts << (ali.isEmpty()
					? QStringLiteral("-(%1)").arg(race)
					: QStringLiteral("%1(%2)").arg(ali, race));
			}
			return parts.join(", ");
		}
		case StateCol:  return g.state;
		case MineCol:   return g.my_invitation_status;
	}
	return {};
}

QVariant RemoteGamesModel::headerData(int section, Qt::Orientation o,
                                      int role) const {
	if (o != Qt::Horizontal || role != Qt::DisplayRole) return {};
	switch (section) {
		case GameIdCol: return QStringLiteral("Game");
		case MapCol:    return QStringLiteral("Map");
		case OwnerCol:  return QStringLiteral("Owner");
		case PlayersCol: return QStringLiteral("Players");
		case StateCol:  return QStringLiteral("State");
		case MineCol:   return QStringLiteral("Mine");
	}
	return {};
}

}   // namespace simsc_desktop
