#pragma once

// Loader and player-list calls that do not exist yet. Each stub compiles
// against the current SDK and names the design section it waits on, so the
// real call drops in later. Design: drafts\server\host-rules-design.md.

#include "SDK/Engine_classes.hpp"

namespace HostRulesPending
{
	// What a player's [Players] entry says (design 11.2).
	struct PlayerFlags
	{
		bool admin         = false;
		bool exemptVersion = false;
	};

	// WAITS ON leg S1 (design 10.2 and 11.2): read the player's ID with
	// ACrPlayerStateBase::GetUniqueNetIdAsString and look it up in [Players].
	// Until then nobody has an entry, so nobody is exempt.
	inline PlayerFlags FlagsFor(SDK::APlayerController* /*pc*/)
	{
		return {};
	}

	// WAITS ON loader legs L3 and L5 (design 11.5 "Interface mechanism note",
	// 11.7 Open 1 option A): true when the running loader has interface v79,
	// the version that adds the authority calls. Until the SDK pin has v79
	// there is no way to ask, so the answer is no.
	inline bool LoaderHasHostRules()
	{
		return false;
	}

	// WAITS ON loader leg L3 (design 11.3): the loader's
	// OnClientBelowMinimum(playerController, plainReason) callback. The loader
	// must call it on the game thread, once per join (design 11.4 step 4).
	using BelowMinimumFn = void (*)(void* playerController, const char* plainReason);
	inline void RegisterBelowMinimum(BelowMinimumFn /*fn*/)
	{
	}
}
