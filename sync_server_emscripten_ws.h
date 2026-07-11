// Browser-side sync transport for the WASM observer.
//
// Public API mirrors sync_server_asio_ws exactly -- sync.h is
// transport-polymorphic via templates and calls the same set of methods
// (new_message, send_message, set_on_message, set_on_kill, poll, connect,
// allow_send, kill_client, set_timeout). Because sync.h only ever needs
// the *client* side of the transport in a browser (browsers cannot
// bind()/accept()), bind() is intentionally omitted here.
//
// Under the hood we drive emscripten's WebSocket API
// (<emscripten/websocket.h>). That API delivers one complete WebSocket
// message per onmessage callback, which matches sync.h's message-oriented
// on_message(data, size) shape -- no reframing needed.
//
// Threading: in a non-pthread build (our case) all emscripten callbacks
// run on the same thread as the wasm main-loop callback, so no locking
// is necessary. If we ever enable -pthread we'll need to guard the
// message queue.
//
// See also: sync_server_asio_ws.h (native counterpart, server + client
// side, drives asio + does the full RFC 6455 handshake in userspace).

#ifndef BWGAME_SYNC_SERVER_EMSCRIPTEN_WS_H
#define BWGAME_SYNC_SERVER_EMSCRIPTEN_WS_H

#include "bwgame.h"

#include <emscripten.h>
#include <emscripten/websocket.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace bwgame {

struct sync_server_emscripten_ws {

	// ------------------------------------------------------------------
	// Message builder -- byte-for-byte compatible with the native ws
	// transport's message_t.put<T>() / put(data, size). sync.h fills one
	// of these and hands it to send_message(); we ship the bytes as a
	// single WebSocket binary frame.
	// ------------------------------------------------------------------
	struct message_t {
		std::vector<uint8_t> data;

		template<typename T>
		void put(T v) {
			// LE encode to match sync_server_asio_ws / sync_server_asio_socket.
			std::array<uint8_t, sizeof(T)> buf;
			for (size_t i = 0; i < sizeof(T); ++i) {
				buf[i] = (uint8_t)(v >> (i * 8));
			}
			put(buf.data(), buf.size());
		}
		void put(const void* p, size_t n) {
			auto* b = (const uint8_t*)p;
			data.insert(data.end(), b, b + n);
		}
	};
	message_t new_message() { return {}; }

	// ------------------------------------------------------------------
	// Client handle exposed to sync.h. All calls that take `const void* h`
	// are pointers to one of these. In practice there's exactly one
	// client_t per browser observer instance (we only ever connect to a
	// single server), but keep the shape flexible in case a future
	// portal fans us out across multiple servers.
	// ------------------------------------------------------------------
	struct client_t {
		EMSCRIPTEN_WEBSOCKET_T socket = 0;
		bool is_open   = false;
		bool is_dead   = false;
		bool allow_send_flag = false;

		// If we try to send before onopen fires, buffer here and flush
		// once open. Same behavior as native asio transport, which
		// buffers into send_queue until the WS handshake completes.
		std::vector<std::vector<uint8_t>> pending_sends;

		// Messages received from the server. Filled by the JS-side
		// onmessage callback, drained by the caller's set_on_message
		// hook during poll(). Deque so we can pop from the front cheaply.
		std::deque<std::vector<uint8_t>> incoming;

		std::function<void(const void*, size_t)> on_message;
		std::function<void()> on_kill;

		// Diagnostic counters -- surface counts periodically so we can
		// tell "the transport is delivering messages" vs "the transport
		// is dead / stuck". If sim frames advance but these counters
		// don't grow, the transport is broken.
		int msgs_received = 0;
		int msgs_delivered = 0;
		int msgs_sent = 0;
		uint64_t bytes_received = 0;
		std::array<int, 256> msg_id_hist{};
	};

	// The transport owns its clients. Single-item vector in the
	// observer case; keeping it as a vector matches the native
	// transport's shape and lets a future portal manage several.
	std::vector<std::unique_ptr<client_t>> clients;

	// Newly-connected clients (post-onopen) waiting to be handed to the
	// caller via poll(on_new_client). Emptied on each poll() call.
	std::vector<client_t*> new_clients;

