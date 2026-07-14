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
	ACT_UNLOAD_ALL = 40,     // payload: u8 queue. Kicks all passengers out of the
	                         // selected transport/bunker at retail unload cadence.
	ACT_UNLOAD = 41,         // payload: u16 target_unit. Ejects one specific
	                         // passenger from the selected transport/bunker.
	ACT_UNIT_MORPH = 35,     // payload: u16 unit_type. Zerg Larva -> unit,
	                         // Hydralisk -> Lurker, Mutalisk -> Guardian/Devourer.
	ACT_UNSIEGE = 37,        // payload: u8 queue. Terran Siege Tank -> Tank Mode.
	ACT_SIEGE = 38,          // payload: u8 queue. Terran Siege Tank -> Siege Mode.
	ACT_TRAIN_FIGHTER = 39,  // no payload; applies to selected Carrier / Reaver
	ACT_LIFTOFF = 47,        // payload: i16 x, i16 y. Terran building takes off.
	ACT_BUILDING_MORPH = 53, // payload: u16 unit_type. Zerg building tier morph
	                         // (Hatch->Lair->Hive, Creep_Colony->Sunken/Spore,
	                         // Spire->Greater_Spire). NOT for Drone->building
	                         // (that uses ACT_BUILD with order=DroneStartBuild=25).
	ACT_DEFAULT_ORDER = 20,
	ACT_ORDER = 21,
	ACT_RESEARCH = 48,  // payload: TechTypes u8
	ACT_UPGRADE = 50,   // payload: UpgradeTypes u8
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

	// --- Repair (SCV → damaged mechanical unit/building) ---
	// {"verb":"repair","unit":<scv_id>,"target_unit":<friendly_id>}
	// Dedicated verb because Orders::Repair is normally reached only
	// via ACT_DEFAULT_ORDER's routing logic (see bwgame.h:3598
	// get_default_order), which the agent protocol doesn't expose.
	// The `attack` verb won't work here -- it forces AttackUnit.
	// Silent-reject inside the sim on non-SCV workers, non-mech
	// targets, undamaged targets, or non-friendly targets.
	if (verb == "repair") {
		auto* u = need("unit"); auto* t = need("target_unit");
		if (!u || !t) return encode_error{"repair: needs unit, target_unit"};
		uint16_t unit_id = u->get<uint16_t>();
		uint16_t target_id = t->get<uint16_t>();
		if (target_id == 0)
			return encode_error{"repair: target_unit must not be 0"};

		out.push_back(make_select(unit_id));

		action_blob b;
		put_u8(b, ACT_ORDER);
		put_i16(b, 0); put_i16(b, 0);            // position ignored for repair
		put_u16(b, target_id);
		put_u16(b, (uint16_t)bwgame::UnitTypes::None);
		put_u8(b, (uint8_t)bwgame::Orders::Repair);
		put_u8(b, 0);                            // queue = false
		out.push_back(std::move(b));
		return std::nullopt;
	}

	// --- Load (passenger -> transport/bunker) ---
	// {"verb":"load","unit":<passenger_id>,"target_unit":<transport_id>}
	// Retail BW uses the right-click path, which the agent protocol
	// doesn't expose. We piggyback ACT_ORDER + Orders::EnterTransport,
	// same as `repair` does with Orders::Repair. Silent-reject inside
	// the sim if the target doesn't provide space, the passenger type
	// can't enter (SCV cannot enter Bunker; Marine/Firebat/Ghost can),
	// or the two units are on different teams.
	if (verb == "load") {
		auto* u = need("unit"); auto* t = need("target_unit");
		if (!u || !t) return encode_error{"load: needs unit, target_unit"};
		uint16_t unit_id = u->get<uint16_t>();
		uint16_t target_id = t->get<uint16_t>();
		if (target_id == 0)
			return encode_error{"load: target_unit must not be 0"};

		out.push_back(make_select(unit_id));

		action_blob b;
		put_u8(b, ACT_ORDER);
		put_i16(b, 0); put_i16(b, 0);            // position ignored
		put_u16(b, target_id);
		put_u16(b, (uint16_t)bwgame::UnitTypes::None);
		put_u8(b, (uint8_t)bwgame::Orders::EnterTransport);
		put_u8(b, 0);                            // queue = false
		out.push_back(std::move(b));
		return std::nullopt;
	}

	// --- Unload one passenger ---
	// {"verb":"unload","unit":<transport_id>,"target_unit":<passenger_id>}
	// Selects the transport/bunker, then issues ACT_UNLOAD with the
	// specific passenger to eject. Engine handler at actions.h:1117
	// (read_action_unload -> action_unload).
	if (verb == "unload") {
		auto* u = need("unit"); auto* t = need("target_unit");
		if (!u || !t) return encode_error{"unload: needs unit, target_unit"};
		uint16_t transport_id = u->get<uint16_t>();
		uint16_t passenger_id = t->get<uint16_t>();
		if (passenger_id == 0)
			return encode_error{"unload: target_unit must not be 0"};

		out.push_back(make_select(transport_id));

		action_blob b;
		put_u8(b, ACT_UNLOAD);
		put_u16(b, passenger_id);
		out.push_back(std::move(b));
		return std::nullopt;
	}

	// --- Unload all passengers ---
	// {"verb":"unload_all","unit":<transport_id>,"queue":false}
	// Kicks every passenger out at retail unload cadence (not
	// instantaneous). Engine handler at actions.h:1111.
	if (verb == "unload_all") {
		auto* u = need("unit");
		if (!u) return encode_error{"unload_all: needs unit"};
		uint16_t transport_id = u->get<uint16_t>();
		bool queue = cmd.value("queue", false);

		out.push_back(make_select(transport_id));

		action_blob b;
		put_u8(b, ACT_UNLOAD_ALL);
		put_u8(b, queue ? 1 : 0);
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

	// --- Research (single-target tech) ---
	// {"verb":"research","unit":<building_id>,"tech":<TechTypes int>}
	// The sim looks up the tech on the currently-selected building and
	// starts research if prereqs / cost / etc. are met. Silent reject
	// otherwise.
	if (verb == "research") {
		auto* u = need("unit"); auto* t = need("tech");
		if (!u || !t) return encode_error{"research: needs unit, tech"};
		uint16_t unit_id = u->get<uint16_t>();
		uint8_t tech_id = t->get<uint8_t>();

		out.push_back(make_select(unit_id));

		action_blob b;
		put_u8(b, ACT_RESEARCH);
		put_u8(b, tech_id);
		out.push_back(std::move(b));
		return std::nullopt;
	}

	// --- Upgrade (level-N stat upgrade) ---
	// {"verb":"upgrade","unit":<building_id>,"upgrade":<UpgradeTypes int>}
	// Same shape as research, different action code. The sim increments
	// the player's upgrade level for this UpgradeTypes when done. Level
	// is inferred from current progress; caller doesn't specify a level.
	if (verb == "upgrade") {
		auto* u = need("unit"); auto* ut = need("upgrade");
		if (!u || !ut) return encode_error{"upgrade: needs unit, upgrade"};
		uint16_t unit_id = u->get<uint16_t>();
		uint8_t upg_id = ut->get<uint8_t>();

		out.push_back(make_select(unit_id));

		action_blob b;
		put_u8(b, ACT_UPGRADE);
		put_u8(b, upg_id);
		out.push_back(std::move(b));
		return std::nullopt;
	}

	// --- TrainFighter (build a baby unit inside a Carrier or Reaver) ---
	// {"verb":"train_fighter","unit":<carrier_or_reaver_id>}
	// Zero-payload action beyond the select+action bytes. The sim
	// looks at the selected unit's type and picks Interceptor for a
	// Carrier or Scarab for a Reaver (see actions.h::action_train_
	// fighter). Silent-reject if the selected unit is neither of
	// those, or if it's already at its fighter cap.
	if (verb == "train_fighter") {
		auto* u = need("unit");
		if (!u) return encode_error{"train_fighter: needs unit"};
		uint16_t unit_id = u->get<uint16_t>();

		out.push_back(make_select(unit_id));

		action_blob b;
		put_u8(b, ACT_TRAIN_FIGHTER);
		out.push_back(std::move(b));
		return std::nullopt;
	}

	// --- Morph (Zerg unit morph) ---
	// {"verb":"morph","unit":<larva_or_hydra_or_muta_id>,
	//  "unit_type":<UnitTypes int>}
	// Selects the source Zerg unit and issues action_morph
	// (actions.h:871 -> read_action_morph:1237). The sim consumes the
	// source unit into a Zerg_Egg (or Lurker_Egg / Cocoon) and starts
	// Orders::ZergUnitMorph. Payload is just a u16 unit_type after
	// the action byte. Sim silent-rejects if:
	//   - selection is not a Larva / Hydralisk / Mutalisk
	//   - target unit_type is not a valid morph target from that source
	//   - insufficient minerals / gas / supply / tech
	if (verb == "morph") {
		auto* u = need("unit"); auto* ut = need("unit_type");
		if (!u || !ut) return encode_error{"morph: needs unit, unit_type"};
		uint16_t unit_id = u->get<uint16_t>();
		uint16_t unit_type = ut->get<uint16_t>();

		out.push_back(make_select(unit_id));

		action_blob b;
		put_u8(b, ACT_UNIT_MORPH);
		put_u16(b, unit_type);
		out.push_back(std::move(b));
		return std::nullopt;
	}

	// --- Morph Building (Zerg building tier morph) ---
	// {"verb":"morph_building","unit":<building_id>,
	//  "unit_type":<UnitTypes int>}
	// Selects the source Zerg building and issues action_morph_building
	// (actions.h:888 -> read_action_morph_building:1243). Payload is
	// just a u16 unit_type. Handles:
	//   Hatchery -> Lair -> Hive
	//   Spire -> Greater_Spire
	//   Creep_Colony -> Sunken_Colony / Spore_Colony
	// action_morph_building REQUIRES the selection to already be a Zerg
	// building (actions.h:893 unit_is_zerg_building check). Drone ->
	// new building goes through the "build" verb with order=25
	// (DroneStartBuild), NOT this verb.
	if (verb == "morph_building") {
		auto* u = need("unit"); auto* ut = need("unit_type");
		if (!u || !ut) return encode_error{"morph_building: needs unit, unit_type"};
		uint16_t unit_id = u->get<uint16_t>();
		uint16_t unit_type = ut->get<uint16_t>();

		out.push_back(make_select(unit_id));

		action_blob b;
		put_u8(b, ACT_BUILDING_MORPH);
		put_u16(b, unit_type);
		out.push_back(std::move(b));
		return std::nullopt;
	}

	// --- Build ---
	// {"verb":"build","unit":<worker_id>,"unit_type":<UnitTypes int>,
	//  "tile_x":<u16>,"tile_y":<u16>, "order":<optional int>}
	// The order type depends on the target building's race:
	//   Terran   -> PlaceBuilding        (order 30, order_PlaceBuilding)
	//   Protoss  -> PlaceProtossBuilding (order 31, order_PlaceProtossBuilding)
	//   Zerg     -> DroneStartBuild      (order 25, order_DroneStartBuild;
	//                                    caller must set "order":25 explicitly)
	//   Terran addon -> PlaceAddon       (order 36, caller passes "order":36)
	// The bwgame.h::place_building special-cases Zerg Drones (line 2452)
	// so the same action_build path handles Drone->egg->building.
	// unit_build_order_valid at bwgame.h:18167 accepts all three
	// DroneStartBuild / PlaceBuilding / PlaceProtossBuilding, but the
	// dispatch table at bwgame.h:7652 routes them to different order
	// handlers with different placement + creep + power-matrix logic.
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

	// --- Siege / Unsiege (Terran Siege Tank mode toggle) ---
	// {"verb":"siege","unit":<tank_id>}
	// {"verb":"unsiege","unit":<tank_id>}
	// action_siege / action_unsiege in actions.h:1394 validate:
	//   - selection contains a Terran Siege Tank
	//   - Tank_Siege_Mode tech is researched
	// then issue Orders::Sieging / Orders::Unsieging. The unit-type
	// morph (Tank_Mode <-> Siege_Mode) happens inside those order
	// handlers, not here. Silent-reject if tech missing.
	if (verb == "siege" || verb == "unsiege") {
		auto* u = need("unit");
		if (!u) return encode_error{verb + ": needs unit"};
		uint16_t unit_id = u->get<uint16_t>();

		out.push_back(make_select(unit_id));

		action_blob b;
		put_u8(b, verb == "siege" ? ACT_SIEGE : ACT_UNSIEGE);
		put_u8(b, 0);  // queue = false
		out.push_back(std::move(b));
		return std::nullopt;
	}

	// --- Place Mine (Vulture Spider Mine drop) ---
	// {"verb":"place_mine","unit":<vulture_id>,"x":<int16>,"y":<int16>}
	// The Vulture must have Spider_Mines tech researched (silent-
	// reject otherwise). Reuses ACT_ORDER with Orders::PlaceMine --
	// no dedicated opcode. Payload same shape as move/attack: pixel
	// position, target_unit=0, target_type=None.
	if (verb == "place_mine") {
		auto* u = need("unit"); auto* x = need("x"); auto* y = need("y");
		if (!u || !x || !y) return encode_error{"place_mine: needs unit, x, y"};
		uint16_t unit_id = u->get<uint16_t>();
		int16_t px = x->get<int16_t>();
		int16_t py = y->get<int16_t>();

		out.push_back(make_select(unit_id));

		action_blob b;
		put_u8(b, ACT_ORDER);
		put_i16(b, px); put_i16(b, py);
		put_u16(b, 0);                                    // no target unit
		put_u16(b, (uint16_t)bwgame::UnitTypes::None);    // no target type
		put_u8(b, (uint8_t)bwgame::Orders::PlaceMine);
		put_u8(b, 0);                                     // queue = false
		out.push_back(std::move(b));
		return std::nullopt;
	}

	// --- Lift (Terran building takes off) ---
	// {"verb":"lift","unit":<building_id>,"x":<int16>,"y":<int16>}
	// x/y is the pixel destination the building will fly toward
	// (typically its current position for a straight liftoff, or
	// a nearby tile to relocate). Only applies to lift-capable
	// buildings: CC(106), Barracks(111), Factory(113), Starport(114),
	// Science_Facility(116). action_liftoff at actions.h:660 checks
	// unit_can_receive_order(BuildingLiftoff); sim rejects otherwise.
	if (verb == "lift") {
		auto* u = need("unit"); auto* x = need("x"); auto* y = need("y");
		if (!u || !x || !y) return encode_error{"lift: needs unit, x, y"};
		uint16_t unit_id = u->get<uint16_t>();
		int16_t px = x->get<int16_t>();
		int16_t py = y->get<int16_t>();

		out.push_back(make_select(unit_id));

		action_blob b;
		put_u8(b, ACT_LIFTOFF);
		put_i16(b, px); put_i16(b, py);
		out.push_back(std::move(b));
		return std::nullopt;
	}

	// --- Land (a flying Terran building descends to a tile) ---
	// {"verb":"land","unit":<flying_building_id>,"unit_type":<building_type_id>,
	//  "tile_x":<u16>,"tile_y":<u16>}
	// The unit_type must equal the flying building's own type
	// (unit_build_order_valid at bwgame.h:18183 requires it).
	// Reuses ACT_BUILD with Orders::BuildingLand as the order byte,
	// distinguishing landing from initial placement.
	if (verb == "land") {
		auto* u = need("unit"); auto* ut = need("unit_type");
		auto* tx = need("tile_x"); auto* ty = need("tile_y");
		if (!u || !ut || !tx || !ty)
			return encode_error{"land: needs unit, unit_type, tile_x, tile_y"};
		uint16_t unit_id = u->get<uint16_t>();
		uint16_t unit_type = ut->get<uint16_t>();
		uint16_t tile_x = tx->get<uint16_t>();
		uint16_t tile_y = ty->get<uint16_t>();

		out.push_back(make_select(unit_id));

		action_blob b;
		put_u8(b, ACT_BUILD);
		put_u8(b, (uint8_t)bwgame::Orders::BuildingLand);
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
//       {"verb":"repair", "unit":42, "target_unit":800}   // SCV -> damaged friendly mech
//       {"verb":"siege",  "unit":42}                       // Tank -> Siege Mode
//       {"verb":"unsiege","unit":42}                       // Tank -> Tank Mode
//       {"verb":"place_mine","unit":42, "x":1024, "y":768} // Vulture drops Spider Mine at pos
//       {"verb":"lift",   "unit":42, "x":1024, "y":768}   // Terran building takes off
//       {"verb":"land",   "unit":42, "unit_type":106,     // flying building descends
//                          "tile_x":24, "tile_y":30}       //   (unit_type = the flying bldg's type)
//       {"verb":"stop",   "unit":123, "queue":false}
//       {"verb":"train",  "unit":42, "unit_type":7}      // Terran_SCV
//       {"verb":"build",  "unit":42, "unit_type":106,     // CC = 106
//                          "tile_x":24, "tile_y":30}
//       {"verb":"research", "unit":42, "tech":0}          // TechTypes int
//       {"verb":"upgrade",  "unit":42, "upgrade":0}       // UpgradeTypes int
//       {"verb":"train_fighter", "unit":42}                // Carrier or Reaver
//       {"verb":"morph", "unit":42, "unit_type":37}        // Zerg unit morph
//                                                          //   (Larva->unit, Hydra->Lurker, Muta->Guardian/Devourer)
//       {"verb":"morph_building", "unit":42, "unit_type":132} // Zerg building tier morph
//                                                          //   (Hatch->Lair->Hive, Creep_Colony->Sunken/Spore)
//       {"verb":"build", "unit":42, "unit_type":142,       // Zerg: Drone -> building
//                          "tile_x":24, "tile_y":30, "order":25} //   order=25 (DroneStartBuild)
//
// Server -> client (sent per frame while any command is being executed):
//   {"type":"welcome", "slot":N, "current_frame":F}    // sent on WS open
//   {"type":"ack",     "id":"...", "queued_at_frame":F}
//   {"type":"error",   "id":"...", "message":"..."}
//
// UnitTypes and Orders are the integer values from bwenums.h.
// -----------------------------------------------------------------------------

#endif
