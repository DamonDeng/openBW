// WebSocket transport for sync.h.
//
// Same API contract as sync_server_asio_tcp.h (bind, connect, new_message,
// send_message, poll, ...) so sync.h doesn't care which transport it runs
// on. Wire format is RFC 6455 WebSocket frames instead of raw TCP with a
// u16 length prefix.
//
// - Server side: accepts HTTP-upgrade connections at ws://host:port/PATH?key=API_KEY.
//   PATH is fixed per instance (default "/observer"); API_KEY is validated
//   against a user_registry* injected by the server. Once upgraded, each
//   sync.h message travels as one WebSocket BINARY frame (opcode 0x2).
//   Server-sent frames are unmasked; client-sent frames MUST be masked
//   per spec, which we enforce.
//
// - Client side: connects to ws://host:port/PATH?key=API_KEY. Sends the
//   HTTP upgrade request, verifies the 101 response's Sec-WebSocket-Accept,
//   then transitions to frame mode. Client-sent frames are masked; the
//   server-sent frames it receives are unmasked (unmasked-server-frame
//   is required by the spec).
//
// Non-goals: text frames, fragmented messages, permessage-deflate, TLS.
// Sync.h payloads are opaque byte blobs so binary is what we want.
//
// This file duplicates a fair amount of sync_server_asio_socket.h. Rather
// than templating that with a framing policy, we copy the send-buffer/
// message_t machinery and the client_t struct verbatim, and swap out the
// two framing-specific pieces: send_send_queue (encodes an outgoing WS
// frame) and read_handler (decodes incoming WS frames).

#ifndef BWGAME_SYNC_SERVER_ASIO_WS_H
#define BWGAME_SYNC_SERVER_ASIO_WS_H

#include "util.h"
#include "data_loading.h"

#define ASIO_STANDALONE
#include "deps/asio/asio.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <random>
#include <sstream>
#include <string>

