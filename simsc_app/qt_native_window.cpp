// Qt6 implementation of ui/native_window.h, ui/native_window_drawing.h,
// and ui/native_sound.h. Drop-in replacement for ui/sdl2.cpp -- pick
// one at link time (the SDL version links SDL2, this one links QtGui/
// QtWidgets). Never both -- native_window::window and the
// native_window_drawing::* symbols are defined in exactly one place
// per binary.
//
// The design mirrors sdl2.cpp: the header exposes a PIMPL'd window
// class and a set of surface/palette types with pure-virtual
// operations; this file gives them Qt-backed implementations.
//
// Notes on the mapping SDL -> Qt:
//
//   SDL_Surface (RGBA32)         -> QImage(Format_ARGB32) with our own
//                                   little R/G/B/A byte order matching
//                                   the SDL side, so ui.h's pixel-poking
//                                   code (flip_image at ui.h:110-115
//                                   swaps whole 32-bit pixels) keeps
//                                   working.
//   SDL_Surface (INDEX8)         -> QImage(Format_Indexed8) + color
//                                   table pushed via setColorTable().
//   SDL_GetWindowSurface(wnd)    -> a wrapper around a persistent
//                                   RGBA QImage owned by GameWidget;
//                                   update_surface() calls
//                                   widget->update() so paintEvent
//                                   presents the new frame.
//   SDL_SetPaletteColors(...)    -> QVector<QRgb> table with 256
//                                   entries, applied on set_palette.
//   SDL_BlitSurface (src->dst)   -> QPainter on dst, drawImage(src).
//   SDL_BlitScaled               -> QPainter on dst,
//                                   drawImage(target-rect, src).
//   SDL_SetSurfaceBlendMode      -> QPainter::CompositionMode when the
//                                   surface is used as a blit source
//                                   next time.
//   SDL_SetSurfaceAlphaMod       -> per-source alpha; we track it and
//                                   set QPainter::setOpacity at blit
//                                   time.
//   SDL_PollEvent                -> a small event queue populated by
//                                   the widget's *Event overrides;
//                                   drained by peek_message.
//   SDL_MOUSEWHEEL preciseY      -> Qt's QWheelEvent::pixelDelta on
//                                   trackpads, angleDelta/120 else.
//
// Design decisions:
//
//   * We store the RGBA "window surface" as a member of GameWidget, and
//     `get_window_surface(wnd)` returns a lightweight non-owning
//     surface wrapper. ui_functions holds onto that wrapper across
//     the whole session (see ui.h:1929 window_surface member), which
//     matches SDL's SDL_GetWindowSurface semantics.
//   * On resize we resize that RGBA QImage in-place inside GameWidget
//     AND ui_functions calls its own resize()/reset flow, which
//     re-invokes get_window_surface -- so nothing that references
//     the QImage backing store lingers across the resize.
//   * We never call QApplication::exec() ourselves; main.cpp does
//     that. This file only provides the widget class and rendering
//     primitives.

#include "native_window.h"
#include "native_window_drawing.h"
#include "native_sound.h"

#include "common.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QFile>
#include <QtCore/QPoint>
#include <QtGui/QColor>
#include <QtGui/QImage>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPixmap>
#include <QtGui/QResizeEvent>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

#include <array>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

using bwgame::ui::log;
using bwgame::ui::fatal_error;