	// URL bits set by the caller before connect(). Same names as native
	// sync_server_asio_ws so observer.cpp / observer_wasm.cpp can point
	// at either transport with identical setup code.
	std::string client_url_path = "/observer";
	std::string client_api_key;  // appended as ?key=... if non-empty

	// Optional timeout, driven manually from poll() -- the browser has
	// no io_service to fire it for us. sync.h uses this only in the
	// noop / test transports so far, but the native ws transport
	// implements it and we mirror the API for parity.
	std::chrono::steady_clock::time_point timeout_time{};
	std::function<void()> timeout_function;

	// ------------------------------------------------------------------
	// Callback trampolines. Emscripten's C API requires plain C-style
	// callbacks; we thread `userData=client_t*` through and dispatch to
	// members here. Declared static so their addresses are stable.
	// ------------------------------------------------------------------
	static EM_BOOL on_ws_open(int /*eventType*/,
	                           const EmscriptenWebSocketOpenEvent* /*e*/,
	                           void* userData) {
		auto* c = (client_t*)userData;
		c->is_open = true;
		// Flush anything queued before the socket was ready.
		for (auto& msg : c->pending_sends) {
			if (msg.empty()) continue;
			emscripten_websocket_send_binary(c->socket,
				msg.data(), (uint32_t)msg.size());
		}
		c->pending_sends.clear();
		return EM_TRUE;
	}

	static EM_BOOL on_ws_message(int /*eventType*/,
	                              const EmscriptenWebSocketMessageEvent* e,
	                              void* userData) {
		auto* c = (client_t*)userData;
		if (c->is_dead) return EM_TRUE;
		// e->isText: text vs binary. sync.h only speaks binary, so
		// text frames are a protocol error -- kill the connection.
		if (e->isText) {
			mark_dead(c);
			return EM_TRUE;
		}
		// Copy into an owned buffer. e->data is only valid inside the
		// callback; the on_message hook fires later (from poll()).
		std::vector<uint8_t> buf(e->data, e->data + e->numBytes);
		c->msgs_received++;
		c->bytes_received += e->numBytes;
		// Log only the first 6 messages (handshake) verbatim; steady-
		// state message rate is thousands/sec at speed=42 and floods
		// the browser console otherwise. Periodic per-id histogram is
		// dumped from observer_wasm.cpp instead.
		if (c->msgs_received <= 6 && e->numBytes >= 1) {
			printf("[ws-rx] msg#%d id=0x%02x len=%u\n",
				c->msgs_received, buf[0], (unsigned)e->numBytes);
			fflush(stdout);
		}
		// Track per-msg-id counts so a periodic summary can show the
		// distribution (id_agent_action=0x10 dominant, etc).
		if (e->numBytes >= 1) c->msg_id_hist[buf[0]]++;
		c->incoming.push_back(std::move(buf));
		return EM_TRUE;
	}

	static EM_BOOL on_ws_error(int /*eventType*/,
	                            const EmscriptenWebSocketErrorEvent* /*e*/,
	                            void* userData) {
		mark_dead((client_t*)userData);
		return EM_TRUE;
	}

	static EM_BOOL on_ws_close(int /*eventType*/,
	                            const EmscriptenWebSocketCloseEvent* /*e*/,
	                            void* userData) {
		mark_dead((client_t*)userData);
		return EM_TRUE;
	}

	// Mark for kill; the on_kill callback (if installed) is fired from
	// poll() to keep the "caller hooks always run on the main thread"
	// invariant. Also matches native transport's deferred-kill flow.
	static void mark_dead(client_t* c) { c->is_dead = true; }

