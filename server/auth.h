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

			// Duplicate-alias policy: same alias may appear multiple
			// times to bind multiple API keys to one identity — but the
			// (role, assigned_slot) triple must match, else it's a
			// definition conflict.
			for (const auto& existing : users_) {
				if (existing.alias == u.alias) {
					if (existing.role != u.role || existing.assigned_slot != u.assigned_slot) {
						throw std::runtime_error(
							"auth: alias '" + u.alias
							+ "' has conflicting role/slot across entries");
					}
				}
			}
			users_.push_back(std::move(u));
			++added;
		}
		return added;
	}

	// Add a single user from an inline "alias:api_key:role[:slot]" spec.
	// Used by the server's --user CLI flag so a control plane (k8s, EKS)
	// can pass credentials as pod args instead of shipping a users.json
	// file into the pod. role must be one of player/observer/admin.
	// slot is optional (required for player, ignored otherwise). Throws
	// std::runtime_error on malformed input.
	size_t add_from_spec(const std::string& spec) {
		// Split on ':'. Careful: api_key may contain '-' or '_' but NOT
		// ':', so plain colon split is safe.
		std::vector<std::string> parts;
		size_t start = 0;
		for (size_t i = 0; i <= spec.size(); ++i) {
			if (i == spec.size() || spec[i] == ':') {
				parts.push_back(spec.substr(start, i - start));
				start = i + 1;
			}
		}
		if (parts.size() < 3 || parts.size() > 4) {
			throw std::runtime_error(
				"auth: --user expects alias:api_key:role[:slot], got '"
				+ spec + "'");
		}
		user_t u;
		u.alias = parts[0];
		if (u.alias.empty())
			throw std::runtime_error("auth: --user: alias must be non-empty");
		const std::string& key = parts[1];
		if (key.empty())
			throw std::runtime_error("auth: --user '" + u.alias + "': api_key empty");
		u.api_key_hash = sha256::hash(key.data(), key.size());
		const std::string& role_s = parts[2];
		if (role_s == "player") u.role = role_t::player;
		else if (role_s == "observer") u.role = role_t::observer;
		else if (role_s == "admin") u.role = role_t::admin;
		else throw std::runtime_error(
			"auth: --user '" + u.alias + "': unknown role '" + role_s
			+ "' (want player/observer/admin)");
		if (parts.size() == 4 && !parts[3].empty()) {
			int slot = std::atoi(parts[3].c_str());
			if (slot < -1 || slot > 7)
				throw std::runtime_error(
					"auth: --user '" + u.alias + "': slot out of range");
			u.assigned_slot = slot;
		}
		if (u.role == role_t::player && u.assigned_slot < 0) {
			throw std::runtime_error(
				"auth: --user '" + u.alias + "': role=player requires a slot");
		}
		for (const auto& existing : users_) {
			if (existing.alias == u.alias) {
				if (existing.role != u.role || existing.assigned_slot != u.assigned_slot) {
					throw std::runtime_error(
						"auth: --user '" + u.alias
						+ "' conflicts with prior entry (role/slot mismatch)");
				}
			}
		}
		users_.push_back(std::move(u));
		return 1;
	}

	// Add a single user from a `alias:sha256hex:role[:slot]` spec.
	// Same shape as add_from_spec but the middle field is a
	// hex-encoded SHA-256 of the API key — the plaintext never
	// enters the server. Used by control planes that keep hashed
	// keys in a DB and don't want to leak plaintext on the CLI.
	//
	// One caller may hand us many hashes for the same alias (all
	// active keys for that user); duplicate-alias handling matches
	// add_from_spec.
	size_t add_from_spec_hash(const std::string& spec) {
		std::vector<std::string> parts;
		size_t start = 0;
		for (size_t i = 0; i <= spec.size(); ++i) {
			if (i == spec.size() || spec[i] == ':') {
				parts.push_back(spec.substr(start, i - start));
				start = i + 1;
			}
		}
		if (parts.size() < 3 || parts.size() > 4) {
			throw std::runtime_error(
				"auth: --user-hash expects alias:sha256hex:role[:slot], got '"
				+ spec + "'");
		}
		user_t u;
		u.alias = parts[0];
		if (u.alias.empty())
			throw std::runtime_error("auth: --user-hash: alias must be non-empty");
		const std::string& hex = parts[1];
		if (hex.size() != 64)
			throw std::runtime_error(
				"auth: --user-hash '" + u.alias
				+ "': hash must be 64 hex chars, got " + std::to_string(hex.size()));
		for (size_t i = 0; i < 32; ++i) {
			int hi = _hex_nibble(hex[i * 2]);
			int lo = _hex_nibble(hex[i * 2 + 1]);
			if (hi < 0 || lo < 0)
				throw std::runtime_error(
					"auth: --user-hash '" + u.alias
					+ "': non-hex char in hash");
			u.api_key_hash[i] = (uint8_t)((hi << 4) | lo);
		}
		const std::string& role_s = parts[2];
		if (role_s == "player") u.role = role_t::player;
		else if (role_s == "observer") u.role = role_t::observer;
		else if (role_s == "admin") u.role = role_t::admin;
		else throw std::runtime_error(
			"auth: --user-hash '" + u.alias + "': unknown role '" + role_s
			+ "' (want player/observer/admin)");
		if (parts.size() == 4 && !parts[3].empty()) {
			int slot = std::atoi(parts[3].c_str());
			if (slot < -1 || slot > 7)
				throw std::runtime_error(
					"auth: --user-hash '" + u.alias + "': slot out of range");
			u.assigned_slot = slot;
		}
		if (u.role == role_t::player && u.assigned_slot < 0) {
			throw std::runtime_error(
				"auth: --user-hash '" + u.alias + "': role=player requires a slot");
		}
		for (const auto& existing : users_) {
			if (existing.alias == u.alias) {
				if (existing.role != u.role || existing.assigned_slot != u.assigned_slot) {
					throw std::runtime_error(
						"auth: --user-hash '" + u.alias
						+ "' conflicts with prior entry (role/slot mismatch)");
				}
			}
		}
		users_.push_back(std::move(u));
		return 1;
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
	static int _hex_nibble(char c) {
		if (c >= '0' && c <= '9') return c - '0';
		if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
		if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
		return -1;
	}

	std::vector<user_t> users_;
};

} // namespace openbw_auth

#endif
