#include "remote_games_tab.h"

#include <QtWidgets/QLabel>
#include <QtWidgets/QVBoxLayout>

namespace simsc_desktop {

RemoteGamesTab::RemoteGamesTab(QWidget* parent) : QWidget(parent) {
	auto* root  = new QVBoxLayout(this);
	auto* label = new QLabel(
		tr("Remote games are coming in phase 1c.\n\n"
		   "Set your simsc API key in Settings first."),
		this);
	label->setAlignment(Qt::AlignCenter);
	label->setWordWrap(true);
	root->addStretch();
	root->addWidget(label);
	root->addStretch();
}

}   // namespace simsc_desktop