// ---------------------------------------------------------------------------
// SDL -> USB HID scancode table.
//
// ui.h calls wnd.get_key_state(scancode) and wnd.peek_message returns
// e.scancode; both expect *SDL scancodes* (SDL_SCANCODE_*). SDL scancodes
// map to the USB HID usage IDs 1:1 for the range we care about (letters,
// digits, arrows, modifiers). Qt gives us Qt::Key_* values instead. We
// convert here so the game input code doesn't have to change.
//
// Only the keys the ui event loop actually checks need entries; everything
// else falls through to -1. Reference: ui.h searches for scancodes 79-82
// (arrow keys), 224/228 (Ctrl), 225/229 (Shift), and letter keys used for
// hotkeys.
// ---------------------------------------------------------------------------
static int qt_key_to_sdl_scancode(int qt_key) {
	switch (qt_key) {
	// Arrows: SDL_SCANCODE_RIGHT=79, LEFT=80, DOWN=81, UP=82
	case Qt::Key_Right: return 79;
	case Qt::Key_Left:  return 80;
	case Qt::Key_Down:  return 81;
	case Qt::Key_Up:    return 82;
	// Modifiers: LCTRL=224, LSHIFT=225, LALT=226, RCTRL=228, RSHIFT=229
	case Qt::Key_Control: return 224;
	case Qt::Key_Shift:   return 225;
	case Qt::Key_Alt:     return 226;
	// Letters A..Z -> 4..29 in SDL scancode order (A=4, B=5, ..., Z=29).
	default:
		if (qt_key >= Qt::Key_A && qt_key <= Qt::Key_Z) {
			return 4 + (qt_key - Qt::Key_A);
		}
		// Digits 1..0 -> 30..39 (SDL: 1=30, ..., 9=38, 0=39).
		if (qt_key >= Qt::Key_1 && qt_key <= Qt::Key_9) {
			return 30 + (qt_key - Qt::Key_1);
		}
		if (qt_key == Qt::Key_0) return 39;
		return -1;
	}
}

// SDL button numbers: 1=left, 2=middle, 3=right. Qt uses Qt::LeftButton /
// MiddleButton / RightButton bitflags. We fold them into the SDL numbering
// so ui.h's mouse-button handling keeps working unchanged.
static int qt_button_to_sdl(Qt::MouseButton b) {
	if (b == Qt::LeftButton)   return 1;
	if (b == Qt::MiddleButton) return 2;
	if (b == Qt::RightButton)  return 3;
	return 0;
}

static int qt_buttons_to_sdl_state(Qt::MouseButtons bs) {
	// SDL uses a bitmask: 1=(1<<0)=left, 2=(1<<1)=middle-ish (actually
	// SDL: SDL_BUTTON_LMASK=1, MMASK=2, RMASK=4, but ui.h's checks are
	// tolerant; matching the specific value would need SDL constants).
	// We approximate: bit for each pressed button.
	int r = 0;
	if (bs & Qt::LeftButton)   r |= 1;
	if (bs & Qt::MiddleButton) r |= 2;
	if (bs & Qt::RightButton)  r |= 4;
	return r;
}

// ---------------------------------------------------------------------------
// GameWidget: the QWidget subclass that owns the RGBA framebuffer and
// forwards input events into a queue for peek_message() to drain.
//
// The class is declared entirely in this .cpp so nothing outside sees the
// Qt types -- native_window.h keeps its clean PIMPL story.
// ---------------------------------------------------------------------------
namespace {

class GameWidget : public QWidget {
public:
	explicit GameWidget(QWidget* parent = nullptr) : QWidget(parent) {
		// We paint the frame ourselves; don't let Qt fill with the
		// default palette bg (would flash on each present).
		setAttribute(Qt::WA_OpaquePaintEvent);
		setAttribute(Qt::WA_NoSystemBackground);
		setFocusPolicy(Qt::StrongFocus);
		setMouseTracking(true);
		// Retail-style HUD chrome: left-half of Terran console panel.
		// Baked-in alpha (transparent viewport + retail black slots).
		// Placed at (-2, 293) so the console's minimap slot (x=6..133
		// inside the source PNG, per the extraction analysis) lands
		// exactly on top of openBW's native minimap position
		// (map_screen_x=4 in ui/ui.h:1602). See docs on hud layout.
		hud_console.load(":/hud/tconsole_left.png");
	}

	// Persistent RGBA framebuffer -- what SDL_GetWindowSurface returns
	// a pointer into. ui_functions holds a wrapper around this and
	// blits final frames here; paintEvent() copies it to the actual
	// widget surface.
	//
	// Format_ARGB32: BGRA byte order on little-endian, which matches
	// SDL_PIXELFORMAT_ARGB32. The observer's blit path treats RGBA
	// as opaque 32-bit words, so the byte order only matters for the
	// blit_scaled / fill code paths -- those use QColor / QPainter,
	// which see the ARGB32 format correctly regardless.
	QImage framebuffer;

