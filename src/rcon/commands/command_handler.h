#pragma once

#include <string>

// ServerUtility's console commands, registered into the mod loader's own
// command registry (IPluginConsole) instead of a table of our own.
//
// That makes them ordinary console commands: listed by `help` under this
// plugin's name, typeable in the -console window, and reachable over RCON
// alongside the loader's commands, other plugins' commands and the engine's.
// RCON therefore no longer has a command set of its own -- it hands every
// line to the loader (IPluginConsole::ExecuteWithEngine) exactly as if it had
// been typed into the server console.
namespace PluginCommands
{
	// A command's body. args is everything after the command name, joined
	// with single spaces. The returned text is written back one line at a
	// time; a line starting "Error:" is written as an error line.
	using CommandFunc = std::string (*)(const std::string& args);

	// Add a command to the loader's registry. It always runs on the game
	// thread. aliases is space-separated and may be null.
	//
	// Names are global across the loader and every plugin, so this fails (and
	// logs) when the name or an alias is already taken.
	bool Register(const char* name, const char* aliases, const char* usage,
	              const char* help, CommandFunc fn);

	// Remove every command this plugin registered.
	void UnregisterAll();

	// Run a command line through the loader -- registered commands first, the
	// engine for anything else -- and block until its output is complete or
	// 30 seconds pass. An empty line runs `help`.
	//
	// The command runs on the game thread, so this must NOT be called from
	// the game thread: it would wait for a tick it is itself holding up.
	std::string Execute(const std::string& line);
}
