// New Local Game dialog.
//
// User picks a map from the catalog; a row per map slot appears
// with two dropdowns: [alias from local roster] and
// [zerg/terran/protoss/random]. OK returns a filled-in
// NewLocalGameParams to the caller (via getParams()).
//
// The row count follows the map's player_count, so the number of
// rows tracks the map selection live (add rows when a
// higher-player-count map is picked; drop rows when the count
// shrinks). Slot number is implicit -- row index i -> slot i --
// which matches how the remote server assigns slots too.

#pragma once

#include "../local_server_manager.h"   // NewLocalGameParams

#include <QtWidgets/QDialog>
#include <QtCore/QVector>

class QComboBox;
class QVBoxLayout;

namespace simsc_desktop {

class MapCatalog;
class LocalUserRoster;

class NewLocalGameDialog : public QDialog {
	Q_OBJECT
public:
	NewLocalGameDialog(MapCatalog* catalog,
	                   LocalUserRoster* roster,
	                   quint16 default_port,
	                   const QString& maps_dir_abspath,
	                   QWidget* parent = nullptr);

	// Filled in after the dialog is accepted. Undefined content
	// before that -- always check result() == Accepted first.
	NewLocalGameParams getParams() const { return params_; }

private slots:
	void onMapChanged(int row);
	void onAccept();

private:
	void rebuildSlotRows(int player_count);

	MapCatalog*      catalog_ = nullptr;
	LocalUserRoster* roster_  = nullptr;
	QString          maps_dir_abspath_;

	QComboBox*   map_combo_    = nullptr;
	QVBoxLayout* slots_layout_ = nullptr;

	struct SlotRow {
		QComboBox* alias_combo = nullptr;
		QComboBox* race_combo  = nullptr;
	};
	QVector<SlotRow> slots_;
	QVector<QWidget*> slot_widgets_;   // parent widgets of each row

	NewLocalGameParams params_;
};

}   // namespace simsc_desktop
