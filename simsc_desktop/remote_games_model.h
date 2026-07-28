// QAbstractTableModel of RemoteGame rows.
//
// Lives inside RemoteGamesTab; not shared. Rebuilt in-place on
// every SimscApiClient::gamesReceived so the QTableView survives
// data reloads without losing selection state.
//
// Column mapping:
//   0 GameId     -- short 8-char UUID prefix
//   1 Map
//   2 Owner
//   3 Players    -- comma-joined "alias(race)" or "-" for empty slots
//   4 State      -- "pending_invitations" | "running" | ...
//   5 Mine       -- caller's invitation status; empty when N/A

#pragma once

#include "simsc_types.h"

#include <QtCore/QAbstractTableModel>
#include <QtCore/QVector>

namespace simsc_desktop {

class RemoteGamesModel : public QAbstractTableModel {
	Q_OBJECT
public:
	enum Column {
		GameIdCol   = 0,
		MapCol      = 1,
		OwnerCol    = 2,
		PlayersCol  = 3,
		StateCol    = 4,
		MineCol     = 5,
		ColumnCount
	};

	explicit RemoteGamesModel(QObject* parent = nullptr);

	// Replace the whole list. Preserves current row selection only
	// if the caller re-selects by game_id after this returns.
	void setGames(QVector<RemoteGame> games);

	const RemoteGame* gameAtRow(int row) const;
	const RemoteGame* gameById(const QString& game_id) const;
	int               rowOfGame(const QString& game_id) const;

	int rowCount(const QModelIndex& parent = {}) const override;
	int columnCount(const QModelIndex& parent = {}) const override;
	QVariant data(const QModelIndex& idx, int role) const override;
	QVariant headerData(int section, Qt::Orientation, int) const override;

private:
	QVector<RemoteGame> games_;
};

}   // namespace simsc_desktop
