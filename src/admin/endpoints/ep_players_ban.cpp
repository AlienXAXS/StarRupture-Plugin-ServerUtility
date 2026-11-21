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
#  include "plugin_map.h"
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
// Kicks the player at playerIndex and copies that same player's name into
// nameOut, so the ban record is about the player who was removed.
// Returns nullptr on success, or an error JSON string.
static const char* BanPlayer_SEH(int playerIndex, wchar_t* nameOut, int nameCap)
{
	__try
	{
		auto* world = static_cast<SDK::UWorld*>(PluginSdkRuntime::GetWorld());
		if (!world) return R"({"ok":false,"error":"world unavailable"})";

		SDK::AGameStateBase* gs = world->GameState;
		if (!gs) return R"({"ok":false,"error":"game state unavailable"})";

		if (playerIndex >= gs->PlayerArray.Num())
			return R"({"ok":false,"error":"player not found"})";

		SDK::APlayerState* ps = gs->PlayerArray[playerIndex];
		if (!ps) return R"({"ok":false,"error":"player not found"})";

		SDK::APlayerController* pc = static_cast<SDK::APlayerController*>(ps->GetOwner());
		if (!pc) return R"({"ok":false,"error":"player controller unavailable"})";

		const wchar_t* name = ps->PlayerNamePrivate.CStr();
		if (name)
			wcsncpy_s(nameOut, nameCap, name, _TRUNCATE);

		KickPlayerController(pc);
		return nullptr;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return R"({"ok":false,"error":"exception during ban"})";
	}
}

static std::string ToUtf8(const wchar_t* wide)
{
	const int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
	if (len <= 1) return {};

	std::string out(static_cast<size_t>(len - 1), '\0');
	WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), len, nullptr, nullptr);
	return out;
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

		const std::string result = AdminGT::Dispatch([playerIndex]() -> std::string
		{
#if BAN_HAS_SDK
			wchar_t name[64] = {};
			if (const char* error = BanPlayer_SEH(playerIndex, name, static_cast<int>(std::size(name))))
				return error;

			// The IP comes from the cached list, matched by the name of the player just kicked.
			const std::string playerName = ToUtf8(name);
			for (const auto& player : ServerState::Get().GetPlayers())
			{
				if (player.name == playerName && !player.ipAddress.empty())
				{
					AppendBan(player.ipAddress.c_str(), playerName.c_str());
					break;
				}
			}
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
