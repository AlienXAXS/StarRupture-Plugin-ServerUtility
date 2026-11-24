#include "admin_wire/admin_crypto.h"

#include <windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

namespace AdminCrypto
{
bool RandomBytes(size_t n, Bytes& out)
{
	out.assign(n, 0);
	if (n == 0) return true;
	return BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
}

bool HmacSha256(const Bytes& key, const Bytes& data, Bytes& out)
{
	out.clear();
	BCRYPT_ALG_HANDLE alg = nullptr;
	BCRYPT_HASH_HANDLE hash = nullptr;
	bool ok = false;
	Bytes digest(32);
	if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) >= 0)
	{
		// BCrypt needs a non-null key pointer even for an empty key.
		uint8_t dummy = 0;
		PUCHAR k = key.empty() ? &dummy : const_cast<PUCHAR>(key.data());
		PUCHAR d = data.empty() ? &dummy : const_cast<PUCHAR>(data.data());
		if (BCryptCreateHash(alg, &hash, nullptr, 0, k, static_cast<ULONG>(key.size()), 0) >= 0
			&& BCryptHashData(hash, d, static_cast<ULONG>(data.size()), 0) >= 0
			&& BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0)
			ok = true;
	}
	if (hash) BCryptDestroyHash(hash);
	if (alg) BCryptCloseAlgorithmProvider(alg, 0);
	if (ok) out = digest;
	return ok;
}

bool ConstantTimeEqual(const Bytes& a, const Bytes& b)
{
	if (a.size() != b.size()) return false;
	uint8_t diff = 0;
	for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
	return diff == 0;
}

bool ConstantTimeEqual(const std::string& a, const std::string& b)
{
	return ConstantTimeEqual(ToBytes(a), ToBytes(b));
}

std::string ToHex(const Bytes& b)
{
	static const char* d = "0123456789abcdef";
	std::string s;
	s.reserve(b.size() * 2);
	for (uint8_t v : b) { s += d[v >> 4]; s += d[v & 15]; }
	return s;
}

static int Nibble(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

bool FromHex(const std::string& hex, Bytes& out)
{
	out.clear();
	if (hex.size() % 2) return false;
	for (size_t i = 0; i < hex.size(); i += 2)
	{
		int a = Nibble(hex[i]), b = Nibble(hex[i + 1]);
		if (a < 0 || b < 0) { out.clear(); return false; }
		out.push_back(static_cast<uint8_t>(a * 16 + b));
	}
	return true;
}

Bytes ToBytes(const std::string& s) { return Bytes(s.begin(), s.end()); }

bool MakeProof(const std::string& secretHex, const std::string& nonce, const std::string& serverId, std::string& proofHex)
{
	proofHex.clear();
	Bytes key, mac;
	if (!FromHex(secretHex, key) || key.empty()) return false;
	if (!HmacSha256(key, ToBytes(nonce + serverId), mac)) return false;
	proofHex = ToHex(mac);
	return true;
}
}
