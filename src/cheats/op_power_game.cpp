#include "op_power_game.h"
#include "op_power.h"
#include "plugin_helpers.h"

#include <optional>
#include <string>
#include <vector>

namespace OpPowerGame
{
	namespace
	{
		// The Mass entity calls are not written yet: the feature reports off, so `cap` lists the op
		// off and a request is refused with "Not available on this server's game build."
		bool Available() { return false; }

		OpPower::Lookup Find(const std::string&, OpPower::Building&) { return OpPower::Lookup::NoWorld; }

		std::optional<std::vector<OpPower::Building>> List() { return std::nullopt; }

		int Edited() { return 0; }

		bool Set(const CheatDispatch::Context&, uint32_t, const std::string&, float) { return false; }

		bool Reset(const CheatDispatch::Context&, uint32_t, const std::string&) { return false; }

		bool ResetAll(const CheatDispatch::Context&, uint32_t) { return false; }
	}

	void Init()
	{
		OpPower::Env env;
		env.available = &Available;
		env.find      = &Find;
		env.list      = &List;
		env.edited    = &Edited;
		env.set       = &Set;
		env.reset     = &Reset;
		env.resetAll  = &ResetAll;
		env.log       = [](const std::string& line) { LOG_INFO("[cheat.power] %s", line.c_str()); };
		OpPower::SetEnv(std::move(env));
	}
}
