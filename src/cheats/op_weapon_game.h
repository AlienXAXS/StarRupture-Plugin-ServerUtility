#pragma once

// The game side of the cheat.weapon op. It finds the caller's pawn, resolves its WeaponAttributes
// fresh each time, and composes the multiplier onto the current value of DamageModMultiplier and FireRateModMultiplier
// (the base value stays as the game has it), through the loader's version map. Each multiplier is a personal Toggle in the
// revert ledger, feature OpWeapon::kFeature, slots OpWeapon::kSlotDamage and OpWeapon::kSlotRate.
// Server builds only. Game thread.
namespace OpWeaponGame
{
	// From PluginInit, after CheatLedgerGame::Init: gives OpWeapon its game side.
	void Init();
}
