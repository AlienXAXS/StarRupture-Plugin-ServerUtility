#pragma once

// How the player list numbers players, in one place.
//
// The RCON/query player list (and the admin panel's players/list) shows only live
// players that have a name, in PlayerArray order, at most kMax of them. The panel's
// kick, ban and give take that list index back, so they must count players the
// same way, and must also confirm the player is still the one the list showed:
// the list is a cache refreshed every few seconds, and a player leaving shifts
// every later index. A request names its player by "player_id" (the ID `player ids`
// shows), or by "player_index" together with "player_name"; the name must belong to
// exactly one connected player, at that index. Anything else is refused.

#if defined(MODLOADER_SERVER_BUILD) || defined(MODLOADER_CLIENT_BUILD)

#include "SDK/Engine_classes.hpp"
#include "state/server_state.h"
#include "../host_rules/player_ids.h"
#include "../server_utility_fields.h"

#include <cstdint>
#include <cwchar>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace ListedPlayers
{
	// The list never shows more than this many players.
	constexpr int kMax = 128;

	// The game state of the world, or nullptr. Reads engine memory: call inside __try.
	inline SDK::AGameStateBase* GameStateOf(void* world)
	{
		SDK::AGameStateBase** at = ServerUtilityFields::WorldGameState.In(world);
		return at ? *at : nullptr;
	}

	// The owner of a player state, which is its controller while the player is
	// connected, or nullptr. Reads engine memory: call inside __try.
	inline SDK::AActor* OwnerOf(SDK::APlayerState* ps)
	{
		SDK::AActor** at = ServerUtilityFields::ActorOwner.In(ps);
		return at ? *at : nullptr;
	}

	// True when the live player appears in the list. Nothing is listed while
	// the loader has no row for the name.
	inline bool IsListed(SDK::APlayerState* ps)
	{
		const SDK::FString* name = ServerUtilityFields::PlayerStateName.In(ps);
		return name && name->Num() > 1;
	}

	// The live player at listIndex in list numbering, but only when its name still
	// equals expectedName (the name the list showed) and, when expectedId is not empty,
	// its ID equals expectedId. Returns nullptr when the player has left or the
	// numbering has shifted. Reads engine memory: call inside __try.
	inline SDK::APlayerState* Find(SDK::AGameStateBase* gs, int listIndex, const wchar_t* expectedName,
	                               const char* expectedId = "")
	{
		if (!gs || listIndex < 0 || listIndex >= kMax) return nullptr;

		auto* players = ServerUtilityFields::GameStatePlayerArray.In(gs);
		if (!players) return nullptr;

		int listed = 0;
		const int32_t count = players->Num();
		for (int32_t i = 0; i < count; ++i)
		{
			SDK::APlayerState* ps = (*players)[i];
			if (!IsListed(ps)) continue;

			if (listed++ != listIndex) continue;

			const wchar_t* name = ServerUtilityFields::PlayerStateName.In(ps)->CStr();
			if (!name || wcscmp(name, expectedName) != 0) return nullptr;
			if (expectedId && *expectedId && PlayerIds::Read(ps) != expectedId) return nullptr;
			return ps;
		}
		return nullptr;
	}

	// The cached list entry a panel request means, and its position in the list.
	// With playerId, the one connected player with that ID (listIndex is not used).
	// Without it, playerName is required and must belong to exactly one connected
	// player, the one at listIndex. On failure error says why.
	inline bool Resolve(int listIndex, const std::string& playerId, const std::string& playerName,
	                    PlayerInfo& out, int& position, std::string& error)
	{
		const std::vector<PlayerInfo> players = ServerState::Get().GetPlayers();

		int found = -1;
		if (!playerId.empty())
		{
			for (size_t i = 0; i < players.size(); ++i)
			{
				if (players[i].id != playerId) continue;
				if (found >= 0) { error = "more than one connected player has that ID"; return false; }
				found = static_cast<int>(i);
			}
			if (found < 0) { error = "player not found"; return false; }
		}
		else
		{
			if (playerName.empty()) { error = "player_id or player_name required"; return false; }

			int sameName = 0;
			for (const PlayerInfo& p : players) if (p.name == playerName) ++sameName;
			if (sameName > 1) { error = "more than one connected player has that name, send player_id"; return false; }

			if (listIndex < 0 || static_cast<size_t>(listIndex) >= players.size() ||
			    players[static_cast<size_t>(listIndex)].name != playerName)
			{ error = "player not found"; return false; }
			found = listIndex;
		}

		out = players[static_cast<size_t>(found)];
		position = found;
		return true;
	}

	inline std::wstring ToWide(const std::string& utf8)
	{
		if (utf8.empty()) return {};

		const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
		if (len <= 0) return {};

		std::wstring out(static_cast<size_t>(len), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), out.data(), len);
		return out;
	}
}

#endif