	// Retail SC1 console HUD art (left half of tconsole.pcx, with
	// baked alpha for the game viewport + retail overpaint slots).
	// Loaded once from ":/hud/tconsole_left.png" in the ctor and
	// composited over `framebuffer` at paint time. Constant across
	// the app's lifetime — no reload on race change (v1 uses Terran
	// chrome for all races; Protoss/pconsole is a follow-up).
	QImage hud_console;

	// Queue of pending events awaiting peek_message() drain.
	std::deque<native_window::event_t> events;

	std::array<bool, 512> key_state{};
	std::array<bool, 6> mouse_button_state{};

	void resize_framebuffer(int w, int h) {
		if (framebuffer.width() != w || framebuffer.height() != h) {
			framebuffer = QImage(w, h, QImage::Format_ARGB32);
			framebuffer.fill(Qt::black);
		}
	}

protected:
	void paintEvent(QPaintEvent*) override {
		QPainter p(this);
		if (framebuffer.isNull()) {
			p.fillRect(rect(), Qt::black);
			return;
		}
		p.drawImage(0, 0, framebuffer);
		// HUD overlay: bottom-left retail Terran console chrome.
		// Placed at x=-2 so the console's minimap slot (x=6 inside
		// the source PNG) lines up with openBW's native minimap
		// draw position (map_screen_x=4, per ui/ui.h:1602). The
		// two off-screen columns are outer chrome, unnoticeable.
		if (!hud_console.isNull()) {
			const int hud_x = -2;
			const int hud_y = height() - hud_console.height();
			p.drawImage(hud_x, hud_y, hud_console);
			// The console's minimap slot (source PNG x=6..133,
			// y=55..182 within the trimmed 406×187 asset) is opaque
			// RGB(8,8,8) — a designed-to-be-overpainted placeholder.
			// openBW has already drawn the real 128×128 minimap into
			// the framebuffer at (4, height-4-128). Re-blit exactly
			// that region on top of the HUD so the real minimap shows
			// through the slot. Same trick retail BW's engine used to
			// composite the live minimap over the console PNG.
			const int mm_size = 128;
			const int mm_x = 4;
			const int mm_y = height() - 4 - mm_size;
			p.drawImage(QRect(mm_x, mm_y, mm_size, mm_size),
			            framebuffer,
			            QRect(mm_x, mm_y, mm_size, mm_size));
		}
	}

	void resizeEvent(QResizeEvent* e) override {
		native_window::event_t ev;
		ev.type   = native_window::event_t::type_resize;
		ev.width  = e->size().width();
		ev.height = e->size().height();
		events.push_back(ev);
		// Grow (or shrink) the backing image so ui_functions'
		// next-frame path finds the new dimensions when it calls
		// get_window_surface. Content is discarded -- the next
		// blit is a full-screen paint anyway.
		resize_framebuffer(e->size().width(), e->size().height());
		QWidget::resizeEvent(e);
	}

	void keyPressEvent(QKeyEvent* e) override {
		int scancode = qt_key_to_sdl_scancode(e->key());
		if (scancode >= 0 && scancode < (int)key_state.size()) {
			key_state[scancode] = true;
		}
		if (!e->isAutoRepeat()) {
			native_window::event_t ev;
			ev.type     = native_window::event_t::type_key_down;
			ev.sym      = e->key();
			ev.scancode = scancode;
			events.push_back(ev);
		}
	}
	void keyReleaseEvent(QKeyEvent* e) override {
		int scancode = qt_key_to_sdl_scancode(e->key());
		if (scancode >= 0 && scancode < (int)key_state.size()) {
			key_state[scancode] = false;
		}
		if (!e->isAutoRepeat()) {
			native_window::event_t ev;
			ev.type     = native_window::event_t::type_key_up;
			ev.sym      = e->key();
			ev.scancode = scancode;
			events.push_back(ev);
		}
	}

