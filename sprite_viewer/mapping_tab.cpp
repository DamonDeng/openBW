// See mapping_tab.h for the design.

#include "mapping_tab.h"
#include "hd_asset_loader.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QStringList>
#include <QtGui/QPixmap>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QCompleter>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QListWidgetItem>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressDialog>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

namespace sprite_viewer {

// Fixed sentinel at index 0 of every combobox. Guarantees the
// dropdown starts in an "unassigned" state regardless of prior
// filtering.
static const QString kUnassigned = QStringLiteral("(unassigned)");

// ---- ctor / dtor ----------------------------------------------------

MappingTab::MappingTab(HdAssetLoader* loader, QString autosave_path,
                       QString sd_previews_dir, QWidget* parent)
	: QWidget(parent), loader_(loader),
	  autosave_path_(std::move(autosave_path)),
	  sd_previews_dir_(std::move(sd_previews_dir))
{
	auto* root = new QVBoxLayout(this);

	// Top bar.
	auto* top = new QHBoxLayout();
	scan_btn_    = new QPushButton("Scan HD anims");
	save_as_btn_ = new QPushButton("Save As…");
	load_btn_    = new QPushButton("Load…");
	save_as_btn_->setEnabled(false);
	top->addWidget(scan_btn_);
	top->addWidget(load_btn_);
	top->addWidget(save_as_btn_);
	top->addStretch(1);
	status_ = new QLabel(loader_
		? QStringLiteral(
			"Click 'Scan HD anims'. Progress auto-saves to %1")
			.arg(autosave_path_.isEmpty()
				? QStringLiteral("(no autosave path)")
				: autosave_path_)
		: QStringLiteral(
			"HD mode not active. Relaunch with "
			"--sc-version=remastered and --sc-remastered-path."));
	status_->setStyleSheet("color: #667;");
	status_->setWordWrap(true);
	top->addWidget(status_, 2);
	root->addLayout(top);

	// The list. We use setItemWidget with a properly composed
	// QWidget per row (H-layout of icon | text | combo | comment |
	// confirm-btn) so nothing overlaps. Each row is ~140 px tall.
	list_ = new QListWidget();
	list_->setSpacing(2);
	list_->setUniformItemSizes(true);
	// Selecting a row shouldn't visually recolor it -- the row's
	// own controls speak for state. Turning off the selection
	// stylesheet override keeps focus rings off.
	list_->setSelectionMode(QAbstractItemView::NoSelection);
	list_->setFocusPolicy(Qt::NoFocus);
	root->addWidget(list_, 1);

	connect(scan_btn_, &QPushButton::clicked,
	        this, &MappingTab::on_scan_clicked);
	connect(save_as_btn_, &QPushButton::clicked,
	        this, &MappingTab::on_save_as_clicked);
	connect(load_btn_, &QPushButton::clicked,
	        this, &MappingTab::on_load_clicked);

	if (!loader_) {
		scan_btn_->setEnabled(false);
		save_as_btn_->setEnabled(false);
		load_btn_->setEnabled(false);
	}
}

MappingTab::~MappingTab() = default;

// ---- Scan -----------------------------------------------------------

void MappingTab::on_scan_clicked() {
	if (!loader_) return;
	scan_btn_->setEnabled(false);
	list_->clear();
	rows_.clear();
	row_ws_.clear();
	used_names_.clear();

	// 1. Load the images.tbl vocabulary. Keep the raw order in
	//    images_tbl_by_id_ so we can look up an ordinal for the
	//    SD-preview file lookup. Prepend "(unassigned)" and sort
	//    the display vector so type-to-search behaves predictably.
	unit_names_.clear();
	unit_names_.push_back(kUnassigned);
	auto raw = loader_->read_images_tbl();
	if (raw.empty()) {
		status_->setText(QString("images.tbl load failed: %1")
			.arg(loader_->last_error()));
		scan_btn_->setEnabled(true);
		return;
	}
	images_tbl_by_id_ = raw;
	std::vector<QString> display = raw;
	std::sort(display.begin(), display.end(),
		[](const QString& a, const QString& b) {
			return a.compare(b, Qt::CaseInsensitive) < 0;
		});
	for (auto& n : display) unit_names_.push_back(std::move(n));

	// 2. Enumerate the HD rows (decodes atlases -- shows a
	//    progress bar).
	QProgressDialog prog("Scanning HD anims…", QString(), 0, 1, this);
	prog.setWindowModality(Qt::WindowModal);
	prog.setMinimumDuration(0);
	prog.show();
	auto infos = loader_->enumerate_rows(/*flag=*/8,
		[&](int done, int total) {
			prog.setMaximum(total);
			prog.setValue(done);
			QCoreApplication::processEvents();
		});
	prog.close();
	if (infos.empty()) {
		status_->setText(QString("Scan failed: %1")
			.arg(loader_->last_error()));
		scan_btn_->setEnabled(true);
		return;
	}

	// 3. Build one Row + one composed widget per entry.
	rows_.reserve(infos.size());
	row_ws_.reserve(infos.size());
	for (auto& info : infos) {
		Row r;
		r.sc_r_row    = info.sc_r_row;
		r.anim_num    = info.anim_num;
		r.atlas_w     = info.atlas_w;
		r.atlas_h     = info.atlas_h;
		r.sprite_w    = info.sprite_w;
		r.sprite_h    = info.sprite_h;
		r.frame_count = info.frame_count;
		rows_.push_back(r);

		auto* item = new QListWidgetItem();
		item->setSizeHint(QSize(0, 148));
		list_->addItem(item);

		// Compose the two previews side-by-side into a single
		// 256x128 image so the row layout stays simple (one QLabel
		// for both frames). Left half = frame 0, right half = frame
		// mid. Thin vertical separator between them.
		QImage combined(256 + 4, 128, QImage::Format_RGBA8888);
		combined.fill(0x00000000);
		for (int y = 0; y < 128; ++y) {
			// Frame 0 left.
			std::memcpy(combined.scanLine(y),
				info.frame0_preview.scanLine(y), 128 * 4);
			// 4-px transparent gutter.
			// Frame mid right (offset 128+4 = 132).
			std::memcpy(combined.scanLine(y) + 132 * 4,
				info.frame_mid_preview.scanLine(y), 128 * 4);
		}

		QWidget* rw = build_row_widget(
			(int)rows_.size() - 1, combined);
		list_->setItemWidget(item, rw);
	}

	// 4. Auto-load any existing autosave to restore prior work.
	int applied = 0;
	if (!autosave_path_.isEmpty()
	    && QFile::exists(autosave_path_)) {
		applied = load_json_from(autosave_path_);
	}
	refresh_used_names();
	for (int i = 0; i < (int)rows_.size(); ++i) refresh_sd_preview(i);

	save_as_btn_->setEnabled(true);
	status_->setText(
		QString("Scanned %1 HD rows. Applied %2 from autosave. "
		        "Autosave -> %3")
			.arg(rows_.size()).arg(applied).arg(autosave_path_));
}

// ---- Per-row widget -------------------------------------------------

QWidget* MappingTab::build_row_widget(int row_idx,
                                       const QImage& preview)
{
	auto* w = new QWidget();
	w->setAutoFillBackground(true);
	auto* h = new QHBoxLayout(w);
	h->setContentsMargins(6, 6, 6, 6);
	h->setSpacing(10);

	// Preview thumbnail. Composited image is 260x128 (two 128x128
	// crops + a 4-px gutter). Fixed size keeps rows aligned.
	auto* thumb = new QLabel();
	thumb->setPixmap(QPixmap::fromImage(preview));
	thumb->setFixedSize(preview.width(), preview.height());
	thumb->setStyleSheet("background: #222; border: 1px solid #444;");
	h->addWidget(thumb, 0);

	// Right-hand column: summary label + controls in a vertical
	// stack so text never overlaps the combobox.
	auto* col = new QVBoxLayout();
	col->setSpacing(4);

	const Row& r = rows_[row_idx];
	auto* summary = new QLabel(
		QString("row %1 -> main_%2.anim   atlas %3x%4   frames %5")
			.arg(r.sc_r_row, 3)
			.arg(r.anim_num, 3, 10, QChar('0'))
			.arg(r.atlas_w).arg(r.atlas_h)
			.arg(r.frame_count));
	summary->setStyleSheet("color: #556; font-family: monospace;");
	col->addWidget(summary);

	// Controls row: combo + comment + confirm.
	auto* ctrl = new QHBoxLayout();
	ctrl->setSpacing(6);

	auto* combo = new QComboBox();
	combo->setEditable(true);
	combo->setInsertPolicy(QComboBox::NoInsert);
	for (const auto& n : unit_names_) combo->addItem(n);
	combo->setCurrentIndex(0);
	// Enable completer over the dropdown items so the user can
	// type a substring; Qt's default completer matches from the
	// start of each item, which we widen to "contains" by using
	// MatchContains.
	if (auto* cmp = combo->completer()) {
		cmp->setCaseSensitivity(Qt::CaseInsensitive);
		cmp->setFilterMode(Qt::MatchContains);
	}
	combo->setSizePolicy(QSizePolicy::Expanding,
	                     QSizePolicy::Preferred);
	combo->setMinimumWidth(260);
	ctrl->addWidget(combo, 3);

	auto* comment = new QLineEdit();
	comment->setPlaceholderText("comment…");
	comment->setSizePolicy(QSizePolicy::Expanding,
	                       QSizePolicy::Preferred);
	comment->setMinimumWidth(180);
	ctrl->addWidget(comment, 2);

	auto* confirm_btn = new QPushButton("Confirm");
	confirm_btn->setFixedWidth(90);
	ctrl->addWidget(confirm_btn, 0);

	col->addLayout(ctrl);
	h->addLayout(col, 1);

	// SD reference preview on the far right. Renders whichever
	// image the combobox currently points at, letting the user
	// eyeball SD vs HD side by side. Blank until a name is picked.
	auto* sd_preview = new QLabel();
	sd_preview->setFixedSize(128, 128);
	sd_preview->setAlignment(Qt::AlignCenter);
	sd_preview->setStyleSheet(
		"background: #222; border: 1px solid #444; color: #556;");
	sd_preview->setText("SD");
	h->addWidget(sd_preview, 0);

	// Track widgets so we can flip them after confirm and rebuild
	// dropdowns on refresh_used_names.
	RowWidgets rws;
	rws.combo       = combo;
	rws.comment     = comment;
	rws.confirm_btn = confirm_btn;
	rws.summary     = summary;
	rws.sd_preview  = sd_preview;
	row_ws_.push_back(rws);

	// Signals.
	connect(confirm_btn, &QPushButton::clicked, this,
		[this, row_idx]() {
			// A single button toggles Confirm <-> Unconfirm to
			// keep the UI compact -- state lives in rows_[k].confirmed
			// and we branch on it.
			if (rows_[row_idx].confirmed) on_unconfirm_clicked(row_idx);
			else                          on_confirm_clicked(row_idx);
		});

	connect(comment, &QLineEdit::textEdited, this,
		[this, row_idx](const QString& t) {
			on_comment_edited(row_idx, t);
		});

	// SD preview updates every time the combobox text changes,
	// whether via dropdown selection or type-to-search text edit.
	// currentTextChanged fires for both.
	connect(combo, &QComboBox::currentTextChanged, this,
		[this, row_idx](const QString&) {
			refresh_sd_preview(row_idx);
		});

	return w;
}

// ---- Confirm / Unconfirm --------------------------------------------

void MappingTab::on_confirm_clicked(int row_idx) {
	if (row_idx < 0 || row_idx >= (int)rows_.size()) return;
	auto& r = rows_[row_idx];
	auto& ws = row_ws_[row_idx];

	QString picked = ws.combo->currentText().trimmed();
	if (picked.isEmpty() || picked == kUnassigned) {
		// Nothing selected -- keep the button as-is.
		ws.summary->setStyleSheet(
			"color: #c60; font-family: monospace;");
		ws.summary->setText(ws.summary->text() +
			QStringLiteral("   (pick a name first)"));
		return;
	}
	// Reject if some OTHER confirmed row already took this name.
	auto uk = picked.toStdString();
	if (used_names_.count(uk)) {
		QMessageBox::information(this, "Already used",
			QString("The name '%1' is already confirmed on "
			        "another row. Unconfirm it there first.")
				.arg(picked));
		return;
	}

	r.picked_name = picked;
	r.comment     = ws.comment->text();
	r.confirmed   = true;
	used_names_.insert(uk);

	// Lock the row's inputs and flip the button.
	ws.combo->setEnabled(false);
	ws.comment->setEnabled(false);
	ws.confirm_btn->setText("Unconfirm");
	ws.summary->setStyleSheet(
		"color: #063; font-family: monospace; font-weight: bold;");

	// Rebuild every OTHER unconfirmed row's dropdown so the
	// just-picked name disappears from them.
	refresh_used_names();
	autosave_now();
}

void MappingTab::on_unconfirm_clicked(int row_idx) {
	if (row_idx < 0 || row_idx >= (int)rows_.size()) return;
	auto& r = rows_[row_idx];
	auto& ws = row_ws_[row_idx];

	if (!r.picked_name.isEmpty()) {
		used_names_.erase(r.picked_name.toStdString());
	}
	r.confirmed = false;
	// Keep picked_name so the combobox retains its selection;
	// only clear it if the user explicitly changes the combo.

	ws.combo->setEnabled(true);
	ws.comment->setEnabled(true);
	ws.confirm_btn->setText("Confirm");
	ws.summary->setStyleSheet("color: #556; font-family: monospace;");

	refresh_used_names();
	autosave_now();
}

void MappingTab::on_comment_edited(int row_idx, const QString& text) {
	if (row_idx < 0 || row_idx >= (int)rows_.size()) return;
	rows_[row_idx].comment = text;
	// Debounce lightly by autosaving on every keystroke -- the
	// file is small (~50 KB even fully mapped) so the write cost
	// is negligible compared to the safety of never losing edits.
	autosave_now();
}

// ---- SD preview ----------------------------------------------------

// Mirror of tools/sd_sprite_dump.cpp::safe_name(). Keep in sync!
// Any character that isn't [A-Za-z0-9_-] becomes '_', so
// "terran\\marine.grp" -> "terran_marine_grp".
static QString sd_safe_name(const QString& s) {
	QString out;
	out.reserve(s.size());
	for (QChar ch : s) {
		char c = ch.toLatin1();
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
		    || (c >= '0' && c <= '9') || c == '_' || c == '-')
			out.append(ch);
		else
			out.append('_');
	}
	return out;
}

