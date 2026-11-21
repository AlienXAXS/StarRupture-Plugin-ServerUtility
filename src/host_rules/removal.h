#pragma once

#include <string>

namespace SDK { class APlayerController; }

// Removing a player with a reason they can read.
namespace Removal
{
	// Send the player to the main menu with this text. Game thread only.
	// An empty reason shows no text, as before.
	void Kick(SDK::APlayerController* pc, const std::string& reason);

	// Hook the loader's below-minimum callback: a player below a version minimum
	// the host set is sent to the main menu with the loader's plain reason,
	// unless the player's list entry exempts them (ExemptVersion, Admin). With a
	// loader that does not have host rules, logs one line and leaves it off.
	void Init();
	// Unhook it. From the engine shutdown.
	void Shutdown();
}
