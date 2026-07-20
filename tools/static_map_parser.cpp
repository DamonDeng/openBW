// static_map_parser: convert Blizzard .scm/.scx map archives into plain
// static-only JSON dumps. Reuses the engine's own MPQ + CHK + tileset
// pipeline via game_load_functions::load_map_file, so the output is
// definitionally identical to what the runtime sees.
//
// Emits only STATIC information (never changes during a game):
//   - dim, tileset, version
//   - start locations
//   - players (controller + race per map)
//   - neutrals (mineral fields, geysers) with starting amounts
//   - per-tile flags grid  (32 px tiles, uint16 flags each)
//   - per-walk-cell walkable grid (8 px cells, uint8 each; bit 7 =
//     unwalkable, matches openbw's `unwalkable_flags` convention)
//
// Excludes DYNAMIC state: fog visible/explored, occupied flag,
// creep, unit HP/energy/orders, player resources, etc.
//
// Batch mode iterates a directory of maps and writes one <slug>.json
// per map plus an _index.json manifest for the runtime MapRegistry.

#include "bwgame.h"
#include "../server/sha256.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

// ---------- CLI ----------

struct Args {
    std::string data_path;              // stardat.mpq / broodat.mpq dir
    std::string single_map;             // --map <file>, mutually exclusive with --maps-dir
    std::string maps_dir;               // --maps-dir <dir>
    std::string out_dir = "./local_map_static";
    std::string pattern = "*.sc?";      // simple glob: * ? only, no [] or **
    bool skip_existing = false;
    bool strict = false;                // fail on slug collision instead of hashing
    bool verbose = false;
};

void print_usage() {
    std::fprintf(stderr,
        "usage: openbw_static_map_parser --data <dir> "
        "(--map <file> | --maps-dir <dir>) --out-dir <dir> [options]\n"
        "\n"
        "  --data <dir>         directory containing stardat.mpq/broodat.mpq\n"
        "  --map <file>         parse a single .scm/.scx file\n"
        "  --maps-dir <dir>     parse every map in <dir> (non-recursive)\n"
        "  --out-dir <dir>      output folder (created if missing)\n"
        "  --pattern <glob>     filter for --maps-dir (default '*.sc?')\n"
        "  --skip-existing      don't re-parse maps whose output already exists\n"
        "  --strict             fail if two source maps sanitize to the same slug\n"
        "  --verbose            print each map's slug/counts on success\n");
}

bool parse_args(int argc, char** argv, Args& out) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s expects a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--data")            { auto v = next("--data"); if (!v) return false; out.data_path = v; }
        else if (a == "--map")        { auto v = next("--map"); if (!v) return false; out.single_map = v; }
        else if (a == "--maps-dir")   { auto v = next("--maps-dir"); if (!v) return false; out.maps_dir = v; }
        else if (a == "--out-dir")    { auto v = next("--out-dir"); if (!v) return false; out.out_dir = v; }
        else if (a == "--pattern")    { auto v = next("--pattern"); if (!v) return false; out.pattern = v; }
        else if (a == "--skip-existing") out.skip_existing = true;
        else if (a == "--strict")     out.strict = true;
        else if (a == "--verbose")    out.verbose = true;
        else if (a == "-h" || a == "--help") { print_usage(); std::exit(0); }
        else {
            std::fprintf(stderr, "error: unknown argument '%s'\n", a.c_str());
            print_usage();
            return false;
        }
    }
    if (out.data_path.empty()) {
        std::fprintf(stderr, "error: --data is required\n");
        return false;
    }
    if (out.single_map.empty() == out.maps_dir.empty()) {
        std::fprintf(stderr, "error: exactly one of --map / --maps-dir must be provided\n");
        return false;
    }
    return true;
}

// ---------- Filesystem helpers ----------

bool mkdir_p(const std::string& path) {
    // Recursive mkdir. Returns true if the directory exists after the call.
    if (path.empty()) return true;
    struct stat st{};
    if (::stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    auto slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0) {
        if (!mkdir_p(path.substr(0, slash))) return false;
    }
    if (::mkdir(path.c_str(), 0755) == 0) return true;
    return errno == EEXIST;
}

bool file_exists(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
}

std::vector<std::string> list_dir(const std::string& dir) {
    std::vector<std::string> out;
    DIR* d = ::opendir(dir.c_str());
    if (!d) return out;
    while (auto* e = ::readdir(d)) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        out.push_back(std::move(n));
    }
    ::closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

