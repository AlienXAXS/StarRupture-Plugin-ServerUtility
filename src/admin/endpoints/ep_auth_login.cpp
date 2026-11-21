#include "ep_auth_login.h"
#include "../admin_json.h"
#include "../session/session_store.h"
#include "../../plugin_config.h"
#include "../../plugin_helpers.h"

#include <chrono>
#include <mutex>

namespace
{
	// After kMaxFailures bad keys inside kFailureWindow, every login is
	// refused until the window runs out.
	constexpr int                       kMaxFailures  = 5;
	constexpr std::chrono::seconds      kFailureWindow{ 60 };

	std::mutex                          g_failMutex;
	int                                 g_failures = 0;
	std::chrono::steady_clock::time_point g_windowStart;

	// Takes the same time for any input of the same length.
	bool KeyMatches(const std::string& given, const std::string& expected)
	{
		unsigned char diff = given.size() == expected.size() ? 0 : 1;
		for (size_t i = 0; i < expected.size(); ++i)
			diff |= static_cast<unsigned char>(i < given.size() ? given[i] : 0) ^ static_cast<unsigned char>(expected[i]);
		return diff == 0;
	}

	// Reserve one attempt, or refuse when the limit is reached. The attempt counts
	// as a failure until the key proves right, so requests that arrive together
	// cannot all pass before the first failure is counted. The lockout is shared by
	// every caller: the request carries no peer address to count by.
	bool BeginAttempt(int& attempts)
	{
		std::lock_guard<std::mutex> lk(g_failMutex);
		if (g_failures > 0 && std::chrono::steady_clock::now() - g_windowStart >= kFailureWindow)
			g_failures = 0;
		if (g_failures >= kMaxFailures)
			return false;
		if (g_failures == 0)
			g_windowStart = std::chrono::steady_clock::now();
		attempts = ++g_failures;
		return true;
	}

	void ClearFailures()
	{
		std::lock_guard<std::mutex> lk(g_failMutex);
		g_failures = 0;
	}
}

namespace Ep_AuthLogin
{
	void Handle(const PluginHttpRequest* req, PluginHttpResponse* resp)
	{
		std::string body;

		if (!AdminJson::RequirePost(req, resp, body)) return;

		int attempts = 0;
		if (!BeginAttempt(attempts))
		{
			LOG_WARN("[AdminPanel] Login refused: too many failed attempts");
			AdminJson::SetError(resp, 429, "too many failed attempts, try again later", body);
			return;
		}

		const std::string apiKey = AdminJson::ExtractString(req->body, req->bodyLen, "api_key");
		const std::string expected = ServerUtilityConfig::Config::GetAdminApiKey();

		if (apiKey.empty() || !KeyMatches(apiKey, expected))
		{
			LOG_WARN("[AdminPanel] Failed login (%d of %d before lockout)", attempts, kMaxFailures);
			AdminJson::SetError(resp, 401, "invalid api key", body);
			return;
		}

		ClearFailures();

		const int expiry = ServerUtilityConfig::Config::GetAdminSessionExpiry();
		const std::string token = AdminSessionStore::Get().CreateSession(expiry);

		AdminJson::SetOk(resp, body, "\"session_token\":\"" + token + "\"");
	}
}
