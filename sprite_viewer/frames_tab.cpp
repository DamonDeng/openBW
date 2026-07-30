// See frames_tab.h.

#include "frames_tab.h"

#include "hd_asset_loader.h"

#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtGui/QPixmap>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QVBoxLayout>

#include <cstdio>
#include <cstdlib>

namespace sprite_viewer {

namespace {

// Fixed thumbnail cell size. Frames vary from ~30px to ~200px on
// a side; letterbox into this uniform box so the grid stays
// tidy. Includes room for a caption below.
constexpr int kThumb = 160;
constexpr int kCaption = 32;

// Draw one frame's atlas sub-rect onto a fixed-size preview tile
// (uniform kThumb x kThumb + caption band). Centered, aspect
// preserved, black background.
QImage make_thumb(const QImage& atlas, const HdFrame& fr) {
	QImage cell(kThumb, kThumb + kCaption, QImage::Format_RGBA8888);
	cell.fill(0x00000000);
	if (fr.w == 0 || fr.h == 0) return cell;
	QImage sub = atlas.copy(fr.atlas_x, fr.atlas_y, fr.w, fr.h);
	int W = kThumb, H = kThumb;
	double scale = std::min((double)W / fr.w, (double)H / fr.h);
	int sw = std::max(1, (int)(fr.w * scale));
	int sh = std::max(1, (int)(fr.h * scale));
	QImage scaled = sub.scaled(sw, sh,
		Qt::KeepAspectRatio, Qt::SmoothTransformation);
	QPainter p(&cell);
	p.fillRect(0, 0, kThumb, kThumb, QColor(0x20, 0x20, 0x20));
	p.drawImage((W - sw) / 2, (H - sh) / 2, scaled);
	return cell;
}

}   // namespace

FramesTab::FramesTab(HdAssetLoader* loader, QWidget* parent)
	: QWidget(parent), loader_(loader)
{
	auto* root = new QVBoxLayout(this);

	// Top: two inputs so we can either request by anim_num
	// directly, or by image_id (which routes through images.rel
	// exactly like the render path does). image_id is what the
	// sim gives us, so it's the most useful for cross-checking
	// what the shadow branch is actually seeing.
	auto* top = new QHBoxLayout();
	top->addWidget(new QLabel("image_id:"));
	image_id_edit_ = new QLineEdit();
	image_id_edit_->setFixedWidth(80);
	top->addWidget(image_id_edit_);
	top->addSpacing(20);
	top->addWidget(new QLabel("or anim:"));
	anim_edit_ = new QLineEdit();
	anim_edit_->setFixedWidth(80);
	top->addWidget(anim_edit_);
	auto* load_btn = new QPushButton("Load");
	top->addWidget(load_btn);
	top->addSpacing(20);
	top->addWidget(new QLabel("layer:"));
	layer_combo_ = new QComboBox();
	layer_combo_->setFixedWidth(140);
	top->addWidget(layer_combo_);
	top->addStretch(1);
	root->addLayout(top);

	header_ = new QLabel(loader_
		? QStringLiteral("Enter an image_id (routes through images.rel) "
		                 "or an anim number (opens anim\\main_<N>.anim "
		                 "directly, bypasses images.rel), then Load.")
		: QStringLiteral("Frames browser is HD-only. "
		                 "Launch with --sc-version=remastered."));
	header_->setWordWrap(true);
	root->addWidget(header_);

	scroll_ = new QScrollArea();
	scroll_->setWidgetResizable(true);
	root->addWidget(scroll_, /*stretch*/1);

	grid_container_ = new QWidget();
	scroll_->setWidget(grid_container_);

	connect(load_btn, &QPushButton::clicked, this, &FramesTab::reload);
	connect(image_id_edit_, &QLineEdit::returnPressed,
		this, &FramesTab::reload);
	connect(anim_edit_, &QLineEdit::returnPressed,
		this, &FramesTab::reload);
	// Layer change: re-render without re-parsing the anim. reload()
	// happens to do a full re-parse currently, which is cheap enough
	// (a few ms). If it ever gets slow, cache the parsed anim per
	// image_id and re-decode only the selected layer.
	connect(layer_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
		this, [this](int) { reload(); });
}

void FramesTab::set_current_image_id(int image_id) {
	if (image_id < 0) return;
	image_id_edit_->setText(QString::number(image_id));
	// Clear anim override so the image_id path wins.
	anim_edit_->clear();
	reload();
}

void FramesTab::reload() {
	if (!loader_ || !loader_->is_open()) {
		header_->setText("HD loader not open.");
		return;
	}

	// Two input modes:
	//   * image_id set -> route through images.rel like the render
	//     path does. Fails on rows without a flag=8/4 anim.
	//   * anim set (image_id blank) -> open anim\main_<N>.anim
	//     directly, bypassing images.rel entirely. Lets us inspect
	//     orphan / SD-only / Carbot anims (e.g. anim 249, which no
	//     images.rel row points at with an HD flag but which does
	//     exist on disk).
	// If both are set, image_id wins (keeps the existing behavior
	// of "type an image_id, hit Enter, see that unit's frames").
	bool have_iid = false, have_anim = false;
	int image_id = image_id_edit_->text().toInt(&have_iid);
	int anim = anim_edit_->text().toInt(&have_anim);
	bool by_anim = (!have_iid && have_anim);
	// Auto-fallback: if the caller filled only image_id but that
	// row has no HD anim (flag=1 SD-only, flag=16 Carbot, ...), we'd
	// end up returning an empty layer list and 0 frames. Try treating
	// the same number as an anim directly. That's usually what the
	// user meant when they typed "249" into the image_id box: they
	// want to see anim\main_249.anim, which just happens to also
	// be the SD-only row 249 in images.rel.
	if (have_iid && !have_anim) {
		bool row_has_hd = false;
		if (loader_ && image_id >= 0) {
			// list_layers_for_image_id() returns empty when the
			// row has no HD anim, so use it as the probe. Cheap
			// (no atlas decode).
			auto probe = loader_->list_layers_for_image_id(image_id);
			row_has_hd = !probe.empty();
		}
		if (!row_has_hd) {
			by_anim  = true;
			anim     = image_id;
			have_anim = true;
		}
	}
	if (!have_iid && !have_anim) {
		header_->setText("Enter an image_id or anim number first.");
		return;
	}

	// Key for the current selection. image_id path stays keyed on
	// the image_id (positive); anim path uses a negative key
	// (-anim - 1) so it can't collide.
	int key = by_anim ? (-(anim) - 1) : image_id;

	// (Re)populate the layer combo when the selection changes.
	if (key != current_image_id_) {
		current_image_id_ = key;
		layer_combo_->blockSignals(true);
		layer_combo_->clear();
		auto layers = by_anim
			? loader_->list_layers_for_anim((uint32_t)anim)
			: loader_->list_layers_for_image_id(image_id);
		for (const auto& n : layers) {
			layer_combo_->addItem(QString::fromStdString(n));
		}
		int diff_idx = layer_combo_->findText("diffuse");
		if (diff_idx >= 0) layer_combo_->setCurrentIndex(diff_idx);
		layer_combo_->blockSignals(false);
	}

	// Pick the layer to load. Both paths honor the selected layer
	// so the user can flip between diffuse / bright / teamcolor /
	// etc without changing the id / anim inputs.
	std::unique_ptr<HdSprite> sp;
	const QString layer_qs = layer_combo_->currentText();
	std::string layer_std = layer_qs.isEmpty()
		? std::string("diffuse") : layer_qs.toStdString();
	if (by_anim) {
		sp = loader_->load_sprite_layer_by_anim(
			(uint32_t)anim, layer_std);
	} else if (!layer_qs.isEmpty()) {
		sp = loader_->load_sprite_layer(image_id, layer_std);
	} else {
		sp = loader_->load_sprite(image_id);
	}
	if (!sp) {
		header_->setText(
			QStringLiteral("load(%1=%2, layer=%3) failed: %4")
				.arg(by_anim ? QStringLiteral("anim")
				             : QStringLiteral("image_id"))
				.arg(by_anim ? anim : image_id)
				.arg(QString::fromStdString(layer_std))
				.arg(loader_->last_error()));
		return;
	}

	// Rebuild the grid.
	if (grid_container_->layout()) {
		QLayoutItem* item;
		while ((item = grid_container_->layout()->takeAt(0)) != nullptr) {
			if (auto* w = item->widget()) w->deleteLater();
			delete item;
		}
		delete grid_container_->layout();
	}
	auto* grid = new QGridLayout(grid_container_);
	grid->setSpacing(6);

	// Choose the best-available atlas: composited (diffuse +
	// teamcolor) if present, else raw diffuse. This is the same
	// choice paint_hd_image() makes, so what we see here matches
	// what would go on screen.
	const QImage& atlas = sp->composited.isNull()
		? sp->diffuse : sp->composited;

	// Header: show what was loaded and via which path. If the
	// image_id -> images.rel route was used, also surface the
	// underlying anim_num so it's obvious that (say) image_id=248
	// actually opens anim\main_247.anim (rows share anim files).
	QString src_desc;
	if (by_anim) {
		src_desc = QStringLiteral("anim=%1 (direct)").arg(anim);
	} else {
		int resolved_anim = -1;
		if (loader_) {
			// Small helper: peek images.rel via list-layers path
			// isn't quite what we want; walk to images_rel via
			// image_id_for_anim inverse -- but there's no direct
			// getter. Show the header text without it if we can't
			// cheaply resolve it here; the user still sees the
			// image_id they typed.
			(void)resolved_anim;
		}
		src_desc = QStringLiteral("image_id=%1").arg(image_id);
	}
	header_->setText(
		QStringLiteral(
			"%1  layer=%2  frames=%3  sprite=%4x%5  atlas=%6x%7")
		.arg(src_desc)
		.arg(layer_qs.isEmpty() ? QStringLiteral("diffuse") : layer_qs)
		.arg((int)sp->frames.size())
		.arg(sp->sprite_w).arg(sp->sprite_h)
		.arg(atlas.width()).arg(atlas.height()));

	const int cols = 6;
	for (int i = 0; i < (int)sp->frames.size(); ++i) {
		const auto& fr = sp->frames[i];
		QImage thumb = make_thumb(atlas, fr);
		// Draw caption directly into the thumb so it prints
		// inline (avoids nested widgets per cell).
		{
			QPainter p(&thumb);
			p.setPen(QColor(0xff, 0xff, 0xff));
			p.drawText(QRect(0, kThumb + 2, kThumb, kCaption - 2),
				Qt::AlignHCenter | Qt::AlignTop,
				QStringLiteral("#%1  %2x%3\noff=(%4,%5)")
					.arg(i).arg(fr.w).arg(fr.h)
					.arg((int)fr.offset_x).arg((int)fr.offset_y));
		}
		auto* lbl = new QLabel();
		lbl->setPixmap(QPixmap::fromImage(thumb));
		lbl->setFixedSize(kThumb, kThumb + kCaption);
		grid->addWidget(lbl, i / cols, i % cols);
	}
	grid->setRowStretch(((int)sp->frames.size() + cols - 1) / cols, 1);
}

}   // namespace sprite_viewer
