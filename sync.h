#ifndef BWGAME_SYNC_H
#define BWGAME_SYNC_H

#include "bwgame.h"
#include "actions.h"
#include "replay.h"
#include "replay_saver.h"

#include <chrono>
#include <random>
#include <thread>
#include <functional>
#include <type_traits>
#include <typeinfo>

namespace bwgame {

struct sync_state {
	struct scheduled_action {
		// Widened from uint8_t. Stock BW sync uses an 8-bit rolling
		// frame counter, fine when all peers advance in lock-step at
		// 42ms/frame. Our agent+observer setup can produce action bursts
		// where the observer falls >128 frames behind the server, at
		// which point the uint8_t (and its 2's-complement wraparound
		// arithmetic in all_clients_in_sync) sign-flips and actions
		// scheduled with frame==200 get skipped when sync_frame wraps
		// past them. Widening to uint32_t eliminates the wraparound
		// window entirely (128M frames = 33+ hours at 100 FPS).
		uint32_t frame;
		size_t data_begin;
		size_t data_end;
	};

	int latency = 2;
	bool is_first_bwapi_compatible_frame = true;

	int game_starting_countdown = 0;
	uint32_t start_game_seed = 0;
	bool game_started = false;

	struct uid_t {
		std::array<uint32_t, 8> vals{};
		static uid_t generate() {
			uid_t r;
			std::array<uint32_t, 8> arr;
			arr[0] = 42;
			arr[1] = (uint32_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
			arr[2] = (uint32_t)std::hash<std::thread::id>()(std::this_thread::get_id());
			arr[3] = (uint32_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
			arr[4] = (uint32_t)std::chrono::steady_clock::now().time_since_epoch().count();
			arr[5] = (uint32_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
			arr[6] = (uint32_t)std::chrono::system_clock::now().time_since_epoch().count();
			arr[7] = 1;
			std::seed_seq seq(arr.begin(), arr.end());
			seq.generate(r.vals.begin(), r.vals.end());
			data_loading::crc32_t crc32;
			const uint8_t* c = (const uint8_t*)arr.data();
			size_t n = 32;
			for (auto& v : r.vals) {
				v ^= crc32(c, n);
				c += 2;
				n -= 2;
			}
			return r;
		}
		bool operator<(const uid_t& n) const {
			return vals > n.vals;
		}
		bool operator==(const uid_t& n) const {
			return vals == n.vals;
		}
		bool operator!=(const uid_t& n) const {
			return vals != n.vals;
		}
		a_string str() {
			a_string r;
			for (auto& v : vals) r += format(r.empty() ? "%08x" : "-%08x", v);
			return r;
		}
	};

	struct client_t {
		uid_t uid;
		bool has_uid = false;
		int local_id = 0;
		int player_slot = -1;
		const void* h = nullptr;
		a_vector<uint8_t> buffer;
		size_t buffer_begin = 0;
		size_t buffer_end = 0;
		a_circular_vector<scheduled_action> scheduled_actions;
		// Widened from uint8_t; see scheduled_action::frame comment.
		uint32_t frame = 0;
		a_string name;
		bool game_started = false;
		bool has_greeted = false;
		std::chrono::steady_clock::time_point last_synced;

		// Auth state (used when sync_state::auth_check is installed).
		// The embedding server drives verification via the callback and
		// stashes an opaque per-user pointer here (e.g. openbw_auth::user_t*).
		bool has_auth = false;
		const void* auth_user = nullptr;
	};

	a_list<client_t> clients = {{uid_t::generate(), true}};
	int next_client_id = 1;
	client_t* local_client = &clients.front();

	int sync_frame = 0;

	bool has_initialized = false;
	std::array<race_t, 12> initial_slot_races;
	std::array<int, 12> initial_slot_controllers;
	std::array<race_t, 12> picked_races;

	bool game_type_melee = false;

	game_load_functions::setup_info_t* setup_info = nullptr;
	replay_saver_state* save_replay = nullptr;

	std::array<a_string, 12> player_names;

	int successful_action_count = 0;
	int failed_action_count = 0;
	std::array<uint32_t, 4> insync_hash{};
	uint8_t insync_hash_index = 0;

	// Captured at start_game() time so late-joiners can seed their own sim
	// identically to what the server (or earlier peers) computed. This is
	// the post-mix rand_state that start_game() derived from the raw seed
	// XOR'd with all client UIDs at that moment.
	uint32_t initial_rand_state = 0;

	// Optional per-connection authentication hook. If set, every incoming
	// remote client must send id_auth as its first sync message; the check
	// is called with the raw key bytes. Return non-null (an opaque user
	// pointer to be stashed on client_t::auth_user) to accept, or nullptr
	// to reject and disconnect. When null (default), no auth is required
	// -- preserving existing BW-peer flows.
	std::function<const void*(const uint8_t* key, size_t key_len)> auth_check;

	// Client-side: if non-empty, on_new_client will send id_auth with this
	// key immediately after the greeting. Set on the observer side; leave
	// empty on the server side.
	a_string outgoing_api_key;

	// Server-side: when a client successfully authenticates, this callback
	// (if set) is asked what perspective (player slot 0..7, or -1 for full
	// vision) that client should observe from. The server then sends
	// id_assign_perspective. The callback receives the opaque auth_user
	// pointer stashed on client_t. Default: nullptr = don't send perspective
	// assignment (client renders with full vision).
	std::function<int8_t(const void* auth_user)> perspective_for;

	// Server-side: called when a late-joining observer needs the catch-up
	// bundle. Must fill `out` with the concatenated replay_saver action
	// history (frame + chunk + owner + bytes, repeated) and return the
	// server's current sim frame + seed. If nullptr, no catch-up data is
	// sent (observer will start from frame 0 with no history).
	struct catchup_bundle_t {
		uint32_t current_frame;
		uint32_t seed;
		// Authoritative per-slot race the server settled on for this game.
		// The server may have started with map-default races and then
		// applied a --race override (or, in the future, a race chosen by
		// the connecting agent via a lobby message). Either way, the
		// observer's map load sees only the map's default races -- if it
		// runs start_game_impl without knowing what the server picked,
		// the "race > 2 -> lcg_rand(144)" path (sync.h line ~1342) fires
		// on the observer but not on the server (or vice versa), the two
		// sides consume different rand values, and lcg_rand_state
		// silently diverges for the rest of the game.
		//
		// Fix: server ships its st.players[i].race in the catchup bundle;
		// observer writes them into its own st.players before calling
		// start_game_local. Value at index i is the race enum id.
		std::array<uint8_t, 12> slot_races;
		a_vector<uint8_t> action_bytes;
	};
	std::function<catchup_bundle_t()> catchup_provider;

	// Client-side: perspective slot assigned by the server. -1 means full
	// vision. Updated when id_assign_perspective arrives. The UI polls
	// this each frame to filter rendering.
	int8_t viewing_slot = -1;

	// Client-side: set to true when an id_catchup_data message is being
	// processed. The observer's main loop watches this so the UI can
	// render a "catching up" indicator instead of stale state. Cleared
	// once fast-forward completes.
	bool catching_up = false;

	// Optional diagnostic sink for agent-action lifecycle events. When
	// set, both server and observer emit one line per interesting event
	// (SCHED / APPLY / etc.) so we can diff the two logs and see
	// exactly where they diverge. Format is tab-separated so awk/diff
	// are easy. See the AGENT_LOG_* call sites below for schemas.
	std::function<void(const a_string&)> sync_log;

	// Both sides: pointers into `clients` for the per-slot virtual clients
	// that carry agent-issued actions. NULL for slots without an agent
	// (inactive slots, or the observer-side before catchup wires them up).
	// The server owns the virtuals as first-class clients (has_uid/has_auth
	// = true, no socket). The observer creates matching entries on demand
	// when the first id_agent_action for a slot arrives.
	std::array<client_t*, 8> virtual_clients_by_slot{};

};

// Hex-dump the first up-to-`max` bytes of `data` into a comma-separated
// string. Used to include a fingerprint of each agent-action payload in
// the log so we can tell "was it the same action bytes both sides saw?"
inline a_string sync_log_bytes(const uint8_t* data, size_t n, size_t max = 8) {
	a_string out;
	size_t k = n < max ? n : max;
	for (size_t i = 0; i < k; ++i) {
		if (i) out += ",";
		char buf[4];
		snprintf(buf, sizeof(buf), "%02x", (unsigned)data[i]);
		out += buf;
	}
	if (n > k) out += "..";
	return out;
}

// Emit one line to the sync_log sink if it's installed. `side` is 'S'
// for server or 'O' for observer.
inline void sync_log_line(sync_state& sync_st, char side, const a_string& body) {
	if (!sync_st.sync_log) return;
	char prefix[24];
	snprintf(prefix, sizeof(prefix), "%c\t%d\t", side, sync_st.sync_frame);
	a_string line;
	line += prefix;
	line += body;
	line += "\n";
	sync_st.sync_log(line);
}

struct sync_server_noop {
	struct message_t {
		template<typename T>
		void put(T v) {}
		void put(const void* data, size_t size) {}
	};
	message_t new_message() {return {};}
	void send_message(const message_t& d, const void* h) {}
	void allow_send(const void* h, bool allow) {}
	void kill_client(const void* h) {}
	template<typename F>
	void set_on_kill(const void* h, F&& f) {}
	template<typename F>
	void set_on_message(const void* h, F&& f) {}
	std::chrono::steady_clock::time_point timeout_time;
	std::function<void()> timeout_function;
	template<typename duration_T, typename callback_F>
	void set_timeout(duration_T&& duration, callback_F&& callback) {
		timeout_time = std::chrono::steady_clock::now() + duration;
		timeout_function = std::forward<callback_F>(callback);
	}
	template<typename on_new_client_F>
	void poll(on_new_client_F&& on_new_client) {
		if (timeout_function && std::chrono::steady_clock::now() >= timeout_time) {
			auto f = std::move(timeout_function);
			timeout_function = nullptr;
			f();
		}
	}
	template<typename on_new_client_F>
	void run_one(on_new_client_F&& on_new_client) {
		if (timeout_function) {
			while (std::chrono::steady_clock::now() < timeout_time) {
				std::this_thread::sleep_until(timeout_time);
			}
			auto f = std::move(timeout_function);
			timeout_function = nullptr;
			f();
		} else error("sync_server_noop::run_one: can't wait without a timeout");
	}
	template<typename on_new_client_F, typename pred_F>
	void run_until(on_new_client_F&& on_new_client, pred_F&& pred) {
		while (!pred()) {
			run_one(on_new_client);
		}
	}
};

namespace sync_messages {
	enum {
		id_client_uid,
		id_client_frame,
		id_occupy_slot,
		id_start_game,
		id_game_info,
		id_set_race,
		id_game_started,
		id_leave_game,
		id_insync_check,
		id_create_unit,
		id_kill_unit,
		id_remove_unit,
		id_custom_action,
		// The auth message is sent by an incoming client immediately after
		// the greeting, BEFORE any other sync message. Payload: [uint16_t
		// key_length][key bytes]. The server calls sync_state::auth_check
		// (installed by the embedding server) to verify. On success the
		// server may also stash a per-user pointer on client_t::auth_user.
		// Without this message (and with auth_check installed), the client
		// is killed when the next message arrives.
		id_auth,
		// Server -> client: which player slot's perspective the observer
		// should render from. Payload: [int8_t slot]. slot == -1 means
		// "full vision" (spectator). Sent once after auth succeeds; the
		// receiver stashes it on sync_state::viewing_slot for the UI to
		// pick up.
		id_assign_perspective,
		// Server -> client: mid-game catch-up bundle for a late joiner.
		// Payload:
		//   [uint32_t current_frame]   -- server's current frame
		//   [uint32_t seed]            -- lcg_rand_state at game start
		//   [uint32_t action_bytes_len]
		//   [action_bytes]             -- concatenated replay_saver history
		//                                 (the BW replay action stream format)
		// On receipt, the observer:
		//   1. runs local start_game(seed) to spawn starting units + set race
		//   2. fast-forwards through the action stream up to current_frame
		//   3. transitions into normal live-sync mode
		// The action stream layout is [uint32_t frame][uint8_t chunk_size]
		// [uint8_t owner][action bytes] repeated -- see replay_saver.h.
		id_catchup_data,
		// Server -> observer: a BW action byte sequence executed on behalf
		// of a specific player slot. Payload: [uint8_t slot][action bytes].
		// The observer looks up its virtual client for that slot and
		// schedule_action's the payload there, so execute_scheduled_actions
		// will apply it on the same frame the server does. This is how
		// agent-issued commands reach observers who are already connected
		// (late joiners get the equivalent via id_catchup_data).
		id_agent_action
	};
	enum {
		id_game_started_escape = 0xdc
	};
}

struct sync_functions: action_functions {
	sync_state& sync_st;
	explicit sync_functions(state& st, action_state& action_st, sync_state& sync_st) : action_functions(st, action_st), sync_st(sync_st) {}

	std::function<void(int player_slot, data_loading::data_reader_le&)> on_custom_action;

	template<typename action_F>
	void execute_scheduled_actions(action_F&& action_f) {
		for (auto i = sync_st.clients.begin(); i != sync_st.clients.end();) {
			sync_state::client_t* c = &*i;
			++i;
			while (!c->scheduled_actions.empty() && (uint32_t)sync_st.sync_frame == c->scheduled_actions.front().frame) {
				auto act = c->scheduled_actions.front();
				c->scheduled_actions.pop_front();
				c->buffer_begin = act.data_end;
				const uint8_t* data = c->buffer.data();
				if (data + act.data_end > data + c->buffer.size()) error("data beyond end");

				// Log the apply BEFORE action_f runs so we see it even if
				// action_f throws.
				//
				// Includes: sim_frame (st.current_frame at apply time),
				// target_frame (from scheduled_action -- was set to
				// vc->frame + latency at schedule time). If server and
				// observer disagree on either value for the same action
				// bytes, we've caught the sim-frame drift that causes
				// downstream lcg divergence (SyncBreaker #5 hypothesis).
				if (sync_st.sync_log
				    && c->h == nullptr
				    && c->player_slot >= 0)
				{
					char side = sync_st.auth_check ? 'S' : 'O';
					size_t n = act.data_end - act.data_begin;
					char buf[192];
					snprintf(buf, sizeof(buf),
						"AGENT_APPLY\tslot=%d\tsim_frame=%d"
						"\ttarget_frame=%u\tvc_frame=%u"
						"\tlcg=%08x\tn_bytes=%zu\tbytes=",
						c->player_slot, (int)st.current_frame,
						act.frame, c->frame,
						(unsigned)st.lcg_rand_state, n);
					a_string body = buf;
					body += sync_log_bytes(data + act.data_begin, n);
					sync_log_line(sync_st, side, body);
				}

				data_loading::data_reader_le r(data + act.data_begin, data + act.data_end);
				// Guard the action-decode step: if actions.h's read_action*
				// throws (e.g. "invalid selection of N units", or unknown
				// opcode), catch it, log the offending frame with full
				// context, then SKIP this action and keep the observer
				// alive. Without the guard the process terminates and we
				// lose the ability to observe subsequent state. This is
                // ONLY on observers (auth_check == null) -- server-side
                // authoritative decode should still hard-fail loudly.
				//
				// On resume, the observer's sim state is one action behind
				// server's from this point forward, so subsequent
				// INVENTORY diverges. That's fine -- we want the diagnostic
				// bytes more than we want the observer to stay in sync.
				bool is_observer_side = (sync_st.auth_check == nullptr);
				if (is_observer_side) {
					try {
						if (!action_f(c, r)) break;
					} catch (bwgame::exception& e) {
						size_t n = act.data_end - act.data_begin;
						if (sync_st.sync_log) {
							char buf[256];
							snprintf(buf, sizeof(buf),
								"BAD_ACTION\tslot=%d\tvc_frame=%u\ttarget_frame=%u"
								"\tsync_frame=%d\tn_bytes=%zu\terror=%.100s\tbytes=",
								c->player_slot, c->frame, act.frame,
								sync_st.sync_frame, n, e.what());
							a_string body = buf;
							body += sync_log_bytes(data + act.data_begin, n);
							sync_log_line(sync_st, 'O', body);
						}
						// Also print to stderr for immediate visibility.
						fprintf(stderr,
							"[obs] BAD_ACTION slot=%d frame=%u/%u n=%zu: %s\n",
							c->player_slot, c->frame, act.frame, n, e.what());
						fflush(stderr);
						// Continue the loop -- try the next scheduled action.
					}
				} else {
					if (!action_f(c, r)) break;
				}
			}
		}
	}

	void next_frame() = delete;

	template<typename server_T>
	void next_frame(server_T& server) {
		sync(server);
		action_functions::next_frame();
		// LCG snapshot every sim frame (was every 30 -- bumped for a
		// bisect-the-race investigation). Cost: 1 short line per frame
		// per side. At speed=10 with 6-min games this is ~52000 lines/
		// side/game = a few MB of sync-log per participant. Fine for
		// local debugging; revert after we've localized the bug.
		//
		// After each sim tick both sides emit `LCG_TICK lcg=<hex>`. Diff
		// of the two logs at the SAME current_frame reveals the exact
		// frame lcg first differs -- which pins the divergence to a
		// specific bwgame::state_functions call between that frame and
		// the previous one.
		if (sync_st.sync_log && sync_st.game_started
		    && this->st.current_frame > 0)
		{
			char side = sync_st.auth_check ? 'S' : 'O';
			char buf[64];
			snprintf(buf, sizeof(buf), "LCG_TICK\tlcg=%08x",
				(unsigned)this->st.lcg_rand_state);
			bwgame::sync_log_line(sync_st, side, bwgame::a_string(buf));
		}

		// Per-tick loop-state trace: for each tick, log the current
		// values of sync_frame, current_frame, and every virtual-client
		// frame counter. This surfaces WHY the two sides get to
		// different lcg -- if their vc->frame trails diverge, we know
		// scheduling is offset. If sync_frame and current_frame stay
		// aligned but lcg drifts, the divergence is deep inside the
		// sim itself.
		if (sync_st.sync_log && sync_st.game_started) {
			char side = sync_st.auth_check ? 'S' : 'O';
			// Assemble a compact per-slot vc-frame vector. Uses
			// virtual_clients_by_slot -- server registers these at
			// startup, observer creates them lazily as id_agent_action
			// arrives, so early frames may have "-" for unpopulated
			// slots. Recording that unpopulated state IS the signal
			// we want.
			a_string vc_body = "\tvcs=";
			for (int slot = 0; slot < 8; ++slot) {
				if (slot > 0) vc_body += ",";
				auto* vc = sync_st.virtual_clients_by_slot[slot];
				if (vc) {
					char b[32];
					snprintf(b, sizeof(b), "%u", (unsigned)vc->frame);
					vc_body += b;
				} else {
					vc_body += "-";
				}
			}
			// Field-partitioned FNV-1a state hashes. Server and observer
			// disagree on lcg once their sims diverge. To pin the FIRST
			// state field that differs we compute a hash per group and
			// print all four. Whichever group's hash first splits between
			// server and observer is the divergence bucket:
			//   nu  = unit count
			//   ht  = hp + exact_position (same body as insync_hash)
			//   od  = order_type->id, order_state, main_order_timer
			//   tp  = unit_type->id, owner (identity + ownership)
			// If nu splits, list membership itself changed (unit spawned /
			// died on one side only). If ht splits, position or hp
			// changed. If od splits, an order/state machine advanced
			// differently. If tp splits, an owner/type field changed.
			uint32_t h_ht = 2166136261u;
			uint32_t h_od = 2166136261u;
			uint32_t h_tp = 2166136261u;
			auto mix = [](uint32_t& h, uint32_t v) {
				h ^= v; h *= 16777619u;
			};
			uint32_t nu = 0;
			for (unit_t* u : ptr(this->st.visible_units)) {
				++nu;
				mix(h_ht, (uint32_t)(u->shield_points + u->hp).raw_value);
				mix(h_ht, (uint32_t)u->exact_position.x.raw_value);
				mix(h_ht, (uint32_t)u->exact_position.y.raw_value);
				mix(h_od, u->order_type ? (uint32_t)u->order_type->id : 0u);
				mix(h_od, (uint32_t)u->order_state);
				mix(h_od, (uint32_t)u->main_order_timer);
				mix(h_tp, u->unit_type ? (uint32_t)u->unit_type->id : 0u);
				mix(h_tp, (uint32_t)u->owner);
			}
			char buf[288];
			snprintf(buf, sizeof(buf),
				"TICK\tsync_frame=%d\tcurrent_frame=%d\tlcg=%08x"
				"\tnu=%u\tht=%08x\tod=%08x\ttp=%08x",
				sync_st.sync_frame, (int)this->st.current_frame,
				(unsigned)this->st.lcg_rand_state,
				nu, h_ht, h_od, h_tp);
			a_string body = buf;
			body += vc_body;
			bwgame::sync_log_line(sync_st, side, body);
		}
	}

	// Diagnostic: dump a per-slot unit-type inventory to the sync-log.
	// Called manually at whatever cadence the caller wants (server main
	// loop / observer main loop). Output format:
	//   S/O <sync_frame> INVENTORY slot=N min=M gas=G <type_id>:<count> ...
	// If server and observer log the same frame with the same counts,
	// their sims are in sync. Difference reveals divergence and points
	// at which unit-type first diverges.
	void log_inventory(char side, int slot) {
		if (!sync_st.sync_log) return;
		if (slot < 0 || slot > 7) return;
		std::array<int, 256> counts{};
		std::array<int, 256> ipcounts{};
		for (auto* u : bwgame::ptr(this->st.player_units[slot])) {
			int t = (int)u->unit_type->id;
			if (t < 0 || t >= 256) continue;
			if (u->status_flags & bwgame::unit_t::status_flag_completed) counts[t]++;
			else ipcounts[t]++;
		}
		a_string body = "INVENTORY";
		char buf[64];
		// lcg=<hex> is the sim's current lcg_rand_state at dump time.
		// Emit it once per slot (redundant, since it's per-state not
		// per-slot, but keeps the row self-contained for easy diff).
		// If two sync-logs disagree on lcg at a given frame, the sims
		// are running different code paths since the last matching
		// point -- root cause is nondeterminism, not action drop.
		snprintf(buf, sizeof(buf), "\tslot=%d\tmin=%d\tgas=%d\tlcg=%08x",
			slot, this->st.current_minerals[slot],
			this->st.current_gas[slot],
			(unsigned)this->st.lcg_rand_state);
		body += buf;
		body += "\tcompleted=";
		bool first = true;
		for (int t = 0; t < 256; ++t) {
			if (counts[t] == 0) continue;
			if (!first) body += ",";
			first = false;
			snprintf(buf, sizeof(buf), "%d:%d", t, counts[t]);
			body += buf;
		}
		body += "\tin_progress=";
		first = true;
		for (int t = 0; t < 256; ++t) {
			if (ipcounts[t] == 0) continue;
			if (!first) body += ",";
			first = false;
			snprintf(buf, sizeof(buf), "%d:%d", t, ipcounts[t]);
			body += buf;
		}
		sync_log_line(sync_st, side, body);
	}

	template<typename server_T>
	void bwapi_compatible_next_frame(server_T& server) {
		if (sync_st.is_first_bwapi_compatible_frame) sync_st.is_first_bwapi_compatible_frame = false;
		else action_functions::next_frame();
		sync(server);
	}

	template<typename reader_T>
	bool schedule_action(sync_state::client_t* client, reader_T&& r) {
		size_t n = r.left();
		auto& buffer = client->buffer;
		auto& buffer_begin = client->buffer_begin;
		auto& buffer_end = client->buffer_end;
		size_t pos = buffer_end;
		size_t new_end = pos + n;
		auto grow_buffer = [&]() {
			const size_t max_size = 1024u * 4 * sync_st.latency;
			size_t new_size = buffer.size() + buffer.size() / 2;
			if (new_size > max_size) new_size = max_size;
			size_t required_size = n;
			for (auto& v : client->scheduled_actions) {
				required_size += v.data_end - v.data_begin;
			}
			if (new_size < required_size) new_size = required_size;
			//if (new_size >= max_size) error("action buffer is full for client (%d-%s)", client->local_id, client->uid.str());
			if (new_size >= max_size) return false;
			a_vector<uint8_t> new_buffer(new_size);
			buffer_begin = 0;
			size_t end = 0;
			const uint8_t* src = buffer.data();
			uint8_t* dst = new_buffer.data();
			for (auto& v : client->scheduled_actions) {
				size_t vn = v.data_end - v.data_begin;
				std::memcpy(dst + end, src + v.data_begin, vn);
				v.data_begin = end;
				v.data_end = end + vn;
				end += vn;
			}
			buffer = std::move(new_buffer);
			buffer_end = end;
			return true;
		};
		if (buffer_end < buffer_begin) {
			if (new_end >= buffer_begin) {
				if (!grow_buffer()) return false;
				pos = buffer_end;
				new_end = pos + n;
			}
		} else if (new_end > buffer.size()) {
			if (n < buffer_begin) {
				pos = 0;
				new_end = n;
			} else {
				if (buffer.size() < new_end) {
					if (!grow_buffer()) return false;
					pos = buffer_end;
					new_end = pos + n;
				}
			}
		}
		buffer_end = new_end;
		r.get_bytes(buffer.data() + pos, n);
		a_string str;
		for (size_t i = 0; i != n; ++i) str += format("%02x", (buffer.data() + pos)[i]);
		uint32_t target_frame = (uint32_t)(client->frame + sync_st.latency);
		client->scheduled_actions.push_back({target_frame, pos, buffer_end});

		// Only log agent-slot schedules -- observer/server sync heartbeats
		// use schedule_action too but aren't interesting for the divergence
		// diff. Real BW peers have client->h != nullptr; agent virtuals
		// have h == nullptr AND player_slot >= 0. That's our filter.
		if (sync_st.sync_log
		    && client->h == nullptr
		    && client->player_slot >= 0)
		{
			char side = sync_st.auth_check ? 'S' : 'O';
			char buf[128];
			snprintf(buf, sizeof(buf),
				"AGENT_SCHED_LOCAL\tslot=%d\tvc_frame=%u\ttarget_frame=%u\tn_bytes=%zu\tbytes=",
				client->player_slot, client->frame, target_frame, n);
			a_string body = buf;
			body += sync_log_bytes(buffer.data() + pos, n);
			sync_log_line(sync_st, side, body);
		}
		return true;
	}

	bool schedule_action(sync_state::client_t* client, const uint8_t* data, size_t data_size) {
		data_loading::data_reader_le r(data, data + data_size);
		return schedule_action(client, r);
	}

	template<size_t max_size, bool default_little_endian = true>
	struct writer {
		std::array<uint8_t, max_size> arr;
		size_t pos = 0;
		template<typename T, bool little_endian = default_little_endian>
		void put(T v) {
			static_assert(std::is_integral<T>::value, "don't know how to write this type");
			size_t n = pos;
			skip(sizeof(T));
			data_loading::set_value_at<little_endian>(data() + n, v);
		}
		void skip(size_t n) {
			pos += n;
			if (pos > arr.size()) error("sync_functions::writer: attempt to write past end");
		}
		void put_bytes(const uint8_t* src, size_t n) {
			skip(n);
			memcpy(data() + pos - n, src, n);
		}
		size_t size() const {
			return pos;
		}
		const uint8_t* data() const {
			return arr.data();
		}
		uint8_t* data() {
			return arr.data();
		}
	};

	template<bool default_little_endian = true>
	struct dynamic_writer {
		std::vector<uint8_t> vec;
		size_t pos = 0;
		dynamic_writer() = default;
		dynamic_writer(size_t initial_size) : vec(initial_size) {}
		template<typename T, bool little_endian = default_little_endian>
		void put(T v) {
			static_assert(std::is_integral<T>::value, "don't know how to write this type");
			size_t n = pos;
			skip(sizeof(T));
			data_loading::set_value_at<little_endian>(data() + n, v);
		}
		void skip(size_t n) {
			pos += n;
			if (pos >= vec.size()) {
				if (vec.size() < 2048) vec.resize(std::max(pos, vec.size() + vec.size()));
				else vec.resize(std::max(pos, std::max(vec.size() + vec.size() / 2, (size_t)32)));
			}
		}
		void put_bytes(const uint8_t* src, size_t n) {
			skip(n);
			memcpy(data() + pos - n, src, n);
		}
		size_t size() const {
			return pos;
		}
		const uint8_t* data() const {
			return vec.data();
		}
		uint8_t* data() {
			return vec.data();
		}
	};

	template<typename server_T>
	struct syncer_t {
		sync_functions& funcs;
		server_T& server;
		state& st;
		sync_state& sync_st;
		syncer_t(sync_functions& funcs, server_T& server) : funcs(funcs), server(server), st(funcs.st), sync_st(funcs.sync_st) {}

		const uint32_t greeting_value = 0x39e25069;

		void send(const uint8_t* data, size_t size, const void* h = nullptr) {
			if (size == 0) error("attempt to send no data");
			auto d = server.new_message();
			d.put(data, size);
			server.send_message(d, h);
			if (!h || h == sync_st.local_client) recv(sync_st.local_client, data, size);
		}
		template<typename data_T>
		void send(data_T&& data, const void* h = nullptr) {
			send(data.data(), data.size(), h);
		}
		template<typename reader_T>
		void recv(sync_state::client_t* client, reader_T&& r) {
			auto t = r.tell();
			int id = r.template get<uint8_t>();

			// If auth is required and this client hasn't yet passed it,
			// only id_auth is allowed. Anything else -> disconnect.
			// local_client is always trusted (it's ourselves, receiving
			// our own broadcast echoes).
			if (sync_st.auth_check && !client->has_auth && client != sync_st.local_client
				&& id != sync_messages::id_auth) {
				kill_client(client);
				return;
			}

			switch (id) {
			case sync_messages::id_auth: {
				uint16_t key_len = r.template get<uint16_t>();
				if (key_len > r.left() || key_len == 0 || key_len > 4096) {
					kill_client(client);
					return;
				}
				const uint8_t* key_bytes = r.get_n(key_len);
				if (sync_st.auth_check) {
					const void* user = sync_st.auth_check(key_bytes, key_len);
					if (!user) {
						kill_client(client);
						return;
					}
					client->has_auth = true;
					client->auth_user = user;
				} else {
					// No auth configured; still accept the message but
					// mark the client as authed so subsequent flows work.
					client->has_auth = true;
				}
				// Note: the id_assign_perspective response is sent AFTER
				// id_client_uid completes, because allow_send stays false
				// on the client's socket until that point. Handled below.
				break;
			}
			case sync_messages::id_assign_perspective: {
				int8_t slot = (int8_t)r.template get<uint8_t>();
				if (slot < -1 || slot > 7) slot = -1;
				sync_st.viewing_slot = slot;
				break;
			}
			case sync_messages::id_client_frame:
				// Wire widened from uint8_t to uint32_t so an observer
				// that lags >128 frames still schedules actions on the
				// correct target frame instead of wrapping. Client and
				// server must be built together -- no cross-version
				// compat with retail BW replays.
				client->frame = r.template get<uint32_t>();
				break;
			case sync_messages::id_client_uid: {
				sync_state::uid_t uid;
				for (auto& v : uid.vals) v = r.template get<uint32_t>();
				if (get_client(uid)) {
					this->kill_client(client);
				} else {
					// The stock check is `clients_with_uid >= 2`, which caps
					// pre-game peers at "local + one remote". That's fine for
					// BW-style 1v1 lobbies, but rejects extra observers.
					//
					// For our observer+agent setup this check needs to skip:
					//   - other observers (player_slot == -1, not local) --
					//     N observers should all be accepted.
					//   - server-owned virtual player clients (h == nullptr,
					//     player_slot >= 0) -- these are placeholders the
					//     agent WebSocket layer uses to inject actions; they
					//     aren't real lobby peers competing for a slot.
					//
					// Only local_client and other REAL network peers with a
					// player_slot claim a lobby slot.
					size_t peers_with_slot = 0;
					for (auto* c : ptr(sync_st.clients)) {
						if (!c->has_uid) continue;
						if (c == sync_st.local_client) {
							++peers_with_slot;
						} else if (c->player_slot != -1 && c->h != nullptr) {
							++peers_with_slot;
						}
					}
					if (peers_with_slot >= 2) {
						this->kill_client(client);
					} else {
						client->uid = uid;
						client->has_uid = true;

						client->name.clear();
						client->name.reserve(31);
						while (client->name.size() < 31) {
							char c = r.template get<uint8_t>();
							if (!c) break;
							client->name += c;
						}

						// Pre-game: reset all peers so the lobby is
						// consistent. Post-game (late-join): only initialize
						// the new peer; do NOT reset sync_frame or the other
						// peers, or the running sim would break.
						if (!sync_st.game_started) {
							for (int i = 0; i != 12; ++i) {
								st.players[i].controller = sync_st.initial_slot_controllers[i];
								st.players[i].race = sync_st.initial_slot_races[i];
							}
							for (auto* c : ptr(sync_st.clients)) {
								c->player_slot = -1;
								clear_scheduled_actions(c);
								c->frame = 0;
							}
							sync_st.sync_frame = 0;
						} else {
							// Late-join: align this peer's frame counter with
							// the current sync frame so all_clients_in_sync
							// doesn't stall waiting for their id_client_frame.
							client->player_slot = -1;
							clear_scheduled_actions(client);
							client->frame = (uint32_t)sync_st.sync_frame;
						}

						if (client->h) {
							server.allow_send(client->h, true);
							// Now that this client's socket is enabled for
							// sending, deliver the perspective assignment
							// (server-installed callback drives it). Only
							// fires for authenticated remote clients.
							if (client != sync_st.local_client
								&& client->has_auth
								&& sync_st.perspective_for) {
								int8_t slot = sync_st.perspective_for(client->auth_user);
								writer<2> pw;
								pw.put<uint8_t>(sync_messages::id_assign_perspective);
								pw.put<int8_t>(slot);
								send(pw, client->h);
							}

							// If the game is already running and we have a
							// catchup provider, ship the action log to this
							// late joiner so they can fast-forward.
							if (sync_st.game_started
								&& client != sync_st.local_client
								&& client->has_auth
								&& sync_st.catchup_provider) {
								auto bundle = sync_st.catchup_provider();
								dynamic_writer<> cw;
								cw.put<uint8_t>(sync_messages::id_catchup_data);
								cw.put<uint32_t>(bundle.current_frame);
								cw.put<uint32_t>(bundle.seed);
								// Ship the server's authoritative per-slot
								// races BEFORE the action-bytes payload so
								// handle_catchup can apply them BEFORE it
								// runs start_game_local. Order matters here
								// -- see catchup_bundle_t::slot_races doc.
								for (int i = 0; i < 12; ++i) {
									cw.put<uint8_t>(bundle.slot_races[i]);
								}
								cw.put<uint32_t>((uint32_t)bundle.action_bytes.size());
								cw.put_bytes(bundle.action_bytes.data(), bundle.action_bytes.size());
								send(cw, client->h);

								// Late-join broadcast race fix: the
								// catchup bundle above is built from
								// replay_saver.history, which records
								// actions at APPLY time (inside
								// execute_scheduled_actions, latency
								// frames after schedule). But agent
								// commands broadcast via id_agent_action
								// go out to peers at SCHEDULE time, not
								// apply time. If an agent command was
								// scheduled at server_frame T and this
								// late-joiner's greeting arrives before
								// server_frame T+latency (when the
								// action lands in history), the observer
								// gets: (a) nothing via live broadcast
								// -- the broadcast fired before it was
								// in sync_st.clients, and (b) nothing
								// via catchup -- history doesn't have
								// the action yet. Result: observer's
								// sim silently misses the action and
								// diverges by one morph forever.
								//
								// Fix: replay every server-side virtual
								// client's pending scheduled_actions to
								// this new peer as synthesized
								// id_agent_action messages. The observer
								// receives them, schedules them locally
								// at the same target_frame the server
								// uses, and applies them at the correct
								// sim frame -- byte-identical to what a
								// peer connected before the schedule
								// would have processed.
								for (auto* vc : ptr(sync_st.clients)) {
									if (vc->player_slot < 0) continue;
									if (vc->h != nullptr) continue;  // real peer, not virtual
									for (auto& sa : vc->scheduled_actions) {
										size_t n = sa.data_end - sa.data_begin;
										if (n == 0) continue;
										// Reconstruct the server_frame that was
										// used when this action was broadcast.
										// schedule_action set target_frame =
										// client->frame + latency, and at
										// broadcast time client->frame was
										// sync_st.sync_frame. So the recorded
										// sa.frame == server_frame + latency,
										// meaning server_frame_at_broadcast =
										// sa.frame - latency.
										uint32_t server_frame = sa.frame - (uint32_t)sync_st.latency;
										dynamic_writer<> aw;
										aw.put<uint8_t>(sync_messages::id_agent_action);
										aw.put<uint8_t>((uint8_t)vc->player_slot);
										aw.put<uint32_t>(server_frame);
										aw.put_bytes(vc->buffer.data() + sa.data_begin, n);
										send(aw, client->h);
									}
								}
							}
						}

						sync_st.clients.sort([&](auto& a, auto& b) {
							return a.uid < b.uid;
						});
					}
				}
				break;
			}
			case sync_messages::id_catchup_data: {
				// Client-side receipt: deserialize + fast-forward locally.
				// Delegated to a member function so the code is testable.
				handle_catchup(r);
				break;
			}
			case sync_messages::id_agent_action: {
				// Server -> observer live agent action. Payload:
				//   [uint8_t slot][uint32_t server_frame][action bytes...]
				// Route to our own virtual client for that slot so
				// execute_scheduled_actions applies it on the same
				// ABSOLUTE frame the server did -- we stamp vc->frame
				// with the server's sync_frame (from the message) rather
				// than our own local sync_frame, then schedule_action
				// enqueues at server_frame + latency. Since our local
				// sim advances one frame per id_client_frame heartbeat
				// after we're in sync, this fires on the same game
				// state the server had.
				//
				// Create the virtual client lazily on first sighting; a
				// fresh observer joining mid-game wouldn't have any yet.
				int slot = (int)r.template get<uint8_t>();
				if (slot < 0 || slot > 7) break;
				uint32_t server_frame = r.template get<uint32_t>();
				size_t n = r.left();
				if (n == 0) break;

				if (sync_st.sync_log) {
					char buf[128];
					snprintf(buf, sizeof(buf),
						"AGENT_RECV\tslot=%d\tserver_frame=%u\tn_bytes=%zu\tbytes=",
						slot, server_frame, n);
					a_string body = buf;
					// r.get_n(n) below advances the reader; snapshot bytes
					// without consuming.
					const uint8_t* peek = r.ptr;
					body += sync_log_bytes(peek, n);
					sync_log_line(sync_st, 'O', body);
				}

				sync_state::client_t* vc = sync_st.virtual_clients_by_slot[slot];
				if (!vc) {
					sync_st.clients.emplace_back();
					vc = &sync_st.clients.back();
					vc->local_id = sync_st.next_client_id++;
					vc->uid = sync_state::uid_t::generate();
					vc->has_uid = true;
					vc->has_auth = true;
					vc->has_greeted = true;
					vc->game_started = true;
					vc->player_slot = slot;
					vc->frame = server_frame;
					sync_st.virtual_clients_by_slot[slot] = vc;
				}
				// Anchor the schedule at the server's authoritative
				// frame, not ours.
				vc->frame = server_frame;
				funcs.schedule_action(vc, r.get_n(n), n);
				break;
			}
			default:
				if (!client->has_uid) kill_client(client);
				else {
					r.seek(t);
					funcs.schedule_action(client, r);
				}
			}
		}

		void recv(sync_state::client_t* client, const uint8_t* data, size_t data_size) {
			data_loading::data_reader_le r(data, data + data_size);
			return recv(client, r);
		}

		// Storage for the action bytes shipped in id_catchup_data. Kept as
		// a member so execute_actions can walk it across many next_frame
		// iterations without re-copying. Only the catch-up path touches
		// this; once live, action_st.actions_data_position is left alone.
		a_vector<uint8_t> catchup_action_bytes;

		// Called by the observer on receipt of id_catchup_data. Reads the
		// payload, seeds the local sim, and fast-forwards to the server's
		// current frame. Runs entirely locally: no messages are sent.
		template<typename reader_T>
		void handle_catchup(reader_T& r) {
			uint32_t target_frame = r.template get<uint32_t>();
			uint32_t rand_state = r.template get<uint32_t>();

			// Read the server's PRE-random-pick slot races. The server
			// captures sync_st.initial_slot_races at first sync_next_frame
			// (post setup_f, so --race overrides + map defaults are
			// baked in) and ships those here. That means "any race"
			// slots (map race==5) arrive as 5 on the observer, NOT as
			// the post-pick 0/1/2 the server ended up with. We install
			// them into st.players[i].race and start_game_impl runs
			// identically on both sides: same lcg_rand(144) calls for
			// race==5 slots, same lcg_rand(33) calls in randomize_slots,
			// same permutation. Post-catchup RNG state is byte-identical
			// to the server's post-start_game_impl state.
			//
			// SyncBreaker #3 (2026-07-11): before this fix, the server
			// shipped POST-pick races so observer's line-1370 saw
			// race<=2 and skipped the lcg_rand(144) call. On 4+ player
			// melee maps the resulting RNG desync fed into
			// randomize_slots and observer saw a slot-swapped view of
			// the game. 2-player maps happened to end up identical
			// even with different rand state (only 1 shuffle iteration
			// and swaps between two identical layouts).
			std::array<uint8_t, 12> slot_races{};
			for (int i = 0; i < 12; ++i) {
				slot_races[i] = r.template get<uint8_t>();
			}
			// Apply to st.players BEFORE start_game_local runs. Guard on
			// !game_started: if the observer is somehow reconnecting to
			// a game it already had bootstrapped, we don't want to stomp
			// the running sim's races. In practice handle_catchup is
			// only invoked on fresh joins, but the guard costs nothing.
			if (!sync_st.game_started) {
				for (int i = 0; i < 12; ++i) {
					st.players[i].race = (bwgame::race_t)slot_races[i];
				}
			}

			uint32_t action_bytes_len = r.template get<uint32_t>();

			if (action_bytes_len > r.left()) {
				// Corrupt bundle; abandon.
				return;
			}
			catchup_action_bytes.resize(action_bytes_len);
			if (action_bytes_len > 0) {
				r.get_bytes(catchup_action_bytes.data(), action_bytes_len);
			}

			sync_st.catching_up = true;

			// Bootstrap the sim identically to how the server did it. This
			// installs starting units, initial resources, races, etc.
			if (!sync_st.game_started) {
				start_game_local(rand_state);
			}

			// Feed the action stream through execute_actions, alternating
			// with next_frame(), until current_frame reaches target_frame.
			// action_functions::execute_actions is the same driver the
			// replay path uses.
			if (!catchup_action_bytes.empty()) {
				uint8_t* buf_begin = catchup_action_bytes.data();
				uint8_t* buf_end = buf_begin + catchup_action_bytes.size();
				while ((uint32_t)st.current_frame < target_frame) {
					funcs.action_functions::execute_actions(buf_begin, buf_end);
					funcs.action_functions::next_frame();
				}
			} else {
				// No actions in the log yet; just advance frames locally.
				while ((uint32_t)st.current_frame < target_frame) {
					funcs.action_functions::next_frame();
				}
			}

			// Align our sync_frame counter with the server's so future
			// id_client_frame heartbeats + all_clients_in_sync work.
			sync_st.sync_frame = (int)target_frame;
			sync_st.local_client->frame = (uint32_t)target_frame;

			sync_st.catching_up = false;
		}

		void send_greeting(const void* h) {
			auto d = server.new_message();
			d.template put<uint32_t>(greeting_value);
			// Widened from uint8_t along with the id_client_frame /
			// scheduled_action counter widening. The reader in
			// on_message() only checks greeting_value and ignores this
			// byte, but we keep it in the payload for future use and
			// widen it to stay consistent with the rest of the frame
			// counter representation.
			d.template put<uint32_t>(sync_st.sync_frame);
			server.send_message(d, h);
		}

		sync_state::client_t* get_client(const sync_state::uid_t& uid) {
			for (auto& c : sync_st.clients) {
				if (c.uid == uid) return &c;
			}
			return nullptr;
		}

		auto get_player_left_action(bool player_left) {
			writer<2> w;
			w.put<uint8_t>(87);
			w.put<uint8_t>(player_left ? 0 : 6);
			return w;
		}

		void kill_client(sync_state::client_t* client, bool player_left = false) {
			if (client->player_slot != -1) {
				if (sync_st.game_started) {
					auto w = get_player_left_action(player_left);
					data_loading::data_reader_le r(w.data(), w.data() + w.size());
					if (sync_st.save_replay) replay_saver_functions(*sync_st.save_replay).add_action(st.current_frame, client->player_slot, w.data(), w.size());
					if (funcs.read_action(client->player_slot, r)) ++sync_st.successful_action_count;
					else ++sync_st.failed_action_count;
				} else {
					st.players[client->player_slot].controller = player_t::controller_open;
				}
				client->player_slot = -1;
			}
			if (client == sync_st.local_client) error("attempt to kill local client");
			if (client->h) server.kill_client(client->h);
			for (auto i = sync_st.clients.begin(); i != sync_st.clients.end(); ++i) {
				if (&*i == client) {
					sync_st.clients.erase(i);
					break;
				}
			}
		}
		sync_state::client_t* new_client(const void* h) {
			sync_st.clients.emplace_back();
			sync_st.clients.back().local_id = sync_st.next_client_id++;
			sync_st.clients.back().h = h;
			sync_st.clients.back().last_synced = std::chrono::steady_clock::now();
			return &sync_st.clients.back();
		}
		void send_uid(const void* h) {
			writer<1 + 32 + 32> w;
			w.put<uint8_t>(sync_messages::id_client_uid);
			for (auto& v : sync_st.local_client->uid.vals) w.put<uint32_t>(v);
			size_t n = 0;
			for (char c : sync_st.local_client->name) {
				if (n >= 31) break;
				++n;
				w.put<uint8_t>(c);
			}
			w.put<uint8_t>(0);
			send(w, h);
		}

		void send_start_game() {
			writer<5> w;
			w.put<uint8_t>(sync_messages::id_start_game);
			uint32_t seed = 0;
			for (uint32_t v : sync_state::uid_t::generate().vals) {
				seed ^= v;
			}
			w.put<uint32_t>(seed);
			send(w);
		}
		void send_switch_to_slot(int n) {
			writer<3> w;
			if (sync_st.game_started) w.put<uint8_t>(sync_messages::id_game_started_escape);
			w.put<uint8_t>(sync_messages::id_occupy_slot);
			w.put<uint8_t>(n);
			send(w);
		}
		void send_set_race(race_t race) {
			writer<2> w;
			w.put<uint8_t>(sync_messages::id_set_race);
			w.put<uint8_t>((int)race);
			send(w);
		}
		void send_create_unit(const unit_type_t* unit_type, xy pos, int owner) {
			writer<15> w;
			if (sync_st.game_started) w.put<uint8_t>(sync_messages::id_game_started_escape);
			w.put<uint8_t>(sync_messages::id_create_unit);
			w.put<uint32_t>((int)unit_type->id);
			w.put<int32_t>(pos.x);
			w.put<int32_t>(pos.y);
			w.put<uint8_t>(owner);
			send(w);
		}
		void send_kill_unit(unit_t* u) {
			writer<6> w;
			if (sync_st.game_started) w.put<uint8_t>(sync_messages::id_game_started_escape);
			w.put<uint8_t>(sync_messages::id_kill_unit);
			w.put<uint32_t>(funcs.get_unit_id_32(u).raw_value);
			send(w);
		}
		void send_remove_unit(unit_t* u) {
			writer<6> w;
			if (sync_st.game_started) w.put<uint8_t>(sync_messages::id_game_started_escape);
			w.put<uint8_t>(sync_messages::id_remove_unit);
			w.put<uint32_t>(funcs.get_unit_id_32(u).raw_value);
			send(w);
		}

		void clear_scheduled_actions(sync_state::client_t* client) {
			client->buffer.clear();
			client->buffer_begin = 0;
			client->buffer_end = 0;
			client->scheduled_actions.clear();
		}

		void send_game_info(const void* h) {
			dynamic_writer<> w(0x100);
			w.put<uint8_t>(sync_messages::id_game_info);
			for (sync_state::client_t* c : ptr(sync_st.clients)) {
				if (c->uid == sync_state::uid_t{}) continue;
				w.put<uint8_t>(1);
				for (auto& v : c->uid.vals) w.put<uint32_t>(v);
				w.put<int8_t>(c->player_slot);
				if (c->player_slot == -1) w.put<int8_t>(-1);
				else w.put<int8_t>((int)st.players.at(c->player_slot).race);
				size_t n = std::min(c->name.size(), (size_t)0x20);
				w.put<uint8_t>(n);
				for (size_t i = 0; i != n; ++i) w.put<uint8_t>((uint8_t)*(c->name.data() + i));
			}
			w.put<uint8_t>(0);
			send(w, h);
		}

		void on_new_client(const void* h) {
			// Previously we rejected connections once the game was running,
			// forcing observers to be present before start_game. That gap is
			// now closed by id_catchup_data: a late joiner is admitted, and
			// the id_client_uid handler ships the action history + seed so
			// they can fast-forward locally.
			auto* c = new_client(h);
			send_greeting(h);
			// If we have an API key to present (client-side observer), send
			// id_auth before id_client_uid. The server, if it has auth
			// enabled, requires this before accepting any other message.
			if (!sync_st.outgoing_api_key.empty()) {
				dynamic_writer<> w;
				w.put<uint8_t>(sync_messages::id_auth);
				w.put<uint16_t>((uint16_t)sync_st.outgoing_api_key.size());
				w.put_bytes(
					(const uint8_t*)sync_st.outgoing_api_key.data(),
					sync_st.outgoing_api_key.size());
				send(w, h);
			}
			send_uid(h);
			auto frame = sync_st.sync_frame;
			sync_st.sync_frame = 0;
			sync_st.sync_frame = frame;
			server.allow_send(h, false);
			server.set_on_message(h, std::bind(&syncer_t::on_message, this, c, std::placeholders::_1, std::placeholders::_2));
			server.set_on_kill(h, std::bind(&syncer_t::kill_client, this, c, false));
		}
		void on_message(sync_state::client_t* client, const void* data, size_t size) {
			data_loading::data_reader_le r((const uint8_t*)data, (const uint8_t*)data + size);
			if (!client->has_greeted) {
				auto v = r.get<uint32_t>();
				if (v != greeting_value) {
					kill_client(client);
				} else client->has_greeted = true;
				return;
			}
			recv(client, (const uint8_t*)data, size);
		}
		void send_client_frame() {
			// Message layout: [u8 id][u32 frame]. Widened from u8 to
			// support observers that lag >128 frames (see the note on
			// sync_state::scheduled_action::frame).
			writer<5> w;
			w.put<uint8_t>(sync_messages::id_client_frame);
			w.put<uint32_t>(sync_st.sync_frame);
			send(w);
		}
		void timeout_func() {
			auto now = std::chrono::steady_clock::now();
			for (auto i = sync_st.clients.begin(); i != sync_st.clients.end();) {
				auto* c = &*i;
				++i;
				if (now - c->last_synced >= std::chrono::seconds(60)) {
					// Wraparound-safe delta at 32 bits (was int8_t when
					// c->frame was uint8_t).
					if ((int32_t)((uint32_t)sync_st.sync_frame - c->frame) >= (int32_t)sync_st.latency) {
						kill_client(c);
					}
				}
			}
			server.set_timeout(std::chrono::seconds(1), std::bind(&syncer_t::timeout_func, this));
		}

		void update_insync_hash() {
			uint32_t hash = 2166136261u;
			auto add = [&](auto v) {
				hash ^= (uint32_t)v;
				hash *= 16777619u;
			};
			add(sync_st.successful_action_count);
			add(sync_st.failed_action_count);
			add(st.lcg_rand_state);
			for (auto v : st.current_minerals) add(v);
			for (auto v : st.current_gas) add(v);
			for (auto v : st.total_minerals_gathered) add(v);
			for (auto v : st.total_gas_gathered) add(v);
			add(st.active_orders_size);
			add(st.active_bullets_size);
			add(st.active_thingies_size);
			for (unit_t* u : ptr(st.visible_units)) {
				add((u->shield_points + u->hp).raw_value);
				add(u->exact_position.x.raw_value);
				add(u->exact_position.y.raw_value);
			}

			if (sync_st.insync_hash_index == sync_st.insync_hash.size() - 1) sync_st.insync_hash_index = 0;
			else ++sync_st.insync_hash_index;
			sync_st.insync_hash[sync_st.insync_hash_index] = hash;
		}

		void send_insync_check() {
			writer<7> w;
			if (sync_st.game_started) w.put<uint8_t>(sync_messages::id_game_started_escape);
			w.put<uint8_t>(sync_messages::id_insync_check);
			w.put<uint8_t>(sync_st.insync_hash_index);
			w.put<uint32_t>(sync_st.insync_hash[sync_st.insync_hash_index]);
			send(w);
		}

		void send_game_started() {
			writer<1> w;
			w.put<uint8_t>(sync_messages::id_game_started);
			send(w);
		}
		void send_leave_game() {
			if (!sync_st.game_started) {
				writer<1> w;
				w.put<uint8_t>(sync_messages::id_leave_game);
				send(w);
			} else {
				send(get_player_left_action(true));
			}
		}

		void send_custom_action(const uint8_t* data, size_t size) {
			if (size == 0) error("attempt to send no data");
			dynamic_writer<> w;
			if (sync_st.game_started) w.template put<uint8_t>(sync_messages::id_game_started_escape);
			w.template put<uint8_t>(sync_messages::id_custom_action);
			w.put_bytes(data, size);
			send(w);
		}

		// Broadcast an agent-issued action to all connected socket peers
		// (observers). The server has already scheduled this action on
		// its own virtual client via schedule_action; observers need the
		// same bytes so their local sim tracks the server frame-for-frame.
		//
		// Wire layout:
		//   [uint8_t msg_id][uint8_t slot][uint32_t server_frame][action bytes...]
		//
		// server_frame is the sync_frame at which the server called
		// schedule_action for this agent command. The observer stamps
		// its virtual client's frame counter with this value before
		// scheduling, so both sims fire the action at the same absolute
		// frame ((server_frame + latency)). Without this, the observer
		// schedules against its OWN sync_frame, which lags the server;
		// the action then applies earlier in the observer's local sim
		// than it did on the server -- and if that earlier state doesn't
		// have enough resources (e.g. a 100-min Pylon build with only
		// 60 min accumulated locally), unit_build_order_valid rejects
		// the action and the observer silently diverges from server
		// state.
		//
		// We call the transport layer's send_message directly so we skip
		// the sync-layer send()'s local_client loopback, and we broadcast
		// (h == nullptr) so every socket peer receives it.
		void broadcast_agent_action(int slot, const uint8_t* data, size_t size) {
			if (size == 0) return;
			if (slot < 0 || slot > 7) return;
			auto d = server.new_message();
			d.template put<uint8_t>((uint8_t)sync_messages::id_agent_action);
			d.template put<uint8_t>((uint8_t)slot);
			d.template put<uint32_t>((uint32_t)sync_st.sync_frame);
			d.put(data, size);
			// h == nullptr broadcasts to every socket peer. NOTE: send_message
			// does NOT loopback to local_client (that happens in the sync-
			// layer send() wrapper, which we deliberately bypass).
			server.send_message(d, nullptr);

			if (sync_st.sync_log) {
				char buf[128];
				snprintf(buf, sizeof(buf),
					"AGENT_SCHED_SEND\tslot=%d\tserver_frame=%u\tn_bytes=%zu\tbytes=",
					slot, (uint32_t)sync_st.sync_frame, size);
				a_string body = buf;
				body += sync_log_bytes(data, size);
				sync_log_line(sync_st, 'S', body);
			}
		}

		void start_game(uint32_t seed) {
			a_string seed_str;
			for (auto& v : sync_st.clients) seed_str += v.uid.str();
			uint32_t rand_state = seed ^ data_loading::crc32_t()((const uint8_t*)seed_str.data(), seed_str.size());
			start_game_impl(rand_state, /*broadcast=*/true);
		}

		// Same as start_game(seed) but takes the already-mixed
		// lcg_rand_state directly and does NOT broadcast id_game_started.
		// Used by a late-joining observer whose local sim needs to be
		// bootstrapped with the exact rand stream the server computed at
		// its start_game time. Handshake mixing normally XORs the seed
		// with all client UIDs, which differ for late joiners.
		void start_game_local(uint32_t rand_state) {
			start_game_impl(rand_state, /*broadcast=*/false);
		}

		void start_game_impl(uint32_t rand_state, bool broadcast) {

			st.lcg_rand_state = rand_state;
			sync_st.initial_rand_state = rand_state;

			// Populate the identity map action_st.player_id[slot] = slot
			// for slots 0..11. Otherwise action_state is default-constructed
			// with all zeros, and action_functions::read_action's single-arg
			// form (see actions.h:1339-1345) does std::find(player_id, N)
			// to translate a wire "owner" byte into a slot. With the array
			// zeroed, slot 0 accidentally works but every other slot fails
			// with "execute_action: player id N not found". Live scheduled
			// actions on both server and observer use the two-arg
			// read_action(slot, r) form (see execute_scheduled_actions in
			// this file, line 1455) and never trip this. The catchup path
			// on the observer (handle_catchup -> action_functions::
			// execute_actions -> actions.h:1470 single-arg read_action)
			// DOES trip it -- any owner byte >= 1 in the catchup bundle
			// crashes the observer. Replay .rep loading (replay.h:187)
			// already populates player_id for the same reason; do it here
			// too so multiplayer games work symmetrically. Setting it in
			// start_game_impl means both server and observer get it before
			// any action dispatch, and start_game_local (observer's local
			// bootstrap prior to catchup replay) inherits the fix too.
			for (int i = 0; i < 12; ++i) {
				funcs.action_st.player_id[i] = i;
			}

			for (int i = 0; i != 12; ++i) {
				auto& v = st.players[i];
				if (v.controller == player_t::controller_computer) {
					v.controller = player_t::controller_occupied;
				}
				sync_st.picked_races[i] = v.race;
				if (!funcs.player_slot_active(i)) {
					v.controller = player_t::controller_inactive;
				} else {
					if ((int)v.race > 2) v.race = (bwgame::race_t)funcs.lcg_rand(144, 0, 2);
				}
			}

			auto slot_available = [&](size_t index) {
				auto c = sync_st.initial_slot_controllers[index];
				if (c == player_t::controller_open) return true;
				if (c == player_t::controller_computer) return true;
				if (c == player_t::controller_rescue_passive) return true;
				if (c == player_t::controller_unused_rescue_active) return true;
				if (c == player_t::controller_neutral) return true;
				return false;
			};

			auto randomize_slots = [&](auto pred) {
				bwgame::static_vector<size_t, 12> available_slots;
				for (auto& v : st.players) {
					size_t index = (size_t)(&v - st.players.data());
					if (slot_available(index) && pred(index)) {
						size_t index = (size_t)(&v - st.players.data());
						if (index < 8) available_slots.push_back(index);
					}
				}
				for (size_t i = available_slots.size(); i > 1;) {
					--i;
					size_t old_index = available_slots[i];
					size_t new_index = available_slots[funcs.lcg_rand(33, 0, i)];
					if (old_index == new_index) continue;
					std::swap(st.players[old_index], st.players[new_index]);
					std::swap(sync_st.picked_races[old_index], sync_st.picked_races[new_index]);
					for (auto* c : ptr(sync_st.clients)) {
						if ((int)old_index == c->player_slot) c->player_slot = new_index;
						else if ((int)new_index == c->player_slot) c->player_slot = old_index;
					}
				}
			};

			if (sync_st.game_type_melee) {
				randomize_slots([](size_t){return true;});
			} else {
				for (int force = 1; force <= 4; ++force) {
					randomize_slots([&](size_t index){return st.players.at(index).force == force;});
				}
			}
			sync_st.game_started = true;

			// Reset sync_frame to align with st.current_frame at game
			// start. Before this point, the server's pre-game
			// `funcs.sync(server)` busy-loop (used to wait for observers
			// / handshakes) has been incrementing sync_frame via
			// sync_next_frame() while st.current_frame stayed at 0.
			// Meanwhile the observer's sync_frame is still 0 (it hasn't
			// been ticking yet). If we don't reset, the server has
			// sync_frame == N and st.current_frame == 0 at game start,
			// but the observer has sync_frame == 0 == st.current_frame.
			// Actions get scheduled at sync_frame, but the sim state
			// advances by st.current_frame -- so the SAME action applied
			// at sync_frame X on both sides hits DIFFERENT st.current_frame
			// values, and the two sims diverge from that point on.
			//
			// Symptom: server builds many Pylons; observer freezes at
			// 2-3 because minerals/positions/build-validity checks all
			// evaluate against different game-state instants.
			sync_st.sync_frame = 0;
			// Also reset any per-client frame counters that were
			// stamped from the pre-game sync_frame. `local_client`
			// was doing sync ticks; other clients don't matter yet
			// because their sync_frame comes from wire messages.
			if (sync_st.local_client) sync_st.local_client->frame = 0;

			sync_st.clients.sort([&](auto& a, auto& b) {
				if ((unsigned)a.player_slot != (unsigned)b.player_slot) return (unsigned)a.player_slot < (unsigned)b.player_slot;
				return a.uid < b.uid;
			});
			if (broadcast) {
				send_game_started();
			}

			for (auto* c : ptr(sync_st.clients)) {
				if (c->player_slot != -1) sync_st.player_names.at(c->player_slot) = c->name;
			}

			if (sync_st.save_replay) {
				auto& r = *sync_st.save_replay;
				r.random_seed = st.lcg_rand_state;
				r.player_name = sync_st.local_client->name;
				r.map_tile_width = st.game->map_tile_width;
				r.map_tile_height = st.game->map_tile_height;
				r.active_player_count = 1;
				r.slot_count = range_size(funcs.active_players());
				r.game_type = sync_st.game_type_melee ? 2 : 10;
				r.tileset = st.game->tileset_index;

				r.game_name = "openbw game";
				r.map_name = st.game->scenario_name;
				r.setup_info = *sync_st.setup_info;
				r.players = st.players;
				r.player_names = sync_st.player_names;

			}

		}

		void process_messages() {

			if (sync_st.game_starting_countdown) {
				--sync_st.game_starting_countdown;
				if (sync_st.game_starting_countdown == 0) {
					start_game(sync_st.start_game_seed);
				}
			}

			if (sync_st.game_started) {
				funcs.execute_scheduled_actions([this](sync_state::client_t* client, auto& r) {
					if (client->game_started) {
						if (client->player_slot != -1) {
							int sync_message_id = r.template get<uint8_t>();
							if (sync_message_id == sync_messages::id_game_started_escape) {
								int id = r.template get<uint8_t>();
								switch (id) {
								case sync_messages::id_insync_check: {
									uint8_t index = r.template get<uint8_t>();
									uint32_t hash = r.template get<uint32_t>();
									if (hash != sync_st.insync_hash.at(index)) {
										this->kill_client(client);
									}
									break;
								}
								case sync_messages::id_create_unit: {
									const unit_type_t* unit_type = funcs.get_unit_type((UnitTypes)r.template get<uint32_t>());
									int x = r.template get<int32_t>();
									int y = r.template get<int32_t>();
									int owner = r.template get<uint8_t>();
									funcs.trigger_create_unit(unit_type, {x, y}, owner);
									break;
								}
								case sync_messages::id_kill_unit: {
									unit_t* u = funcs.get_unit(unit_id_32(r.template get<uint32_t>()));
									if (u) funcs.state_functions::kill_unit(u);
									break;
								}
								case sync_messages::id_remove_unit: {
									unit_t* u = funcs.get_unit(unit_id_32(r.template get<uint32_t>()));
									if (u) {
										funcs.hide_unit(u);
										funcs.state_functions::kill_unit(u);
									}
									break;
								}
								case sync_messages::id_custom_action: {
									if (funcs.on_custom_action) funcs.on_custom_action(client->player_slot, r);
									break;
								}
								}
								return true;
							} else {
								r.seek(r.tell() - 1);
							}
							if (sync_st.save_replay) {
								size_t t = r.tell();
								size_t n = r.left();
								replay_saver_functions(*sync_st.save_replay).add_action(st.current_frame, client->player_slot, r.get_n(n), n);
								r.seek(t);
							}
							funcs.read_action(client->player_slot, r);
							if (st.players.at(client->player_slot).controller != player_t::controller_occupied) {
								if (client != sync_st.local_client) this->kill_client(client);
								else this->clear_scheduled_actions(client);
								return false;
							}
						}
					} else {
						int id = r.template get<uint8_t>();
						if (id == sync_messages::id_game_started) {
							client->game_started = true;
						}
					}
					return true;
				});
			} else {
				funcs.execute_scheduled_actions([this](sync_state::client_t* client, auto& r) {
					int id = r.template get<uint8_t>();
					switch (id) {
					case sync_messages::id_game_info:
						while (r.template get<uint8_t>() != 0) {
							sync_state::uid_t uid;
							for (auto& v : uid.vals) v = r.template get<uint32_t>();
							sync_state::client_t* c = this->get_client(uid);
							if (!c) {
								c = this->new_client(nullptr);
								c->uid = uid;
							}
							c->player_slot = r.template get<int8_t>();
							if (c->player_slot < 0 || c->player_slot >= 12) c->player_slot = -1;
							int race = r.template get<int8_t>();
							if (c->player_slot != -1) {
								for (auto* c2 : ptr(sync_st.clients)) {
									if (c != c2 && c2->player_slot == c->player_slot) c2->player_slot = -1;
								}
								st.players[c->player_slot].controller = player_t::controller_occupied;
								st.players[c->player_slot].race = (bwgame::race_t)race;
							}
							size_t n = r.template get<uint8_t>();
							c->name.resize(std::min(n, (size_t)0x20));
							for (size_t i = 0; i != n; ++i) {
								if (i < c->name.size()) c->name[i] = (char)r.template get<uint8_t>();
							}
						}
						break;
					case sync_messages::id_occupy_slot: {
						int n = r.template get<uint8_t>();
						for (int i = 0; i != 12; ++i) {
							if (i == n && st.players[i].controller == player_t::controller_open) {
								race_t race = sync_st.initial_slot_races[i];
								if (client->player_slot != -1) {
									race = st.players[client->player_slot].race;
									st.players[client->player_slot].controller = player_t::controller_open;
									st.players[client->player_slot].race = sync_st.initial_slot_races[client->player_slot];
								}
								client->player_slot = i;
								st.players[i].controller = player_t::controller_occupied;
								if (sync_st.initial_slot_races[i] == (bwgame::race_t)5) {
									st.players[i].race = race;
								}
								break;
							}
						}
						break;
					}
					case sync_messages::id_set_race: {
						race_t race = (race_t)r.template get<uint8_t>();
						if (client->player_slot != -1) {
							if (sync_st.initial_slot_races[client->player_slot] == (bwgame::race_t)5) {
								st.players[client->player_slot].race = race;
							}
						}
						break;
					}
					case sync_messages::id_start_game:
						if (!sync_st.game_starting_countdown) {
							sync_st.game_starting_countdown = 10;
							sync_st.start_game_seed = r.template get<uint32_t>();
						}
						break;
					case sync_messages::id_leave_game:
						if (client != sync_st.local_client) this->kill_client(client);
						else this->clear_scheduled_actions(client);
						return false;
					default: error("unknown pre game message id %d", id);
					}
					return true;
				});
			}
		}

		void sync_next_frame() {
			if (!sync_st.has_initialized) {
				if (!sync_st.setup_info) error("sync_state::setup_info is null");
				sync_st.has_initialized = true;
				for (int i = 0; i != 12; ++i) {
					sync_st.initial_slot_races[i] = st.players[i].race;
					sync_st.initial_slot_controllers[i] = st.players[i].controller;
					sync_st.picked_races[i] = st.players[i].race;
				}
			}
			++sync_st.sync_frame;
			send_client_frame();

			if (sync_st.game_started && sync_st.sync_frame % 32 == 0) {
				update_insync_hash();
				send_insync_check();
			}
		}

		bool all_clients_in_sync() {
			// The server side installs an auth_check callback; a plain
			// observer client never does. Use that as the "am I the
			// authoritative server?" discriminator.
			const bool is_server_side = (bool)sync_st.auth_check;

			for (auto* c : ptr(sync_st.clients)) {
				// Virtual clients (server-side placeholders for agent
				// slots, no socket handle) never send id_client_frame
				// heartbeats. Their frame counter would fall behind
				// immediately if we checked it, stalling the sim. They're
				// internal, not real peers -- exclude from the check on
				// both sides.
				if (c != sync_st.local_client && c->h == nullptr) continue;

				if (is_server_side) {
					// On the server: observer clients (player_slot == -1)
					// never issue player commands; they only receive the
					// action broadcast. Don't let a slow observer stall
					// the whole game.
					if (c->player_slot == -1 && c != sync_st.local_client) continue;
				}
				// On the observer side we DO gate on the server-as-peer's
				// frame counter. That's what paces us to the server's
				// tick_ms -- without it we race ahead as fast as our own
				// main loop can iterate.

				// Wraparound-safe delta at 32 bits (was int8_t when
				// c->frame was uint8_t). See scheduled_action::frame
				// comment for why we widened.
				if ((int32_t)((uint32_t)sync_st.sync_frame - c->frame) >= (int32_t)sync_st.latency) {
					return false;
				}
			}
			return true;
		}

		void sync() {
			sync_next_frame();

			server.set_timeout(std::chrono::seconds(1), std::bind(&syncer_t::timeout_func, this));
			server.poll(std::bind(&syncer_t::on_new_client, this, std::placeholders::_1));

			auto pred = [this]() {
				return all_clients_in_sync();
			};

			if (!sync_st.game_started && !sync_st.game_starting_countdown && pred()) {
				auto any_scheduled_actions = [&]() {
					for (auto& c : sync_st.clients) {
						if (!c.scheduled_actions.empty()) return true;
					}
					return false;
				};
				bool timed_out = false;
				server.set_timeout(std::chrono::milliseconds(50), [&]{
					timed_out = true;
				});
				while (!any_scheduled_actions() && !timed_out) {
					server.run_one(std::bind(&syncer_t::on_new_client, this, std::placeholders::_1));
				}
				if (!pred()) {
					server.set_timeout(std::chrono::seconds(1), std::bind(&syncer_t::timeout_func, this));
					server.run_until(std::bind(&syncer_t::on_new_client, this, std::placeholders::_1), pred);
				}
			} else {
				server.run_until(std::bind(&syncer_t::on_new_client, this, std::placeholders::_1), pred);
			}

			auto now = std::chrono::steady_clock::now();
			for (auto& c : sync_st.clients) {
				c.last_synced = now;
			}

			process_messages();
		}

		void final_sync() {
			bool timed_out = false;
			server.set_timeout(std::chrono::milliseconds(250), [&]{
				timed_out = true;
			});
			while (!sync_st.local_client->scheduled_actions.empty() && !timed_out) {
				sync_next_frame();
				server.poll(std::bind(&syncer_t::on_new_client, this, std::placeholders::_1));
				while (!all_clients_in_sync() && !timed_out) {
					server.run_one(std::bind(&syncer_t::on_new_client, this, std::placeholders::_1));
				}
				if (!timed_out) process_messages();
			}
		}

		void leave_game() {
			send_leave_game();
			final_sync();
		}
	};

	struct syncer_container_t {
		static const size_t size = 0x40;
		static const size_t alignment = alignof(std::max_align_t);
		const std::type_info* type = nullptr;
		const void* server_ptr = nullptr;
		std::aligned_storage<size, alignment>::type obj;
		void (syncer_container_t::* destroy_f)();

		syncer_container_t() = default;
		syncer_container_t(const syncer_container_t&) = delete;
		syncer_container_t& operator=(const syncer_container_t&) = delete;
		~syncer_container_t() {
			destroy();
		}

		void destroy() {
			if (type) (this->*destroy_f)();
		}

		template<typename T, typename server_T>
		void construct(sync_functions& funcs, server_T& server) {
			static_assert(sizeof(T) <= size || alignof(T) <= alignment, "syncer_container_t size or alignment too small");
			new ((T*)&obj) T(funcs, server);
			type = &typeid(T);
			server_ptr = &server;
			destroy_f = &syncer_container_t::destroy<T>;
		}
		template<typename T>
		void destroy() {
			as<T>().~T();
			type = nullptr;
		}
		template<typename T>
		T& as() {
			static_assert(sizeof(T) <= size || alignof(T) <= alignment, "syncer_container_t size or alignment too small");
			return (T&)obj;
		}

		template<typename T, typename server_T>
		T& get(sync_functions& funcs, server_T& server) {
			if (type) {
				if (type == &typeid(T) || *type == typeid(T)) {
					if (server_ptr == &server) return as<T>();
				}
				error("sync_functions::syncer_container_t: attempt to use multiple servers in the same instance");
			}
			construct<T>(funcs, server);
			return as<T>();
		}
	};

	syncer_container_t syncer_container;

	template<typename server_T>
	syncer_t<server_T>& get_syncer(server_T& server) {
		return syncer_container.get<syncer_t<server_T>>(*this, server);
	}

	template<typename server_T>
	void sync(server_T& server) {
		get_syncer(server).sync();
	}

	template<typename server_T>
	void start_game(server_T& server) {
		if (sync_st.game_started) return;
		get_syncer(server).send_start_game();
	}

	template<typename server_T>
	void switch_to_slot(server_T& server, int n) {
		get_syncer(server).send_switch_to_slot(n);
	}

	void set_local_client_name(a_string name) {
		if (sync_st.game_started) return;
		sync_st.local_client->name = std::move(name);
	}

	template<typename server_T>
	void set_local_client_race(server_T& server, race_t race) {
		if (sync_st.game_started) return;
		get_syncer(server).send_set_race(race);
	}

	template<typename server_T>
	void input_action(server_T& server, const uint8_t* data, size_t size) {
		get_syncer(server).send(data, size);
	}

	// Server-side helper: after schedule_action'ing an agent-issued
	// action on a virtual client, mirror the bytes to every connected
	// observer so their sim stays in sync. Wraps the bytes in an
	// id_agent_action envelope; observers route via their own
	// virtual_clients_by_slot on receipt. No local loopback.
	template<typename server_T>
	void broadcast_agent_action(server_T& server, int slot,
		const uint8_t* data, size_t size)
	{
		get_syncer(server).broadcast_agent_action(slot, data, size);
	}

	template<typename server_T>
	void leave_game(server_T& server) {
		get_syncer(server).leave_game();
	}

	int connected_player_count() {
		int clients_with_uid = 0;
		for (auto* c : ptr(sync_st.clients)) {
			if (c->has_uid) ++clients_with_uid;
		}
		return clients_with_uid;
	}

	template<typename server_T>
	void create_unit(server_T& server, const unit_type_t* unit_type, xy pos, int owner) {
		get_syncer(server).send_create_unit(unit_type, pos, owner);
	}

	template<typename server_T>
	void kill_unit(server_T& server, unit_t* u) {
		get_syncer(server).send_kill_unit(u);
	}

	template<typename server_T>
	void remove_unit(server_T& server, unit_t* u) {
		get_syncer(server).send_remove_unit(u);
	}

	template<typename server_T>
	void send_custom_action(server_T& server, const void* data, size_t size) {
		get_syncer(server).send_custom_action((const uint8_t*)data, size);
	}

};


}

#endif
