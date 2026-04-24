#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <chrono>

class AdminSessionStore
{
public:
	static AdminSessionStore& Get();

	// Creates a new session and returns a 64-char hex token.
	std::string CreateSession(int expirySeconds);

	// Returns true if the token is valid and not expired.
	// Lazily evicts expired tokens on each call.
	bool ValidateSession(const std::string& token);

private:
	struct Entry
	{
		std::string token;
		std::chrono::steady_clock::time_point expiresAt;
	};

	mutable std::mutex m_mutex;
	std::vector<Entry> m_sessions;

	static std::string GenerateToken();
	void PurgeExpired_Locked();
};
