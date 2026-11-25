#pragma once

#include <string>
#include <vector>

// The server-wide stack size: the text form in HostRules.ini, the check for a
// typed value, and the decision of what the game side does on a tick.
//
// Pure C++17, no game or SDK types, so the unit tests and the admin dispatcher
// share it.
namespace StackSizeModel
{
	// The largest stack size the server accepts (the SDK stack limits helper's cap).
	constexpr int kMaxStack = 9999;

	// The [Server] section of HostRules.ini and its key.
	constexpr const char* kSection = "Server";
	constexpr const char* kKey     = "StackSize";

	// 0..9999 as plain digits. 0 means "the game's own sizes". Anything else is
	// refused and `error` says why.
	bool Validate(const std::string& text, int& out, std::string& error);

	// The stack size in the [Server] section of the file's lines. A missing
	// section or key gives 0. A value that is not 0..9999 gives 0 and an error
	// text (empty otherwise).
	int ParseLines(const std::vector<std::string>& lines, std::string& error);

	// The body of the [Server] section for this size (use with IniFile::ReplaceSection).
	std::vector<std::string> ComposeBody(int stackSize);

	enum class Action { None, Rescan, Apply, Restore };

	struct State
	{
		bool isServerBinary    = false;   // a dedicated server; a listen host or solo game is not
		bool featureAvailable  = false;   // the game build's version map has the stack fields
		int  target            = 0;       // the wanted size, 0 = game default
		int  lastApplied       = 0;       // the size last written to the items, 0 = nothing applied
		                                  // (pass 0 on a guard tick to have it written again)
		int  itemsTracked      = 0;       // items the item limits hold
		bool worldChanged      = false;   // the world is not the one the items were found in
		bool shuttingDown      = false;
	};

	// What the tick does next.
	Action Decide(const State& s);
}