namespace bwgame {

// ============================================================================
// Local SHA-1 + base64 for WebSocket handshake. Duplicates the ones in
// server/ws_server.h intentionally: this file is included by both the
// server binary (which also includes ws_server.h) and the observer
// binary (which does not). Keeping a private copy here avoids leaking
// the server-only ws_server.h into observer builds.
// ============================================================================
namespace ws_hs {

struct sha1_hasher {
	uint32_t s[5]{0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
	uint8_t buf[64]{};
	size_t buf_len = 0;
	uint64_t total = 0;

	static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

	void process(const uint8_t* block) {
		uint32_t w[80];
		for (int i = 0; i < 16; ++i) {
			w[i] = ((uint32_t)block[i * 4] << 24) |
			       ((uint32_t)block[i * 4 + 1] << 16) |
			       ((uint32_t)block[i * 4 + 2] << 8) |
			       ((uint32_t)block[i * 4 + 3]);
		}
		for (int i = 16; i < 80; ++i)
			w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
		uint32_t a = s[0], b = s[1], c = s[2], d = s[3], e = s[4];
		for (int i = 0; i < 80; ++i) {
			uint32_t f, k;
			if (i < 20)      { f = (b & c) | (~b & d); k = 0x5A827999; }
			else if (i < 40) { f = b ^ c ^ d;          k = 0x6ED9EBA1; }
			else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
			else             { f = b ^ c ^ d;          k = 0xCA62C1D6; }
			uint32_t t = rol(a, 5) + f + e + k + w[i];
			e = d; d = c; c = rol(b, 30); b = a; a = t;
		}
		s[0] += a; s[1] += b; s[2] += c; s[3] += d; s[4] += e;
	}

	void update(const uint8_t* data, size_t len) {
		total += len;
		while (len > 0) {
			size_t take = std::min(len, size_t{64} - buf_len);
			std::memcpy(buf + buf_len, data, take);
			buf_len += take;
			data += take;
			len -= take;
			if (buf_len == 64) { process(buf); buf_len = 0; }
		}
	}

	void finalize(uint8_t out[20]) {
		uint64_t bits = total * 8;
		buf[buf_len++] = 0x80;
		if (buf_len > 56) {
			while (buf_len < 64) buf[buf_len++] = 0;
			process(buf); buf_len = 0;
		}
		while (buf_len < 56) buf[buf_len++] = 0;
		for (int i = 7; i >= 0; --i) buf[buf_len++] = (uint8_t)(bits >> (i * 8));
		process(buf);
		for (int i = 0; i < 5; ++i) {
			out[i * 4 + 0] = (uint8_t)(s[i] >> 24);
			out[i * 4 + 1] = (uint8_t)(s[i] >> 16);
			out[i * 4 + 2] = (uint8_t)(s[i] >> 8);
			out[i * 4 + 3] = (uint8_t)(s[i]);
		}
	}
};

inline std::string base64_encode(const uint8_t* data, size_t len) {
	static const char alphabet[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve(((len + 2) / 3) * 4);
	for (size_t i = 0; i < len; i += 3) {
		uint32_t v = (uint32_t)data[i] << 16;
		if (i + 1 < len) v |= (uint32_t)data[i + 1] << 8;
		if (i + 2 < len) v |= (uint32_t)data[i + 2];
		out += alphabet[(v >> 18) & 0x3f];
		out += alphabet[(v >> 12) & 0x3f];
		out += (i + 1 < len) ? alphabet[(v >> 6) & 0x3f] : '=';
		out += (i + 2 < len) ? alphabet[v & 0x3f] : '=';
	}
	return out;
}

inline std::string sec_websocket_accept(const std::string& client_key) {
	// Per RFC 6455 §4.2.2: server appends the well-known GUID and returns
	// base64(SHA1(concat)).
	std::string magic = client_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	sha1_hasher h;
	h.update((const uint8_t*)magic.data(), magic.size());
	uint8_t digest[20];
	h.finalize(digest);
	return base64_encode(digest, 20);
}

// case-insensitive header lookup on an HTTP request/response text.
inline std::string get_header(const std::string& req, const std::string& name) {
	auto lower = [](unsigned char c) { return (char)std::tolower(c); };
	std::string want_lc; want_lc.reserve(name.size());
	for (unsigned char c : name) want_lc += lower(c);
	for (size_t i = 0; i + name.size() + 1 < req.size(); ++i) {
		if (req[i] != '\n' && !(i == 0)) continue;
		size_t start = (i == 0) ? 0 : i + 1;
		if (start + name.size() + 1 > req.size()) break;
		bool ok = true;
		for (size_t j = 0; j < name.size(); ++j) {
			if (lower((unsigned char)req[start + j]) != want_lc[j]) { ok = false; break; }
		}
		if (!ok) continue;
		if (req[start + name.size()] != ':') continue;
		size_t vstart = start + name.size() + 1;
		while (vstart < req.size() && (req[vstart] == ' ' || req[vstart] == '\t')) ++vstart;
		size_t vend = vstart;
		while (vend < req.size() && req[vend] != '\r' && req[vend] != '\n') ++vend;
		return req.substr(vstart, vend - vstart);
	}
	return {};
}

// Parse the path + optional query string from the request line
// "GET /path[?query] HTTP/1.1\r\n".
inline void parse_request_line(const std::string& req,
                               std::string& method_out,
                               std::string& path_out,
                               std::string& query_out) {
	auto eol = req.find("\r\n");
	if (eol == std::string::npos) return;
	std::string line = req.substr(0, eol);
	auto sp = line.find(' ');
	if (sp == std::string::npos) return;
	method_out = line.substr(0, sp);
	auto sp2 = line.find(' ', sp + 1);
	if (sp2 == std::string::npos) return;
	std::string full_path = line.substr(sp + 1, sp2 - sp - 1);
	auto q = full_path.find('?');
	if (q == std::string::npos) {
		path_out = full_path; query_out.clear();
	} else {
		path_out = full_path.substr(0, q);
		query_out = full_path.substr(q + 1);
	}
}

// naive key=value&key=value parsing; returns "" if key not present.
inline std::string query_get(const std::string& qs, const std::string& key) {
	size_t i = 0;
	while (i < qs.size()) {
		auto eq = qs.find('=', i);
		auto amp = qs.find('&', i);
		if (eq == std::string::npos || (amp != std::string::npos && eq > amp)) {
			i = (amp == std::string::npos) ? qs.size() : amp + 1;
			continue;
		}
		std::string k = qs.substr(i, eq - i);
		std::string v = qs.substr(eq + 1,
			(amp == std::string::npos ? qs.size() : amp) - eq - 1);
		if (k == key) return v;
		i = (amp == std::string::npos) ? qs.size() : amp + 1;
	}
	return {};
}

} // namespace ws_hs

// ============================================================================
// Auth hook signature. The server binary injects a lambda that looks up
// the API key in its user_registry; the observer binary passes nullptr
// (no server-side auth on the client itself). Return true to accept.
// ============================================================================
using ws_auth_fn = std::function<bool(const std::string& api_key)>;

// ============================================================================
// The transport.
// ============================================================================
struct sync_server_asio_ws {

	asio::io_service io_service;
	asio::io_service::work work{io_service};
	asio::steady_timer timer{io_service};

	// Server-side config. If set, incoming HTTP upgrades whose ?key= query
	// param fails this predicate are rejected with 401. If unset (client-
	// only usage), auth is skipped.
	ws_auth_fn auth_fn;
	// Path the server accepts on. Requests to any other path get 404.
	// The observer variant sets this to "/observer".
	//
	// If left empty (""), any path is accepted. Used when the server
	// sits behind an ALB that path-multiplexes to a specific port
	// (e.g. /game/{id}/observer -> container port 6114) — the ALB has
	// already gated by path, so the server accepting any path is
	// safe and lets us skip synchronizing path strings across the
	// two layers.
	std::string server_path = "/observer";

