#pragma once

// The game side of the cheat.build op. Server builds only.
namespace OpBuildGame
{
	// From PluginInit, after CheatLedgerGame::Init: gives OpBuild its game side.
	void Init();
}
