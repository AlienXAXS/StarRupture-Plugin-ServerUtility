#pragma once

#include "plugin_interface.h"

#include <string>

// The game side of the cheat.stack op: a tick that writes UAuItemDataBase::MaxStack on every item
// that stacks, through the SDK's ItemStackLimits helper, and puts the game's own sizes back. The
// size goes through the revert ledger as a server-wide Override, so it is put back on size=0, deny
// and server stop. A game build whose version map lacks the two item members turns only this op
// off. Server builds only. Game thread.
//
// The server's own setting is the [Server] StackSize value in HostRules.ini (0 = the game's own
// sizes). It is the baseline the item sizes sit at and the value cheat.stack's put-back returns to.
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

	// The size in effect (the server's setting, or a cheat.stack Override over it), 0 = the game's
	// own sizes. Any thread.
	int Get();
	// True when this game build can raise stack sizes. Any thread.
	bool Available();
	// Items found, and items currently above their own size, as of the last tick. Any thread.
	int Tracked();
	int Raised();

	// Change the server's setting (RCON, panel, admin window): saves the ini value, makes it the
	// baseline and applies it from the next tick. An active cheat.stack Override ends. It is not a
	// ledger entry. One cheats.log line names `source` with the old and new value. Game thread;
	// false with `error` when it could not be saved.
	bool SetSetting(int size, const std::string& source, std::string& error);
}
