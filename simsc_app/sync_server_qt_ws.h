// QWebSocket-based drop-in for sync_server_asio_ws.h. Same duck-typed
// template contract that sync.h expects — sync.h templates on the
// transport type, never names it, so this whole file is opt-in per
// build target. The current native openbw_observer keeps using
// sync_server_asio_ws.h; simsc_app uses this one.
//
// Why Qt-native instead of the asio version:
//   1. Free TLS. QWebSocket handles wss:// URLs with the OS trust store,
//      so simsc_app talks to the ALB directly. No local socat proxy.
//   2. Free RFC 6455. QWebSocket does the HTTP upgrade, masking,
//      fragmentation, PING/PONG. Our asio version reimplemented all of
//      that by hand — ~700 lines we no longer maintain.
//   3. Free event loop integration. Qt drives its socket work from the
//      main event loop; the render QTimer that owns next_frame() also
//      services message delivery for free.
//
// Client-only. This transport does NOT implement bind() / accept — the
// simsc_app is a spectator, never a server.
//
// The API surface below matches sync_server_asio_ws.h method-for-method
// so sync.h's next_frame() template picks the right overloads. If you
// find yourself adding a method here, first check whether sync_asio has
// the same signature — divergence in this contract is the whole point
// we're trying to avoid.

#ifndef BWGAME_SYNC_SERVER_QT_WS_H
#define BWGAME_SYNC_SERVER_QT_WS_H

#include "bwgame.h"

#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QDeadlineTimer>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtWebSockets/QWebSocket>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bwgame {

struct sync_server_qt_ws {

	// ------------------------------------------------------------------
	// message_t — same shape as sync_server_asio_ws's builder. sync.h
	// calls .put<T>(v) and .put(ptr, size) many times per outgoing
	// message. We back it with a plain byte vector; QWebSocket wraps
	// it in a QByteArray at send time.
	// ------------------------------------------------------------------
	struct message_t {
		std::vector<uint8_t> data;

