#pragma once

// The game side of the cheat.plant op. Server builds only.
namespace OpPlantGame
{
	// From PluginInit, after CheatLedgerGame::Init: gives OpPlant its game side.
	void Init();
}
