#include "ep_rules_players.h"
#include "../admin_json.h"
#include "../session/session_store.h"
#include "../../host_rules/player_list.h"

#include <sstream>
#include <string>

namespace
{
	constexpr size_t kMaxFlagsLength = 1024;

	std::string EntryJson(const PlayerList::Entry& e)
	{
		std::ostringstream json;
		json << "{\"id\":\"" << AdminJson::Escape(e.id) << "\","
		     << "\"label\":\"" << AdminJson::Escape(e.label) << "\","
		     << "\"admin\":" << (e.admin ? "true" : "false") << ","
		     << "\"exempt_version\":" << (e.ExemptsVersion() ? "true" : "false") << ","
		     << "\"exempt_rules\":" << (e.ExemptsRules() ? "true" : "false") << ","
		     << "\"sections\":[";
		for (size_t i = 0; i < e.sections.size(); ++i)
			json << (i ? "," : "") << "\"" << AdminJson::Escape(e.sections[i]) << "\"";
		json << "]}";
		return json.str();
	}

	std::string ListJson()
	{
		const auto entries = PlayerList::All();
		std::string json = "\"entries\":[";
		for (size_t i = 0; i < entries.size(); ++i)
			json += (i ? "," : "") + EntryJson(entries[i]);
		return json + "]";
	}
}

namespace Ep_RulesPlayers
{
	void Handle(const PluginHttpRequest* req, PluginHttpResponse* resp)
	{
		std::string body;

		if (!AdminJson::RequirePost(req, resp, body)) return;

		const std::string token = AdminJson::ExtractString(req->body, req->bodyLen, "session_token");
		if (!AdminSessionStore::Get().ValidateSession(token))
		{
			AdminJson::SetError(resp, 401, "unauthorized", body);
			return;
		}

		const std::string action = AdminJson::ExtractString(req->body, req->bodyLen, "action");
		const std::string id     = AdminJson::ExtractString(req->body, req->bodyLen, "id");
		const std::string flags  = AdminJson::ExtractString(req->body, req->bodyLen, "flags");

		std::string error;
		if (action == "set" || action == "remove")
		{
			if (id.empty())
			{
				AdminJson::SetError(resp, 400, "id required", body);
				return;
			}
			if (flags.size() > kMaxFlagsLength)
			{
				AdminJson::SetError(resp, 400, "flags too long", body);
				return;
			}

			const bool ok = (action == "set") ? PlayerList::Set(id, flags, error)
			                                  : PlayerList::Remove(id, error);
			if (!ok)
			{
				AdminJson::SetError(resp, 400, error.c_str(), body);
				return;
			}
		}
		else if (action != "get")
		{
			AdminJson::SetError(resp, 400, "action must be get, set or remove", body);
			return;
		}

		AdminJson::SetOk(resp, body, ListJson());
	}
}
