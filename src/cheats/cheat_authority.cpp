#include "cheat_authority.h"

#include "host_rules/host_rules.h"
#include "host_rules/ingame_admin.h"

namespace CheatAuthority
{
	RulesModel::Decision Check(void* playerController, const std::string& id, const char* featureId)
	{
		Env env;
		env.findPlayer = [](const std::string& who, PlayerList::Entry& out) { return PlayerList::Find(who, out); };
		env.isProven   = [&](const std::string& who) { return InGameAdmin::IsProven(playerController, who); };
		env.model      = [] { return HostRules::Get(); };
		return Decide(env, id, "BetterCheats", featureId);
	}
}
