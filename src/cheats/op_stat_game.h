#pragma once

// The game side of the cheat.stat op. Server builds only. Game thread.
namespace OpStatGame
{
	// From PluginInit, after CheatLedgerGame::Init: gives OpStat its game side.
	void Init();
}
