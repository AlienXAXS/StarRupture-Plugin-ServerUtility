#pragma once

// The game side of the cheat.build op. Server builds only. For now only the unlock rule has one:
// the free and anywhere parts report off. The unlock sets the technology keeper's unlock flag
// through the loader's version map and has the game rebuild its available buildings. Game thread.
#include "plugin_interface.h"

namespace OpBuildGame
{
	// From OnPluginLoadHooks, after ServerUtilityFields::Resolve: the CheckAvailableBuildings call, by map key.
	void Resolve(IPluginSelf* self, IPluginHookScanner* scanner);

	// From PluginInit, after CheatLedgerGame::Init: gives OpBuild its game side.
	void Init();
}
