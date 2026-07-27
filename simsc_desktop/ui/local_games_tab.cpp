#include "local_games_tab.h"

#include <QtWidgets/QLabel>
#include <QtWidgets/QVBoxLayout>

namespace simsc_desktop {

LocalGamesTab::LocalGamesTab(QWidget* parent) : QWidget(parent) {
	auto* root  = new QVBoxLayout(this);
	auto* label = new QLabel(
		tr("Local games are coming in phase 1b.\n\n"
		   "Add local users in Settings, then come back here."),
		this);
	label->setAlignment(Qt::AlignCenter);
	label->setWordWrap(true);
	root->addStretch();
	root->addWidget(label);
	root->addStretch();
}

}   // namespace simsc_desktop
