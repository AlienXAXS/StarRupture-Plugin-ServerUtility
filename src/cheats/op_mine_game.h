#pragma once

// The game side of the cheat.mine op. Server builds only. Game thread.
namespace OpMineGame
{
	// From PluginInit, after CheatLedgerGame::Init: gives OpMine its game side.
	void Init();
}
