#include "ep_players_kick.h"
#include "../admin_json.h"
#include "../session/session_store.h"
#include "../admin_gamethread.h"

#if defined(MODLOADER_SERVER_BUILD) || defined(MODLOADER_CLIENT_BUILD)
#  include "SDK/Engine_classes.hpp"
#  define KICK_HAS_SDK 1
#else
#  define KICK_HAS_SDK 0
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

		const int playerIndex = AdminJson::ExtractInt(req->body, req->bodyLen, "player_index");
		if (playerIndex < 0)
		{
			AdminJson::SetError(resp, 400, "player_index required", body);
			return;
		}

		const std::string result = AdminGT::Dispatch([playerIndex]() -> std::string
		{
#if KICK_HAS_SDK
			__try
			{
				auto* world = static_cast<SDK::UWorld*>(SDK::UWorld::GetWorld());
				if (!world) return R"({"ok":false,"error":"world unavailable"})";

				SDK::AGameStateBase* gs = world->GameState;
				if (!gs) return R"({"ok":false,"error":"game state unavailable"})";

				if (playerIndex >= gs->PlayerArray.Num())
					return R"({"ok":false,"error":"player not found"})";

				SDK::APlayerState* ps = gs->PlayerArray[playerIndex];
				if (!ps) return R"({"ok":false,"error":"player not found"})";

				SDK::APlayerController* pc = ps->GetOwningPlayerController();
				if (!pc) return R"({"ok":false,"error":"player controller unavailable"})";

				// FText with empty string — kick reason shown on client
				SDK::FText reason{};
				pc->ClientReturnToMainMenuWithTextReason(reason);
				return R"({"ok":true})";
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return R"({"ok":false,"error":"exception during kick"})";
			}
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
