#pragma once

// Loader and player-list calls that do not exist yet. Each stub compiles
// against the current SDK, so the real call drops in later.

#include "SDK/Engine_classes.hpp"

namespace HostRulesPending
{
	// What a player's entry in the host's player list says.
	struct PlayerFlags
	{
		bool admin         = false;
		bool exemptVersion = false;
	};

	// Waits on the player list: read the player's ID with
	// ACrPlayerStateBase::GetUniqueNetIdAsString and look it up. Until then
	// nobody has an entry, so nobody is exempt. Safe while the callback below
	// is not registered, because nothing is removed then.
	inline PlayerFlags FlagsFor(SDK::APlayerController* /*pc*/)
	{
		return {};
	}

	// Waits on a mod loader that has the host rules calls (a newer plugin
	// interface than the pinned SDK has): true when the running loader has
	// them. Until the SDK pin has them there is no way to ask, so the answer
	// is no.
	inline bool LoaderHasHostRules()
	{
		return false;
	}

	// Waits on the loader's OnClientBelowMinimum(playerController, plainReason)
	// callback. The loader must call it on the game thread, once per join.
	using BelowMinimumFn = void (*)(void* playerController, const char* plainReason);
	inline void RegisterBelowMinimum(BelowMinimumFn /*fn*/)
	{
	}
}
