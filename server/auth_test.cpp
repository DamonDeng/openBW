// Tiny standalone test for the auth module. Verifies:
//  - SHA-256 vectors (empty string + "abc")
//  - JSON loading of a users file
//  - verify() of correct vs wrong keys
//  - role and slot inference rules
//  - duplicate-alias rejection
//
// Not a full unit-test framework -- just enough to catch regressions.

#include "auth.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, msg, #cond); \
		std::exit(1); \
	} \
} while (0)

using namespace openbw_auth;

static void test_sha256_vectors() {
	// NIST test vectors.
	// empty string
	auto d0 = sha256::hash("", 0);
	static const uint8_t exp0[] = {
		0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,
		0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,
		0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,
		0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55,
	};
	CHECK(std::memcmp(d0.data(), exp0, 32) == 0, "sha256('') mismatch");

	// "abc"
	auto d1 = sha256::hash("abc", 3);
	static const uint8_t exp1[] = {
		0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,
		0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
		0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,
		0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad,
	};
	CHECK(std::memcmp(d1.data(), exp1, 32) == 0, "sha256('abc') mismatch");

	// "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq" (56 bytes)
	const char* longer = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	auto d2 = sha256::hash(longer, std::strlen(longer));
	static const uint8_t exp2[] = {
		0x24,0x8d,0x6a,0x61,0xd2,0x06,0x38,0xb8,
		0xe5,0xc0,0x26,0x93,0x0c,0x3e,0x60,0x39,
		0xa3,0x3c,0xe4,0x59,0x64,0xff,0x21,0x67,
		0xf6,0xec,0xed,0xd4,0x19,0xdb,0x06,0xc1,
	};
	CHECK(std::memcmp(d2.data(), exp2, 32) == 0, "sha256(56-byte vec) mismatch");
	printf("  sha256 vectors: OK\n");
}

static void test_registry_basic() {
	user_registry reg;
	std::string json = R"({
		"users": [
			{"alias": "alice", "api_key": "sk-alice-key", "slot": 0},
			{"alias": "bob",   "api_key": "sk-bob-key",   "slot": 1},
			{"alias": "carol", "api_key": "sk-carol-key", "role": "observer"},
			{"alias": "admin", "api_key": "sk-admin-key", "role": "admin"}
		]
	})";
	size_t n = reg.load_string(json);
	CHECK(n == 4, "expected 4 users");
	CHECK(reg.size() == 4, "registry size mismatch");

	auto* u = reg.verify("sk-alice-key");
	CHECK(u != nullptr, "alice not found");
	CHECK(u->alias == "alice", "alice alias mismatch");
	CHECK(u->role == role_t::player, "alice role should be player (slot inference)");
	CHECK(u->assigned_slot == 0, "alice slot mismatch");
	CHECK(u->can_control_slot(0), "alice should control slot 0");
	CHECK(!u->can_control_slot(1), "alice should not control slot 1");

	u = reg.verify("sk-carol-key");
	CHECK(u != nullptr, "carol not found");
	CHECK(u->role == role_t::observer, "carol role mismatch");
	CHECK(u->assigned_slot == -1, "carol slot should default to -1");
	CHECK(!u->can_control_slot(0), "carol should not control any slot");

	u = reg.verify("sk-admin-key");
	CHECK(u != nullptr, "admin not found");
	CHECK(u->can_administer(), "admin should have admin role");

	CHECK(reg.verify("sk-wrong-key") == nullptr, "unknown key should fail");
	CHECK(reg.verify("") == nullptr, "empty key should fail");

	printf("  registry basic: OK\n");
}

static void test_dup_alias() {
	user_registry reg;
	std::string json = R"({
		"users": [
			{"alias": "dave", "api_key": "sk-1", "slot": 0},
			{"alias": "dave", "api_key": "sk-2", "slot": 1}
		]
	})";
	bool threw = false;
	try { reg.load_string(json); }
	catch (const std::exception& e) {
		threw = true;
		CHECK(std::string(e.what()).find("duplicate") != std::string::npos,
			"expected duplicate error");
	}
	CHECK(threw, "duplicate alias should throw");
	printf("  duplicate alias rejected: OK\n");
}

static void test_bad_slot() {
	user_registry reg;
	std::string json = R"({
		"users": [
			{"alias": "eve", "api_key": "sk-e", "slot": 99}
		]
	})";
	bool threw = false;
	try { reg.load_string(json); }
	catch (const std::exception&) { threw = true; }
	CHECK(threw, "slot=99 should throw");
	printf("  bad slot rejected: OK\n");
}

int main() {
	printf("running auth tests...\n");
	test_sha256_vectors();
	test_registry_basic();
	test_dup_alias();
	test_bad_slot();
	printf("all auth tests passed.\n");
	return 0;
}
