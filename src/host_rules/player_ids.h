#pragma once

#include <string>

namespace SDK { class APlayerState; class APlayerController; }

// A connected player's ID, read by the server from the game's player state.
// A client never sends it. Game thread only.
namespace PlayerIds
{
	// The string the game keys the player by (ACrPlayerStateBase::
	// GetUniqueNetIdAsString), or an empty string when it cannot be read
	// (not a Chimera player state, no ID yet, or an engine fault).
	std::string Read(SDK::APlayerState* ps);

	// The same for a player controller the loader or the game handed over (empty
	// when it has no player state yet or the read faults). Do not keep the pointer.
	std::string OfController(void* playerController);

	// True when the player's list entry exempts it from version checks.
	// A player whose ID cannot be read is not exempt.
	bool ExemptFromVersion(SDK::APlayerController* pc);
}
