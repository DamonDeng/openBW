// Generic per-slot query queue for read-only requests that need
// sim-thread access. Mirrors observe_request.h but carries an opaque
// JSON payload, so it can serve any query type (find_placement,
// future ones, ...) without duplicating the queue plumbing.
//
// Design: observation.h has its own dedicated queue for the hot path
// (agent observe every tick). This queue is for lower-frequency
// queries where a general dispatch table is fine.

#ifndef OPENBW_QUERY_REQUEST_H
#define OPENBW_QUERY_REQUEST_H

#include "../deps/nlohmann/json.hpp"

#include <array>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>

namespace openbw_agents {

struct query_request {
	std::string kind;                    // "find_placement", ...
	std::string request_id;
	nlohmann::json payload;              // whatever the specific kind needs
	// Called on the sim thread with the serialized JSON response.
	std::function<void(std::string)> respond;
};

struct query_queue {
	struct slot_state {
		std::mutex m;
		std::deque<query_request> pending;
	};
	std::array<slot_state, 8> slots;

	bool push(int slot, query_request req) {
		if (slot < 0 || slot > 7) return false;
		auto& s = slots[(size_t)slot];
		std::lock_guard<std::mutex> lock(s.m);
		s.pending.push_back(std::move(req));
		return true;
	}

	template <typename Fn>
	void drain(Fn&& fn) {
		for (int slot = 0; slot < 8; ++slot) {
			std::deque<query_request> local;
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
