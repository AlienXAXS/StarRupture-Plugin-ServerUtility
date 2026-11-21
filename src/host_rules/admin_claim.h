#pragma once

#include <string>

namespace SDK { class APlayerController; }

// The first admin's claim. While the player list has no admin, ServerUtility
// keeps a one-time code in memory and shows it to the host in the server log and
// with RCON `admin code`. Each player whose mod loader can show the loader's admin
// prompt is asked once per server run; whoever types the code becomes an admin.
// Needs a mod loader with interface 79. Game thread, except CodeReport.
namespace AdminClaim
{
	// Hook the loader's answer callback and make the first code. From PluginInit.
	void Init();
	// Unhook it. From the engine shutdown.
	void Shutdown();

	// Ask this player for the code when the list has no admin and they have not
	// answered yet. The loader asks nobody without a mod loader. Game thread.
	void AskPlayer(void* playerController, const std::string& playerId);
	// Forget an open prompt (the player left).
	void PlayerLeft(const std::string& playerId);

	// What RCON `admin code` prints: the code while the list has no admin.
	std::string CodeReport();
}
