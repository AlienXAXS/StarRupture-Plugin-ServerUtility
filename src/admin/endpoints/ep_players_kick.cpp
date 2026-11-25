#include "ep_players_kick.h"
#include "../admin_json.h"
#include "../session/session_store.h"
#include "../admin_gamethread.h"
#include "../../host_rules/removal.h"

#if defined(MODLOADER_SERVER_BUILD) || defined(MODLOADER_CLIENT_BUILD)
#  include "SDK/Engine_classes.hpp"
#  include "plugin_map.h"
#  include "../../rcon/listed_players.h"
#  define KICK_HAS_SDK 1
#else
#  define KICK_HAS_SDK 0
#endif

#if KICK_HAS_SDK
static void KickPlayerController(SDK::APlayerController* pc)
{
	Removal::Kick(pc, std::string());
}

// playerIndex is a players/list index; expectedName and expectedId are what the list showed at it.
static const char* KickPlayer_SEH(int playerIndex, const wchar_t* expectedName, const char* expectedId)
{
	__try
	{
		auto* world = static_cast<SDK::UWorld*>(PluginSdkRuntime::GetWorld());
		if (!world) return R"({"ok":false,"error":"world unavailable"})";

		if (!ServerUtilityFields::PlayerActions.On()) return R"({"ok":false,"error":"player actions unavailable"})";

		SDK::AGameStateBase* gs = ListedPlayers::GameStateOf(world);
		if (!gs) return R"({"ok":false,"error":"game state unavailable"})";

		SDK::APlayerState* ps = ListedPlayers::Find(gs, playerIndex, expectedName, expectedId);
		if (!ps) return R"({"ok":false,"error":"player not found"})";

		SDK::APlayerController* pc = static_cast<SDK::APlayerController*>(ListedPlayers::OwnerOf(ps));
		if (!pc) return R"({"ok":false,"error":"player controller unavailable"})";

		KickPlayerController(pc);
		return R"({"ok":true})";
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return R"({"ok":false,"error":"exception during kick"})";
	}
}
#endif

namespace Ep_PlayersKick
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

		// The player is named by player_id, or by player_index plus player_name; see rcon/listed_players.h.
		const std::string listedId   = AdminJson::ExtractString(req->body, req->bodyLen, "player_id");
		const std::string listedName = AdminJson::ExtractString(req->body, req->bodyLen, "player_name");
		const int playerIndex = AdminJson::ExtractInt(req->body, req->bodyLen, "player_index");
		if (listedId.empty() && playerIndex < 0)
		{
			AdminJson::SetError(resp, 400, "player_id or player_index required", body);
			return;
		}

		const std::string result = AdminGT::Dispatch([playerIndex, listedId, listedName]() -> std::string
		{
#if KICK_HAS_SDK
			PlayerInfo listed;
			int position = 0;
			std::string refusal;
			if (!ListedPlayers::Resolve(playerIndex, listedId, listedName, listed, position, refusal))
				return "{\"ok\":false,\"error\":\"" + AdminJson::Escape(refusal) + "\"}";

			const std::wstring expectedName = ListedPlayers::ToWide(listed.name);
			return KickPlayer_SEH(position, expectedName.c_str(), listed.id.c_str());
#else
			return R"({"ok":false,"error":"SDK not available in this build"})";
#endif
		});

		body = result;
		resp->statusCode  = 200;
		resp->contentType = "application/json";
		resp->body        = body.c_str();
		resp->bodyLen     = body.size();
	}
}
