// openbw_keygen: generate a random API key for a user and print a users.json
// entry snippet that can be added to the server's --users file.
//
//   ./openbw_keygen alice --slot 0
//   ./openbw_keygen bob   --slot 1
//   ./openbw_keygen carol --role observer
//   ./openbw_keygen admin --role admin
//
// Keys are 32 random bytes rendered as base64url (~43 chars). Base64url is
// URL-safe so we can pass keys as HTTP/WS query params without escaping.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace {

std::string base64url_encode(const uint8_t* data, size_t len) {
	static const char alphabet[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	std::string out;
	out.reserve((len * 4 + 2) / 3);
	for (size_t i = 0; i < len; i += 3) {
		uint32_t v = (uint32_t)data[i] << 16;
		if (i + 1 < len) v |= (uint32_t)data[i + 1] << 8;
		if (i + 2 < len) v |= (uint32_t)data[i + 2];
		out += alphabet[(v >> 18) & 0x3f];
		out += alphabet[(v >> 12) & 0x3f];
		if (i + 1 < len) out += alphabet[(v >> 6) & 0x3f];
		if (i + 2 < len) out += alphabet[v & 0x3f];
	}
	return out;
}

bool random_bytes(uint8_t* out, size_t n) {
	std::ifstream in("/dev/urandom", std::ios::binary);
	if (!in) return false;
	in.read((char*)out, (std::streamsize)n);
	return in.gcount() == (std::streamsize)n;
}

void usage(const char* argv0) {
	fprintf(stderr,
		"usage: %s <alias> [--slot N] [--role player|observer|admin]\n"
		"\n"
		"Prints a users.json entry with a fresh 32-byte random API key.\n"
		"Add the entry to the server's --users file. Save the key -- it is\n"
		"only printed once; the server stores the hash and forgets the key.\n"
		"\n"
		"Defaults:\n"
		"  --role observer   (unless --slot is given, which implies player)\n",
		argv0);
}

} // anonymous namespace

int main(int argc, char** argv) {
	if (argc < 2) {
		usage(argv[0]);
		return 1;
	}
	std::string alias;
	int slot = -1;
	std::string role;

	for (int i = 1; i < argc; ++i) {
		auto eq = [&](const char* s) { return std::strcmp(argv[i], s) == 0; };
		if (eq("--slot") && i + 1 < argc) slot = std::atoi(argv[++i]);
		else if (eq("--role") && i + 1 < argc) role = argv[++i];
		else if (eq("--help") || eq("-h")) { usage(argv[0]); return 0; }
		else if (argv[i][0] == '-') {
			fprintf(stderr, "unknown arg: %s\n", argv[i]);
			usage(argv[0]);
			return 1;
		} else if (alias.empty()) {
			alias = argv[i];
		} else {
			fprintf(stderr, "unexpected extra arg: %s\n", argv[i]);
			return 1;
		}
	}
	if (alias.empty()) {
		usage(argv[0]);
		return 1;
	}
	if (slot < -1 || slot > 7) {
		fprintf(stderr, "error: slot must be 0..7\n");
		return 1;
	}

	uint8_t raw[32];
	if (!random_bytes(raw, sizeof(raw))) {
		fprintf(stderr, "error: could not read /dev/urandom\n");
		return 1;
	}
	std::string key = "sk-" + base64url_encode(raw, sizeof(raw));

	// Print the users.json entry. Keep this shape compatible with auth.h's
	// loader.
	printf("{\n");
	printf("  \"alias\": \"%s\",\n", alias.c_str());
	printf("  \"api_key\": \"%s\"", key.c_str());
	if (slot >= 0) {
		printf(",\n  \"slot\": %d", slot);
	}
	if (!role.empty()) {
		printf(",\n  \"role\": \"%s\"", role.c_str());
	} else if (slot >= 0) {
		// Explicit role for clarity even though slot implies player.
		printf(",\n  \"role\": \"player\"");
	}
	printf("\n}\n");

	fprintf(stderr,
		"\n"
		"Save this API key -- it will not be shown again:\n"
		"  %s\n"
		"\n"
		"Add the JSON snippet above to the server's --users file's \"users\" array.\n",
		key.c_str());
	return 0;
}