bool glob_match(const std::string& pat, const std::string& name) {
    // Case-insensitive * / ? glob. Enough for '*.sc?'.
    size_t pi = 0, ni = 0;
    size_t star_p = std::string::npos, star_n = 0;
    auto lc = [](char c) { return (char)std::tolower((unsigned char)c); };
    while (ni < name.size()) {
        if (pi < pat.size() && (pat[pi] == '?' || lc(pat[pi]) == lc(name[ni]))) {
            ++pi; ++ni;
        } else if (pi < pat.size() && pat[pi] == '*') {
            star_p = pi++;
            star_n = ni;
        } else if (star_p != std::string::npos) {
            pi = star_p + 1;
            ni = ++star_n;
        } else {
            return false;
        }
    }
    while (pi < pat.size() && pat[pi] == '*') ++pi;
    return pi == pat.size();
}

std::string basename_of(const std::string& path) {
    auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string slugify(const std::string& name) {
    // Strip extension, lowercase, non-alnum -> '_', collapse repeats, trim.
    std::string s = name;
    auto dot = s.find_last_of('.');
    if (dot != std::string::npos) s.resize(dot);
    for (auto& c : s) {
        if (std::isalnum((unsigned char)c)) c = (char)std::tolower((unsigned char)c);
        else c = '_';
    }
    std::string out;
    out.reserve(s.size());
    bool prev_us = false;
    for (char c : s) {
        if (c == '_') { if (!prev_us) out.push_back('_'); prev_us = true; }
        else          { out.push_back(c); prev_us = false; }
    }
    while (!out.empty() && out.front() == '_') out.erase(out.begin());
    while (!out.empty() && out.back()  == '_') out.pop_back();
    if (out.empty()) out = "map";
    return out;
}

std::string hex_encode(const void* data, size_t n) {
    static const char* h = "0123456789abcdef";
    std::string out;
    out.resize(n * 2);
    auto* p = (const unsigned char*)data;
    for (size_t i = 0; i < n; ++i) {
        out[2 * i]     = h[p[i] >> 4];
        out[2 * i + 1] = h[p[i] & 0xf];
    }
    return out;
}

std::string sha256_hex(const void* data, size_t n) {
    auto d = openbw_auth::sha256::hash(data, n);
    return hex_encode(d.data(), d.size());
}

// ---------- Minimal JSON writer ----------
//
// Zero dependency; the shape of our output is small and mechanical, so a
// hand-rolled writer beats pulling in nlohmann/json.

struct J {
    std::ostringstream ss;
    int depth = 0;
    bool need_comma = false;

    void indent() { for (int i = 0; i < depth; ++i) ss << "  "; }
    void maybe_comma() {
        if (need_comma) ss << ",\n";
        else if (depth) ss << "\n";
        indent();
        need_comma = true;
    }

    void begin_obj() { maybe_comma(); ss << "{"; ++depth; need_comma = false; }
    void end_obj()   { --depth; ss << "\n"; indent(); ss << "}"; need_comma = true; }
    void begin_arr() { maybe_comma(); ss << "["; ++depth; need_comma = false; }
    void end_arr()   { --depth; ss << "\n"; indent(); ss << "]"; need_comma = true; }

    static std::string escape(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 2);
        out.push_back('"');
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if ((unsigned char)c < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                        out += buf;
                    } else {
                        out.push_back(c);
                    }
            }
        }
        out.push_back('"');
        return out;
    }

    void key(const char* k) {
        maybe_comma();
        ss << escape(k) << ": ";
        need_comma = false;
    }

    void v_str(const std::string& s)  { maybe_comma(); ss << escape(s); }
    void v_int(long long i)           { maybe_comma(); ss << i; }
    void v_uint(unsigned long long u) { maybe_comma(); ss << u; }
    void v_bool(bool b)               { maybe_comma(); ss << (b ? "true" : "false"); }

    void kv_str(const char* k, const std::string& v)   { key(k); ss << escape(v); need_comma = true; }
    void kv_int(const char* k, long long v)            { key(k); ss << v;         need_comma = true; }
    void kv_uint(const char* k, unsigned long long v)  { key(k); ss << v;         need_comma = true; }
    void kv_bool(const char* k, bool v)                { key(k); ss << (v ? "true" : "false"); need_comma = true; }
};

// ---------- The actual dump ----------
//
// One MapInfo per map; produced from a fresh game_player so nothing
// bleeds between maps in batch mode.

