#include "attach_agent_dialog.h"

#include "../agent_catalog.h"

#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

namespace simsc_desktop {

AttachAgentDialog::AttachAgentDialog(AgentCatalog* catalog,
                                     const QString& slot_alias,
                                     const QString& slot_race,
                                     QWidget* parent)
	: QDialog(parent), catalog_(catalog) {
	setWindowTitle(tr("Attach agent"));
	setModal(true);

	// Rescan on open so freshly-dropped agent files show up without
	// requiring the user to reopen the whole app.
	catalog_->rescan();

	auto* root = new QVBoxLayout(this);

	auto* form = new QFormLayout;
	auto* slot_label = new QLabel(
		tr("%1 (%2)").arg(slot_alias, slot_race), this);
	slot_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
	form->addRow(tr("Slot:"), slot_label);

	combo_ = new QComboBox(this);
	combo_->setModel(catalog_);
	form->addRow(tr("Agent:"), combo_);
	root->addLayout(form);

	// Show an inline hint if the catalog is empty so the user knows
	// what to do rather than staring at an empty combobox and a
	// disabled OK button.
	if (catalog_->rowCount() == 0) {
		empty_hint_ = new QLabel(
			tr("No agents found. Set the Agents directory in Settings "
			   "and drop executable wrapper files there. See "
			   "simsc_agents/README.md for the wrapper convention."),
			this);
		empty_hint_->setWordWrap(true);
		empty_hint_->setEnabled(false);
		root->addWidget(empty_hint_);
	}

	buttons_ = new QDialogButtonBox(
		QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	buttons_->button(QDialogButtonBox::Ok)
	        ->setEnabled(catalog_->rowCount() > 0);
	connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
	root->addWidget(buttons_);
}

QString AttachAgentDialog::picked_path() const {
	const int row = combo_ ? combo_->currentIndex() : -1;
	if (!catalog_ || row < 0 || row >= catalog_->rowCount()) return {};
	return catalog_->at(row).path;
}

QString AttachAgentDialog::picked_display_name() const {
	const int row = combo_ ? combo_->currentIndex() : -1;
	if (!catalog_ || row < 0 || row >= catalog_->rowCount()) return {};
	return catalog_->at(row).display_name;
}

}   // namespace simsc_desktop
