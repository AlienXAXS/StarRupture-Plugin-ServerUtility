#pragma once

// What a controller's pawn is, as the cheats and the admin endpoints care about it. Plain data, so
// the rule is unit-tested apart from the game.
enum class PawnKind
{
	None,       // no pawn, or a spectator pawn (what a controller holds after its character died)
	Character,  // the player's own character
	Other       // a pawn that is not their character (a vehicle)
};

// A spectator pawn is no pawn: the player has no character to act on and is not in a vehicle.
constexpr PawnKind ClassifyPawn(bool hasPawn, bool isCharacter, bool isSpectator)
{
	if (!hasPawn)
		return PawnKind::None;
	if (isCharacter)
		return PawnKind::Character;
	return isSpectator ? PawnKind::None : PawnKind::Other;
}
