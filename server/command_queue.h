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
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace openbw_agents {

// One entry on the per-slot pending FIFO. Historically this was just
// vector<uint8_t>; we widened it in 2026-07 so the closed-loop
// command-result path can correlate the sim's apply-time outcome back
// to the client-supplied "id" (rid). See docs/agent_command_status_codes.md.
//
// A JSON command like {"verb":"train"} typically encodes to two BW
// action blobs (select + verb). Both share the same rid; only the
// last blob has is_terminal=true. The result message is emitted at
// most once per rid, on the terminal blob's apply.
//
// Untracked producers (in-process HTTP tests, control-plane
// prepopulated actions) leave rid empty and is_terminal=true -- they
// get no result message and no queue overhead per push.
struct queued_cmd {
	std::string rid;
	std::string verb;
	std::vector<uint8_t> bytes;
	bool is_terminal = true;
	// Deliver-result hook. When set (only on the terminal blob of a
	// tracked command), the sim thread calls this AFTER read_action
	// with the resolved status + apply frame. Implementation captures
	// a shared_ptr to the ws connection so it survives async delivery
	// across the io_context.post hop. See docs/agent_command_status_codes.md.
	std::function<void(int status, uint32_t applied_at_frame)> deliver;
};

struct command_queue {
	// N.B. std::deque under mutex is plenty. If profiling ever shows this
	// as hot, swap for a lock-free MPSC queue -- the API stays the same.
	struct slot_state {
		std::mutex m;
		std::deque<queued_cmd> pending;
	};

	std::array<slot_state, 8> slots;

	// Producer API. Called from any thread. Returns false if the slot
	// index is out of range.
	bool push(int slot, const uint8_t* data, size_t size) {
		if (slot < 0 || slot > 7) return false;
		queued_cmd q;
		q.bytes.assign(data, data + size);
		auto& s = slots[(size_t)slot];
		std::lock_guard<std::mutex> lock(s.m);
		s.pending.push_back(std::move(q));
		return true;
	}
	bool push(int slot, std::vector<uint8_t> data) {
		if (slot < 0 || slot > 7) return false;
		queued_cmd q;
		q.bytes = std::move(data);
		auto& s = slots[(size_t)slot];
		std::lock_guard<std::mutex> lock(s.m);
		s.pending.push_back(std::move(q));
		return true;
	}
	// Tracked push: keep the rid + is_terminal alive through the
	// sim thread so on-apply can look up the pending result tracker.
	// The deliver hook is only meaningful on the terminal blob; on
	// non-terminal blobs the caller passes an empty function (default
	// std::function is empty and cheap to move).
	bool push(int slot, std::vector<uint8_t> data,
	          std::string rid, std::string verb, bool is_terminal,
	          std::function<void(int, uint32_t)> deliver) {
		if (slot < 0 || slot > 7) return false;
		queued_cmd q;
		q.bytes = std::move(data);
		q.rid = std::move(rid);
		q.verb = std::move(verb);
		q.is_terminal = is_terminal;
		q.deliver = std::move(deliver);
		auto& s = slots[(size_t)slot];
		std::lock_guard<std::mutex> lock(s.m);
		s.pending.push_back(std::move(q));
		return true;
	}

	// Consumer API. Called on the sim thread. Iterates slots 0..7 in
	// order; for each slot pops all pending commands (FIFO) and invokes
	// `fn(slot, cmd)` for each -- cmd is a queued_cmd (const ref-ish;
	// callback may move out of it since we drop the local afterwards).
	// `fn` is expected to inject the bytes into sync.h::schedule_action
	// for the virtual client that owns that slot.
	//
	// Locking pattern: swap out the deque under the lock, then process
	// it lock-free. Producers can push new commands during the process
	// step; they'll be picked up on the next tick.
	template <typename Fn>
	void drain(Fn&& fn) {
		for (int slot = 0; slot < 8; ++slot) {
			std::deque<queued_cmd> local;
			{
				auto& s = slots[(size_t)slot];
				std::lock_guard<std::mutex> lock(s.m);
				local.swap(s.pending);
			}
			for (auto& cmd : local) {
				fn(slot, cmd);
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
