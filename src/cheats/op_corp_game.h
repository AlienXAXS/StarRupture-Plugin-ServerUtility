#pragma once

// The game side of the cheat.corp op: reads and writes the corporations owner's DataPoints through
// the game state (both members come from the loader's version map). The write is a grant: it stays,
// and CheatLedgerGame logs it. Server builds only. Game thread.
namespace OpCorpGame
{
	// From PluginInit, after CheatLedgerGame::Init: gives OpCorp its game side.
	void Init();
}
