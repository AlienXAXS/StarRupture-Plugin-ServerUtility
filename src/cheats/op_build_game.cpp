#include "op_build_game.h"
#include "op_build.h"

namespace OpBuildGame
{
	namespace
	{
		// Stub: the game side is not written yet, so every part reports off and `cap` lists the op off.
		bool Off() { return false; }
	}

	void Init()
	{
		OpBuild::Env env;
		env.available       = [](OpBuild::Part) { return false; };
		env.unlockAvailable = &Off;
		env.inWorld         = &Off;
		env.apply           = [](const CheatDispatch::Context&, uint32_t, const std::vector<OpBuild::Part>&, bool) { return false; };
		env.installHook     = [](OpBuild::Part) { return false; };
		env.removeHook      = [](OpBuild::Part) {};
		env.unlocked        = &Off;
		env.unlock          = [](const CheatDispatch::Context&, uint32_t) { return false; };
		OpBuild::SetEnv(std::move(env));
	}
}