struct MapInfo {
    std::string source_file;
    std::string source_sha256;
    int version = 0;
    int tile_w = 0, tile_h = 0;
    int tileset_index = 0;
    std::string tileset_name;
    int start_count = 0;
    int mineral_count = 0;
    int geyser_count = 0;
    std::string tile_flags_sha256;
    size_t bytes_written = 0;
};

const char* tileset_name_of(int i) {
    static const char* names[8] = {
        "badlands", "platform", "install", "AshWorld",
        "Jungle", "Desert", "Ice", "Twilight"
    };
    return (i >= 0 && i < 8) ? names[i] : "unknown";
}

std::vector<uint8_t> read_file_bytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    auto n = f.tellg();
    if (n < 0) return {};
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(static_cast<size_t>(n));
    f.read(reinterpret_cast<char*>(buf.data()), buf.size());
    return buf;
}

// Iterate MTXM (via st.tiles_mega_tile_index) + tileset vf4 to produce
// per-walk-cell flags. Result is width*height bytes, row-major.
// Bit 7 (0x80) = unwalkable, matching openbw's `unwalkable_flags`.
std::vector<uint8_t> build_walk_grid(bwgame::state& st, bwgame::game_state& gst) {
    size_t tw = gst.map_tile_width;
    size_t th = gst.map_tile_height;
    size_t ww = tw * 4;
    size_t wh = th * 4;
    std::vector<uint8_t> out(ww * wh, 0);
    for (size_t ty = 0; ty < th; ++ty) {
        for (size_t tx = 0; tx < tw; ++tx) {
            size_t tile_idx = ty * tw + tx;
            uint16_t megatile_index = st.tiles_mega_tile_index[tile_idx] & 0x7fff;
            if (megatile_index >= gst.vf4.size()) continue;
            const auto& mt = gst.vf4[megatile_index];
            for (size_t sy = 0; sy < 4; ++sy) {
                for (size_t sx = 0; sx < 4; ++sx) {
                    size_t wx = tx * 4 + sx;
                    size_t wy = ty * 4 + sy;
                    uint16_t sf = mt.flags[sy * 4 + sx];
                    uint8_t b = 0;
                    if ((sf & bwgame::vf4_entry::flag_walkable) == 0) b |= 0x80;
                    out[wy * ww + wx] = b;
                }
            }
        }
    }
    return out;
}

