#pragma once

#include "plugin_interface.h"

namespace Ep_RulesPlayers
{
	// POST api/rules/players: {session_token, action: get|set|remove, id, flags}.
	// Reads and edits the host's player list. Every action answers with the
	// whole list as it is afterwards.
	void Handle(const PluginHttpRequest* req, PluginHttpResponse* resp);
}
