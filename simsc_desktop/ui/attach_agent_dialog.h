// AttachAgentDialog — picker for launching an agent on a slot.
//
// Modal opened from the per-slot "Attach agent" button in either the
// Local Games or Remote Games tab. Renders a QComboBox of the agents
// AgentCatalog can see, a read-only line showing the slot's resolved
// race, and OK / Cancel. On accept, the caller reads picked_path().
//
// Nothing here talks to AgentSupervisor directly: the tab code that
// opened the dialog is in a better position to compute the WS URL +
// api-key + race, and it's clearer to keep the "spawn a process"
// side-effect at the callsite rather than smuggling it through a
// dialog.

#pragma once

#include <QtWidgets/QDialog>

class QComboBox;
class QDialogButtonBox;
class QLabel;

namespace simsc_desktop {

class AgentCatalog;

class AttachAgentDialog : public QDialog {
	Q_OBJECT
public:
	// slot_race: the resolved race for the slot ("zerg"/"terran"/
	// "protoss"). Shown to the user for confirmation; the agent
	// receives it via --race.
	// slot_alias: the alias whose slot is being attached to. Shown
	// so the user isn't confused about which row's button they hit.
	AttachAgentDialog(AgentCatalog* catalog,
	                  const QString& slot_alias,
	                  const QString& slot_race,
	                  QWidget* parent = nullptr);

	// Absolute path of the picked agent executable, or empty on
	// Cancel / when catalog is empty.
	QString picked_path() const;
	// Display name (for status messages and AgentSupervisor's
	// attached_at() bookkeeping).
	QString picked_display_name() const;

private:
	AgentCatalog*     catalog_ = nullptr;
	QComboBox*        combo_   = nullptr;
	QLabel*           empty_hint_ = nullptr;
	QDialogButtonBox* buttons_ = nullptr;
};

}   // namespace simsc_desktop