void MappingTab::refresh_sd_preview(int row_idx) {
	if (row_idx < 0 || row_idx >= (int)row_ws_.size()) return;
	auto* label = row_ws_[row_idx].sd_preview;
	if (!label) return;

	// Unassigned / no SD dir -> blank the label back to placeholder.
	auto blank = [&](const QString& msg) {
		label->setPixmap(QPixmap());
		label->setText(msg);
	};
	if (sd_previews_dir_.isEmpty()) { blank("no SD dir"); return; }

	QString pick = row_ws_[row_idx].combo->currentText().trimmed();
	if (pick.isEmpty() || pick == kUnassigned) { blank("SD"); return; }

	// Find the image_id: linear scan of images_tbl_by_id_. 929
	// entries * ~230 rows = 214k comparisons total per full refresh,
	// but a single row costs one scan (~929 compares) -- fine.
	int image_id = -1;
	for (int i = 0; i < (int)images_tbl_by_id_.size(); ++i) {
		if (images_tbl_by_id_[i] == pick) { image_id = i; break; }
	}
	if (image_id < 0) { blank("no SD"); return; }

	// tools/sd_sprite_dump names files "<3d image_id>_<safe>.png".
	QString path = QString("%1/%2_%3.png")
		.arg(sd_previews_dir_)
		.arg(image_id, 3, 10, QChar('0'))
		.arg(sd_safe_name(pick));
	QPixmap pm(path);
	if (pm.isNull()) { blank("no SD"); return; }
	// SD canvases are 256x256 by default; scale down to 128x128
	// with smooth transform to keep pixels legible.
	label->setPixmap(pm.scaled(128, 128,
		Qt::KeepAspectRatio, Qt::SmoothTransformation));
	label->setText("");
}

