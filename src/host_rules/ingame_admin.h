#pragma once

#include "ingame_admin_model.h"

#include <string>

// The server half of the in-game admin tool, on the game: it takes the tool's
// plugin packets, reads each sender's player ID itself and hands both to
// InGameAdminModel. A connection is known by a number plus a weak reference to its
// player controller; no controller pointer is kept past a call. Game thread,
// except IssueCode and Unpair.
namespace InGameAdmin
{
	// Register the packet handler and the listeners. From PluginInit, after HostRules::Init.
	void Init();
	void Shutdown();

	// RCON `admin pair <id>` and `admin unpair <id>`.
	bool IssueCode(const std::string& playerId, std::string& code, std::string& error);
	bool Unpair(const std::string& playerId);

	// The player just completed the admin claim: their connection may pair without a code.
	void OnClaimed(const std::string& playerId);

	// A player leaves: forget their connection.
	void PlayerLeft(void* playerController);

	// [Policy] InGameAdmin (default on).
	bool Enabled();

	// For the snapshot and ops layer (SA3b).
	InGameAdminModel::Server* Model();
	bool SendToConnection(InGameAdminModel::ConnId conn, const std::string& message);
	void SendToProven(const std::string& message);
}