	// ------------------------------------------------------------------
	// connect(host, port) -- observer-side entry point. Builds the URL
	//   ws://host:port<client_url_path>?key=<client_api_key>
	// and opens the WebSocket. Callbacks are installed synchronously;
	// they fire later (async) as JS delivers events.
	// ------------------------------------------------------------------
	void connect(const a_string& hostname, int port) {
		auto c = std::unique_ptr<client_t>(new client_t());

		std::string url = "ws://";
		url += hostname.c_str();
		url += ':';
		url += std::to_string(port);
		url += client_url_path;
		if (!client_api_key.empty()) {
			url += "?key=";
			url += client_api_key;
		}

		EmscriptenWebSocketCreateAttributes attrs{};
		attrs.url = url.c_str();
		attrs.protocols = nullptr;   // no subprotocol
		attrs.createOnMainThread = EM_TRUE;

		c->socket = emscripten_websocket_new(&attrs);
		if (c->socket <= 0) {
			// Creation failed synchronously -- e.g. bad URL, WebSocket
			// support unavailable. Mark dead so poll() surfaces it.
			c->is_dead = true;
			auto* raw = c.get();
			clients.push_back(std::move(c));
			new_clients.push_back(raw);
			return;
		}
		auto* raw = c.get();
		emscripten_websocket_set_onopen_callback_on_thread(
			c->socket, raw, &sync_server_emscripten_ws::on_ws_open,
			EM_CALLBACK_THREAD_CONTEXT_CALLING_THREAD);
		emscripten_websocket_set_onmessage_callback_on_thread(
			c->socket, raw, &sync_server_emscripten_ws::on_ws_message,
			EM_CALLBACK_THREAD_CONTEXT_CALLING_THREAD);
		emscripten_websocket_set_onerror_callback_on_thread(
			c->socket, raw, &sync_server_emscripten_ws::on_ws_error,
			EM_CALLBACK_THREAD_CONTEXT_CALLING_THREAD);
		emscripten_websocket_set_onclose_callback_on_thread(
			c->socket, raw, &sync_server_emscripten_ws::on_ws_close,
			EM_CALLBACK_THREAD_CONTEXT_CALLING_THREAD);
		clients.push_back(std::move(c));
		new_clients.push_back(raw);
	}

	// bind() is a compile-time no-op: browsers cannot listen. Any code
	// path that tries to bind is a bug -- fail loudly at link time by
	// leaving this undeclared. sync.h does not touch bind() unless the
	// caller (main.cpp on the server side) explicitly calls it.

	// ------------------------------------------------------------------
	// send_message: caller has filled a message_t; ship it as one
	// binary WS frame. If the socket isn't open yet, stash for onopen.
	// ------------------------------------------------------------------
	void send_message(const message_t& d, const void* h) {
		if (!h) {
			// Broadcast case (matches native transport's contract).
			// Observer only ever has one server-side peer, but keep
			// the semantic.
			for (auto& c : clients) {
				if (c->is_dead || !c->allow_send_flag) continue;
				dispatch_send(c.get(), d.data);
			}
		} else {
			auto* c = (client_t*)h;
			if (c->is_dead || !c->allow_send_flag) return;
			dispatch_send(c, d.data);
		}
	}

	void dispatch_send(client_t* c, const std::vector<uint8_t>& bytes) {
		if (bytes.empty()) return;
		if (!c->is_open) {
			c->pending_sends.push_back(bytes);
			return;
		}
		// send_binary takes non-const void* on the JS side; safe to
		// const_cast because emscripten copies the buffer into a JS
		// ArrayBuffer immediately.
		emscripten_websocket_send_binary(c->socket,
			const_cast<uint8_t*>(bytes.data()),
			(uint32_t)bytes.size());
		c->msgs_sent++;
	}

	void allow_send(const void* h, bool allow) {
		auto* c = (client_t*)h;
		if (c) c->allow_send_flag = allow;
	}

	void kill_client(const void* h) {
		auto* c = (client_t*)h;
		if (!c) return;
		printf("[ws-kill] sync.h asked to kill peer "
		       "(msgs_rx=%d, delivered=%d, last_msg_id_seen=?)\n",
		       c->msgs_received, c->msgs_delivered);
		fflush(stdout);
		c->is_dead = true;
		if (c->socket) {
			emscripten_websocket_close(c->socket, 1000, "kill_client");
		}
	}

	template<typename F>
	void set_on_kill(const void* h, F&& f) {
		auto* c = (client_t*)h;
		if (c) c->on_kill = std::forward<F>(f);
	}

	template<typename F>
	void set_on_message(const void* h, F&& f) {
		auto* c = (client_t*)h;
		if (c) c->on_message = std::forward<F>(f);
	}

	template<typename duration_T, typename callback_F>
	void set_timeout(duration_T&& duration, callback_F&& callback) {
		timeout_time = std::chrono::steady_clock::now() + duration;
		timeout_function = std::forward<callback_F>(callback);
	}

