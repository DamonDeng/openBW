// Settings tab. First-run destination; also the place where the
// SC1 data path, remote API key, remote base URL, default local
// port, and the local user roster live.
//
// The tab holds a Settings* + LocalUserRoster*; both are owned by
// MainWindow and passed in by pointer. The widget just binds
// controls to them and pushes changes back on edits.

#pragma once

#include <QtWidgets/QWidget>

class QComboBox;
class QLineEdit;
class QSpinBox;
class QTableView;
class QPushButton;

namespace simsc_desktop {

class Settings;
class LocalUserRoster;

class SettingsTab : public QWidget {
	Q_OBJECT
public:
	SettingsTab(Settings* settings, LocalUserRoster* roster,
	            QWidget* parent = nullptr);

private slots:
	void onBrowseSc1DataPath();
	void onBrowseServerBinaryOverride();
	void onAddUser();
	void onRemoveUser();
	void pushCurrentValuesToUi();

private:
	Settings*        settings_ = nullptr;
	LocalUserRoster* roster_   = nullptr;

	QLineEdit*   sc1_data_path_edit_          = nullptr;
	QLineEdit*   simsc_api_key_edit_          = nullptr;
	QLineEdit*   simsc_base_url_edit_         = nullptr;
	QSpinBox*    default_local_port_spin_     = nullptr;
	QComboBox*   default_game_speed_combo_    = nullptr;
	QLineEdit*   server_binary_override_edit_ = nullptr;
	QTableView*  roster_view_                 = nullptr;
	QPushButton* remove_user_btn_             = nullptr;
};

}   // namespace simsc_desktop
