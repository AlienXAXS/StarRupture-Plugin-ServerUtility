#pragma once

// The game side of the cheat.skill op. Server builds only.
namespace OpSkillGame
{
	// From PluginInit, after CheatLedgerGame::Init: gives OpSkill its game side.
	void Init();
}
