// Per-slot observe request queue.
//
// Threading model: the WebSocket handler thread receives an observe
// request from a client, enqueues it (per slot). The sim thread drains
// pending requests each tick, builds the observation JSON while holding
// the sim state, and hands the response string back to the WS handler
// via a completion callback that the ws_server posts back on its own
// io_service. This keeps bwgame::state single-threaded and matches the
// command_queue's design.

#ifndef OPENBW_OBSERVE_REQUEST_H
#define OPENBW_OBSERVE_REQUEST_H

#include <array>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace openbw_agents {

struct observe_request {
	std::string request_id;              // echoed back as "id"
	std::vector<std::string> targets;    // filter for units/enemies/resources/map_info
	// Called on the sim thread once the observation is serialized.
	// Impl posts back to the WS io_service to actually send the frame.
	std::function<void(std::string /*json_response*/)> respond;
};

struct observe_queue {
	struct slot_state {
		std::mutex m;
		std::deque<observe_request> pending;
	};
	std::array<slot_state, 8> slots;

	// Producer (WS handler thread).
	bool push(int slot, observe_request req) {
		if (slot < 0 || slot > 7) return false;
		auto& s = slots[(size_t)slot];
		std::lock_guard<std::mutex> lock(s.m);
		s.pending.push_back(std::move(req));
		return true;
	}

	// Consumer (sim thread). fn(slot, request) is called for each pending
	// request, in slot order (0..7). Requests within a slot preserve
	// arrival order.
	template <typename Fn>
	void drain(Fn&& fn) {
		for (int slot = 0; slot < 8; ++slot) {
			std::deque<observe_request> local;
			{
				auto& s = slots[(size_t)slot];
				std::lock_guard<std::mutex> lock(s.m);
				local.swap(s.pending);
			}
			for (auto& req : local) {
				fn(slot, req);
			}
		}
	}
};

} // namespace openbw_agents

#endif