	// Same async_handle machinery as the raw TCP transport.
	template<typename T, typename release_F>
	struct async_handle_t {
		T* obj;
		release_F release_f;
		async_handle_t(T* obj, release_F&& release_f) : obj(obj), release_f(release_f) {
			++obj->async_count;
		}
		~async_handle_t() {
			if (!--obj->async_count) release_f(obj);
		}
		async_handle_t(const async_handle_t& n) : obj(n.obj), release_f(n.release_f) {
			++obj->async_count;
		}
		operator T*() const { return obj; }
		T* get() const { return obj; }
	};
	template<typename T, typename release_F>
	async_handle_t<T, release_F> async_handle(T* obj, release_F&& f) {
		return async_handle_t<T, release_F>(obj, std::forward<release_F>(f));
	}

	// Message assembly buffers. Sync-message payloads may span multiple
	// segments; we allocate large slabs (0x2000) and pack messages into
	// them. Each message keeps refs on the slabs it touched via
	// message_buffer_handle so the slab is only recycled when nothing
	// references it.
	struct send_buffer_t {
		std::array<uint8_t, 0x2000> buffer;
		int refcount = 0;
		size_t pos = 0;
	};
	using send_buffers_t = a_list<send_buffer_t>;
	send_buffers_t send_buffers;

	struct message_buffer_handle {
		sync_server_asio_ws* server = nullptr;
		typename send_buffers_t::iterator buffer;
		size_t offset = 0;
		size_t size = 0;
		message_buffer_handle() = default;
		message_buffer_handle(sync_server_asio_ws& server, typename send_buffers_t::iterator buffer)
			: server(&server), buffer(buffer) { ++buffer->refcount; }
		message_buffer_handle(const message_buffer_handle& n)
			: server(n.server), buffer(n.buffer), offset(n.offset), size(n.size) {
			if (server) ++buffer->refcount;
		}
		message_buffer_handle& operator=(const message_buffer_handle& n) {
			server = n.server; buffer = n.buffer; offset = n.offset; size = n.size;
			if (server) ++buffer->refcount;
			return *this;
		}
		~message_buffer_handle() {
			if (server && --buffer->refcount == 0) {
				server->send_buffers.splice(server->send_buffers.begin(),
					server->send_buffers, buffer);
			}
		}
	};

	// Where a client is in its connection lifecycle:
	// - handshake: reading/sending HTTP; will not accept/send frames.
	// - open: fully upgraded; frames flow both ways.
	// - closing: peer sent close or we're tearing down.
	enum class ws_state { handshake, open, closing };

	struct client_t {
		client_t(asio::ip::tcp::socket socket) : socket(std::move(socket)) {}
		typename a_list<client_t>::iterator my_it;
		asio::ip::tcp::socket socket;
		int async_count = 0;

		// True iff this client_t was made by connect() (client role).
		// Server-role clients accept the peer's mask on incoming frames
		// and MUST NOT mask outgoing frames; client-role does the reverse.
		bool is_client_side = false;

		ws_state state = ws_state::handshake;
		// Handshake buffers.
		std::string http_recv;                 // accumulates until \r\n\r\n
		std::string outgoing_client_key;       // client role: our own Sec-WebSocket-Key

		// Frame parser state (post-handshake). We accumulate raw bytes
		// and pop complete frames as they arrive.
		a_vector<uint8_t> recv_buffer;

		// Held payload of the current message being decoded (concatenated
		// across multi-fragment messages, if any).
		a_vector<uint8_t> current_message;

		// Outgoing send queue -- each entry is one WS frame already framed.
		// Each entry owns its byte buffer via shared_ptr so the buffer
		// outlives partial writes (asio may complete a write_some with
		// bytes_transferred < size, and we resubmit for the remainder).
		//
		// Was a_deque<message_buffer_handle> that pointed into fixed 8 KB
		// slabs; that structure silently corrupted memory for any WS
		// frame > 8 KB (header + payload combined). See the 2026-07-11
		// "message_t: too much data" incident. The vector-per-frame design
		// scales to any single-message size the game will ever emit
		// (catchup bundles, insync-check payloads, agent-action bursts).
		struct outgoing_frame_t {
			std::shared_ptr<a_vector<uint8_t>> bytes;
			size_t offset = 0;  // advanced by partial writes
			size_t remaining() const { return bytes->size() - offset; }
			const uint8_t* data() const { return bytes->data() + offset; }
		};
		a_deque<outgoing_frame_t> send_queue;

		bool is_dead = false;
		std::function<void()> on_kill;
		std::function<void(const void*, size_t)> on_message;
		bool allow_send = false;
	};

	a_list<client_t> clients;
	// Freshly-accepted / freshly-connected clients that finished their
	// WS handshake this tick; will be reported to sync.h on the next
	// poll()/run_one()/run_until() call.
	a_vector<client_t*> new_clients;

	typename send_buffers_t::iterator get_send_buffer_with_space(size_t n) {
		for (auto i = send_buffers.begin(); i != send_buffers.end(); ++i) {
			if (i->buffer.size() - i->pos >= n) return i;
		}
		send_buffers.emplace_back();
		return std::prev(send_buffers.end());
	}

