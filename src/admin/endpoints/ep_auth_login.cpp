#include "ep_auth_login.h"
#include "../admin_json.h"
#include "../session/session_store.h"
#include "../../plugin_config.h"

namespace Ep_AuthLogin
{
	void Handle(const PluginHttpRequest* req, PluginHttpResponse* resp)
	{
		std::string body;

		if (!AdminJson::RequirePost(req, resp, body)) return;

		const std::string apiKey = AdminJson::ExtractString(req->body, req->bodyLen, "api_key");
		const std::string expected = ServerUtilityConfig::Config::GetAdminApiKey();

		if (apiKey.empty() || apiKey != expected)
		{
			AdminJson::SetError(resp, 401, "invalid api key", body);
			return;
		}

		const int expiry = ServerUtilityConfig::Config::GetAdminSessionExpiry();
		const std::string token = AdminSessionStore::Get().CreateSession(expiry);

		AdminJson::SetOk(resp, body, "\"session_token\":\"" + token + "\"");
	}
}
