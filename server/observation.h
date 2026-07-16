// Snapshot serializer: reads bwgame::state and produces a JSON string
// describing what the agent for a given slot can see.
//
// Runs on the sim thread (the same thread that owns state), so no
// locking is needed here. The WS handler thread queues an observe
// request, sim thread produces the JSON in this file, then hands it
// back for the WS thread to write.
//
// Wire format documented in agent_protocol.h.

#ifndef OPENBW_OBSERVATION_H
#define OPENBW_OBSERVATION_H

#include "../bwgame.h"
#include "../deps/nlohmann/json.hpp"

#include <string>
#include <vector>

namespace openbw_agents {

struct observation_options {
	bool include_units = true;
	bool include_enemies = true;
	bool include_resources = true;
	bool include_map_info = false;
};

inline observation_options parse_targets(const std::vector<std::string>& targets) {
	observation_options o{};
	if (targets.empty()) {
		// Default: everything except map_info (that's cacheable from welcome).
		o.include_units = true;
		o.include_enemies = true;
		o.include_resources = true;
		return o;
	}
	// Reset then set only the ones asked for.
	o = observation_options{false, false, false, false};
	for (const auto& t : targets) {
		if (t == "all") {
			o = observation_options{true, true, true, true};
			break;
		}
		if (t == "units") o.include_units = true;
		else if (t == "enemies") o.include_enemies = true;
		// "neutrals" is an alias -- the enemies pass produces both
		// enemies and neutrals in one loop over st.visible_units.
		else if (t == "neutrals") o.include_enemies = true;
		else if (t == "resources") o.include_resources = true;
		else if (t == "map_info") o.include_map_info = true;
	}
	return o;
}

// Serialize one unit into the given json object.
inline void serialize_unit(nlohmann::json& out, const bwgame::state_functions& funcs,
	const bwgame::unit_t* u)
{
	auto uid = funcs.get_unit_id(u);
	out["unit_id"] = (unsigned)uid.raw_value;
	out["type"] = (int)u->unit_type->id;
	out["x"] = u->position.x;
	out["y"] = u->position.y;
	out["hp"] = u->hp.integer_part();
	out["hp_max"] = u->unit_type->hitpoints.integer_part();
	if (u->unit_type->has_shield) {
		out["shields"] = u->shield_points.integer_part();
		out["shields_max"] = u->unit_type->shield_points;
	}
	if (u->unit_type->flags & bwgame::unit_type_t::flag_has_energy) {
		out["energy"] = u->energy.integer_part();
	}
	out["order"] = u->order_type ? (int)u->order_type->id : -1;
	// A couple of useful status bits, packed as booleans.
	if (u->status_flags & bwgame::unit_t::status_flag_completed)      out["completed"] = true;
	if (u->status_flags & bwgame::unit_t::status_flag_flying)         out["flying"] = true;
	if (u->status_flags & bwgame::unit_t::status_flag_burrowed)       out["burrowed"] = true;
	if (u->status_flags & bwgame::unit_t::status_flag_cloaked)        out["cloaked"] = true;
	if (u->status_flags & bwgame::unit_t::status_flag_grounded_building) out["building"] = true;

	// Transport / bunker passenger state.
	//
	// Without this an agent has no way to tell a Marine is already
	// inside a Bunker, so idle-passenger loops keep re-firing `load`
	// every tick. See issue 2026-07-14-observation-missing-bunker-
	// load-state.md in ../simsc_agent_builder/issues/ for the failure
	// mode this fixes.
	//
	// Two forms:
	//   * boolean flags on the passenger ("loaded", "in_bunker")
	//     mirror the two status bits directly;
	//   * "transport_id" on the passenger + "loaded_units" array on
	//     the container give agents the graph they need to compute
	//     "how full is this bunker" and "which container holds this
	//     marine" without extra queries.
	bool is_loaded    = (u->status_flags & bwgame::unit_t::status_flag_loaded)    != 0;
	bool is_in_bunker = (u->status_flags & bwgame::unit_t::status_flag_in_bunker) != 0;
	if (is_loaded)    out["loaded"]    = true;
	if (is_in_bunker) out["in_bunker"] = true;
	if ((is_loaded || is_in_bunker) && u->connected_unit) {
		out["transport_id"] =
			(unsigned)funcs.get_unit_id(u->connected_unit).raw_value;
	}
	// Container side: iterate loaded_units() (filter range that
	// skips freed slot indices) and emit passenger unit_ids. The
	// array is only present when non-empty so quiet units (most
	// combat units) stay small on the wire.
	{
		auto passengers = funcs.loaded_units(u);
		auto it = passengers.begin();
		if (it != passengers.end()) {
			auto arr = nlohmann::json::array();
			for (const bwgame::unit_t* p : passengers) {
				arr.push_back((unsigned)funcs.get_unit_id(p).raw_value);
			}
			out["loaded_units"] = std::move(arr);
		}
	}

	// Carrier / Reaver fighter counts. Interceptors and Scarabs are
	// owned by a parent Carrier/Reaver -- the parent link isn't on
	// the wire (they look like ordinary units otherwise), so exposing
	// these counts here is the only way an agent can decide whether
	// a specific Carrier/Reaver needs another train_fighter fire.
	//
	// `count` = docked-inside + currently-out-attacking.
	// `queued` = fighters being built (haven't hatched yet).
	// `max` = capacity including Capacity upgrade (Carrier_Capacity or
	// Reaver_Capacity). Agents compare count+queued vs max to decide
	// whether to fire train_fighter without wasting commands.
	if (funcs.unit_is_carrier(u)) {
		out["fighter_count"] = (int)funcs.unit_interceptor_count(u);
		out["fighter_queued"] = (int)funcs.unit_queued_fighter_units(u);
		out["fighter_max"] = (int)funcs.unit_max_interceptor_count(u);
	} else if (funcs.unit_is_reaver(u)) {
		out["fighter_count"] = (int)funcs.unit_scarab_count(u);
		out["fighter_queued"] = (int)funcs.unit_queued_fighter_units(u);
		out["fighter_max"] = (int)funcs.unit_max_scarab_count(u);
	}
}

// Produce the observation for a given player slot. Passes the sim's
// state_functions in so we can query get_unit_id, tile visibility, etc.
inline std::string build_observation(
	const bwgame::state_functions& funcs,
	int slot,
	uint32_t current_frame,
	const std::string& request_id,
	const observation_options& opts)
{
	nlohmann::json j;
	j["type"] = "observation";
	j["id"] = request_id;
	j["slot"] = slot;
	j["current_frame"] = current_frame;

	const auto& st = funcs.st;

	if (opts.include_resources && slot >= 0 && slot < 12) {
		// Supply is tracked per-race (index 0=zerg, 1=terran, 2=protoss)
		// because some maps allow a player to have units of multiple
		// races. In practice each player owns exactly one race's supply
		// arrays; the other two are empty. Pick the one with any
		// non-zero data so we return the meaningful values regardless
		// of how the map recorded the player's race.
		int race_idx = -1;
		for (int i = 0; i < 3; ++i) {
			if (st.supply_used[slot][i].raw_value != 0
			 || st.supply_available[slot][i].raw_value != 0) {
				race_idx = i;
				break;
			}
		}
		// Fallback to player's declared race if all three are zero.
		if (race_idx < 0) {
			auto r = (int)st.players[slot].race;
			race_idx = (r == 0) ? 0 : (r == 2) ? 2 : 1;
		}
		nlohmann::json rr;
		rr["minerals"] = st.current_minerals[slot];
		rr["gas"] = st.current_gas[slot];
		// supply is stored as fp1 (half units). Return the integer part.
		rr["supply_used"] = st.supply_used[slot][race_idx].integer_part();
		rr["supply_max"] = st.supply_available[slot][race_idx].integer_part();
		rr["minerals_gathered"] = st.total_minerals_gathered[slot];
		rr["gas_gathered"] = st.total_gas_gathered[slot];

		// Upgrade levels: player-global (see bwgame.h::unit_armor,
		// weapon_damage_amount). Every combat unit of the player uses
		// the SAME level -- putting it per-unit would just duplicate
		// this map for every Zealot on the field. Report as
		// {upgrade_id: level}, level > 0 only. `upgrading` is the same
		// map but for level values currently in progress on any of the
		// player's buildings -- lets an agent see "we started but
		// haven't finished the upgrade yet".
		{
			nlohmann::json upgrades = nlohmann::json::object();
			nlohmann::json upgrading = nlohmann::json::object();
			for (size_t i = 0; i < st.upgrade_levels[slot].size(); ++i) {
				int lvl = st.upgrade_levels[slot].at((bwgame::UpgradeTypes)i);
				if (lvl > 0) {
					upgrades[std::to_string(i)] = lvl;
				}
				if (st.upgrade_upgrading[slot].at((bwgame::UpgradeTypes)i)) {
					upgrading[std::to_string(i)] = true;
				}
			}
			rr["upgrades"] = std::move(upgrades);
			if (!upgrading.empty()) rr["upgrading"] = std::move(upgrading);
		}

		// Researched techs: same story, player-global.
		{
			nlohmann::json tech = nlohmann::json::array();
			nlohmann::json researching = nlohmann::json::array();
			for (size_t i = 0; i < st.tech_researched[slot].size(); ++i) {
				if (st.tech_researched[slot].at((bwgame::TechTypes)i)) {
					tech.push_back((int)i);
				}
				if (st.tech_researching[slot].at((bwgame::TechTypes)i)) {
					researching.push_back((int)i);
				}
			}
			rr["tech"] = std::move(tech);
			if (!researching.empty()) rr["researching"] = std::move(researching);
		}

		j["resources"] = std::move(rr);
	}

	if (opts.include_units && slot >= 0 && slot < 12) {
		auto units = nlohmann::json::array();
		for (auto* u : bwgame::ptr(st.player_units[slot])) {
			nlohmann::json ju;
			serialize_unit(ju, funcs, u);
			units.push_back(std::move(ju));
		}
		j["units"] = std::move(units);
	}

	if (opts.include_enemies && slot >= 0 && slot < 12) {
		// Any unit whose sprite is currently visible to this slot AND is
		// owned by a different (non-neutral) player. Neutrals include
		// mineral fields and vespene geysers -- expose those too if the
		// LLM wants to see resources, but tag them.
		uint8_t vis_bit = (uint8_t)(1u << slot);
		auto enemies = nlohmann::json::array();
		auto neutrals = nlohmann::json::array();
		for (auto* u : bwgame::ptr(st.visible_units)) {
			if (u->owner == slot) continue;
			if (!u->sprite) continue;
			if (!(u->sprite->visibility_flags & vis_bit)) continue;
			nlohmann::json ju;
			serialize_unit(ju, funcs, u);
			ju["owner"] = u->owner;
			if (u->owner >= 8) {
				neutrals.push_back(std::move(ju));
			} else {
				enemies.push_back(std::move(ju));
			}
		}
		j["enemies"] = std::move(enemies);
		j["neutrals"] = std::move(neutrals);
	}

	if (opts.include_map_info) {
		nlohmann::json m;
		m["tile_width"] = (int)st.game->map_tile_width;
		m["tile_height"] = (int)st.game->map_tile_height;
		m["width"] = (int)st.game->map_width;
		m["height"] = (int)st.game->map_height;
		m["tileset"] = (int)st.game->tileset_index;
		j["map_info"] = std::move(m);
	}

	return j.dump();
}

} // namespace openbw_agents

#endif
