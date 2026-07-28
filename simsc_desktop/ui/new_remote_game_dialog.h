// New Remote Game dialog.
//
// Same shape as NewLocalGameDialog -- map dropdown at the top,
// N rows of [alias combo][race combo] driven by the map's
// player_count. The alias combos come from the remote user
// roster (GET /api/users). Empty selections are allowed on the
// server side (creates a pending invitation slot with alias =
// null); we translate them into empty strings and CreateRemoteGameReq
// will serialize back to JSON null.
//
// A slot's alias may be marked "AIBot" -- that's the server-side
// special value that skips the invitation and joins that slot as
// a computer_game controller. We add it as a synthetic option in
// the dropdown so users can pick it without typing.

#pragma once

#include "../simsc_types.h"           // CreateRemoteGameReq

#include <QtWidgets/QDialog>
#include <QtCore/QVector>

class QComboBox;
class QVBoxLayout;

namespace simsc_desktop {

class MapCatalog;

class NewRemoteGameDialog : public QDialog {
	Q_OBJECT
public:
	NewRemoteGameDialog(MapCatalog* catalog,
	                    const QVector<RemoteUser>& roster,
	                    const QString& current_user_alias,
	                    const QString& default_game_speed,
	                    QWidget* parent = nullptr);

	// Filled in after the dialog is accepted.
	CreateRemoteGameReq getParams() const { return params_; }

private slots:
	void onMapChanged(int row);
	void onAccept();

private:
	void rebuildSlotRows(int player_count);

	MapCatalog*        catalog_ = nullptr;
	QVector<RemoteUser> roster_;
	QString            current_user_alias_;

	QComboBox*   map_combo_    = nullptr;
	QComboBox*   speed_combo_  = nullptr;
	QVBoxLayout* slots_layout_ = nullptr;

	struct SlotRow {
		QComboBox* alias_combo = nullptr;
		QComboBox* race_combo  = nullptr;
	};
	QVector<SlotRow>  slots_;
	QVector<QWidget*> slot_widgets_;

	CreateRemoteGameReq params_;
};

}   // namespace simsc_desktop
