#include "ep_players_list.h"
#include "../admin_json.h"
#include "../session/session_store.h"
#include "../../rcon/state/server_state.h"
#include "../../host_rules/player_list.h"

#include <string>
#include <sstream>

namespace Ep_PlayersList
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

		const auto players = ServerState::Get().GetPlayers();

		std::ostringstream json;
		json << "\"players\":[";
		for (size_t i = 0; i < players.size(); ++i)
		{
			if (i > 0) json << ",";

			PlayerList::Entry entry;
			if (!players[i].id.empty())
				PlayerList::Find(players[i].id, entry);
			json << "{"
				<< "\"index\":" << i << ","
				<< "\"name\":\"" << AdminJson::Escape(players[i].name) << "\","
				<< "\"ping\":" << players[i].pingMs << ","
				<< "\"ip\":\"" << AdminJson::Escape(players[i].ipAddress) << "\","
				<< "\"id\":\"" << AdminJson::Escape(players[i].id) << "\","
				<< "\"label\":\"" << AdminJson::Escape(entry.label) << "\","
				<< "\"admin\":" << (entry.admin ? "true" : "false") << ","
				<< "\"exempt_version\":" << (entry.exemptVersion ? "true" : "false") << ","
				<< "\"exempt_rules\":" << (entry.exemptRules ? "true" : "false") << ","
				<< "\"sections\":[";
			for (size_t s = 0; s < entry.sections.size(); ++s)
				json << (s ? "," : "") << "\"" << AdminJson::Escape(entry.sections[s]) << "\"";
			json << "]}";
		}
		json << "]";

		AdminJson::SetOk(resp, body, json.str());
	}
}
