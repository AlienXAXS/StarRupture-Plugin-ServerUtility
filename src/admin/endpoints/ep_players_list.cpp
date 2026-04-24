#include "ep_players_list.h"
#include "../admin_json.h"
#include "../session/session_store.h"
#include "../../rcon/state/server_state.h"

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
			json << "{"
				<< "\"index\":" << i << ","
				<< "\"name\":\"" << AdminJson::Escape(players[i].name) << "\","
				<< "\"ping\":" << players[i].pingMs << ","
				<< "\"ip\":\"" << AdminJson::Escape(players[i].ipAddress) << "\""
				<< "}";
		}
		json << "]";

		AdminJson::SetOk(resp, body, json.str());
	}
}
