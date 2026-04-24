#include "session_store.h"

#include <algorithm>
#include <cstdio>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define _CRT_RAND_S
#include <stdlib.h> // rand_s

AdminSessionStore& AdminSessionStore::Get()
{
	static AdminSessionStore s_instance;
	return s_instance;
}

std::string AdminSessionStore::GenerateToken()
{
	// 32 random bytes → 64 hex chars.  rand_s is MSVC Windows CRT; no external deps.
	unsigned char bytes[32];
	for (int i = 0; i < 32; ++i)
	{
		unsigned int v = 0;
		rand_s(&v);
		bytes[i] = static_cast<unsigned char>(v & 0xFF);
	}

	char hex[65];
	for (int i = 0; i < 32; ++i)
		snprintf(hex + i * 2, 3, "%02x", bytes[i]);
	hex[64] = '\0';

	return hex;
}

void AdminSessionStore::PurgeExpired_Locked()
{
	const auto now = std::chrono::steady_clock::now();
	m_sessions.erase(
		std::remove_if(m_sessions.begin(), m_sessions.end(),
			[&](const Entry& e) { return e.expiresAt <= now; }),
		m_sessions.end());
}

std::string AdminSessionStore::CreateSession(int expirySeconds)
{
	Entry entry;
	entry.token = GenerateToken();
	entry.expiresAt = std::chrono::steady_clock::now() + std::chrono::seconds(expirySeconds);

	std::lock_guard<std::mutex> lk(m_mutex);
	PurgeExpired_Locked();
	m_sessions.push_back(entry);

	return entry.token;
}

bool AdminSessionStore::ValidateSession(const std::string& token)
{
	if (token.empty()) return false;

	std::lock_guard<std::mutex> lk(m_mutex);
	PurgeExpired_Locked();

	const auto now = std::chrono::steady_clock::now();
	for (const auto& e : m_sessions)
	{
		if (e.token == token && e.expiresAt > now)
			return true;
	}
	return false;
}
