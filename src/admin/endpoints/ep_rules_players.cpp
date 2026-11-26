#include "ep_rules_players.h"
#include "../admin_json.h"
#include "../admin_request.h"
#include "../../host_rules/admin_ops.h"
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
		     << "\"exempt_version\":" << (e.exemptVersion ? "true" : "false") << ","
		     << "\"exempt_rules\":" << (e.exemptRules ? "true" : "false") << ","
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

		if (!AdminRequest::PostWithSession(req, resp, body)) return;

		const std::string action = AdminJson::ExtractString(req->body, req->bodyLen, "action");
		const std::string id     = AdminJson::ExtractString(req->body, req->bodyLen, "id");
		const std::string flags  = AdminJson::ExtractString(req->body, req->bodyLen, "flags");

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

			const AdminOps::Op op = (action == "set") ? AdminOps::Op{ "player.set", { id, flags } }
			                                          : AdminOps::Op{ "player.remove", { id } };
			const AdminOps::Result r = AdminOps::Run(AdminOps::Caller::FromPanel(), op);
			if (!r.ok)
			{
				AdminJson::SetError(resp, 400, r.text.c_str(), body);
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
