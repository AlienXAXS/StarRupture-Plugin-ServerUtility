#include "op_stat_game.h"
#include "op_stat.h"

namespace OpStatGame
{
	namespace
	{
		// Stub: the game side is not written yet, so the op reports off in `cap`.
		bool Off() { return false; }
		std::vector<std::string> NoLocks(const std::string&) { return {}; }
	}

	void Init()
	{
		OpStat::Env env;
		env.available = &Off;
		env.inWorld   = &Off;
		env.locked    = &NoLocks;
		env.range     = [](const CheatDispatch::Context&, const std::string&) { return std::optional<OpStat::Range>(); };
		env.read      = [](const CheatDispatch::Context&, const std::string&) { return std::optional<float>(); };
		env.apply     = [](const CheatDispatch::Context&, uint32_t, const std::string&, std::optional<float>, std::optional<bool>) { return false; };
		OpStat::SetEnv(std::move(env));
	}
}
