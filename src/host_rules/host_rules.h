#pragma once

#include "rules_model.h"

#include <cstdint>
#include <string>

namespace SDK { class APlayerController; }

// The host's rules: the [Policy], [Rules] and [MinVersions] sections of
// Plugins\config\HostRules.ini, kept in memory and sent to the mod loader when
// it has host rules support. Thread safe; the changes and Push are for the game
// thread (RCON commands run there).
namespace HostRules
{
	// What a connected player's mod loader reported (the loader tells a plugin
	// that a player's feature list arrived, not what is in it).
	struct Reported
	{
		unsigned count  = 0;     // times the list arrived or changed
		uint64_t lastMs = 0;     // GetTickCount64 of the last time
	};

	// Read the file, start listening to the loader. From PluginInit.
	void Init();
	// Send the rules once the game thread runs. From the engine init callback.
	void PushWhenGameThreadRuns();
	void Shutdown();

	// Re-send each connected player's own rules after the rules or the player
	// list changed. Runs on the game thread (posted when called from another).
	void RefreshPlayers();

	// Game thread. The player's admin check was refused: end the join hold now and apply the
	// server rules. Nothing happens for a player with no hold.
	void EndAdminHold(void* playerController);

	// Game thread only. Fills out with the connected players' controllers (at most
	// max) and returns how many. Fresh each call: use them in this tick and do not keep them.
	int ConnectedControllers(SDK::APlayerController** out, int max);

	bool Reload(std::string& error);
	RulesModel::Model Get();

	bool Commit(const RulesModel::Model& next, std::string& error);

	// True when the running loader can take rules. Without it they are saved only.
	bool LoaderReceives();
	// Last push: "sent", "not sent: <why>" or "not sent yet".
	std::string PushStatus();

	// Last push of the version minimums: "sent", "not sent: <why>" (the loader's
	// own error text when it refused them) or "not sent yet".
	std::string MinimumsStatus();

	// What a player's mod loader listed, read from the loader on the spot.
	enum class FeaturesState
	{
		Ok,               // text is filled
		NoReport,         // connected, but no report (vanilla player or older loader)
		NotConnected,     // no connected player has this ID
		NoLoaderSupport,  // this mod loader (or build) cannot list a player's mods
	};
	// Game thread only. Text: "loader=<tag> interface=<n>", then one
	// "<Plugin>.<id>=on|off" line per feature.
	FeaturesState ClientFeatures(const std::string& playerId, std::string& text);

	// A player's ID is "reported" while its loader's feature list is known.
	bool WhoReported(const std::string& playerId, Reported& out);
}
