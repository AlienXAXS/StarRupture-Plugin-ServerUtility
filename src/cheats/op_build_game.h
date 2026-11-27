#pragma once

// The game side of the cheat.build op. Server builds only. The unlock rule sets the technology
// keeper's unlock flag through the loader's version map and has the game rebuild its available
// buildings. The free rule (cost, stability) hooks the game's placement checks through the loader,
// by their map rows, and forces their result for a player who has the part on. The anywhere parts
// (windows, benches) report off for now. Game thread.
#include "plugin_interface.h"

namespace OpBuildGame
{
	// From OnPluginLoadHooks, after ServerUtilityFields::Resolve: the CheckAvailableBuildings call, by map key.
	void Resolve(IPluginSelf* self, IPluginHookScanner* scanner);

	// From OnEngineShutdown and PluginShutdown, after CheatLedgerGame::Shutdown: removes every hook. Safe twice.
	void Shutdown();

	// From PluginInit, after CheatLedgerGame::Init: gives OpBuild its game side.
	void Init();
}
