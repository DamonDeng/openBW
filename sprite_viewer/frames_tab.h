// frames_tab.h: Frames browser tab for sprite_viewer.
//
// Grid view of every frame in an HD anim's diffuse atlas so we
// can eyeball what actually lives inside e.g. anim_247 (SCV) --
// is the shadow baked into the body atlas? Bundled as a
// separate anim we haven't located? Rendered procedurally? The
// mapping-by-heuristic path has been dead-ending on units like
// SCV; a visual browser lets us make progress without more
// speculative code.
//
// Input: an anim number (main_<N>.anim). Loads via
// HdAssetLoader::load_sprite_for_anim (new). Displays each
// frame's sub-image at its atlas rect + a caption with frame_id,
// wh, offset. Optionally also dumps a manifest to disk for
// external tooling.
//
// Later: multi-layer (bright/teamcolor/normal/...), side-by-side
// with SD frames, side-by-side with the anim's shadow-anim
// candidates. For now: diffuse only, one anim at a time.

#pragma once

#include <QtCore/QString>
#include <QtWidgets/QWidget>

class QComboBox;
class QLineEdit;
class QLabel;
class QScrollArea;

namespace sprite_viewer {

class HdAssetLoader;

class FramesTab : public QWidget {
	Q_OBJECT
public:
	explicit FramesTab(HdAssetLoader* loader, QWidget* parent = nullptr);

	// Called by ViewerWindow when the user's selected unit changes.
	// Auto-loads the frames for that unit's body anim so the tab
	// stays useful without manual refresh.
	void set_current_image_id(int image_id);

private slots:
	void reload();

private:
	HdAssetLoader* loader_ = nullptr;
	QLineEdit*     anim_edit_ = nullptr;
	QLineEdit*     image_id_edit_ = nullptr;
	QComboBox*     layer_combo_ = nullptr;
	QLabel*        header_ = nullptr;
	QScrollArea*   scroll_ = nullptr;
	QWidget*       grid_container_ = nullptr;
	// Populated on each successful load so a layer switch doesn't
	// have to re-parse the anim header.
	int            current_image_id_ = -1;
};

}   // namespace sprite_viewer