	struct message_t {
		sync_server_asio_ws& server;
		// Was 2 (16 KB max per message). Late-join catchup bundles carry
		// the full replay-action history and grow linearly with game
		// length; at ~15 min of play they routinely cross 16 KB and the
		// server crashes with "message_t: too much data". 16 slabs =
		// 128 KB max message, enough for ~2 hours of gameplay before we
		// need to think harder (paginate the catchup bundle across
		// frames, ship deltas, etc.). See the "message_t: too much
		// data :(" incident 2026-07-11.
		static_vector<message_buffer_handle, 16> buffers;
		size_t total_size = 0;
		template<typename T>
		void put(T v) {
			std::array<uint8_t, sizeof(T)> buf;
			data_loading::set_value_at<true>(buf.data(), v);
			put(buf.data(), buf.size());
		}
		void put(const void* data, size_t size) {
			auto* buf = &*buffers.back().buffer;
			size_t left = buf->buffer.size() - buf->pos;
			if (left >= size) {
				memcpy(buf->buffer.data() + buf->pos, data, size);
				buf->pos += size;
				buffers.back().size += size;
				total_size += size;
			} else {
				memcpy(buf->buffer.data() + buf->pos, data, left);
				buf->pos += left;
				buffers.back().size += left;
				total_size += left;
				if (buffers.size() == buffers.max_size())
					error("message_t: too much data :(");
				buffers.emplace_back(server, server.get_send_buffer_with_space(size - left));
				buffers.back().offset = buffers.back().buffer->pos;
				put((const void*)((const char*)data + left), size - left);
			}
		}
	};

	// Unlike the raw-TCP transport, we do NOT reserve 2 header bytes at
	// the front of the message buffer -- WS framing happens per-outgoing-
	// send. The initial `put<uint16_t>(0)` in sync_server_asio_socket was
	// the raw-TCP length prefix; we drop it and just return an empty
	// message ready for payload writes.
	message_t new_message() {
		message_t r{*this};
		auto buffer = get_send_buffer_with_space(0x10);
		r.buffers.emplace_back(*this, buffer);
		r.buffers.back().offset = buffer->pos;
		return r;
	}

	// Serialize `msg` into a fully-framed WS binary frame, then push the
	// frame bytes onto the client's send_queue as a fresh vector-owning
	// entry. Server-role frames are unmasked; client-role frames are
	// masked (per RFC 6455 §5.1).
	void enqueue_ws_frame(const message_t& msg, client_t* client) {
		size_t payload_len = msg.total_size;
		size_t header_len =
			2 +
			(payload_len < 126 ? 0 : (payload_len < 65536 ? 2 : 8)) +
			(client->is_client_side ? 4 : 0);
		size_t total = header_len + payload_len;

		// One a_vector per outgoing frame; sized exactly to fit. shared_ptr
		// so it survives partial writes (write_handler resubmits until
		// remaining()==0). Grows with payload — no static caps.
		auto buf = std::make_shared<a_vector<uint8_t>>();
		buf->resize(total);
		uint8_t* p = buf->data();

		// FIN + binary opcode
		p[0] = 0x82;
		size_t hi = 1;
		if (payload_len < 126) {
			p[1] = (uint8_t)payload_len;
			hi = 2;
		} else if (payload_len < 65536) {
			p[1] = 126;
			p[2] = (uint8_t)(payload_len >> 8);
			p[3] = (uint8_t)payload_len;
			hi = 4;
		} else {
			p[1] = 127;
			for (int i = 0; i < 8; ++i) p[2 + i] = (uint8_t)(payload_len >> ((7 - i) * 8));
			hi = 10;
		}

		uint8_t mask_bytes[4] = {0, 0, 0, 0};
		if (client->is_client_side) {
			p[1] |= 0x80; // MASK bit
			// Ephemeral random mask. Not security-relevant; RFC just
			// requires it be non-predictable enough that the browser
			// intermediaries can't tamper. std::mt19937 is fine.
			static thread_local std::mt19937 rng(std::random_device{}());
			for (int i = 0; i < 4; ++i) {
				mask_bytes[i] = (uint8_t)(rng() & 0xff);
				p[hi + i] = mask_bytes[i];
			}
			hi += 4;
		}

		// Copy the payload out of the message's slab handles, applying
		// the mask if we're the client. Message body can span up to
		// message_t::buffers.max_size() input slabs; that path is
		// well-behaved because message_t::put() emplaces new slabs on
		// overflow.
		size_t written = 0;
		for (auto& b : msg.buffers) {
			const uint8_t* src = b.buffer->buffer.data() + b.offset;
			for (size_t i = 0; i < b.size; ++i) {
				uint8_t byte = src[i];
				if (client->is_client_side) byte ^= mask_bytes[written & 3];
				p[hi + written] = byte;
				++written;
			}
		}

		client_t::outgoing_frame_t out;
		out.bytes = std::move(buf);
		out.offset = 0;

		client->send_queue.push_back(std::move(out));
		if (client->send_queue.size() == 1) send_send_queue(client);
	}

