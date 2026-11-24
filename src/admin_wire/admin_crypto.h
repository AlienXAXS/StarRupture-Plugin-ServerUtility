#pragma once

// Small crypto helper for admin pairing, on Windows CNG (no extra dependency).
// Shared by the client and server halves. Link bcrypt.lib.

#include <cstdint>
#include <string>
#include <vector>

namespace AdminCrypto
{
	using Bytes = std::vector<uint8_t>;

	bool RandomBytes(size_t n, Bytes& out);                             // BCryptGenRandom
	bool HmacSha256(const Bytes& key, const Bytes& data, Bytes& out);   // 32 bytes
	bool ConstantTimeEqual(const Bytes& a, const Bytes& b);             // false when lengths differ
	bool ConstantTimeEqual(const std::string& a, const std::string& b);

	std::string ToHex(const Bytes& b);                                  // lower case
	bool FromHex(const std::string& hex, Bytes& out);                   // accepts either case, even length only

	Bytes ToBytes(const std::string& s);

	// Proof for a challenge: HMAC-SHA256(secret, nonce + serverId) as hex. The secret is hex;
	// nonce and serverId are used as their text (the strings sent in the challenge).
	bool MakeProof(const std::string& secretHex, const std::string& nonce, const std::string& serverId, std::string& proofHex);
}