	void mousePressEvent(QMouseEvent* e) override {
		int b = qt_button_to_sdl(e->button());
		if (b > 0 && b < (int)mouse_button_state.size()) {
			mouse_button_state[b] = true;
		}
		native_window::event_t ev;
		ev.type          = native_window::event_t::type_mouse_button_down;
		ev.button        = b;
		ev.mouse_x       = e->position().x();
		ev.mouse_y       = e->position().y();
		ev.clicks        = 1;   // Qt doesn't distinguish easily; SDL usually 1.
		events.push_back(ev);
	}
	void mouseReleaseEvent(QMouseEvent* e) override {
		int b = qt_button_to_sdl(e->button());
		if (b > 0 && b < (int)mouse_button_state.size()) {
			mouse_button_state[b] = false;
		}
		native_window::event_t ev;
		ev.type          = native_window::event_t::type_mouse_button_up;
		ev.button        = b;
		ev.mouse_x       = e->position().x();
		ev.mouse_y       = e->position().y();
		ev.clicks        = 1;
		events.push_back(ev);
	}
	void mouseMoveEvent(QMouseEvent* e) override {
		native_window::event_t ev;
		ev.type          = native_window::event_t::type_mouse_motion;
		ev.button_state  = qt_buttons_to_sdl_state(e->buttons());
		ev.mouse_x       = e->position().x();
		ev.mouse_y       = e->position().y();
		// Qt doesn't hand us xrel/yrel directly; compute from last pos.
		ev.mouse_xrel    = e->position().x() - last_mouse_x_;
		ev.mouse_yrel    = e->position().y() - last_mouse_y_;
		last_mouse_x_    = e->position().x();
		last_mouse_y_    = e->position().y();
		events.push_back(ev);
	}
	void wheelEvent(QWheelEvent* e) override {
		native_window::event_t ev;
		ev.type = native_window::event_t::type_mouse_wheel;
		// Prefer pixelDelta on high-resolution devices (trackpads);
		// fall back to angleDelta / 120 (one "tick" per notch on a
		// classic wheel). Both are already sign-natural on the
		// platform, matching what SDL's preciseY reports.
		QPoint px = e->pixelDelta();
		if (!px.isNull()) {
			ev.wheel_x = (float)px.x() / 32.0f;
			ev.wheel_y = (float)px.y() / 32.0f;
		} else {
			QPoint ang = e->angleDelta();
			ev.wheel_x = (float)ang.x() / 120.0f;
			ev.wheel_y = (float)ang.y() / 120.0f;
		}
		events.push_back(ev);
	}
	void closeEvent(QCloseEvent* e) override {
		native_window::event_t ev;
		ev.type = native_window::event_t::type_quit;
		events.push_back(ev);
		// Don't accept -- let main.cpp handle the shutdown, so the
		// sync loop gets a chance to drain outbound messages.
		QWidget::closeEvent(e);
	}

private:
	int last_mouse_x_ = 0;
	int last_mouse_y_ = 0;
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// native_window::window_impl: the PIMPL declared (as forward ref) in
// native_window.h. Owns the QWidget lifetime and exposes the same shape
// as sdl2.cpp's window_impl. GameWidget* is a raw pointer here because
// QWidget instances are owned by Qt's parent-tree once shown; when we
// destroy() we call deleteLater() (or delete outright if never shown).
// ---------------------------------------------------------------------------
namespace native_window {

struct window_impl {
	// Owned via QObject parent hierarchy. When we destroy the widget
	// explicitly (from destroy() or ~window_impl()) it's safe.
	GameWidget* widget = nullptr;

	window_impl() {
		// Nothing to do here -- QApplication is constructed by main.cpp
		// before this class is ever instantiated. If it hasn't been,
		// the QWidget constructor would abort with a helpful message.
	}

	~window_impl() { destroy(); }

	void destroy() {
		if (widget) {
			widget->close();
			delete widget;
			widget = nullptr;
		}
	}