// ---- refresh_used_names ---------------------------------------------

void MappingTab::refresh_used_names() {
	// Rebuild each unconfirmed row's dropdown to hide any name
	// currently locked by a confirmed row. Confirmed rows keep
	// their own combobox contents intact (they're disabled anyway).
	for (int i = 0; i < (int)rows_.size(); ++i) {
		if (rows_[i].confirmed) continue;
		auto* combo = row_ws_[i].combo;
		if (!combo) continue;
		// Preserve the user's typed text if any, so a mid-edit
		// combobox doesn't reset on every confirm elsewhere.
		QString cur = combo->currentText();
		combo->blockSignals(true);
		combo->clear();
		for (const auto& n : unit_names_) {
			if (n != kUnassigned && used_names_.count(n.toStdString()))
				continue;
			combo->addItem(n);
		}
		int idx = combo->findText(cur, Qt::MatchExactly);
		if (idx >= 0) combo->setCurrentIndex(idx);
		else combo->setEditText(cur);
		combo->blockSignals(false);
	}
}

// ---- Save / Load ----------------------------------------------------

void MappingTab::on_save_as_clicked() {
	QString path = QFileDialog::getSaveFileName(
		this, "Save HD mapping (as)",
		autosave_path_.isEmpty()
			? QStringLiteral("hd_mapping.json")
			: autosave_path_,
		QStringLiteral("JSON (*.json)"));
	if (path.isEmpty()) return;
	write_json_to(path);
	status_->setText(QString("Saved to %1").arg(path));
}

