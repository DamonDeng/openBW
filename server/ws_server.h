// Minimal WebSocket server on top of asio. Text frames only (no binary,
// no compression, no extensions, no permessage-deflate). Enough for the
// agent JSON protocol.
//
// One asio::io_service runs on the sim thread already; we DO NOT reuse
// it because agent WS activity would starve the sim tick loop. Instead
// the ws_server runs its own io_context on a dedicated worker thread,
// and hands validated commands into the command_queue (mutex-guarded,
// thread-safe) which the sim thread drains each tick.
//
// Auth model: every incoming connection must complete an HTTP upgrade
// where the query string carries ?key=<api_key>. The server verifies
// the key via openbw_auth::user_registry before accepting the upgrade.
// A failed key returns 401 and closes the socket.
//
// URL example:
//   ws://127.0.0.1:6113/agent?key=sk-abc123

#ifndef OPENBW_WS_SERVER_H
#define OPENBW_WS_SERVER_H

#define ASIO_STANDALONE
#include "../deps/asio/asio.hpp"

#include "auth.h"
#include "command_queue.h"
#include "observe_request.h"
#include "agent_protocol.h"

#include "../deps/nlohmann/json.hpp"

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace openbw_agents {

// ============================================================================
// SHA-1 (used only for the WebSocket handshake key). Public-domain style.
// ============================================================================
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
			w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
		uint32_t a=s[0], b=s[1], c=s[2], d=s[3], e=s[4];
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
			out[i*4+0] = (uint8_t)(s[i] >> 24);
			out[i*4+1] = (uint8_t)(s[i] >> 16);
			out[i*4+2] = (uint8_t)(s[i] >> 8);
			out[i*4+3] = (uint8_t)(s[i]);
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

// ============================================================================
// A single client connection.
// ============================================================================
struct ws_connection : std::enable_shared_from_this<ws_connection> {
	asio::ip::tcp::socket socket;
	asio::streambuf request_streambuf;
	std::string request_buf;
	std::vector<uint8_t> recv_buf;
	std::vector<uint8_t> frame_payload;
	bool handshake_done = false;
	std::string alias; // filled in after auth
	int slot = -1;

	// Injected by the server on accept.
	openbw_auth::user_registry* registry = nullptr;
	command_queue* queue = nullptr;
	observe_queue* obs_queue = nullptr;
	std::atomic<int>* server_current_frame = nullptr;
	// So the sim thread can post its serialized observation back to our
	// io_service for delivery over the wire. Owned by ws_server.
	asio::io_service* our_io = nullptr;

	explicit ws_connection(asio::ip::tcp::socket sock) : socket(std::move(sock)) {
		recv_buf.reserve(4096);
	}

	void log(const char* msg) {
		fprintf(stderr, "[ws] %s (alias=%s slot=%d)\n",
			msg, alias.empty() ? "-" : alias.c_str(), slot);
	}

	// --- Handshake ---
	void start() {
		read_http_request();
	}

	void read_http_request() {
		auto self = shared_from_this();
		asio::async_read_until(socket, request_streambuf, "\r\n\r\n",
			[self](const asio::error_code& ec, size_t n) {
				(void)n;
				if (ec) { self->log("read_http_request failed"); return; }
				// Copy the streambuf contents into a std::string for parsing.
				std::ostringstream oss;
				oss << &self->request_streambuf;
				self->request_buf = oss.str();
				self->handle_http_request();
			});
	}

	void write_and_close(const std::string& body) {
		auto self = shared_from_this();
		auto buf = std::make_shared<std::string>(body);
		asio::async_write(socket, asio::buffer(*buf),
			[self, buf](const asio::error_code&, size_t) {
				self->socket.close();
			});
	}

	// Basic header parsing: find lines and Sec-WebSocket-Key + query key.
	static std::string get_header(const std::string& req, const std::string& name) {
		std::string needle = "\r\n" + name + ":";
		auto pos = req.find(needle);
		if (pos == std::string::npos) {
			needle = "\r\n" + name + ":";
			// case-insensitive fallback
			for (size_t i = 0; i + needle.size() <= req.size(); ++i) {
				bool ok = true;
				for (size_t j = 0; j < needle.size(); ++j) {
					if (std::tolower((unsigned char)req[i + j]) !=
					    std::tolower((unsigned char)needle[j])) { ok = false; break; }
				}
				if (ok) { pos = i; break; }
			}
			if (pos == std::string::npos) return {};
		}
		pos += needle.size();
		auto end = req.find("\r\n", pos);
		if (end == std::string::npos) end = req.size();
		std::string v = req.substr(pos, end - pos);
		// trim
		size_t a = 0, b = v.size();
		while (a < b && (v[a] == ' ' || v[a] == '\t')) ++a;
		while (b > a && (v[b-1] == ' ' || v[b-1] == '\t')) --b;
		return v.substr(a, b - a);
	}

	// Extract ?key=... from request line "GET /path?key=... HTTP/1.1".
	static std::string extract_key(const std::string& req) {
		auto sp = req.find(' ');
		if (sp == std::string::npos) return {};
		auto sp2 = req.find(' ', sp + 1);
		if (sp2 == std::string::npos) return {};
		std::string path = req.substr(sp + 1, sp2 - sp - 1);
		auto q = path.find('?');
		if (q == std::string::npos) return {};
		std::string qs = path.substr(q + 1);
		// naive key=value&key=value parsing
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
			if (k == "key") return v;
			i = (amp == std::string::npos) ? qs.size() : amp + 1;
		}
		return {};
	}

	void handle_http_request() {
		// Auth: extract api key from query, verify.
		std::string api_key = extract_key(request_buf);
		if (api_key.empty()) {
			write_and_close("HTTP/1.1 400 Bad Request\r\n"
				"Content-Length: 24\r\n\r\n"
				"missing ?key=<api_key>\n");
			return;
		}
		const auto* user = registry ? registry->verify(api_key) : nullptr;
		if (!user) {
			write_and_close("HTTP/1.1 401 Unauthorized\r\n"
				"Content-Length: 12\r\n\r\n"
				"bad api key\n");
			return;
		}
		if (user->role != openbw_auth::role_t::player) {
			write_and_close("HTTP/1.1 403 Forbidden\r\n"
				"Content-Length: 28\r\n\r\n"
				"agent WS requires role=player\n");
			return;
		}
		alias = user->alias;
		slot = user->assigned_slot;
		fprintf(stderr, "[ws] auth OK: alias=%s slot=%d\n", alias.c_str(), slot);

		// WebSocket handshake: compute Sec-WebSocket-Accept.
		std::string sec_key = get_header(request_buf, "Sec-WebSocket-Key");
		if (sec_key.empty()) {
			write_and_close("HTTP/1.1 400 Bad Request\r\n"
				"Content-Length: 30\r\n\r\n"
				"missing Sec-WebSocket-Key\n");
			return;
		}
		std::string magic = sec_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
		sha1_hasher h;
		h.update((const uint8_t*)magic.data(), magic.size());
		uint8_t digest[20];
		h.finalize(digest);
		std::string accept = base64_encode(digest, 20);

		std::string response =
			"HTTP/1.1 101 Switching Protocols\r\n"
			"Upgrade: websocket\r\n"
			"Connection: Upgrade\r\n"
			"Sec-WebSocket-Accept: " + accept + "\r\n\r\n";

		auto self = shared_from_this();
		auto buf = std::make_shared<std::string>(std::move(response));
		asio::async_write(socket, asio::buffer(*buf),
			[self, buf](const asio::error_code& ec, size_t) {
				if (ec) return;
				self->handshake_done = true;
				self->send_welcome();
				self->read_frame();
			});
	}

	// --- Framing ---
	void send_welcome() {
		nlohmann::json j;
		j["type"] = "welcome";
		j["slot"] = slot;
		j["current_frame"] = server_current_frame ? server_current_frame->load() : 0;
		send_text(j.dump());
	}

	void read_frame() {
		auto self = shared_from_this();
		// Minimum WS frame header is 2 bytes; we accept up to a modest cap.
		// Read incrementally: header first, then payload.
		size_t old = recv_buf.size();
		recv_buf.resize(old + 4096);
		socket.async_read_some(asio::buffer(recv_buf.data() + old, 4096),
			[self, old](const asio::error_code& ec, size_t n) {
				if (ec) return;
				self->recv_buf.resize(old + n);
				self->process_frames();
				self->read_frame();
			});
	}

	// Very small WS frame parser: text frames only, masked (client->server
	// frames MUST be masked per RFC 6455).
	void process_frames() {
		while (recv_buf.size() >= 2) {
			uint8_t b0 = recv_buf[0];
			uint8_t b1 = recv_buf[1];
			bool fin = (b0 & 0x80) != 0;
			uint8_t opcode = b0 & 0x0f;
			bool masked = (b1 & 0x80) != 0;
			uint64_t len = b1 & 0x7f;
			size_t header = 2;
			if (len == 126) {
				if (recv_buf.size() < 4) return;
				len = ((uint16_t)recv_buf[2] << 8) | recv_buf[3];
				header = 4;
			} else if (len == 127) {
				if (recv_buf.size() < 10) return;
				len = 0;
				for (int i = 0; i < 8; ++i) len = (len << 8) | recv_buf[2 + i];
				header = 10;
			}
			if (masked) header += 4;
			if (recv_buf.size() < header + len) return;
			if (!masked) {
				// Spec violation; drop the connection.
				log("client frame not masked; closing");
				socket.close();
				return;
			}
			uint8_t mask[4] = {
				recv_buf[header - 4], recv_buf[header - 3],
				recv_buf[header - 2], recv_buf[header - 1]
			};
			frame_payload.assign(len, 0);
			for (size_t i = 0; i < len; ++i) {
				frame_payload[i] = recv_buf[header + i] ^ mask[i & 3];
			}
			recv_buf.erase(recv_buf.begin(), recv_buf.begin() + header + len);

			if (opcode == 0x8) { // close
				socket.close();
				return;
			} else if (opcode == 0x1 && fin) {
				handle_text_frame(std::string((const char*)frame_payload.data(),
					frame_payload.size()));
			}
			// ignore ping/pong/continuation for MVP
		}
	}

	void handle_text_frame(const std::string& text) {
		nlohmann::json j;
		try { j = nlohmann::json::parse(text); }
		catch (const std::exception& e) {
			send_error("", std::string("bad json: ") + e.what());
			return;
		}
		if (!j.is_object()) { send_error("", "expected json object"); return; }
		std::string type = j.value("type", "");
		std::string id   = j.value("id", "");

		if (type == "cmd") {
			handle_cmd(j, id);
		} else if (type == "observe") {
			handle_observe(j, id);
		} else {
			send_error(id, "unknown message type: " + type);
		}
	}

	void handle_cmd(const nlohmann::json& j, const std::string& id) {
		auto it = j.find("cmd");
		if (it == j.end() || !it->is_object()) {
			send_error(id, "cmd payload missing");
			return;
		}
		encoded_command blobs;
		auto err = encode_command(*it, blobs);
		if (err) {
			send_error(id, err->message);
			return;
		}
		// Push each blob into the slot queue in encoding order (select
		// first, then verb). The sim thread will call schedule_action on
		// them, preserving order within the slot.
		for (auto& b : blobs) {
			if (!queue || !queue->push(slot, std::move(b))) {
				send_error(id, "queue push failed (invalid slot?)");
				return;
			}
		}
		nlohmann::json ack;
		ack["type"] = "ack";
		ack["id"] = id;
		ack["queued_at_frame"] = server_current_frame ? server_current_frame->load() + 1 : 0;
		send_text(ack.dump());
	}

	void handle_observe(const nlohmann::json& j, const std::string& id) {
		if (!obs_queue) {
			send_error(id, "observation service not available");
			return;
		}
		observe_request req;
		req.request_id = id;
		auto tit = j.find("targets");
		if (tit != j.end() && tit->is_array()) {
			for (const auto& t : *tit) {
				if (t.is_string()) req.targets.push_back(t.get<std::string>());
			}
		}
		// Callback runs on the sim thread. Post the response to our
		// io_service so the send happens on the WS worker thread.
		auto self = shared_from_this();
		req.respond = [self](std::string body) {
			if (!self->our_io) return;
			// asio::post isn't in 1.10; use dispatch/io_service::post.
			self->our_io->post([self, body = std::move(body)]() {
				self->send_text(body);
			});
		};
		obs_queue->push(slot, std::move(req));
	}

	void send_error(const std::string& id, const std::string& message) {
		nlohmann::json j;
		j["type"] = "error";
		j["id"] = id;
		j["message"] = message;
		send_text(j.dump());
	}

	// Server -> client text frame. Not masked. Assumes single frame < 64KiB.
	void send_text(const std::string& text) {
		std::vector<uint8_t> out;
		out.push_back(0x81); // fin + text
		size_t n = text.size();
		if (n < 126) {
			out.push_back((uint8_t)n);
		} else if (n < 65536) {
			out.push_back(126);
			out.push_back((uint8_t)(n >> 8));
			out.push_back((uint8_t)(n & 0xff));
		} else {
			out.push_back(127);
			for (int i = 7; i >= 0; --i) out.push_back((uint8_t)(n >> (i * 8)));
		}
		out.insert(out.end(), text.begin(), text.end());
		auto self = shared_from_this();
		auto buf = std::make_shared<std::vector<uint8_t>>(std::move(out));
		asio::async_write(socket, asio::buffer(*buf),
			[self, buf](const asio::error_code&, size_t) {});
	}
};