	bool create(const char* title, int x, int y, int width, int height) {
		if (widget) fatal_error("window already created");
		if (!QApplication::instance()) {
			log("qt_native_window: no QApplication instance -- "
			    "did you forget to create one in main()?\n");
			return false;
		}
		widget = new GameWidget();
		widget->setWindowTitle(QString::fromUtf8(title));
		widget->resize(width, height);
		widget->resize_framebuffer(width, height);
		if (x >= 0 && y >= 0) widget->move(x, y);
		widget->show();
		return true;
	}

	void get_cursor_pos(int* x, int* y) {
		if (!widget) { *x = 0; *y = 0; return; }
		QPoint p = widget->mapFromGlobal(QCursor::pos());
		*x = p.x();
		*y = p.y();
	}

	bool peek_message(event_t& e) {
		// Give Qt a chance to run any pending signal-slot work and
		// post fresh events. Zero-timer -- return immediately, don't
		// re-enter long-running work.
		//
		// Note: ui.h calls peek_message in a tight while loop, so this
		// runs several times per next_frame; hence the zero timeout.
		QCoreApplication::processEvents(QEventLoop::AllEvents, 0);
		if (!widget || widget->events.empty()) return false;
		e = widget->events.front();
		widget->events.pop_front();
		return true;
	}

	bool show_cursor(bool show) {
		if (!widget) return true;
		widget->setCursor(show ? Qt::ArrowCursor : Qt::BlankCursor);
		return true;
	}

	bool get_key_state(int scancode) {
		if (!widget || scancode < 0
		    || scancode >= (int)widget->key_state.size()) return false;
		return widget->key_state[scancode];
	}

	bool get_mouse_button_state(int button) {
		if (!widget || button < 0
		    || button >= (int)widget->mouse_button_state.size()) return false;
		return widget->mouse_button_state[button];
	}

	void update_surface() {
		if (widget) widget->update();
	}

	explicit operator bool() const { return widget != nullptr; }

	QImage* framebuffer() {
		return widget ? &widget->framebuffer : nullptr;
	}
};

// Thin passthrough to the PIMPL.
window::window() { impl = std::make_unique<window_impl>(); }
window::~window() = default;
window::window(window&& n) { impl = std::move(n.impl); }
void window::destroy() { impl->destroy(); }
bool window::create(const char* title, int x, int y, int w, int h) {
	return impl->create(title, x, y, w, h);
}
void window::get_cursor_pos(int* x, int* y) { impl->get_cursor_pos(x, y); }
bool window::peek_message(event_t& e) { return impl->peek_message(e); }
bool window::show_cursor(bool show)   { return impl->show_cursor(show); }
bool window::get_key_state(int sc)    { return impl->get_key_state(sc); }
bool window::get_mouse_button_state(int b) { return impl->get_mouse_button_state(b); }
void window::update_surface()         { impl->update_surface(); }
window::operator bool() const         { return (bool)*impl; }

} // namespace native_window

// ---------------------------------------------------------------------------
// native_window_drawing: palette + surface primitives.
//
// palette_impl -- 256-entry QRgb table.
// qt_surface   -- either owns a QImage (create_rgba_surface,
//                 convert_to_8_bit_indexed, load_image) or holds a
//                 non-owning pointer into GameWidget's framebuffer
//                 (get_window_surface).
// ---------------------------------------------------------------------------
namespace native_window_drawing {

struct palette_impl : palette {
	std::array<QRgb, 256> colors{};
	palette_impl() {
		colors.fill(qRgba(0, 0, 0, 255));
	}
	void set_colors(color c[256]) override {
		for (size_t i = 0; i < 256; ++i) {
			colors[i] = qRgba(c[i].r, c[i].g, c[i].b, 255);
			// Note: the sdl2 backend also comments out the alpha byte
			// here -- palette entries always render opaque. We follow
			// that.
		}
	}
};

// Non-owning surface: wraps a QImage* that lives elsewhere (the
// GameWidget's framebuffer). Destructor is a no-op; drop the wrapper
// and the underlying image lives on.
//
// Owning surface: has its own QImage. Destructor drops it.
struct qt_surface : surface {
	QImage* image = nullptr;         // Non-null when the surface exposes
	                                 // a QImage; nullptr means "invalid".
	std::unique_ptr<QImage> owned;   // Set when we own the QImage.

