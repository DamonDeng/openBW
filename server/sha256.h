// Tiny SHA-256 implementation, public domain / CC0.
// Small enough to bundle; avoids OpenSSL/CommonCrypto dependency.
//
// Ref: Wikipedia pseudocode for SHA-256. Constants and initial hash values
// from NIST FIPS 180-4.

#ifndef OPENBW_SHA256_H
#define OPENBW_SHA256_H

#include <array>
#include <cstdint>
#include <cstring>

namespace openbw_auth {

struct sha256 {
	using digest_t = std::array<uint8_t, 32>;

	static digest_t hash(const void* data, size_t len) {
		sha256 s;
		s.update(data, len);
		return s.finalize();
	}

	sha256() {
		state = {
			0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
			0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
		};
	}

	void update(const void* data_v, size_t len) {
		const uint8_t* data = (const uint8_t*)data_v;
		total_len += len;
		while (len > 0) {
			size_t take = std::min(len, size_t{64} - buf_len);
			std::memcpy(buffer + buf_len, data, take);
			buf_len += take;
			data += take;
			len -= take;
			if (buf_len == 64) {
				process_block(buffer);
				buf_len = 0;
			}
		}
	}

	digest_t finalize() {
		uint64_t bit_len = total_len * 8;
		buffer[buf_len++] = 0x80;
		if (buf_len > 56) {
			while (buf_len < 64) buffer[buf_len++] = 0;
			process_block(buffer);
			buf_len = 0;
		}
		while (buf_len < 56) buffer[buf_len++] = 0;
		for (int i = 7; i >= 0; --i) buffer[buf_len++] = (uint8_t)(bit_len >> (i * 8));
		process_block(buffer);
		digest_t out{};
		for (size_t i = 0; i < 8; ++i) {
			out[i * 4 + 0] = (uint8_t)(state[i] >> 24);
			out[i * 4 + 1] = (uint8_t)(state[i] >> 16);
			out[i * 4 + 2] = (uint8_t)(state[i] >> 8);
			out[i * 4 + 3] = (uint8_t)(state[i]);
		}
		return out;
	}

private:
	static constexpr uint32_t rotr(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }

	void process_block(const uint8_t* block) {
		static const uint32_t k[64] = {
			0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
			0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
			0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
			0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
			0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
			0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
			0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
			0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
		};
		uint32_t w[64];
		for (int i = 0; i < 16; ++i) {
			w[i] = ((uint32_t)block[i * 4 + 0] << 24) |
			       ((uint32_t)block[i * 4 + 1] << 16) |
			       ((uint32_t)block[i * 4 + 2] << 8) |
			       ((uint32_t)block[i * 4 + 3]);
		}
		for (int i = 16; i < 64; ++i) {
			uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
			uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
		uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
		for (int i = 0; i < 64; ++i) {
			uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
			uint32_t ch = (e & f) ^ (~e & g);
			uint32_t t1 = h + S1 + ch + k[i] + w[i];
			uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
			uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
			uint32_t t2 = S0 + mj;
			h = g; g = f; f = e;
			e = d + t1;
			d = c; c = b; b = a;
			a = t1 + t2;
		}
		state[0] += a; state[1] += b; state[2] += c; state[3] += d;
		state[4] += e; state[5] += f; state[6] += g; state[7] += h;
	}

	std::array<uint32_t, 8> state;
	uint64_t total_len = 0;
	uint8_t buffer[64] = {};
	size_t buf_len = 0;
};

} // namespace openbw_auth

#endif
