#pragma once

#include <string>

namespace SDK { class APlayerController; }

// Removing a player with a reason they can read (design 11.4).
namespace Removal
{
	// Send the player to the main menu with this text. Game thread only.
	// An empty reason shows no text, as before.
	void Kick(SDK::APlayerController* pc, const std::string& reason);

	// Hook the loader's below-minimum callback. With a loader that does not
	// have host rules, logs one line and leaves the feature off.
	void Init();
}
