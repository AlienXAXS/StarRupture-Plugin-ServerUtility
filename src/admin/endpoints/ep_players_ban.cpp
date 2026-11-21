#include "ep_players_ban.h"
#include "../admin_json.h"
#include "../session/session_store.h"
#include "../admin_gamethread.h"

#include <fstream>
#include <cstdio>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#if defined(MODLOADER_SERVER_BUILD) || defined(MODLOADER_CLIENT_BUILD)
#  include "SDK/Engine_classes.hpp"
#  include "plugin_map.h"
#  include "../../rcon/listed_players.h"
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
// playerIndex is a players/list index; expectedName is the name the list showed at it.
// Returns nullptr once that player is kicked, or an error JSON string.
static const char* BanPlayer_SEH(int playerIndex, const wchar_t* expectedName)
{
	__try
	{
		auto* world = static_cast<SDK::UWorld*>(PluginSdkRuntime::GetWorld());
		if (!world) return R"({"ok":false,"error":"world unavailable"})";

		SDK::AGameStateBase* gs = world->GameState;
		if (!gs) return R"({"ok":false,"error":"game state unavailable"})";

		SDK::APlayerState* ps = ListedPlayers::Find(gs, playerIndex, expectedName);
		if (!ps) return R"({"ok":false,"error":"player not found"})";

		SDK::APlayerController* pc = static_cast<SDK::APlayerController*>(ps->GetOwner());
		if (!pc) return R"({"ok":false,"error":"player controller unavailable"})";

		KickPlayerController(pc);
		return nullptr;
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

		// Optional: the name the list showed at player_index; see rcon/listed_players.h.
		const std::string listedName = AdminJson::ExtractString(req->body, req->bodyLen, "player_name");

		const std::string result = AdminGT::Dispatch([playerIndex, listedName]() -> std::string
		{
#if BAN_HAS_SDK
			PlayerInfo listed;
			if (!ListedPlayers::Cached(playerIndex, listedName, listed))
				return R"({"ok":false,"error":"player not found"})";

			const std::wstring expectedName = ListedPlayers::ToWide(listed.name);
			if (const char* error = BanPlayer_SEH(playerIndex, expectedName.c_str()))
				return error;

			// The list entry is the player just kicked, so its IP is the one to ban.
			if (!listed.ipAddress.empty())
				AppendBan(listed.ipAddress.c_str(), listed.name.c_str());
			return R"({"ok":true})";
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
