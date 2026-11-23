#pragma once

#include "admin_json.h"
#include "session/session_store.h"

// The check every admin panel endpoint starts with.
namespace AdminRequest
{
	// True when the request is a POST with a valid session_token. Otherwise
	// fills resp (405 or 401) with the error JSON in outBody and returns false.
	inline bool PostWithSession(const PluginHttpRequest* req, PluginHttpResponse* resp, std::string& outBody)
	{
		if (!AdminJson::RequirePost(req, resp, outBody)) return false;

		const std::string token = AdminJson::ExtractString(req->body, req->bodyLen, "session_token");
		if (!AdminSessionStore::Get().ValidateSession(token))
		{
			AdminJson::SetError(resp, 401, "unauthorized", outBody);
			return false;
		}
		return true;
	}
}