		template<typename T>
		void put(T v) {
			// Little-endian encoder to match the asio transport's
			// data_loading::set_value_at<true>. Every field we send
			// is fixed-width integer; no need for a general codec.
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
	// client_t — one per open connection. The observer only ever holds
	// one server-side peer, so `clients` is a single-element vector in
	// practice. Vector shape preserved to match the asio transport's
	// interface exactly.
	// ------------------------------------------------------------------
	struct client_t {
		// QWebSocket instance for this peer. Owned via unique_ptr so
		// it can be reparented into a QObject tree if needed later
		// (Qt's parent-owns-child model). null until connect_url()
		// has fired.
		std::unique_ptr<QWebSocket> ws;

		// Lifecycle flags, matching the asio transport semantics.
		bool is_connected  = false;   // WS handshake completed
		bool is_dead       = false;   // fatal error / close received
		bool allow_send_flag = false; // set true after on_new_client

		// Queued incoming binary frames. Populated by our slot for
		// QWebSocket::binaryMessageReceived; drained on the next
		// poll() call.
		std::deque<QByteArray> incoming;

		// If we send before the WS handshake completes, stash here
		// and flush on connected. Matches asio's send_queue pattern.
		std::vector<QByteArray> pending_sends;

		std::function<void(const void*, size_t)> on_message;
		std::function<void()> on_kill;

		// Diagnostic counters, mirroring asio's per-client stats so
		// snapshot dumps look familiar.
		int msgs_received = 0;
		int msgs_delivered = 0;
		int msgs_sent = 0;
		uint64_t bytes_received = 0;
		std::array<int, 256> msg_id_hist{};
	};

	std::vector<std::unique_ptr<client_t>> clients;

	// Freshly-connected clients waiting for the caller to install
	// on_message / on_kill hooks via poll(on_new_client). Drained on
	// each poll() call. Same as asio's `new_clients` vector.
	std::vector<client_t*> new_clients;

	// Configurable URL bits. Kept as public data members with the
	// same names as sync_server_asio_ws so observer.cpp-style setup
	// code works verbatim.
	std::string client_url_path = "/observer";
	std::string client_api_key;      // appended as ?key=… if non-empty
	std::string client_host_header;  // unused in the Qt path (QWebSocket
	                                 // fills Host from the QUrl), kept
	                                 // for API parity.
	std::string server_path;         // server-side only; unused.

	// The asio version accepts auth_fn as the server-side hook; we
	// keep the field for API parity but never call it (client-only
	// transport).
	using ws_auth_fn = std::function<bool(const std::string&)>;
	ws_auth_fn auth_fn;

	// One-shot timeout, driven manually from poll(). sync.h's
	// noop/test transports use this; we mirror the interface but our
	// Qt event loop already runs QTimer so nothing to do at poll
	// time (the QTimer we schedule fires on its own).
	std::chrono::steady_clock::time_point timeout_time{};
	std::function<void()> timeout_function;

	// ------------------------------------------------------------------
	// connect_url — primary entry point. Takes a full wss:// URL
	// (typically from the simsc /api/games response) plus an optional
	// API key that gets appended as ?key=…. QWebSocket handles TLS.
	// ------------------------------------------------------------------
	void connect_url(const std::string& url) {
		auto c = std::make_unique<client_t>();
		c->ws = std::make_unique<QWebSocket>();
		auto* raw = c.get();
		auto* ws  = c->ws.get();

		// Slot: WS handshake finished.
		QObject::connect(ws, &QWebSocket::connected, ws, [raw, ws]() {
			raw->is_connected = true;
			// Flush anything queued while the socket was still
			// handshaking.
			for (auto& pending : raw->pending_sends) {
				ws->sendBinaryMessage(pending);
			}
			raw->pending_sends.clear();
		});

		// Slot: binary frame arrived. Copy into the client's queue;
		// drain on the next poll(). Buffering here lets sync.h see
		// messages in the same order Qt received them, regardless
		// of when the caller's poll runs.
		QObject::connect(ws, &QWebSocket::binaryMessageReceived, ws,
		                 [raw](const QByteArray& msg) {
			if (raw->is_dead) return;
			raw->msgs_received++;
			raw->bytes_received += msg.size();
			if (msg.size() >= 1) {
				raw->msg_id_hist[(uint8_t)msg[0]]++;
			}
			raw->incoming.push_back(msg);
		});

		// Slot: socket dropped.
		QObject::connect(ws, &QWebSocket::disconnected, ws, [raw]() {
			raw->is_dead = true;
		});

		// Slot: any error (auth failure, TLS handshake failure,
		// remote close, ...). Qt overload noise — use the errorSignal
		// signal via QOverload for clarity. We log the error string to
		// stderr because errors happen exactly once in the socket's
		// life and are usually the only clue for "why did I never
		// receive a message" bugs.
		QObject::connect(ws,
			QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::errorOccurred),
			ws, [raw, ws](QAbstractSocket::SocketError err) {
			std::fprintf(stderr, "[qt_ws] error code=%d msg=%s\n",
				(int)err, ws->errorString().toStdString().c_str());
			raw->is_dead = true;
		});

		// Append ?key=… if the caller set client_api_key. The
		// server's ws_hs::extract_key parses either separator.
		std::string full = url;
		if (!client_api_key.empty()) {
			full += (full.find('?') == std::string::npos ? '?' : '&');
			full += "key=";
			full += client_api_key;
		}

		clients.push_back(std::move(c));
		new_clients.push_back(raw);

		// Kick the QWebSocket handshake. Returns immediately; the
		// `connected` / `errorOccurred` signals fire from the Qt
		// event loop later.
		ws->open(QUrl(QString::fromStdString(full)));
	}

	// Legacy shape kept for source-compat with observer.cpp's
	// existing setup code, which calls server.connect(host, port).
	// Constructs a ws:// URL and delegates. wss:// callers should
	// prefer connect_url directly.
	void connect(const a_string& hostname, int port) {
		std::string url = "ws://";
		url += hostname.c_str();
		url += ':';
		url += std::to_string(port);
		url += client_url_path;
		connect_url(url);
	}