	void write_handler(client_t* c, const asio::error_code& ec, size_t bytes_transferred) {
		if (ec) {
			if (c->on_kill) c->on_kill();
		} else {
			auto& v = c->send_queue.front();
			if (bytes_transferred > v.remaining()) error("write_handler: bytes_transferred > remaining");
			v.offset += bytes_transferred;
			if (v.remaining() == 0) c->send_queue.pop_front();
			if (!c->send_queue.empty()) send_send_queue(c);
		}
	}

	void send_send_queue(client_t* client) {
		auto& v = client->send_queue.front();
		client->socket.async_write_some(
			asio::buffer(v.data(), v.remaining()),
			std::bind(&sync_server_asio_ws::write_handler, this,
				async_handle(client, std::bind(&sync_server_asio_ws::async_release, this, std::placeholders::_1)),
				std::placeholders::_1, std::placeholders::_2));
	}

	// send_to for a single client_t*; broadcast is handled by send_message.
	void send_to(const message_t& d, client_t* client) {
		if (!client->allow_send) return;
		if (client->state != ws_state::open) return;
		enqueue_ws_frame(d, client);
	}

	void allow_send(const void* h, bool allow) {
		((client_t*)h)->allow_send = allow;
	}

	void send_message(const message_t& d, const void* h) {
		if (h) {
			send_to(d, (client_t*)h);
		} else {
			for (auto& v : clients) send_to(d, &v);
		}
	}

	void new_connection_handler(asio::ip::tcp::socket socket, bool is_client_side) {
		clients.emplace_back(std::move(socket));
		client_t* c = &clients.back();
		c->my_it = std::prev(clients.end());
		c->is_client_side = is_client_side;
		++c->async_count;
		// Start the HTTP handshake. Server accepts, client initiates.
		if (is_client_side) {
			start_client_handshake(c);
		} else {
			start_server_handshake(c);
		}
	}

	void kill_client(const void* h) {
		client_t* c = (client_t*)h;
		c->is_dead = true;
		c->on_kill = {};
		c->on_message = {};
		if (c->socket.is_open()) c->socket.close();
		if (--c->async_count == 0) async_release(c);
	}

	template<typename duration_T, typename callback_F>
	void set_timeout(duration_T&& duration, callback_F&& callback) {
		timer.expires_from_now(duration);
		timer.async_wait([callback = std::forward<callback_F>(callback)](const asio::error_code& ec) {
			if (!ec) callback();
		});
	}

	void async_release(client_t* c) {
		clients.erase(c->my_it);
	}

	// ------------------------------------------------------------------
	// HTTP handshake (server side).
	// ------------------------------------------------------------------
	void start_server_handshake(client_t* c) {
		read_http_more(c);
	}

	void read_http_more(client_t* c) {
		c->recv_buffer.resize(0x1000);
		c->socket.async_read_some(
			asio::buffer(c->recv_buffer.data(), c->recv_buffer.size()),
			std::bind(&sync_server_asio_ws::server_handshake_read_handler, this,
				async_handle(c, std::bind(&sync_server_asio_ws::async_release, this, std::placeholders::_1)),
				std::placeholders::_1, std::placeholders::_2));
	}

	void server_handshake_read_handler(client_t* c, const asio::error_code& ec, size_t n) {
		if (ec) {
			if (c->on_kill) c->on_kill();
			return;
		}
		c->http_recv.append((const char*)c->recv_buffer.data(), n);
		auto end = c->http_recv.find("\r\n\r\n");
		if (end == std::string::npos) {
			if (c->http_recv.size() > 16384) {
				// Bogus giant request; drop.
				kill_client(c);
				return;
			}
			read_http_more(c);
			return;
		}
		std::string req = c->http_recv.substr(0, end + 4);
		std::string leftover = c->http_recv.substr(end + 4);

		// Parse request line + relevant headers.
		std::string method, path, query;
		ws_hs::parse_request_line(req, method, path, query);
		std::string sec_key = ws_hs::get_header(req, "Sec-WebSocket-Key");
		std::string api_key = ws_hs::query_get(query, "key");

		auto reject = [&](const char* status) {
			std::string body = std::string("HTTP/1.1 ") + status + "\r\n"
				"Content-Length: 0\r\n\r\n";
			auto self = shared_from_this_via_client(c);
			auto buf = std::make_shared<std::string>(std::move(body));
			asio::async_write(c->socket, asio::buffer(*buf),
				[this, c, buf](const asio::error_code&, size_t) {
					kill_client(c);
				});
		};

		if (method != "GET" || sec_key.empty() ||
		    ws_hs::get_header(req, "Upgrade").find("websocket") == std::string::npos) {
			reject("400 Bad Request");
			return;
		}
		if (!server_path.empty() && path != server_path) {
			reject("404 Not Found");
			return;
		}
		if (auth_fn && !auth_fn(api_key)) {
			reject("401 Unauthorized");
			return;
		}

		std::string accept = ws_hs::sec_websocket_accept(sec_key);
		std::string response =
			"HTTP/1.1 101 Switching Protocols\r\n"
			"Upgrade: websocket\r\n"
			"Connection: Upgrade\r\n"
			"Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
		auto buf = std::make_shared<std::string>(std::move(response));
		asio::async_write(c->socket, asio::buffer(*buf),
			[this, c, buf, leftover](const asio::error_code& ec, size_t) {
				if (ec) { kill_client(c); return; }
				c->state = ws_state::open;
				c->http_recv.clear();
				c->recv_buffer.clear();
				new_clients.push_back(c);
				if (!leftover.empty()) {
					c->recv_buffer.insert(c->recv_buffer.end(),
						leftover.begin(), leftover.end());
					parse_ws_frames(c);
				}
				start_ws_read(c);
			});
	}

