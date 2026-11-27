#pragma once

// The game side of the cheat.grid op. Server builds only. Game thread.
namespace OpGridGame
{
	// From PluginInit, after CheatLedgerGame::Init: gives OpGrid its game side.
	void Init();
}
