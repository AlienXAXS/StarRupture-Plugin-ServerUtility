#pragma once

// How the player list numbers players, in one place.
//
// The RCON/query player list (and the admin panel's players/list) shows only live
// players that have a name, in PlayerArray order, at most kMax of them. The panel's
// kick, ban and give take that list index back, so they must count players the
// same way, and must also confirm the player is still the one the list showed:
// the list is a cache refreshed every few seconds, and a player leaving shifts
// every later index.

#if defined(MODLOADER_SERVER_BUILD) || defined(MODLOADER_CLIENT_BUILD)

#include "SDK/Engine_classes.hpp"
#include "state/server_state.h"

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

	// True when the live player appears in the list.
	inline bool IsListed(SDK::APlayerState* ps)
	{
		return ps && ps->PlayerNamePrivate.Num() > 1;
	}

	// The live player at listIndex in list numbering, but only when its name still
	// equals expectedName (the name the list showed). Returns nullptr when the player
	// has left or the numbering has shifted. Reads engine memory: call inside __try.
	inline SDK::APlayerState* Find(SDK::AGameStateBase* gs, int listIndex, const wchar_t* expectedName)
	{
		if (!gs || listIndex < 0 || listIndex >= kMax) return nullptr;

		int listed = 0;
		const int32_t count = gs->PlayerArray.Num();
		for (int32_t i = 0; i < count; ++i)
		{
			SDK::APlayerState* ps = gs->PlayerArray[i];
			if (!IsListed(ps)) continue;

			if (listed++ != listIndex) continue;

			const wchar_t* name = ps->PlayerNamePrivate.CStr();
			return (name && wcscmp(name, expectedName) == 0) ? ps : nullptr;
		}
		return nullptr;
	}

	// The cached list entry at listIndex, or false when the list has no such entry.
	inline bool Cached(int listIndex, PlayerInfo& out)
	{
		const std::vector<PlayerInfo> players = ServerState::Get().GetPlayers();
		if (listIndex < 0 || static_cast<size_t>(listIndex) >= players.size()) return false;

		out = players[static_cast<size_t>(listIndex)];
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