// ============================================================================
// Acceptor.
// ============================================================================
class ws_server {
public:
	ws_server(openbw_auth::user_registry& reg, command_queue& q,
		observe_queue& oq, std::atomic<int>& current_frame)
		: registry(reg), queue(q), obs_queue(oq),
		  current_frame(current_frame), acceptor(io) {}

	void start(uint16_t port) {
		asio::ip::tcp::endpoint ep(asio::ip::tcp::v4(), port);
		acceptor.open(ep.protocol());
		acceptor.set_option(asio::socket_base::reuse_address(true));
		acceptor.bind(ep);
		acceptor.listen();
		accept_next();
		worker = std::thread([this]() { io.run(); });
		fprintf(stderr, "[ws] agent WebSocket server listening on 0.0.0.0:%u\n",
			(unsigned)port);
	}

	void stop() {
		acceptor.close();
		io.stop();
		if (worker.joinable()) worker.join();
	}

	~ws_server() { stop(); }

private:
	void accept_next() {
		// asio 1.10.8 style: pass a pre-created socket by reference. The
		// socket must outlive the async op, so we stash it in a shared_ptr.
		auto sock = std::make_shared<asio::ip::tcp::socket>(io);
		acceptor.async_accept(*sock,
			[this, sock](const asio::error_code& ec) {
				if (!ec) {
					auto conn = std::make_shared<ws_connection>(std::move(*sock));
					conn->registry = &registry;
					conn->queue = &queue;
					conn->obs_queue = &obs_queue;
					conn->our_io = &io;
					conn->server_current_frame = &current_frame;
					conn->start();
				}
				accept_next();
			});
	}

	openbw_auth::user_registry& registry;
	command_queue& queue;
	observe_queue& obs_queue;
	std::atomic<int>& current_frame;
	asio::io_service io;
	asio::io_service::work work{io};
	asio::ip::tcp::acceptor acceptor;
	std::thread worker;
};

} // namespace openbw_agents

#endif