	// Blit-source state, applied when this surface is used as the src
	// in blit() / blit_scaled(). Roughly matches SDL's per-surface
	// alpha-mod and blend-mode.
	blend_mode blend = blend_mode::none;
	int        alpha_mod = 255;

	// Optional palette held for indexed surfaces. Kept as a shared_ptr
	// because ui.h calls set_palette once and expects the palette's
	// lifetime to outlive the surface.
	palette_impl* attached_palette = nullptr;

	void set_image(QImage* img, bool own) {
		if (own) {
			owned.reset(img);
			image = img;
		} else {
			owned.reset();
			image = img;
		}
		w     = img ? img->width()  : 0;
		h     = img ? img->height() : 0;
		pitch = img ? img->bytesPerLine() : 0;
	}

	void set_palette(palette* pal) override {
		attached_palette = (palette_impl*)pal;
		if (!image || image->format() != QImage::Format_Indexed8) return;
		// Push the 256 QRgb entries as the image's color table so
		// subsequent conversions / blits render with the right hues.
		QVector<QRgb> table(256);
		for (size_t i = 0; i < 256; ++i) {
			table[i] = attached_palette->colors[i];
		}
		image->setColorTable(table);
	}

	void* lock() override {
		if (!image) return nullptr;
		// QImage::bits() forces detach so the caller gets a writable
		// pointer that isn't shared with any implicit copy.
		return image->bits();
	}

	void unlock() override {
		// No-op -- QImage doesn't need explicit unlock, and Qt tracks
		// dirty state through the shared-image machinery.
	}

	// Set QPainter composition mode for a given blend_mode.
	static QPainter::CompositionMode composition_mode(blend_mode b) {
		switch (b) {
		case blend_mode::none:  return QPainter::CompositionMode_Source;
		case blend_mode::alpha: return QPainter::CompositionMode_SourceOver;
		case blend_mode::add:   return QPainter::CompositionMode_Plus;
		case blend_mode::mod:   return QPainter::CompositionMode_Multiply;
		}
		return QPainter::CompositionMode_SourceOver;
	}

	// If we're an Indexed8 QImage, produce a temporary ARGB32 view
	// so we can draw into an ARGB destination. QPainter::drawImage
	// handles the palette lookup for us when the source has a color
	// table set.
	void blit_impl(qt_surface* dst, int x, int y, int w_scale, int h_scale) {
		if (!image || !dst || !dst->image) return;
		QPainter painter(dst->image);
		painter.setCompositionMode(composition_mode(blend));
		// SDL semantics: alpha-mod is a no-op when the surface's blend
		// mode is BLENDMODE_NONE (see SDL_SetSurfaceAlphaMod docs).
		// ui.h relies on this -- it calls set_alpha(0) on rgba_surface
		// and window_surface at startup as a "just in case" reset, then
		// never touches it again. If we applied opacity=0 with blend=
		// none here, every frame would paint with 0% opacity -> black
		// window. Match SDL's rule: only apply alpha when a blend mode
		// is actually engaged.
		if (blend != blend_mode::none && alpha_mod < 255) {
			painter.setOpacity(alpha_mod / 255.0);
		}
		if (w_scale > 0 && h_scale > 0) {
			QRect target(x, y, w_scale, h_scale);
			painter.drawImage(target, *image);
		} else {
			painter.drawImage(QPoint(x, y), *image);
		}
	}

	void blit(surface* dst, int x, int y) override {
		blit_impl((qt_surface*)dst, x, y, 0, 0);
	}

	void blit_scaled(surface* dst, int x, int y, int ww, int hh) override {
		blit_impl((qt_surface*)dst, x, y, ww, hh);
	}

	void fill(int r, int g, int b, int a) override {
		if (!image) return;
		if (image->format() == QImage::Format_Indexed8) {
			// SDL_FillRect on an indexed surface would look up
			// the palette entry closest to (r,g,b) via
			// SDL_MapRGBA. We don't have an accurate reverse
			// lookup, but ui.h's fill() calls on indexed surfaces
			// are always fill(0,0,0,255) => palette index 0
			// (traditionally black). Use that.
			image->fill(0);
		} else {
			image->fill(qRgba(r, g, b, a));
		}
	}

