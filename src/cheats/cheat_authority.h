#pragma once

#include "host_rules/player_list.h"
#include "host_rules/rules_model.h"

#include <functional>
#include <string>

// Who may use a server-side cheat. The host's rules decide (RulesModel::Resolve); a player's
// admin flags count only once the player proved the pairing secret. The decision is pure (Decide,
// behind Env) so it can be tested alone; Check is the game-facing entry that fills Env from the
// live player list, the admin proof and the live rules.
namespace CheatAuthority
{
	struct Env
	{
		std::function<bool(const std::string& id, PlayerList::Entry& out)> findPlayer;
		std::function<bool(const std::string& id)>                         isProven;
		std::function<RulesModel::Model()>                                 model;
	};

	inline RulesModel::Decision Decide(const Env& env, const std::string& id, const char* plugin, const char* featureId)
	{
		PlayerList::Entry entry;
		if (!env.findPlayer(id, entry)) entry = PlayerList::Entry();
		const RulesModel::PlayerFlags flags = RulesModel::FlagsFor(entry.admin, entry.exemptRules, entry.sections, env.isProven(id));
		return RulesModel::Resolve(env.model(), flags, plugin, featureId);
	}

	// Game thread. id is the ID the server reads for this controller now (PlayerIds::OfController),
	// never one the client sent.
	RulesModel::Decision Check(void* playerController, const std::string& id, const char* featureId);
}
