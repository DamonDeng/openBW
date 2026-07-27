// mapping_tab.h: HD Mapping tab for sprite_viewer.
//
// Long-form eyeball workflow for building the authoritative
// mapping between openBW's arr/images.tbl entries and SC:R's
// images.rel HD rows. One list row per images.rel HD entry:
//
//   [128x128 preview]  row 244 -> main_243.anim
//                      [combo: terran\marine.grp ▾] [comment: ""]
//                      [Confirm]
//
// The combobox vocabulary is arr/images.tbl (~929 entries: units,
// buildings, addons, projectiles, effects). Combobox is editable
// so the user can type-to-search. Confirming a row locks in the
// picked name and REMOVES that name from every other row's
// dropdown so it can only map once.
//
// Progress is auto-saved to <sc-remastered-path>/../hd_mapping.json
// after every confirm / unconfirm / comment change. The same file
// is auto-loaded on Scan so the user can close and resume.
//
// Only functions when the viewer was launched in remastered mode.

#pragma once

#include <QtCore/QString>
#include <QtWidgets/QWidget>

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;

namespace sprite_viewer {

class HdAssetLoader;

class MappingTab : public QWidget {
	Q_OBJECT
public:
	// `loader` may be null (classic mode). The tab detects and shows
	// a stub in that case. `autosave_path` is where confirms/comment
	// edits stream to; empty disables autosave. `sd_previews_dir` is
	// the folder tools/sd_sprite_dump wrote SD reference PNGs to;
	// empty disables the per-row SD preview column. Ownership stays
	// with the caller for `loader`.
	MappingTab(HdAssetLoader* loader,
	           QString autosave_path,
	           QString sd_previews_dir,
	           QWidget* parent = nullptr);
	~MappingTab();

	// Backing state for one row. Kept separately from the Qt
	// widgets so save / load / autosave can serialize without
	// touching the UI.
	struct Row {
		int sc_r_row = -1;
		uint32_t anim_num = 0;
		int atlas_w = 0, atlas_h = 0;
		int sprite_w = 0, sprite_h = 0;
		int frame_count = 0;
		QString picked_name;    // "" = unassigned / unconfirmed
		QString comment;
		bool confirmed = false; // true after user hit Confirm
	};

private slots:
	void on_scan_clicked();
	void on_save_as_clicked();
	void on_load_clicked();

private:
	// Row-widget builders + event handlers. All row widgets are
	// created once during on_scan_clicked() and never rebuilt --
	// we only mutate their state.
	QWidget* build_row_widget(int row_idx, const QImage& preview);
	void on_confirm_clicked(int row_idx);
	void on_unconfirm_clicked(int row_idx);
	void on_comment_edited(int row_idx, const QString& text);

	// Rebuild the "used names" set from confirmed rows, then walk
	// every unconfirmed row's combobox and hide the used entries.
	// Cheap: O(rows * names) but rows*names fits in ~230k comparisons.
	void refresh_used_names();

	// autosave / load helpers.
	void autosave_now();
	void write_json_to(const QString& path);
	// Returns count of applied rows.
	int load_json_from(const QString& path);

	HdAssetLoader* loader_ = nullptr;
	QString autosave_path_;
	QString sd_previews_dir_;
	// Map from arr/images.tbl filename (e.g. "terran\\marine.grp")
	// to its 0-based image_id. Built once during Scan by walking
	// the loader's read_images_tbl(). Used to locate the SD preview
	// PNG file (image_id encoded in its filename prefix).
	std::vector<QString> images_tbl_by_id_;

	std::vector<Row> rows_;

	// Per-row widget refs so we can toggle enabled state on
	// Confirm/Unconfirm and rebuild dropdowns on refresh_used_names.
	struct RowWidgets {
		QComboBox* combo = nullptr;
		QLineEdit* comment = nullptr;
		QPushButton* confirm_btn = nullptr;
		QLabel* summary = nullptr;
		QLabel* sd_preview = nullptr;   // right-side SD preview, may be null
	};
	std::vector<RowWidgets> row_ws_;

	// Rebuild the SD preview label for row_idx to show the PNG for
	// its current combobox pick. Called on combo change + on scan.
	void refresh_sd_preview(int row_idx);

	// Dropdown vocab: arr/images.tbl strings, prepended with
	// "(unassigned)" at index 0. Sorted alphabetically after that
	// so type-to-search works predictably.
	std::vector<QString> unit_names_;

	// Names currently locked by a confirmed row -- these get hidden
	// from every other row's combobox.
	std::unordered_set<std::string> used_names_;

	// Top-bar UI (owned by Qt).
	QListWidget* list_ = nullptr;
	QLabel* status_ = nullptr;
	QPushButton* scan_btn_ = nullptr;
	QPushButton* save_as_btn_ = nullptr;
	QPushButton* load_btn_ = nullptr;
};

}   // namespace sprite_viewer
