#pragma once

#include <string>

struct IPluginSelf;
struct IPluginHookScanner;

// The server-wide stack size: every stackable item's stack raised to one size
// on a dedicated server, put back to the game's own sizes on 0, on shutdown and
// when the game build's version map lacks the fields. The size lives in the
// [Server] section of Plugins\config\HostRules.ini.
//
// The items are written on the game thread only, from a tick. Get, Available,
// Tracked and Raised are safe from any thread.
namespace StackSize
{
	// From OnPluginLoadHooks, after the other resolves. Declares the feature
	// and asks the loader for the two item members. A miss turns this feature
	// off (one log line) and never refuses the plugin.
	void Resolve(const IPluginSelf* self, IPluginHookScanner* scanner);

	// Read the saved size and start the tick. From PluginInit, server only.
	void Init();
	// Put the items back and stop the tick. From the engine shutdown, which runs
	// on the game thread.
	void Shutdown();

	// The wanted size, 0 = the game's own sizes.
	int Get();
	// True when this game build can raise stack sizes.
	bool Available();
	// Items found, and items currently above their own size (as of the last tick).
	int Tracked();
	int Raised();

	// Save the size and use it from the next tick. 0 = the game's own sizes.
	// False with an error text when it could not be saved.
	bool Set(int size, std::string& error);
}
