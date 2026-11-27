#pragma once

// The game side of the cheat.enemy op. It finds the server's Mass enemy actors, zeroes their attack
// ranges and stops their agent (Disable AI), pins their health through the Mass entity state
// (One-Hit Kill), and puts Disable AI back, through the loader's version map. Each setting is a
// server-wide Override in the revert ledger, feature OpEnemy::kFeature, slots OpEnemy::kSlotAi and
// OpEnemy::kSlotOneHit. Server builds only. Game thread.
#include "plugin_interface.h"

namespace OpEnemyGame
{
	// From OnPluginLoadHooks, after ServerUtilityFields::Resolve: the three game functions, by map key,
	// and the Mass entity manager member of the entity subsystem.
	void Resolve(IPluginSelf* self, IPluginHookScanner* scanner);

	// From PluginInit, after CheatLedgerGame::Init: gives OpEnemy its game side.
	void Init();

	// From PluginShutdown: unregisters the tick and puts Disable AI back.
	void Shutdown();
}
