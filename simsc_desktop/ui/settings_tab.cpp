// See settings_tab.h.

#include "settings_tab.h"

#include "../local_user_roster.h"
#include "../settings.h"

#include <QtCore/QSignalBlocker>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTableView>
#include <QtWidgets/QVBoxLayout>

namespace simsc_desktop {

namespace {

// Named BW speeds -> ms/frame. Matches the mapping in
// server/main.cpp; the two must stay in sync. "custom" is a
// sentinel row that appears at the bottom of the combobox only
// when the persisted ms value doesn't match a named speed --
// picking it is a no-op (the combobox falls back to the current
// value); actual custom values must be set via a preexisting
// QSettings entry (e.g. `defaults write`).
struct SpeedOpt { const char* label; int ms; };
constexpr SpeedOpt kSpeeds[] = {
	{"turbosuper (10 ms)", 10},
	{"superfast (20 ms)",  20},
	{"fastest (42 ms)",    42},
	{"faster (56 ms)",     56},
	{"fast (67 ms)",       67},
	{"normal (83 ms)",     83},
	{"slow (111 ms)",     111},
	{"slower (167 ms)",   167},
	{"slowest (250 ms)",  250},
};

}   // namespace

SettingsTab::SettingsTab(Settings* settings, LocalUserRoster* roster,
                         QWidget* parent)
	: QWidget(parent), settings_(settings), roster_(roster) {

	auto* root = new QVBoxLayout(this);

	// --- Paths & endpoints ----------------------------------------
	auto* paths_box    = new QGroupBox(tr("Paths and endpoints"), this);
	auto* paths_layout = new QFormLayout(paths_box);

	sc1_data_path_edit_          = new QLineEdit(paths_box);
	simsc_api_key_edit_          = new QLineEdit(paths_box);
	simsc_api_key_edit_->setEchoMode(QLineEdit::PasswordEchoOnEdit);
	simsc_base_url_edit_         = new QLineEdit(paths_box);
	default_local_port_spin_     = new QSpinBox(paths_box);
	default_local_port_spin_->setRange(1024, 65535);
	default_game_speed_combo_    = new QComboBox(paths_box);
	for (const auto& opt : kSpeeds) {
		default_game_speed_combo_->addItem(
			QString::fromLatin1(opt.label), opt.ms);
	}
	server_binary_override_edit_ = new QLineEdit(paths_box);

	auto add_row_with_browse = [&](const QString& label, QLineEdit* edit,
	                               void (SettingsTab::*slot)()) {
		auto* row  = new QHBoxLayout;
		auto* btn  = new QPushButton(tr("Browse..."), paths_box);
		row->addWidget(edit, 1);
		row->addWidget(btn);
		connect(btn, &QPushButton::clicked, this, slot);
		paths_layout->addRow(label, row);
	};

	add_row_with_browse(tr("SC1 data path"), sc1_data_path_edit_,
	                    &SettingsTab::onBrowseSc1DataPath);
	paths_layout->addRow(tr("simsc API key"),  simsc_api_key_edit_);
	paths_layout->addRow(tr("simsc base URL"), simsc_base_url_edit_);
	paths_layout->addRow(tr("Default local port"), default_local_port_spin_);
	paths_layout->addRow(tr("Default game speed"), default_game_speed_combo_);
	add_row_with_browse(tr("openbw_server override"),
	                    server_binary_override_edit_,
	                    &SettingsTab::onBrowseServerBinaryOverride);

	// Push back into Settings on edit.
	connect(sc1_data_path_edit_, &QLineEdit::editingFinished, this, [this] {
		settings_->set_sc1_data_path(sc1_data_path_edit_->text());
	});
	connect(simsc_api_key_edit_, &QLineEdit::editingFinished, this, [this] {
		settings_->set_simsc_api_key(simsc_api_key_edit_->text());
	});
	connect(simsc_base_url_edit_, &QLineEdit::editingFinished, this, [this] {
		settings_->set_simsc_base_url(simsc_base_url_edit_->text());
	});
	connect(default_local_port_spin_,
		QOverload<int>::of(&QSpinBox::valueChanged),
		this, [this](int v) { settings_->set_default_local_port(v); });
	connect(default_game_speed_combo_,
		QOverload<int>::of(&QComboBox::currentIndexChanged),
		this, [this](int idx) {
			const auto v = default_game_speed_combo_->itemData(idx);
			if (v.isValid()) {
				settings_->set_default_game_speed_ms(v.toInt());
			}
		});
	connect(server_binary_override_edit_, &QLineEdit::editingFinished,
		this, [this] {
			settings_->set_server_binary_override(
				server_binary_override_edit_->text());
		});

	// --- Local user roster ----------------------------------------
	auto* roster_box    = new QGroupBox(tr("Local users"), this);
	auto* roster_layout = new QVBoxLayout(roster_box);

	roster_view_ = new QTableView(roster_box);
	roster_view_->setModel(roster_);
	roster_view_->horizontalHeader()->setStretchLastSection(true);
	roster_view_->verticalHeader()->setVisible(false);
	roster_view_->setSelectionBehavior(QAbstractItemView::SelectRows);
	roster_view_->setSelectionMode(QAbstractItemView::SingleSelection);

	auto* roster_btns = new QHBoxLayout;
	auto* add_btn     = new QPushButton(tr("Add..."), roster_box);
	remove_user_btn_  = new QPushButton(tr("Remove"), roster_box);
	remove_user_btn_->setEnabled(false);
	roster_btns->addWidget(add_btn);
	roster_btns->addWidget(remove_user_btn_);
	roster_btns->addStretch();

	roster_layout->addWidget(roster_view_, 1);
	roster_layout->addLayout(roster_btns);

	// The roster auto-persists on every add/remove/edit, so no
	// explicit save button is needed.
	connect(add_btn,          &QPushButton::clicked, this, &SettingsTab::onAddUser);
	connect(remove_user_btn_, &QPushButton::clicked, this, &SettingsTab::onRemoveUser);
	connect(roster_view_->selectionModel(),
		&QItemSelectionModel::selectionChanged, this, [this] {
			remove_user_btn_->setEnabled(
				roster_view_->selectionModel()->hasSelection());
		});

	root->addWidget(paths_box);
	root->addWidget(roster_box, 1);

	// Reflect Settings back into the widgets whenever they change
	// externally (or on our own initial load).
	connect(settings_, &Settings::changed,
		this, &SettingsTab::pushCurrentValuesToUi);
	pushCurrentValuesToUi();
}

void SettingsTab::pushCurrentValuesToUi() {
	// Block signals so we don't ping-pong through editingFinished
	// while writing programmatic values.
	QSignalBlocker b1(sc1_data_path_edit_);
	QSignalBlocker b2(simsc_api_key_edit_);
	QSignalBlocker b3(simsc_base_url_edit_);
	QSignalBlocker b4(default_local_port_spin_);
	QSignalBlocker b5(server_binary_override_edit_);
	QSignalBlocker b6(default_game_speed_combo_);
	sc1_data_path_edit_->setText(settings_->sc1_data_path());
	simsc_api_key_edit_->setText(settings_->simsc_api_key());
	simsc_base_url_edit_->setText(settings_->simsc_base_url());
	default_local_port_spin_->setValue(settings_->default_local_port());
	server_binary_override_edit_->setText(settings_->server_binary_override());

	const int cur_ms = settings_->default_game_speed_ms();
	int idx = default_game_speed_combo_->findData(cur_ms);
	if (idx < 0) {
		// Persisted value doesn't map to any named preset. Append
		// a synthetic "custom" row so we can still show it, and
		// select it without clobbering the value.
		default_game_speed_combo_->addItem(
			tr("custom (%1 ms)").arg(cur_ms), cur_ms);
		idx = default_game_speed_combo_->count() - 1;
	}
	default_game_speed_combo_->setCurrentIndex(idx);
}

void SettingsTab::onBrowseSc1DataPath() {
	const auto d = QFileDialog::getExistingDirectory(this,
		tr("Select SC1 data directory (with StarDat.mpq, BrooDat.mpq, ...)"),
		sc1_data_path_edit_->text());
	if (d.isEmpty()) return;
	sc1_data_path_edit_->setText(d);
	settings_->set_sc1_data_path(d);
}

void SettingsTab::onBrowseServerBinaryOverride() {
	const auto f = QFileDialog::getOpenFileName(this,
		tr("Select openbw_server binary"),
		server_binary_override_edit_->text());
	if (f.isEmpty()) return;
	server_binary_override_edit_->setText(f);
	settings_->set_server_binary_override(f);
}

void SettingsTab::onAddUser() {
	// Local api keys are shared secrets between the agent + local
	// server; nothing on this machine has to remember or type them.
	// Auto-generate at add time so the user only picks the alias.
	bool ok = false;
	const auto alias = QInputDialog::getText(this,
		tr("Add local user"), tr("Alias:"),
		QLineEdit::Normal, {}, &ok);
	if (!ok || alias.isEmpty()) return;
	roster_->addUser(alias, LocalUserRoster::generateApiKey());
}

void SettingsTab::onRemoveUser() {
	const auto sel = roster_view_->selectionModel();
	if (!sel->hasSelection()) return;
	roster_->removeUserAt(sel->currentIndex().row());
}

}   // namespace simsc_desktop
