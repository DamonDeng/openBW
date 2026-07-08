// User registry + API key verification. Used by:
//   - sync.h observer handshake (task #18)
//   - future HTTP control API (task #9)
//   - future WebSocket agent protocol (task #10)
//
// Load a users.json file at startup. Server hashes each api_key on load
// and discards the plaintext; only sha256(key) lives in memory. Verify by
// hashing the presented key and constant-time comparing to stored hashes.
//
// Roles:
//   role="player"    (default when "slot" is set)  -> can control that slot
//   role="observer"  spectator only; assigned_slot is which perspective to view
//   role="admin"     control-plane privileges
//
// A single user CAN be both a player and an observer of their own slot --
// role="player" with slot N implies "observes slot N" for viewer clients
// authenticated with the same key.

#ifndef OPENBW_AUTH_H
#define OPENBW_AUTH_H

#include "sha256.h"

#include "../deps/nlohmann/json.hpp"

#include <algorithm>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace openbw_auth {

enum class role_t {
	player,      // can control assigned_slot; observes assigned_slot by default
	observer,    // spectator only; assigned_slot is the perspective
	admin,       // control plane (start/pause/reset games, manage users)
};

struct user_t {
	std::string alias;
	sha256::digest_t api_key_hash;
	int assigned_slot = -1;  // 0..7 for player perspective; -1 = full vision
	role_t role = role_t::observer;

	bool can_control_slot(int slot) const {
		return role == role_t::player && assigned_slot == slot;
	}
	bool can_administer() const { return role == role_t::admin; }
};

// Timing-attack-safe byte compare.
inline bool ct_eq(const sha256::digest_t& a, const sha256::digest_t& b) {
	uint8_t diff = 0;
	for (size_t i = 0; i < a.size(); ++i) diff |= a[i] ^ b[i];
	return diff == 0;
}

class user_registry {
public:
	// Load a users.json file. Throws std::runtime_error on any parse error
	// or malformed entry. Returns the number of users loaded.
	size_t load_file(const std::string& path) {
		std::ifstream in(path);
		if (!in) throw std::runtime_error("auth: cannot open " + path);
		std::stringstream ss;
		ss << in.rdbuf();
		return load_string(ss.str(), path);
	}

	size_t load_string(const std::string& content, const std::string& source_label = "<inline>") {
		nlohmann::json j;
		try {
			j = nlohmann::json::parse(content);
		} catch (const std::exception& e) {
			throw std::runtime_error("auth: " + source_label + ": json parse: " + e.what());
		}
		if (!j.contains("users") || !j["users"].is_array()) {
			throw std::runtime_error("auth: " + source_label + ": missing 'users' array");
		}
		size_t added = 0;
		for (const auto& jv : j["users"]) {
			user_t u;
			if (!jv.contains("alias") || !jv["alias"].is_string())
				throw std::runtime_error("auth: user missing 'alias'");
			u.alias = jv["alias"].get<std::string>();
			if (!jv.contains("api_key") || !jv["api_key"].is_string())
				throw std::runtime_error("auth: user '" + u.alias + "' missing 'api_key'");
			auto key = jv["api_key"].get<std::string>();
			u.api_key_hash = sha256::hash(key.data(), key.size());

			// role: explicit "admin"/"observer" or implied "player" when
			// "slot" is present.
			bool has_role_field = jv.contains("role") && jv["role"].is_string();
			bool has_slot_field = jv.contains("slot") && jv["slot"].is_number_integer();
			if (has_role_field) {
				auto r = jv["role"].get<std::string>();
				if (r == "player") u.role = role_t::player;
				else if (r == "observer") u.role = role_t::observer;
				else if (r == "admin") u.role = role_t::admin;
				else throw std::runtime_error("auth: user '" + u.alias + "': unknown role '" + r + "'");
			} else if (has_slot_field) {
				u.role = role_t::player;
			} else {
				u.role = role_t::observer;
			}
			if (has_slot_field) {
				int slot = jv["slot"].get<int>();
				if (slot < -1 || slot > 7)
					throw std::runtime_error("auth: user '" + u.alias + "': slot out of range");
				u.assigned_slot = slot;
			}

			// Alias uniqueness check.
			for (const auto& existing : users_) {
				if (existing.alias == u.alias)
					throw std::runtime_error("auth: duplicate alias '" + u.alias + "'");
			}
			users_.push_back(std::move(u));
			++added;
		}
		return added;
	}

	// Verify a raw API key. Returns nullptr if unknown.
	const user_t* verify(std::string_view key) const {
		auto h = sha256::hash(key.data(), key.size());
		// Constant-time linear scan.
		const user_t* found = nullptr;
		for (const auto& u : users_) {
			if (ct_eq(u.api_key_hash, h)) found = &u;
		}
		return found;
	}

	size_t size() const { return users_.size(); }
	const std::vector<user_t>& users() const { return users_; }

private:
	std::vector<user_t> users_;
};

} // namespace openbw_auth

#endif
