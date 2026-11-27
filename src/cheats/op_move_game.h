#pragma once

// The game side of the cheat.move op. Server builds only.
namespace OpMoveGame
{
	// From PluginInit, after CheatLedgerGame::Init: gives OpMove its game side.
	void Init();
}