void MappingTab::on_load_clicked() {
	QString path = QFileDialog::getOpenFileName(
		this, "Load HD mapping",
		autosave_path_.isEmpty() ? QString() : autosave_path_,
		QStringLiteral("JSON (*.json)"));
	if (path.isEmpty()) return;
	int n = load_json_from(path);
	refresh_used_names();
	status_->setText(
		QString("Loaded %1 rows from %2").arg(n).arg(path));
}

void MappingTab::autosave_now() {
	if (autosave_path_.isEmpty()) return;
	write_json_to(autosave_path_);
}

void MappingTab::write_json_to(const QString& path) {
	QJsonArray mapped, unmapped;
	for (const auto& r : rows_) {
		if (!r.confirmed) {
			QJsonObject o;
			o["sc_r_row"] = r.sc_r_row;
			o["anim_num"] = (qint64)r.anim_num;
			// Preserve any in-progress combo pick / comment so
			// closing the app mid-session doesn't lose the
			// half-typed guesses.
			if (!r.picked_name.isEmpty()) o["picked_name"] = r.picked_name;
			if (!r.comment.isEmpty())     o["comment"]     = r.comment;
			unmapped.append(o);
			continue;
		}
		QJsonObject o;
		o["unit_name"] = r.picked_name;
		o["sc_r_row"]  = r.sc_r_row;
		o["anim_num"]  = (qint64)r.anim_num;
		if (!r.comment.isEmpty()) o["comment"] = r.comment;
		mapped.append(o);
	}
	QJsonObject root;
	root["mapped"]   = mapped;
	root["unmapped"] = unmapped;

	QFile f(path);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
	f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
	f.close();
}