void dump_map(const Args& args, const std::string& map_path,
              const std::string& out_json_path, MapInfo& info) {
    info.source_file = basename_of(map_path);

    // Fingerprint the source file so agents can trigger a rebuild
    // if the .scm is edited.
    auto src_bytes = read_file_bytes(map_path);
    info.source_sha256 = sha256_hex(src_bytes.data(), src_bytes.size());

    bwgame::game_player player{bwgame::a_string(args.data_path.c_str())};
    bwgame::state& st = player.st();
    bwgame::game_load_functions loader(st);
    // Melee-mode setup: makes create_starting_units spawn neutrals
    // (mineral fields, geysers) at their map-defined positions.
    for (size_t i = 0; i < 8; ++i) loader.setup_info.create_melee_units_for_player[i] = true;
    loader.load_map_file(bwgame::a_string(map_path.c_str()), {});

    auto& gst = *st.game;
    info.tile_w        = (int)gst.map_tile_width;
    info.tile_h        = (int)gst.map_tile_height;
    info.tileset_index = (int)gst.tileset_index;
    info.tileset_name  = tileset_name_of(info.tileset_index);

    // Static-only tile flags: strip runtime bits (occupied, has_creep,
    // creep_receding, temporary_creep) before serialization.
    // Everything else — walkable/unwalkable/partially_walkable, elevation
    // (high/middle/very_high), unbuildable, provides_cover — is static.
    const uint16_t dynamic_mask =
        bwgame::tile_t::flag_occupied
        | bwgame::tile_t::flag_has_creep
        | bwgame::tile_t::flag_creep_receding
        | bwgame::tile_t::flag_temporary_creep;

    size_t n_tiles = (size_t)info.tile_w * (size_t)info.tile_h;
    std::vector<uint8_t> tile_bytes(n_tiles * 2);
    for (size_t i = 0; i < n_tiles; ++i) {
        uint16_t f = (uint16_t)(st.tiles[i].flags & ~dynamic_mask);
        tile_bytes[2*i]     = (uint8_t)(f & 0xff);
        tile_bytes[2*i + 1] = (uint8_t)((f >> 8) & 0xff);
    }
    info.tile_flags_sha256 = sha256_hex(tile_bytes.data(), tile_bytes.size());

    std::vector<uint8_t> walk_bytes = build_walk_grid(st, gst);

    // Enumerate what the runtime actually placed on the map.
    struct Neutral { int type_id; std::string kind; int x; int y; int amount; };
    std::vector<Neutral> neutrals;
    for (bwgame::unit_t* u : bwgame::ptr(st.player_units[11])) {
        auto id_e = u->unit_type->id;
        int id = (int)id_e;
        std::string kind;
        bool is_mineral =
            id_e == bwgame::UnitTypes::Resource_Mineral_Field
            || id_e == bwgame::UnitTypes::Resource_Mineral_Field_Type_2
            || id_e == bwgame::UnitTypes::Resource_Mineral_Field_Type_3;
        bool is_geyser = id_e == bwgame::UnitTypes::Resource_Vespene_Geyser;
        if (is_mineral) { kind = "mineral"; info.mineral_count++; }
        else if (is_geyser) { kind = "geyser"; info.geyser_count++; }
        else continue;  // skip other neutral doodads (critters, etc.) for now
        Neutral n;
        n.type_id = id;
        n.kind = kind;
        n.x = u->sprite->position.x;
        n.y = u->sprite->position.y;
        n.amount = u->building.resource.resource_count;
        neutrals.push_back(std::move(n));
    }

    // Only slots with a real start location count.
    struct StartLoc { int slot; int x; int y; };
    std::vector<StartLoc> starts;
    for (int i = 0; i < 12; ++i) {
        auto p = gst.start_locations[i];
        if (p.x == 0 && p.y == 0) continue;
        starts.push_back({i, p.x, p.y});
    }
    info.start_count = (int)starts.size();

    // ------- Serialize -------

    J j;
    j.begin_obj();
      j.kv_str("source_file",   info.source_file);
      j.kv_str("source_sha256", info.source_sha256);
      j.kv_int("version",       info.version);  // TODO: capture VER chunk
      j.kv_int("tile_w",        info.tile_w);
      j.kv_int("tile_h",        info.tile_h);
      j.kv_int("tileset_index", info.tileset_index);
      j.kv_str("tileset_name",  info.tileset_name);

      j.key("start_locations"); j.begin_arr();
        for (auto& s : starts) {
          j.begin_obj();
            j.kv_int("slot", s.slot);
            j.kv_int("x", s.x);
            j.kv_int("y", s.y);
          j.end_obj();
        }
      j.end_arr();

      j.key("players"); j.begin_arr();
        for (int i = 0; i < 12; ++i) {
          j.begin_obj();
            j.kv_int("slot",       i);
            j.kv_int("controller", (int)st.players[i].controller);
            j.kv_int("race",       (int)st.players[i].race);
          j.end_obj();
        }
      j.end_arr();

      j.key("neutrals"); j.begin_arr();
        for (auto& n : neutrals) {
          j.begin_obj();
            j.kv_int("type_id", n.type_id);
            j.kv_str("kind",    n.kind);
            j.kv_int("x",       n.x);
            j.kv_int("y",       n.y);
            j.kv_int("amount",  n.amount);
          j.end_obj();
        }
      j.end_arr();

      j.key("tile_flags"); j.begin_obj();
        j.kv_str ("encoding", "hex-uint16-le-row-major");
        j.kv_int ("width",    info.tile_w);
        j.kv_int ("height",   info.tile_h);
        j.kv_str ("sha256",   info.tile_flags_sha256);
        j.kv_str ("data",     hex_encode(tile_bytes.data(), tile_bytes.size()));
      j.end_obj();

      j.key("walk_flags"); j.begin_obj();
        j.kv_str ("encoding", "hex-uint8-row-major");
        j.kv_int ("width",    info.tile_w * 4);
        j.kv_int ("height",   info.tile_h * 4);
        j.kv_str ("data",     hex_encode(walk_bytes.data(), walk_bytes.size()));
      j.end_obj();
    j.end_obj();

    std::string blob = j.ss.str() + "\n";
    std::ofstream f(out_json_path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open output file: " + out_json_path);
    f.write(blob.data(), blob.size());
    if (!f) throw std::runtime_error("write failed: " + out_json_path);
    info.bytes_written = blob.size();
}

// Try parsing one map. Returns true on success. Sets *out_slug + *out_info.
bool try_dump(const Args& args, const std::string& map_path,
              std::unordered_map<std::string, int>& slug_uses,
              std::string* out_slug, MapInfo* out_info) {
    std::string base = basename_of(map_path);
    std::string slug = slugify(base);
    auto& n = slug_uses[slug];
    if (n > 0) {
        if (args.strict) {
            std::fprintf(stderr, "error: slug collision on '%s' (from '%s')\n",
                         slug.c_str(), base.c_str());
            return false;
        }
        // Short (8-hex) hash of the full basename disambiguates.
        std::string h = sha256_hex(base.data(), base.size()).substr(0, 8);
        slug = slug + "_" + h;
    }
    n++;

    std::string out_path = args.out_dir + "/" + slug + ".json";
    if (args.skip_existing && file_exists(out_path)) {
        if (args.verbose) {
            std::fprintf(stderr, "[skip] %s (exists)\n", slug.c_str());
        }
        // Still need info if we want to rebuild the manifest; for now
        // report as skipped by returning false with a special path.
        return false;
    }

    MapInfo info;
    try {
        dump_map(args, map_path, out_path, info);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[fail] %s: %s\n", base.c_str(), e.what());
        return false;
    }

    if (out_slug) *out_slug = slug;
    if (out_info) *out_info = info;
    if (args.verbose) {
        std::fprintf(stderr,
            "[ok]   %-40s tiles=%dx%d ts=%d starts=%d min=%d gas=%d %zuKB\n",
            slug.c_str(), info.tile_w, info.tile_h, info.tileset_index,
            info.start_count, info.mineral_count, info.geyser_count,
            info.bytes_written / 1024);
    }
    return true;
}

void write_index(const Args& args,
                 const std::vector<std::pair<std::string, MapInfo>>& entries) {
    std::string path = args.out_dir + "/_index.json";
    J j;
    j.begin_obj();
      j.kv_int("version", 1);
      j.kv_int("map_count", (long long)entries.size());
      j.key("maps"); j.begin_arr();
        for (auto& [slug, info] : entries) {
          j.begin_obj();
            j.kv_str("slug",              slug);
            j.kv_str("source_file",       info.source_file);
            j.kv_str("source_sha256",     info.source_sha256);
            j.kv_int("tile_w",            info.tile_w);
            j.kv_int("tile_h",            info.tile_h);
            j.kv_int("tileset_index",     info.tileset_index);
            j.kv_str("tileset_name",      info.tileset_name);
            j.kv_int("start_count",       info.start_count);
            j.kv_int("mineral_count",     info.mineral_count);
            j.kv_int("geyser_count",      info.geyser_count);
            j.kv_str("tile_flags_sha256", info.tile_flags_sha256);
            j.kv_uint("size_bytes",       (unsigned long long)info.bytes_written);
          j.end_obj();
        }
      j.end_arr();
    j.end_obj();
    std::string blob = j.ss.str() + "\n";
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "warn: cannot write %s\n", path.c_str());
        return;
    }
    f.write(blob.data(), blob.size());
}

} // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, args)) return 2;

    if (!mkdir_p(args.out_dir)) {
        std::fprintf(stderr, "error: cannot create out-dir '%s'\n", args.out_dir.c_str());
        return 2;
    }

    std::vector<std::string> map_paths;
    if (!args.single_map.empty()) {
        map_paths.push_back(args.single_map);
    } else {
        for (auto& n : list_dir(args.maps_dir)) {
            if (!glob_match(args.pattern, n)) continue;
            map_paths.push_back(args.maps_dir + "/" + n);
        }
        if (map_paths.empty()) {
            std::fprintf(stderr, "error: no maps matched '%s' in %s\n",
                         args.pattern.c_str(), args.maps_dir.c_str());
            return 2;
        }
    }

    std::unordered_map<std::string, int> slug_uses;
    std::vector<std::pair<std::string, MapInfo>> entries;
    int ok = 0, failed = 0;

    for (auto& p : map_paths) {
        std::string slug;
        MapInfo info;
        if (try_dump(args, p, slug_uses, &slug, &info)) {
            entries.emplace_back(std::move(slug), std::move(info));
            ++ok;
        } else {
            ++failed;
        }
    }

    write_index(args, entries);

    std::fprintf(stderr, "\nstatic_map_parser: parsed=%d failed=%d out=%s\n",
                 ok, failed, args.out_dir.c_str());
    return failed == 0 ? 0 : 1;
}