	// ------------------------------------------------------------------
	// HTTP handshake (client side, used by the observer).
	// ------------------------------------------------------------------
	std::string client_url_path = "/observer";
	std::string client_api_key;
	std::string client_host_header;

	void start_client_handshake(client_t* c) {
		// Build a random Sec-WebSocket-Key: 16 random bytes, base64.
		uint8_t rand_bytes[16];
		{
			static thread_local std::mt19937 rng(std::random_device{}());
			for (int i = 0; i < 16; ++i) rand_bytes[i] = (uint8_t)(rng() & 0xff);
		}
		c->outgoing_client_key = ws_hs::base64_encode(rand_bytes, 16);

		std::string path = client_url_path;
		if (!client_api_key.empty()) {
			path += (path.find('?') == std::string::npos ? "?" : "&");
			path += "key=";
			path += client_api_key;
		}

		std::ostringstream oss;
		oss << "GET " << path << " HTTP/1.1\r\n"
		    << "Host: " << (client_host_header.empty() ? "localhost" : client_host_header) << "\r\n"
		    << "Upgrade: websocket\r\n"
		    << "Connection: Upgrade\r\n"
		    << "Sec-WebSocket-Key: " << c->outgoing_client_key << "\r\n"
		    << "Sec-WebSocket-Version: 13\r\n"
		    << "\r\n";

		auto req = std::make_shared<std::string>(oss.str());
		asio::async_write(c->socket, asio::buffer(*req),
			[this, c, req](const asio::error_code& ec, size_t) {
				if (ec) { if (c->on_kill) c->on_kill(); return; }
				client_handshake_read(c);
			});
	}

	void client_handshake_read(client_t* c) {
		c->recv_buffer.resize(0x1000);
		c->socket.async_read_some(
			asio::buffer(c->recv_buffer.data(), c->recv_buffer.size()),
			std::bind(&sync_server_asio_ws::client_handshake_read_handler, this,
				async_handle(c, std::bind(&sync_server_asio_ws::async_release, this, std::placeholders::_1)),
				std::placeholders::_1, std::placeholders::_2));
	}

	void client_handshake_read_handler(client_t* c, const asio::error_code& ec, size_t n) {
		if (ec) { if (c->on_kill) c->on_kill(); return; }
		c->http_recv.append((const char*)c->recv_buffer.data(), n);
		auto end = c->http_recv.find("\r\n\r\n");
		if (end == std::string::npos) {
			if (c->http_recv.size() > 16384) { kill_client(c); return; }
			client_handshake_read(c);
			return;
		}
		std::string resp = c->http_recv.substr(0, end + 4);
		std::string leftover = c->http_recv.substr(end + 4);

		// Validate 101 status and Sec-WebSocket-Accept.
		if (resp.compare(0, 12, "HTTP/1.1 101") != 0) {
			// Server rejected; drop.
			kill_client(c);
			return;
		}
		std::string accept = ws_hs::get_header(resp, "Sec-WebSocket-Accept");
		std::string expected = ws_hs::sec_websocket_accept(c->outgoing_client_key);
		if (accept != expected) {
			kill_client(c);
			return;
		}
		c->state = ws_state::open;
		c->http_recv.clear();
		c->recv_buffer.clear();
		new_clients.push_back(c);
		if (!leftover.empty()) {
			c->recv_buffer.insert(c->recv_buffer.end(), leftover.begin(), leftover.end());
			parse_ws_frames(c);
		}
		start_ws_read(c);
	}

	// ------------------------------------------------------------------
	// Frame-mode read loop.
	// ------------------------------------------------------------------
	void start_ws_read(client_t* c) {
		size_t old = c->recv_buffer.size();
		c->recv_buffer.resize(old + 0x1000);
		c->socket.async_read_some(
			asio::buffer(c->recv_buffer.data() + old, 0x1000),
			std::bind(&sync_server_asio_ws::ws_read_handler, this,
				async_handle(c, std::bind(&sync_server_asio_ws::async_release, this, std::placeholders::_1)),
				std::placeholders::_1, std::placeholders::_2, old));
	}

	void ws_read_handler(client_t* c, const asio::error_code& ec, size_t n, size_t old) {
		if (ec) {
			if (c->on_kill) c->on_kill();
			return;
		}
		c->recv_buffer.resize(old + n);
		parse_ws_frames(c);
		if (c->state == ws_state::open) start_ws_read(c);
	}

