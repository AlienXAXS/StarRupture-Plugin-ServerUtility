#pragma once

// Main entry point for the RCON / Steam Query subsystem.
//
// Call Init()     from OnEngineInit (after Winsock is available).
// Call Shutdown() from OnEngineShutdown (before UObject teardown).
//
// Player state is refreshed every 5 seconds from the loader's engine tick
// (game thread) using PluginSdkRuntime::GetWorld(), and copied into plain
// values in ServerState. The RCON and query threads only read that copy.
namespace Rcon
{
	// Reads -QueryPort= and -RconPassword=, starts TCP + UDP servers,
	// registers commands, and registers the player-refresh tick.
	void Init();

	// Stops all servers and the refresh tick; cleans up Winsock.
	void Shutdown();

	// Re-read the connected players into ServerState now. Game thread only
	// (it reads the world); the `players` command runs there, so its answer
	// is fresh instead of up to 5 seconds old.
	void RefreshPlayers();
}