	// ------------------------------------------------------------------
	// send / kill / flags — one-liners against QWebSocket.
	// ------------------------------------------------------------------
	void send_message(const message_t& d, const void* h) {
		if (!h) {
			// Broadcast. observer only has one peer.
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
		QByteArray buf(reinterpret_cast<const char*>(bytes.data()),
		               (int)bytes.size());
		if (!c->is_connected) {
			c->pending_sends.push_back(std::move(buf));
			return;
		}
		c->ws->sendBinaryMessage(buf);
		c->msgs_sent++;
	}

	void allow_send(const void* h, bool allow) {
		if (auto* c = (client_t*)h) c->allow_send_flag = allow;
	}

	void kill_client(const void* h) {
		auto* c = (client_t*)h;
		if (!c) return;
		c->is_dead = true;
		if (c->ws) c->ws->close();
	}

	template<typename F>
	void set_on_kill(const void* h, F&& f) {
		if (auto* c = (client_t*)h) c->on_kill = std::forward<F>(f);
	}

	template<typename F>
	void set_on_message(const void* h, F&& f) {
		if (auto* c = (client_t*)h) c->on_message = std::forward<F>(f);
	}

	template<typename duration_T, typename callback_F>
	void set_timeout(duration_T&& duration, callback_F&& cb) {
		timeout_time =
			std::chrono::steady_clock::now() + duration;
		timeout_function = std::forward<callback_F>(cb);
	}

	// ------------------------------------------------------------------
	// poll(on_new_client) — drain queues, deliver messages, fire kill
	// callbacks for dead clients. Called once per next_frame from the
	// render QTimer. Qt's event loop has already run signal-slot work
	// by the time we enter this function; poll() just moves the
	// buffered incoming bytes into sync.h.
	// ------------------------------------------------------------------
	template<typename on_new_client_F>
	void poll(on_new_client_F&& on_new_client) {
		// (0) Give Qt a chance to run pending signals before we drain.
		// The render QTimer that calls us is already inside the Qt
		// event loop, so pending queued signals have run — but be
		// defensive: processEvents() with an immediate deadline drains
		// anything left over (e.g., messages that arrived between
		// event-loop cycles). Zero-timer form avoids re-entering
		// long-running work.
		QCoreApplication::processEvents(
			QEventLoop::AllEvents, 0);

		// (1) Hand off newly-connected clients. Only surface those
		// whose WS handshake completed; keep the still-connecting
		// ones on new_clients for a later poll.
		auto pending = std::move(new_clients);
		new_clients.clear();
		for (auto* c : pending) {
			if (c->is_dead) {
				// Bad URL / TLS failure — still call on_new_client
				// so the caller can install on_kill and get the
				// close notification.
				c->allow_send_flag = true;
				on_new_client(c);
			} else if (c->is_connected) {
				c->allow_send_flag = true;
				on_new_client(c);
			} else {
				new_clients.push_back(c);
			}
		}

		// (2) Drain queued messages.
		for (auto& c : clients) {
			if (!c->on_message) continue;
			while (!c->incoming.empty()) {
				QByteArray msg = std::move(c->incoming.front());
				c->incoming.pop_front();
				c->on_message(msg.constData(), msg.size());
				c->msgs_delivered++;
				if (c->is_dead) break;
			}
		}

		// (3) Fire on_kill for dead clients, once.
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

	// run_one / run_until — spin Qt's event loop briefly. Used only
	// by the observer's pre-loop connect wait, where we want to
	// block until the WS handshake completes. Once inside the render
	// QTimer we never call these — poll() is enough.
	template<typename on_new_client_F>
	void run_one(on_new_client_F&& on_new_client) {
		// Wait for at least one event, but bounded so a dead-server
		// scenario doesn't hang the caller. 10 ms is enough for the
		// TCP + TLS handshake to make progress; caller loops.
		QCoreApplication::processEvents(
			QEventLoop::AllEvents, /*maxtime_ms=*/10);
		poll(std::forward<on_new_client_F>(on_new_client));
	}
	template<typename on_new_client_F, typename pred_F>
	void run_until(on_new_client_F&& on_new_client, pred_F&& pred) {
		// Bounded loop so a hanging server can't hard-freeze the
		// caller: 5 s total budget in 10 ms slices.
		auto deadline = std::chrono::steady_clock::now()
			+ std::chrono::seconds(5);
		while (!pred() && std::chrono::steady_clock::now() < deadline) {
			run_one(on_new_client);
		}
		poll(std::forward<on_new_client_F>(on_new_client));
	}
};

} // namespace bwgame

#endif  // BWGAME_SYNC_SERVER_QT_WS_H
