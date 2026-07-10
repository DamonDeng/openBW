// Read-only server-side query helpers. Runs on the sim thread, same
// as observation.h. Anything that needs to look at sim state but does
// NOT mutate it (find valid placement, check unit-type stats, etc.)
// belongs here.
//
// Wire format for find_placement:
//   client -> server:
//     {"type":"find_placement", "id":"...", "unit_type": <int>,
//      "worker_unit": <optional int, id of worker that would build>,
//      "center_x": <optional int, pixel>, "center_y": <optional int, pixel>,
//      "radius_tiles": <optional int, default 12>,
//      "max_results": <optional int, default 24>}
//
//   server -> client:
//     {"type":"placement_result", "id":"...",
//      "unit_type": <int>,
//      "tile_size_x": <int>, "tile_size_y": <int>,
//      "spots": [{"tile_x": <int>, "tile_y": <int>,
//                 "center_x": <int, pixel>, "center_y": <int, pixel>}, ...]}
//
// The server samples a spiral of candidate tiles around center_x/y (or
// around the worker if no center given) and returns up to max_results
// tiles where can_place_building() approves. Empty spots array means
// nothing valid found in the search radius.

#ifndef OPENBW_QUERIES_H
#define OPENBW_QUERIES_H

#include "../bwgame.h"
#include "../deps/nlohmann/json.hpp"

#include <cmath>
#include <string>
#include <vector>

namespace openbw_agents {

// Build a spiral of tile offsets radiating outward from (0,0). Used
// so a caller with a rough hint ("place near here") finds the CLOSEST
// valid spot rather than the top-left-most one.
inline std::vector<std::pair<int, int>> spiral_offsets(int max_radius) {
	std::vector<std::pair<int, int>> out;
	out.reserve((2 * max_radius + 1) * (2 * max_radius + 1));
	out.emplace_back(0, 0);
	for (int r = 1; r <= max_radius; ++r) {
		// Walk the ring at radius r counter-clockwise starting at (r, 0).
		int x = r, y = 0;
		for (; y < r; ++y) out.emplace_back(x, y);
		for (; x > -r; --x) out.emplace_back(x, y);
		for (; y > -r; --y) out.emplace_back(x, y);
		for (; x < r; ++x) out.emplace_back(x, y);
		for (; y < 0; ++y) out.emplace_back(x, y);
	}
	return out;
}

inline std::string build_placement_response(
	bwgame::state_functions& funcs,
	int slot,
	const std::string& request_id,
	const nlohmann::json& req)
{
	nlohmann::json j;
	j["type"] = "placement_result";
	j["id"] = request_id;

	if (!req.contains("unit_type") || !req["unit_type"].is_number_integer()) {
		j["error"] = "unit_type is required";
		return j.dump();
	}
	int type_id = req["unit_type"].get<int>();
	if (type_id < 0 || type_id >= 228) {
		j["error"] = "unit_type out of range";
		return j.dump();
	}
	auto* unit_type = funcs.get_unit_type((bwgame::UnitTypes)type_id);
	if (!unit_type) {
		j["error"] = "unknown unit_type";
		return j.dump();
	}

	auto& st = funcs.st;
	const auto& game_st = *st.game;

	// Resolve center and (separately) the worker pointer.
	//
	// Priority for the SEARCH CENTER:
	//   1. explicit center_x / center_y in the request (agent knows
	//      where it wants to build -- honor it)
	//   2. worker_unit's current position (agent said "search near
	//      this probe" without a specific point)
	//   3. any owned building for this slot (fallback)
	//   4. the slot's start location (last resort)
	//
	// The worker pointer is resolved INDEPENDENTLY of the center: an
	// agent asking for a specific center still needs the worker
	// pointer for can_place_building (which uses the worker's tile
	// occupancy). So worker + center_x/y can coexist -- explicit
	// center wins for search, worker still gets validated at that
	// candidate spot.
	int cx = 0, cy = 0;
	bool have_center = false;

	const bwgame::unit_t* worker = nullptr;
	if (req.contains("worker_unit") && req["worker_unit"].is_number_integer()) {
		int wid = req["worker_unit"].get<int>();
		worker = funcs.get_unit(bwgame::unit_id((uint16_t)wid));
	}
	if (req.contains("center_x") && req.contains("center_y")) {
		cx = req["center_x"].get<int>();
		cy = req["center_y"].get<int>();
		have_center = true;
	}
	if (!have_center && worker) {
		cx = worker->position.x;
		cy = worker->position.y;
		have_center = true;
	}
	if (!have_center) {
		// Fallback: find any owned building for this slot and use its
		// position. If no building either, use the slot's start location.
		for (auto* u : bwgame::ptr(st.player_units[slot])) {
			if (u->unit_type && u->unit_type->flags & bwgame::unit_type_t::flag_building) {
				cx = u->position.x;
				cy = u->position.y;
				have_center = true;
				break;
			}
		}
		if (!have_center) {
			auto sl = game_st.start_locations[slot];
			cx = sl.x;
			cy = sl.y;
		}
	}

	int radius_tiles = 12;
	if (req.contains("radius_tiles") && req["radius_tiles"].is_number_integer()) {
		radius_tiles = std::max(1, std::min(64, req["radius_tiles"].get<int>()));
	}
	int max_results = 24;
	if (req.contains("max_results") && req["max_results"].is_number_integer()) {
		max_results = std::max(1, std::min(256, req["max_results"].get<int>()));
	}

	// Convert center pixel -> center tile (rounded).
	int center_tile_x = cx / 32;
	int center_tile_y = cy / 32;

	// Building footprint in tiles.
	int tile_size_x = unit_type->placement_size.x / 32;
	int tile_size_y = unit_type->placement_size.y / 32;
	j["unit_type"] = type_id;
	j["tile_size_x"] = tile_size_x;
	j["tile_size_y"] = tile_size_y;

	auto spots = nlohmann::json::array();
	for (auto [dx, dy] : spiral_offsets(radius_tiles)) {
		if ((int)spots.size() >= max_results) break;
		int tile_x = center_tile_x + dx;
		int tile_y = center_tile_y + dy;
		if (tile_x < 0 || tile_y < 0) continue;
		if (tile_x + tile_size_x > (int)game_st.map_tile_width) continue;
		if (tile_y + tile_size_y > (int)game_st.map_tile_height) continue;

		// can_place_building takes a WORLD position, which the sim treats
		// as the CENTER of the building. Compute it from the tile.
		bwgame::xy center_pos(
			tile_x * 32 + unit_type->placement_size.x / 2,
			tile_y * 32 + unit_type->placement_size.y / 2);
		bool ok = funcs.can_place_building(
			worker, slot, unit_type, center_pos,
			/*check_undetected_units=*/true,
			/*check_invisible_tiles=*/false);
		if (!ok) continue;

		nlohmann::json spot;
		spot["tile_x"] = tile_x;
		spot["tile_y"] = tile_y;
		spot["center_x"] = center_pos.x;
		spot["center_y"] = center_pos.y;
		spots.push_back(std::move(spot));
	}
	j["spots"] = std::move(spots);
	return j.dump();
}

} // namespace openbw_agents

#endif
