#pragma once

#include "plugin_interface.h"

// The game side of the cheat.stack op: a tick that writes UAuItemDataBase::MaxStack on every item
// that stacks, through the SDK's ItemStackLimits helper, and puts the game's own sizes back. The
// size goes through the revert ledger as a server-wide Override, so it is put back on size=0, deny
// and server stop. A game build whose version map lacks the two item members turns only this op
// off. Server builds only. Game thread.
namespace OpStackGame
{
	// From OnPluginLoadHooks: declares the feature and asks the loader for the two item members.
	// A miss turns the op off (one log line) and never refuses the plugin.
	void Resolve(IPluginSelf* self, IPluginHookScanner* scanner);

	// From PluginInit, after CheatLedgerGame::Init: gives OpStack its game side, registers the
	// ledger accessor and starts the tick.
	void Init();

	// From the engine shutdown and PluginShutdown, after CheatLedgerGame::Shutdown: stops the tick
	// and puts every item back to the game's size (game thread only).
	void Shutdown();
}