	// Decode as many complete WS frames as we have buffered; deliver each
	// to on_message. Handles PING (opcode 0x9) by echoing PONG, drops
	// PONG (0xA) silently, and closes on CLOSE (0x8). Fragmented messages
	// (opcode 0x0 continuation) accumulate into current_message.
	void parse_ws_frames(client_t* c) {
		while (c->recv_buffer.size() >= 2) {
			uint8_t b0 = c->recv_buffer[0];
			uint8_t b1 = c->recv_buffer[1];
			bool fin = (b0 & 0x80) != 0;
			uint8_t opcode = b0 & 0x0f;
			bool masked = (b1 & 0x80) != 0;
			uint64_t plen = b1 & 0x7f;
			size_t hi = 2;
			if (plen == 126) {
				if (c->recv_buffer.size() < 4) return;
				plen = ((uint16_t)c->recv_buffer[2] << 8) | c->recv_buffer[3];
				hi = 4;
			} else if (plen == 127) {
				if (c->recv_buffer.size() < 10) return;
				plen = 0;
				for (int i = 0; i < 8; ++i) plen = (plen << 8) | c->recv_buffer[2 + i];
				hi = 10;
			}
			if (masked) hi += 4;
			if (c->recv_buffer.size() < hi + plen) return;

			// Server-side MUST receive masked, client-side MUST receive unmasked.
			if (!c->is_client_side && !masked) {
				// Spec violation from peer; close.
				kill_client(c);
				return;
			}
			if (c->is_client_side && masked) {
				kill_client(c);
				return;
			}

			// Unmask (if applicable) into the payload buffer.
			a_vector<uint8_t> payload(plen);
			if (masked) {
				uint8_t mask[4] = {
					c->recv_buffer[hi - 4], c->recv_buffer[hi - 3],
					c->recv_buffer[hi - 2], c->recv_buffer[hi - 1]
				};
				for (size_t i = 0; i < plen; ++i) {
					payload[i] = c->recv_buffer[hi + i] ^ mask[i & 3];
				}
			} else {
				for (size_t i = 0; i < plen; ++i) {
					payload[i] = c->recv_buffer[hi + i];
				}
			}
			// Discard the consumed bytes.
			c->recv_buffer.erase(c->recv_buffer.begin(),
				c->recv_buffer.begin() + hi + plen);

			if (opcode == 0x8) {
				// close
				kill_client(c);
				return;
			} else if (opcode == 0x9) {
				// ping -> pong echo
				send_ws_control(c, 0xA, payload.data(), payload.size());
				continue;
			} else if (opcode == 0xA) {
				// pong -- ignore
				continue;
			} else if (opcode == 0x2 || opcode == 0x0) {
				// binary or continuation. Accumulate into current_message.
				if (opcode == 0x2 && !c->current_message.empty()) {
					// Unfragmented new frame arriving; drop old partial.
					c->current_message.clear();
				}
				c->current_message.insert(c->current_message.end(),
					payload.begin(), payload.end());
				if (fin) {
					if (c->on_message) {
						c->on_message(c->current_message.data(),
							c->current_message.size());
					}
					c->current_message.clear();
				}
			} else if (opcode == 0x1) {
				// text -- unexpected for sync channel. Drop silently.
			}
		}
	}

	// Send a WS control frame (close/ping/pong). Payload capped at 125
	// bytes per spec. Same vector-per-frame ownership as
	// enqueue_ws_frame; the old slab path silently corrupted memory
	// for oversized data frames (see the 2026-07-11 message_t bug).
	void send_ws_control(client_t* c, uint8_t opcode,
	                     const uint8_t* payload, size_t n) {
		if (n > 125) n = 125;
		size_t hi = 2 + (c->is_client_side ? 4 : 0);
		size_t total = hi + n;
		auto buf = std::make_shared<a_vector<uint8_t>>();
		buf->resize(total);
		uint8_t* p = buf->data();
		p[0] = (uint8_t)(0x80 | (opcode & 0x0f));
		p[1] = (uint8_t)n;
		if (c->is_client_side) {
			p[1] |= 0x80;
			static thread_local std::mt19937 rng(std::random_device{}());
			uint8_t m[4];
			for (int i = 0; i < 4; ++i) { m[i] = (uint8_t)(rng() & 0xff); p[2 + i] = m[i]; }
			for (size_t i = 0; i < n; ++i) p[6 + i] = payload[i] ^ m[i & 3];
		} else {
			for (size_t i = 0; i < n; ++i) p[2 + i] = payload[i];
		}
		client_t::outgoing_frame_t out;
		out.bytes = std::move(buf);
		out.offset = 0;
		c->send_queue.push_back(std::move(out));
		if (c->send_queue.size() == 1) send_send_queue(c);
	}

	// Shared_from_this stub -- we don't inherit enable_shared_from_this.
	// This function exists just to type-erase the client_t* to a shared
	// state (unused in current code path but kept for future callers).
	void* shared_from_this_via_client(client_t* c) { return c; }

	// ------------------------------------------------------------------
	// on_kill / on_message hooks -- same shape as sync_server_asio_socket.
	// ------------------------------------------------------------------
	template<typename F>
	void set_on_kill(const void* h, F&& f) {
		client_t* c = (client_t*)h;
		c->on_kill = std::forward<F>(f);
	}

