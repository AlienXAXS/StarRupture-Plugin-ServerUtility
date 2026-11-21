#pragma once

#include "rules_model.h"

#include <cstdint>
#include <string>

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

	bool Reload(std::string& error);
	RulesModel::Model Get();

	bool Commit(const RulesModel::Model& next, std::string& error);

	// Apply one change to a copy, write the file, keep the copy, push. False
	// with an error when the change or the write failed (nothing changed).
	template <typename Fn>
	bool Change(std::string& error, Fn fn)
	{
		RulesModel::Model next = Get();
		if (!fn(next, error)) return false;
		return Commit(next, error);
	}

	// True when the running loader can take rules. Without it they are saved only.
	bool LoaderReceives();
	// Last push: "sent", "not sent: <why>" or "not sent yet".
	std::string PushStatus();

	// A player's ID is "reported" while its loader's feature list is known.
	bool WhoReported(const std::string& playerId, Reported& out);
}
