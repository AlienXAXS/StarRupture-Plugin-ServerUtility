#pragma once

#include "plugin_interface.h"

// The game side of the cheat.wave op: reads the wave subsystem's state and calls its StartWave,
// PauseCurrentWave, ResumeCurrentWave and CancelCurrentWave by reflection (UFunction name through
// the loader's object walker, parameter blocks sized from the running game). A pause goes through
// the revert ledger as a server-wide Override, so it is put back on resume, cancel, deny and
// server stop. Server builds only. Game thread.
namespace OpWaveGame
{
	// From PluginInit, after CheatLedgerGame::Init: gives OpWave its game side and registers the
	// ledger accessor for the pause.
	void Init();
}
