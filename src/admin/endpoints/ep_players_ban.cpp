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
#  include "../../host_rules/removal.h"
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

// One line per ban: the player ID, then "# name" and the IP when it is known.
// The file is a record for the host; nothing reads it back or refuses a rejoin.
static std::string OneLine(std::string s)
{
	for (char& c : s)
		if (static_cast<unsigned char>(c) < 0x20) c = ' ';
	return s;
}

static bool AppendBan(const PlayerInfo& player)
{
	if (player.id.empty() && player.ipAddress.empty()) return false;

	std::ofstream f(GetBanFilePath(), std::ios::app);
	if (!f.is_open()) return false;

	f << OneLine(player.id.empty() ? player.ipAddress : player.id) << " # " << OneLine(player.name);
	if (!player.id.empty() && !player.ipAddress.empty()) f << " " << OneLine(player.ipAddress);
	f << "\n";
	f.flush();
	return f.good();
}

#if BAN_HAS_SDK
// Kept out of the __try frame below: the empty reason is a std::string.
static void KickPlayerController(SDK::APlayerController* pc)
{
	Removal::Kick(pc, std::string());
}

// No C++ objects with destructors in this frame — SEH is legal.
// playerIndex is a players/list index; expectedName and expectedId are what the list showed at it.
// Returns nullptr once that player is kicked, or an error JSON string.
static const char* BanPlayer_SEH(int playerIndex, const wchar_t* expectedName, const char* expectedId)
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
#if BAN_HAS_SDK
			PlayerInfo listed;
			int position = 0;
			std::string refusal;
			if (!ListedPlayers::Resolve(playerIndex, listedId, listedName, listed, position, refusal))
				return "{\"ok\":false,\"error\":\"" + AdminJson::Escape(refusal) + "\"}";

			const std::wstring expectedName = ListedPlayers::ToWide(listed.name);
			if (const char* error = BanPlayer_SEH(position, expectedName.c_str(), listed.id.c_str()))
				return error;

			// The list entry is the player just kicked.
			if (!AppendBan(listed))
				return R"({"ok":false,"error":"kicked, but the ban could not be recorded"})";
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
