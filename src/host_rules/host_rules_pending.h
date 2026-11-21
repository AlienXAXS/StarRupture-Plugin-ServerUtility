#pragma once

// Loader calls that do not exist yet. Each stub has the shape of the real
// call, so the real one drops in later.

namespace HostRulesPending
{
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
