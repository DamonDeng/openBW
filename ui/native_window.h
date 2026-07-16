#ifndef NATIVE_WINDOW_H
#define NATIVE_WINDOW_H

#include <memory>

namespace native_window {
	struct window_impl;
	
	struct event_t {
		enum {
			type_none,
			type_quit,
			type_key_down,
			type_key_up,
			type_resize,
			type_mouse_button_down,
			type_mouse_button_up,
			type_mouse_motion,
			type_mouse_wheel
		};
		int type = type_none;
		int sym = -1;
		int scancode = -1;
		int width = -1;
		int height = -1;
		int button = -1;
		int mouse_x = -1;
		int mouse_y = -1;
		int button_state = 0;
		int mouse_xrel = -1;
		int mouse_yrel = -1;
		int clicks = -1;
		// Mouse-wheel delta in fractional "lines" -- SDL provides int
		// wheel.x/y (whole ticks) and preciseX/preciseY (fractional,
		// non-zero on high-resolution wheels + trackpads). We forward
		// preciseX/Y as float so a two-finger trackpad drag on macOS
		// produces smooth scrolling. Sign: positive Y means the user
		// scrolled UP (natural on macOS); the ui event loop decides
		// how to interpret that.
		float wheel_x = 0.0f;
		float wheel_y = 0.0f;
	};

	// Numeric HUD readouts shown alongside the retail console icons.
	// The observer's main loop refreshes these each frame from the
	// sim state; the window backend paints them into its HUD strip
	// during the next paintEvent. Fields set to -1 mean "hide this
	// readout" (used for spectator perspective where per-slot
	// numbers don't apply).
	struct hud_state_t {
		int minerals = -1;
		int gas = -1;
		int supply_used = -1;
		int supply_max = -1;
	};

	struct window {
		std::unique_ptr<window_impl> impl;
		window();
		~window();
		window(window&&);
		void destroy();
		bool create(const char* title, int x, int y, int width, int height);
		void get_cursor_pos(int* x, int* y);
		bool peek_message(event_t& e);
		bool show_cursor(bool show);
		bool get_key_state(int scancode);
		bool get_mouse_button_state(int button);
		void update_surface();
		// Update the numeric readouts drawn alongside the HUD icons.
		// Currently honored by the Qt backend; SDL/WASM ignore it
		// until they grow their own HUD blit.
		void set_hud_state(const hud_state_t& s);
		explicit operator bool() const;
	};
}

#endif

