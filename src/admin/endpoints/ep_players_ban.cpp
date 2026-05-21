#include "ep_players_ban.h"
#include "../admin_json.h"
#include "../session/session_store.h"
#include "../admin_gamethread.h"
#include "../../rcon/state/server_state.h"

#include <fstream>
#include <cstdio>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#if defined(MODLOADER_SERVER_BUILD) || defined(MODLOADER_CLIENT_BUILD)
#  include "SDK/Engine_classes.hpp"
#  define BAN_HAS_SDK 1
#else
#  define BAN_HAS_SDK 0
#endif

static std::string GetBanFilePath()
{
	char exePath[MAX_PATH] = {};
	GetModuleFileNameA(nullptr, exePath, MAX_PATH);

	// Strip the filename to get the exe directory
	char* last = strrchr(exePath, '\\');
	if (last) *(last + 1) = '\0';

	return std::string(exePath) + "Plugins\\ServerUtility\\bans.txt";
}

static void AppendBan(const char* ip, const char* playerName)
{
	const std::string path = GetBanFilePath();
	std::ofstream f(path, std::ios::app);
	if (f.is_open())
		f << ip << " # " << (playerName ? playerName : "") << "\n";
}

#if BAN_HAS_SDK
static void KickPlayerController(SDK::APlayerController* pc)
{
	SDK::FText reason;
	pc->ClientReturnToMainMenuWithTextReason(reason);
}

// No C++ objects with destructors in this frame — SEH is legal.
// ip and playerName are passed as const char* to avoid std::string dtors in scope.
static const char* BanPlayer_SEH(int playerIndex, const char* ip, const char* playerName)
{
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

		SDK::APlayerController* pc = static_cast<SDK::APlayerController*>(ps->GetOwner());
		if (!pc) return R"({"ok":false,"error":"player controller unavailable"})";

		KickPlayerController(pc);

		if (ip && ip[0] != '\0')
			AppendBan(ip, playerName);

		return R"({"ok":true})";
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return R"({"ok":false,"error":"exception during ban"})";
	}
}
#endif

namespace Ep_PlayersBan
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

		// Grab player info from cached state (thread-safe, no game thread needed)
		std::string playerIp;
		std::string playerName;
		{
			const auto players = ServerState::Get().GetPlayers();
			if (playerIndex < static_cast<int>(players.size()))
			{
				playerIp   = players[playerIndex].ipAddress;
				playerName = players[playerIndex].name;
			}
		}

		const std::string result = AdminGT::Dispatch([playerIndex, playerIp, playerName]() -> std::string
		{
#if BAN_HAS_SDK
			return BanPlayer_SEH(playerIndex, playerIp.c_str(), playerName.c_str());
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
