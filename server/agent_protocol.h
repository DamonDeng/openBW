// JSON <-> BW action byte encoding for the agent protocol.
//
// Agents speak JSON over WebSocket. This module translates each JSON
// command into (a) a select action for the target unit(s) and (b) the
// actual verb bytes, so the sim will apply the verb to the just-selected
// units. Everything is deterministic and stateless -- pass the same JSON
// in, get the same bytes out.
//
// Message shapes are documented at the bottom of this file.

#ifndef OPENBW_AGENT_PROTOCOL_H
#define OPENBW_AGENT_PROTOCOL_H

#include "../bwenums.h"
#include "../deps/nlohmann/json.hpp"

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace openbw_agents {

// Raw BW action ids (from actions.h::read_action switch).
enum : uint8_t {
	ACT_SELECT = 9,
	ACT_SHIFT_SELECT = 10,
	ACT_BUILD = 12,
	ACT_STOP = 26,
	ACT_TRAIN = 31,
	ACT_DEFAULT_ORDER = 20,
	ACT_ORDER = 21,
};

// Encoded output: one command may produce multiple sequential action
// byte-blobs. Each blob is one BW action (already framed for
// funcs.schedule_action). We return them as a flat vector of blobs
// because the sim's schedule_action wants one blob at a time.
using action_blob = std::vector<uint8_t>;
using encoded_command = std::vector<action_blob>;

struct encode_error {
	std::string message;
};

inline void put_u8(action_blob& out, uint8_t v) { out.push_back(v); }
inline void put_u16(action_blob& out, uint16_t v) {
	out.push_back((uint8_t)(v & 0xff));
	out.push_back((uint8_t)((v >> 8) & 0xff));
}
inline void put_i16(action_blob& out, int16_t v) {
	put_u16(out, (uint16_t)v);
}

// Build the "select unit(s)" action blob. Takes one unit id (u16).
inline action_blob make_select(uint16_t unit_id) {
	action_blob b;
	put_u8(b, ACT_SELECT);
	put_u8(b, 1); // count
	put_u16(b, unit_id);
	return b;
}

// Encode a JSON command into a sequence of BW action blobs, ready to
// hand to funcs.schedule_action on the sim thread.
//
// Returns either the encoded blobs (empty on unknown/invalid input)
// or an error message. Caller decides what to do with errors -- send
// them back to the agent over WS, log, drop.
inline std::optional<encode_error> encode_command(
	const nlohmann::json& cmd,
	encoded_command& out
) {
	out.clear();
	if (!cmd.is_object() || !cmd.contains("verb") || !cmd["verb"].is_string()) {
		return encode_error{"command must be an object with a 'verb' string"};
	}
	const auto verb = cmd["verb"].get<std::string>();

	auto need = [&](const char* key) -> const nlohmann::json* {
		auto it = cmd.find(key);
		if (it == cmd.end()) return nullptr;
		return &(*it);
	};

	// --- Move (queue = false by default) ---
	// {"verb":"move","unit":<id>,"x":<int16>,"y":<int16>,"queue":false}
	if (verb == "move") {
		auto* u = need("unit"); auto* x = need("x"); auto* y = need("y");
		if (!u || !x || !y) return encode_error{"move: needs unit, x, y"};
		uint16_t unit_id = u->get<uint16_t>();
		int16_t px = x->get<int16_t>();
		int16_t py = y->get<int16_t>();
		bool queue = cmd.value("queue", false);

		out.push_back(make_select(unit_id));

		action_blob b;
		put_u8(b, ACT_ORDER);
		put_i16(b, px); put_i16(b, py);
		put_u16(b, 0); // target unit id (0 = no target)
		put_u16(b, (uint16_t)bwgame::UnitTypes::None); // target unit type
		put_u8(b, (uint8_t)bwgame::Orders::Move);
		put_u8(b, queue ? 1 : 0);
		out.push_back(std::move(b));
		return std::nullopt;
	}

	// --- Attack (unit or ground) ---
	// {"verb":"attack","unit":<id>,"target_unit":<id or 0>,"x":<int16>,"y":<int16>,"queue":false}
	if (verb == "attack") {
		auto* u = need("unit"); auto* x = need("x"); auto* y = need("y");
		if (!u || !x || !y) return encode_error{"attack: needs unit, x, y"};
		uint16_t unit_id = u->get<uint16_t>();
		int16_t px = x->get<int16_t>();
		int16_t py = y->get<int16_t>();
		uint16_t target_id = cmd.value("target_unit", 0);
		bool queue = cmd.value("queue", false);

		out.push_back(make_select(unit_id));

		action_blob b;
		put_u8(b, ACT_ORDER);
		put_i16(b, px); put_i16(b, py);
		put_u16(b, target_id);
		put_u16(b, (uint16_t)bwgame::UnitTypes::None);
		// AttackUnit if target given, AttackMove if not.
		put_u8(b, target_id != 0 ? (uint8_t)bwgame::Orders::AttackUnit : (uint8_t)bwgame::Orders::AttackMove);
		put_u8(b, queue ? 1 : 0);
		out.push_back(std::move(b));
		return std::nullopt;
	}

	// --- Gather ---
	// {"verb":"gather","unit":<worker_id>,"target_unit":<mineral or geyser id>}
	// Sends a worker to harvest a mineral field or vespene geyser. This
	// is a dedicated verb rather than an alias of attack because BW's
	// sim only starts a gather cycle for Orders::Harvest1, not
	// Orders::AttackUnit -- retail clients translate the right-click
	// on a mineral to Harvest1 on the client side, which we can't do
	// from the sim.
	if (verb == "gather") {
		auto* u = need("unit"); auto* t = need("target_unit");
		if (!u || !t) return encode_error{"gather: needs unit, target_unit"};
		uint16_t unit_id = u->get<uint16_t>();
		uint16_t target_id = t->get<uint16_t>();
		if (target_id == 0)
			return encode_error{"gather: target_unit must not be 0"};

		out.push_back(make_select(unit_id));

		action_blob b;
		put_u8(b, ACT_ORDER);
		put_i16(b, 0); put_i16(b, 0);            // position ignored for gather
		put_u16(b, target_id);
		put_u16(b, (uint16_t)bwgame::UnitTypes::None);
		put_u8(b, (uint8_t)bwgame::Orders::Harvest1);
		put_u8(b, 0);                            // queue = false
		out.push_back(std::move(b));
		return std::nullopt;
	}

	// --- Stop ---
	// {"verb":"stop","unit":<id>,"queue":false}
	if (verb == "stop") {
		auto* u = need("unit");
		if (!u) return encode_error{"stop: needs unit"};
		uint16_t unit_id = u->get<uint16_t>();
		bool queue = cmd.value("queue", false);

		out.push_back(make_select(unit_id));

		action_blob b;
		put_u8(b, ACT_STOP);
		put_u8(b, queue ? 1 : 0);
		out.push_back(std::move(b));
		return std::nullopt;
	}

	// --- Train ---
	// {"verb":"train","unit":<producer_id>,"unit_type":<UnitTypes int>}
	// Selects the training building/unit then issues action_train.
	if (verb == "train") {
		auto* u = need("unit"); auto* ut = need("unit_type");
		if (!u || !ut) return encode_error{"train: needs unit, unit_type"};
		uint16_t unit_id = u->get<uint16_t>();
		uint16_t unit_type = ut->get<uint16_t>();

		out.push_back(make_select(unit_id));

		action_blob b;
		put_u8(b, ACT_TRAIN);
		put_u16(b, unit_type);
		out.push_back(std::move(b));
		return std::nullopt;
	}

	// --- Build ---
	// {"verb":"build","unit":<worker_id>,"unit_type":<UnitTypes int>,
	//  "tile_x":<u16>,"tile_y":<u16>}
	// The order type depends on the target building's race:
	//   Terran   -> PlaceBuilding  (order 30, dispatches to order_PlaceBuilding)
	//   Protoss  -> PlaceProtossBuilding (order 31)
	//   Zerg     -> handled via train verb, not build (Larva morph)
	// unit_build_order_valid at bwgame.h:18167 accepts both, but the
	// dispatch table at bwgame.h:7652 routes to different order handlers
	// with different placement + power-matrix logic.
	if (verb == "build") {
		auto* u = need("unit"); auto* ut = need("unit_type");
		auto* tx = need("tile_x"); auto* ty = need("tile_y");
		if (!u || !ut || !tx || !ty)
			return encode_error{"build: needs unit, unit_type, tile_x, tile_y"};
		uint16_t unit_id = u->get<uint16_t>();
		uint16_t unit_type = ut->get<uint16_t>();
		uint16_t tile_x = tx->get<uint16_t>();
		uint16_t tile_y = ty->get<uint16_t>();

		// Pick order by unit_type id. Buildings are grouped in
		// bwenums.h roughly by race:
		//   Terran buildings:   106..122 (with a gap for Vulture_Mine)
		//   Zerg buildings:     130..150
		//   Protoss buildings:  154..172
		// Any override via optional "order" field in JSON.
		uint8_t order = (uint8_t)bwgame::Orders::PlaceBuilding;
		if (cmd.contains("order") && cmd["order"].is_number_integer()) {
			order = (uint8_t)cmd["order"].get<int>();
		} else if (unit_type >= 154 && unit_type <= 172) {
			order = (uint8_t)bwgame::Orders::PlaceProtossBuilding;
		}
		// Zerg building placement isn't reachable through the build
		// verb -- the Larva morph path is a train.

		out.push_back(make_select(unit_id));

		action_blob b;
		put_u8(b, ACT_BUILD);
		put_u8(b, order);
		put_u16(b, tile_x);
		put_u16(b, tile_y);
		put_u16(b, unit_type);
		out.push_back(std::move(b));
		return std::nullopt;
	}

	return encode_error{"unknown verb: " + verb};
}

} // namespace openbw_agents

// -----------------------------------------------------------------------------
// Message shapes (JSON, single-line per WebSocket text frame)
//
// Client -> server:
//   {"type":"cmd", "id":"<agent-request-id>", "cmd":{...}}
//     where cmd is one of:
//       {"verb":"move",   "unit":123, "x":1024, "y":768, "queue":false}
//       {"verb":"attack", "unit":123, "x":1024, "y":768, "target_unit":0}
//       {"verb":"gather", "unit":42, "target_unit":800}   // mineral or geyser id
//       {"verb":"stop",   "unit":123, "queue":false}
//       {"verb":"train",  "unit":42, "unit_type":7}      // Terran_SCV
//       {"verb":"build",  "unit":42, "unit_type":106,     // CC = 106
//                          "tile_x":24, "tile_y":30}
//
// Server -> client (sent per frame while any command is being executed):
//   {"type":"welcome", "slot":N, "current_frame":F}    // sent on WS open
//   {"type":"ack",     "id":"...", "queued_at_frame":F}
//   {"type":"error",   "id":"...", "message":"..."}
//
// UnitTypes and Orders are the integer values from bwenums.h.
// -----------------------------------------------------------------------------

#endif
