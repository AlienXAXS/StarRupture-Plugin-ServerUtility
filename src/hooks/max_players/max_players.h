#pragma once

#include <cstdint>

// Patches the hardcoded max-player check in ACrGameModeBase::PreLogin.
//
// The game compares the current player count against a hardcoded value of 4
// and rejects connections with "Max amount of players reached" when it is
// reached.  This module locates the `cmp ebx, 4` instruction inside
// PreLogin via pattern scanning and patches the immediate operand to the
// configured MaxPlayers value.
struct IPluginSelf;
struct IPluginHookScanner;

namespace MaxPlayersHook
{
	// Resolve ACrGameModeBase::PreLogin. Callable only from the plugin's
	// OnPluginLoadHooks export -- the loader refuses scans made anywhere else.
	void Resolve(IPluginSelf* self, IPluginHookScanner* scanner);

	// Called once the engine is ready.
	// maxPlayers: the desired maximum (1–127, clamped).  0 = disabled / don't patch.
	void Install(int maxPlayers);

	// Called on engine shutdown to restore the original byte.
	void Remove();
}