	void set_alpha(int a) override { alpha_mod = a; }
	void set_blend_mode(blend_mode b) override { blend = b; }
};

palette* new_palette() { return new palette_impl(); }
void delete_palette(palette* p) { delete p; }

std::unique_ptr<surface> create_rgba_surface(int width, int height) {
	auto* img = new QImage(width, height, QImage::Format_ARGB32);
	img->fill(Qt::transparent);
	auto s = std::make_unique<qt_surface>();
	s->set_image(img, /*own=*/true);
	return std::unique_ptr<surface>(s.release());
}

std::unique_ptr<surface> get_window_surface(native_window::window* wnd) {
	auto* fb = wnd->impl->framebuffer();
	if (!fb) fatal_error("get_window_surface: window has no framebuffer");
	auto s = std::make_unique<qt_surface>();
	s->set_image(fb, /*own=*/false);
	return std::unique_ptr<surface>(s.release());
}

std::unique_ptr<surface> convert_to_8_bit_indexed(surface* src) {
	auto* orig = ((qt_surface*)src)->image;
	if (!orig) fatal_error("convert_to_8_bit_indexed: null source");
	// ui.h uses this to make an indexed blank canvas the same size as
	// its RGBA source; the source pixels are irrelevant, the caller
	// paints fresh content in every frame. So we allocate a blank
	// Indexed8 image with a default (all-black) color table.
	auto* img = new QImage(orig->width(), orig->height(),
	                       QImage::Format_Indexed8);
	QVector<QRgb> table(256, qRgba(0, 0, 0, 255));
	img->setColorTable(table);
	img->fill(0);
	auto s = std::make_unique<qt_surface>();
	s->set_image(img, /*own=*/true);
	return std::unique_ptr<surface>(s.release());
}

std::unique_ptr<surface> load_image(const char* filename) {
	QImage img;
	if (!img.load(QString::fromUtf8(filename))) {
		fatal_error("load_image(%s) failed", filename);
	}
	auto* boxed = new QImage(img.convertToFormat(QImage::Format_ARGB32));
	auto s = std::make_unique<qt_surface>();
	s->set_image(boxed, /*own=*/true);
	return std::unique_ptr<surface>(s.release());
}

std::unique_ptr<surface> load_image(const void* data, size_t size) {
	QImage img;
	if (!img.loadFromData(reinterpret_cast<const uchar*>(data),
	                      (int)size)) {
		fatal_error("load_image(mem, %zu bytes) failed", size);
	}
	auto* boxed = new QImage(img.convertToFormat(QImage::Format_ARGB32));
	auto s = std::make_unique<qt_surface>();
	s->set_image(boxed, /*own=*/true);
	return std::unique_ptr<surface>(s.release());
}

} // namespace native_window_drawing

// ---------------------------------------------------------------------------
// native_sound: stubbed. The SDL2 backend links SDL2_mixer; on the Qt
// side we don't ship a mixer yet -- the observer plays no unit sounds
// today anyway (spectator UI, so the sound calls only fire from the
// campaign path we don't touch). Every call becomes a no-op.
// ---------------------------------------------------------------------------
namespace native_sound {

int frequency = 0;
int channels  = 64;
bool initialized = false;

void init() { initialized = true; }

struct qt_sound : sound {
	~qt_sound() override {}
};

void play(int /*channel*/, sound* /*s*/, int /*vol*/, int /*pan*/) {}
bool is_playing(int /*channel*/) { return false; }
void stop(int /*channel*/) {}
void set_volume(int /*channel*/, int /*volume*/) {}
std::unique_ptr<sound> load_wav(const void* /*data*/, size_t /*size*/) {
	// Return an owned dummy so callers that store the pointer don't
	// blow up (the SDL backend also returns null on Mix failures, but
	// most callers null-check first).
	return std::unique_ptr<sound>(new qt_sound());
}

} // namespace native_sound
