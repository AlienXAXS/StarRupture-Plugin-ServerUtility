#pragma once

// The game side of su.cheat: takes the packets from client mods, reads each sender's player ID
// itself, runs CheatDispatch and sends the reply. Server builds only.
#include <string>

namespace CheatNet
{
	// From PluginInit, after InGameAdmin::Init. Registers nothing (and logs once) when the server
	// mod loader or the SDK this was built with lacks the addressed send (interface v83).
	void Init();
	void Shutdown();
	// The player left: drop their request rate bucket. Game thread.
	void Forget(const std::string& playerId);
}