	// ------------------------------------------------------------------
	// poll(on_new_client): drain queues. Called once per frame from
	// funcs.sync() / funcs.next_frame(). We:
	//   1. Fire on_new_client for freshly-connected clients (opens the
	//      transport for the sync.h greeting handshake).
	//   2. Deliver any queued incoming messages.
	//   3. Fire on_kill for any client that died since last poll.
	//   4. Trip the manual timeout if scheduled.
	// ------------------------------------------------------------------
	template<typename on_new_client_F>
	void poll(on_new_client_F&& on_new_client) {
		// (1) Hand off new clients. Note native transport waits until
		// the WS handshake completes before signalling; here we do the
		// same by only surfacing clients whose is_open flipped true --
		// EXCEPT that sync.h's greeting protocol needs to install its
		// on_message hook *before* the first message arrives. Since
		// emscripten queues messages until an on_message is attached
		// (they land in c->incoming and just wait), it's actually
		// safe to signal on connect() and let on_message be installed
		// while messages queue. Match native's timing to be safe:
		// only surface clients that are open.
		auto pending = std::move(new_clients);
		new_clients.clear();
		for (auto* c : pending) {
			if (c->is_dead) {
				// Bad URL / early failure -- caller still needs an
				// on_new_client to install hooks, then we'll fire
				// on_kill below.
				c->allow_send_flag = true;
				on_new_client(c);
			} else if (c->is_open) {
				c->allow_send_flag = true;
				on_new_client(c);
			} else {
				// Not open yet -- put back for next poll().
				new_clients.push_back(c);
			}
		}

		// (2) Deliver queued messages.
		for (auto& c : clients) {
			if (!c->on_message) continue;
			while (!c->incoming.empty()) {
				auto msg = std::move(c->incoming.front());
				c->incoming.pop_front();
				c->on_message(msg.data(), msg.size());
				c->msgs_delivered++;
				if (c->is_dead) break;
			}
		}

		// (3) Fire on_kill callbacks for dead clients, once.
		for (auto& c : clients) {
			if (c->is_dead && c->on_kill) {
				auto f = std::move(c->on_kill);
				c->on_kill = nullptr;
				f();
			}
		}

		// (4) Manual timeout.
		if (timeout_function
		    && std::chrono::steady_clock::now() >= timeout_time) {
			auto f = std::move(timeout_function);
			timeout_function = nullptr;
			f();
		}
	}

	// run_one / run_until: in the browser we can't block outright, but
	// we CAN yield to the JS event loop via emscripten_sleep(). That
	// requires -sASYNCIFY at compile time. When ASYNCIFY is on, this
	// implementation actually behaves like the native asio transport:
	// run_one yields once (~1 ms) so pending WS messages can be
	// delivered, then polls; run_until loops that until pred() is
	// true or a bounded budget elapses (to keep the browser tab
	// responsive even if the server disappears).
	//
	// Without ASYNCIFY these are no-ops and the sim races ahead of the
	// server heartbeats -- see the pacing bug diagnosed 2026-07-11.
	template<typename on_new_client_F>
	void run_one(on_new_client_F&& on_new_client) {
		emscripten_sleep(1);  // yield to JS event loop; requires ASYNCIFY
		poll(std::forward<on_new_client_F>(on_new_client));
	}
	template<typename on_new_client_F, typename pred_F>
	void run_until(on_new_client_F&& on_new_client, pred_F&& pred) {
		// Bounded loop: at most ~30 ms of yielding per call, so a
		// misbehaving server can't freeze the tab forever. 30 ms is
		// more than one server tick at speed=42 -- if we haven't
		// caught a heartbeat by then, we return and let the next
		// main-loop tick try again (the render still runs).
		int budget_ms = 30;
		while (!pred() && budget_ms > 0) {
			emscripten_sleep(1);
			poll(on_new_client);
			budget_ms -= 1;
		}
		// One final poll to catch anything that landed on the last
		// sleep boundary.
		poll(std::forward<on_new_client_F>(on_new_client));
	}
};

} // namespace bwgame

#endif // BWGAME_SYNC_SERVER_EMSCRIPTEN_WS_H
