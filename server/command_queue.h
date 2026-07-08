// Per-slot command queue for agent inputs.
//
// The producer side (WebSocket handlers, HTTP handlers, in-process test
// harnesses) pushes raw BW action byte sequences into one of 8 slot
// queues. The consumer (sim thread) drains all slots in strict slot order
// (0 -> 7, FIFO within a slot) at the start of every tick and hands each
// action to sync.h's schedule_action.
//
// Deterministic drain order is essential -- otherwise two agents whose
// commands land in the same tick could see different effects on different
// runs.

#ifndef OPENBW_COMMAND_QUEUE_H
#define OPENBW_COMMAND_QUEUE_H

#include <array>
#include <cstdint>
#include <cstddef>
#include <deque>
#include <mutex>
#include <vector>

namespace openbw_agents {

struct command_queue {
	// N.B. std::deque under mutex is plenty. If profiling ever shows this
	// as hot, swap for a lock-free MPSC queue -- the API stays the same.
	struct slot_state {
		std::mutex m;
		std::deque<std::vector<uint8_t>> pending;
	};

	std::array<slot_state, 8> slots;

	// Producer API. Called from any thread. Returns false if the slot
	// index is out of range.
	bool push(int slot, const uint8_t* data, size_t size) {
		if (slot < 0 || slot > 7) return false;
		std::vector<uint8_t> copy(data, data + size);
		auto& s = slots[(size_t)slot];
		std::lock_guard<std::mutex> lock(s.m);
		s.pending.push_back(std::move(copy));
		return true;
	}
	bool push(int slot, std::vector<uint8_t> data) {
		if (slot < 0 || slot > 7) return false;
		auto& s = slots[(size_t)slot];
		std::lock_guard<std::mutex> lock(s.m);
		s.pending.push_back(std::move(data));
		return true;
	}

	// Consumer API. Called on the sim thread. Iterates slots 0..7 in
	// order; for each slot pops all pending commands (FIFO) and invokes
	// `fn(slot, data, size)` for each. `fn` is expected to inject the
	// bytes into sync.h::schedule_action for the virtual client that
	// owns that slot.
	//
	// Locking pattern: swap out the deque under the lock, then process
	// it lock-free. Producers can push new commands during the process
	// step; they'll be picked up on the next tick.
	template <typename Fn>
	void drain(Fn&& fn) {
		for (int slot = 0; slot < 8; ++slot) {
			std::deque<std::vector<uint8_t>> local;
			{
				auto& s = slots[(size_t)slot];
				std::lock_guard<std::mutex> lock(s.m);
				local.swap(s.pending);
			}
			for (auto& cmd : local) {
				fn(slot, cmd.data(), cmd.size());
			}
		}
	}

	size_t total_pending() const {
		// Snapshot count; useful for diagnostics only. Not consistent
		// with drain but that's fine for a heartbeat log.
		size_t total = 0;
		for (auto& s : slots) {
			std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(s.m));
			total += s.pending.size();
		}
		return total;
	}
};

} // namespace openbw_agents

#endif
