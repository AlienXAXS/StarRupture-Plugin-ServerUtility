#pragma once

// The game side of the cheat.power op. It finds the building's config asset by name, reads and
// writes the electricity trait value and the shared block of placed buildings, and rebuilds the
// template, through the game's Mass entity state (every offset and function from the loader's
// version map). Each building's edit is a server-wide Override in the revert ledger, feature
// OpPower::kFeature, slot OpPower::Slot(asset). Server builds only. Game thread.
#include "plugin_interface.h"

namespace OpPowerGame
{
	// From OnPluginLoadHooks, after ServerUtilityFields::Resolve: the four game functions, by map key.
	void Resolve(IPluginSelf* self, IPluginHookScanner* scanner);

	// From PluginInit, after CheatLedgerGame::Init: gives OpPower its game side.
	void Init();

	// From PluginShutdown: unregisters the tick.
	void Shutdown();
}