int MappingTab::load_json_from(const QString& path) {
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly)) return 0;
	auto doc = QJsonDocument::fromJson(f.readAll());
	f.close();
	if (!doc.isObject()) return 0;
	auto root = doc.object();
	int n = 0;

	auto apply_row = [&](const QJsonObject& o, bool confirmed) {
		int r = o.value("sc_r_row").toInt(-1);
		if (r < 0) return;
		for (size_t i = 0; i < rows_.size(); ++i) {
			if (rows_[i].sc_r_row != r) continue;
			auto& row  = rows_[i];
			auto& ws   = row_ws_[i];
			QString name = o.value(
				confirmed ? "unit_name" : "picked_name").toString();
			QString comment = o.value("comment").toString();
			row.picked_name = name;
			row.comment     = comment;
			row.confirmed   = false;
			// Update UI first (so combobox has the item to select).
			if (!name.isEmpty()) {
				int idx = ws.combo->findText(name, Qt::MatchExactly);
				if (idx >= 0) ws.combo->setCurrentIndex(idx);
				else          ws.combo->setEditText(name);
			}
			ws.comment->setText(comment);
			if (confirmed && !name.isEmpty()) {
				// Re-run the same path as a fresh Confirm click so
				// used_names_/button-state stay in sync.
				used_names_.insert(name.toStdString());
				row.confirmed = true;
				ws.combo->setEnabled(false);
				ws.comment->setEnabled(false);
				ws.confirm_btn->setText("Unconfirm");
				ws.summary->setStyleSheet(
					"color: #063; font-family: monospace; "
					"font-weight: bold;");
			}
			++n;
			break;
		}
	};

	for (const auto& v : root.value("mapped").toArray()) {
		if (v.isObject()) apply_row(v.toObject(), true);
	}
	for (const auto& v : root.value("unmapped").toArray()) {
		if (v.isObject()) apply_row(v.toObject(), false);
	}
	return n;
}

}   // namespace sprite_viewer