	template<typename F>
	void set_on_message(const void* h, F&& f) {
		client_t* c = (client_t*)h;
		c->on_message = std::forward<F>(f);
		// The frame reader was started when the handshake completed; no
		// action needed here beyond installing the callback. If any
		// frames arrived before this callback was set they would have
		// been dropped, but sync.h always installs on_message inside
		// on_new_client (from poll()) before any frame can arrive.
	}

	// ------------------------------------------------------------------
	// Event loop drivers -- delegate straight to asio.
	// ------------------------------------------------------------------
	template<typename on_new_client_F>
	void poll(on_new_client_F&& on_new_client) {
		io_service.poll();
		for (auto* c : new_clients) {
			c->allow_send = true;
			on_new_client(c);
		}
		new_clients.clear();
	}

	template<typename on_new_client_F>
	void run_one(on_new_client_F&& on_new_client) {
		if (!io_service.run_one()) error("asio io_service has no work");
		for (auto* c : new_clients) {
			c->allow_send = true;
			on_new_client(c);
		}
		new_clients.clear();
	}

	template<typename on_new_client_F, typename pred_F>
	void run_until(on_new_client_F&& on_new_client, pred_F&& pred) {
		while (!pred()) run_one(on_new_client);
	}

	// ------------------------------------------------------------------
	// bind / connect wrappers (server / client side, respectively).
	// ------------------------------------------------------------------
	struct acceptor_t {
		sync_server_asio_ws& server;
		asio::ip::tcp::acceptor acceptor{server.io_service};
		asio::ip::tcp::socket socket{server.io_service};
		acceptor_t(sync_server_asio_ws& server) : server(server) {}
	};

	void accept_handler(const asio::error_code& ec, std::shared_ptr<acceptor_t> acceptor) {
		if (!ec) new_connection_handler(std::move(acceptor->socket), /*is_client_side=*/false);
		auto* a = &*acceptor;
		a->acceptor.async_accept(a->socket,
			std::bind(&sync_server_asio_ws::accept_handler, this,
				std::placeholders::_1, std::move(acceptor)));
	}

	void bind(const asio::ip::tcp::endpoint& ep) {
		auto a = std::make_shared<acceptor_t>(*this);
		auto& acceptor = a->acceptor;
		asio::error_code ec;
		acceptor.open(ep.protocol());
		acceptor.set_option(asio::socket_base::reuse_address(true));
		acceptor.bind(ep, ec);
		if (ec) return;
		acceptor.listen(asio::socket_base::max_connections, ec);
		if (ec) return;
		acceptor.async_accept(a->socket,
			std::bind(&sync_server_asio_ws::accept_handler, this,
				std::placeholders::_1, a));
	}

	void bind(const a_string& hostname, int port) {
		asio::error_code ec;
		asio::ip::address address = asio::ip::address::from_string(hostname.c_str(), ec);
		if (ec) {
			auto resolver = std::make_shared<asio::ip::tcp::resolver>(io_service);
			asio::ip::tcp::resolver::query query(hostname.c_str(), "");
			using it_t = asio::ip::tcp::resolver::iterator;
			auto* r = &*resolver;
			r->async_resolve(query,
				[this, port, resolver = std::move(resolver)]
				(const asio::error_code& ec, it_t iterator) {
					(void)ec;
					for (; iterator != it_t{}; ++iterator) {
						bind({iterator->endpoint().address(), (unsigned short)port});
					}
				});
		} else {
			bind(asio::ip::tcp::endpoint(address, port));
		}
	}

	void connect(const asio::ip::tcp::endpoint& ep) {
		auto socket = std::make_shared<asio::ip::tcp::socket>(io_service);
		auto* s = &*socket;
		s->async_connect(ep,
			[this, socket = std::move(socket)](const asio::error_code& ec) {
				if (!ec) new_connection_handler(std::move(*socket), /*is_client_side=*/true);
			});
	}

	void connect(const a_string& hostname, int port) {
		// Only default the Host header from the TCP hostname if the
		// caller hasn't set it explicitly. This lets a TLS-proxy
		// setup work: TCP hostname = 127.0.0.1 (the local proxy),
		// but the HTTP Host header must match the ALB's virtual
		// host so path-routing works on the far side.
		if (client_host_header.empty()) {
			client_host_header = std::string(hostname.c_str());
		}
		asio::error_code ec;
		asio::ip::address address = asio::ip::address::from_string(hostname.c_str(), ec);
		if (ec) {
			auto resolver = std::make_shared<asio::ip::tcp::resolver>(io_service);
			asio::ip::tcp::resolver::query query(hostname.c_str(), "");
			using it_t = asio::ip::tcp::resolver::iterator;
			auto* r = &*resolver;
			r->async_resolve(query,
				[this, port, resolver = std::move(resolver)]
				(const asio::error_code& ec, it_t iterator) {
					(void)ec;
					for (; iterator != it_t{}; ++iterator) {
						connect({iterator->endpoint().address(), (unsigned short)port});
					}
				});
		} else {
			connect({address, (unsigned short)port});
		}
	}
};

}

#endif
