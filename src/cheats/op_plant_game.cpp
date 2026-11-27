#include "op_plant_game.h"
#include "op_plant.h"

namespace OpPlantGame
{
	namespace
	{
		// Stub: the game side is not written yet, so the op reports off in `cap`.
		bool Off() { return false; }
	}

	void Init()
	{
		OpPlant::Env env;
		env.available   = &Off;
		env.inWorld     = &Off;
		env.apply       = [](const CheatDispatch::Context&, uint32_t, int) { return false; };
		env.installHook = &Off;
		env.removeHook  = [] {};
		OpPlant::SetEnv(std::move(env));
	}
}
