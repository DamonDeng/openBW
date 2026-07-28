// One instance = one open observer view.
//
// The visible top-level window is a GameWidget created inside
// ui_functions::wnd (see simsc_app/qt_native_window.cpp:468); this
// class doesn't inherit QWidget itself. It's a QObject that owns
// the sim + transport + sim-tick QTimer and dies when the
// GameWidget's closeEvent surfaces a type_quit event through the
// sim's own peek_message loop.
//
// Lifetime rules:
//   * new ObserverWindow(params) -- starts a running observer
//     immediately. Constructor blocks on MPQ load + map load
//     (~0.5s for a small map) then returns; sim ticks start on
//     the next Qt event loop turn.
//   * When the user closes the game widget, the QTimer sees a
//     type_quit event and deleteLater()s this ObserverWindow.
//   * Multiple ObserverWindows for the same local game are OK --
//     the local server has --any-ws-path enabled and doesn't
//     police observer counts.

#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>

class QTimer;

namespace simsc_desktop {

struct ObserverParams {
	QString title;                 // shown in the GameWidget's window title
	QString url;                   // ws:// or wss://
	QString data_path;             // MPQ directory
	QString map_path;              // absolute path to .scm/.scx
	QString api_key;               // plaintext, sent as ?key=... by transport
	// Per-slot race overrides matching what the SERVER was given.
	// Mismatched values desync the observer at tick 0 because the
	// starting-unit spawner keys off race.
	int race_overrides[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
	int screen_width  = 1280;
	int screen_height = 800;
	int client_batch_size = 1;
};

class ObserverWindow : public QObject {
	Q_OBJECT
public:
	explicit ObserverWindow(ObserverParams params, QObject* parent = nullptr);
	~ObserverWindow() override;

private:
	// bwgame.h / ui.h aren't safe to expose in a header (huge TU,
	// order-sensitive), so we hide them behind a PIMPL.
	struct Impl;
	Impl* d_ = nullptr;
};

}   // namespace simsc_desktop
