#include "op_grid_game.h"
#include "op_grid.h"

namespace OpGridGame
{
	void Init()
	{
		// The game calls are not written yet: every call reports off, so `cap` lists the op off.
		OpGrid::Env env;
		env.available = [] { return false; };
		env.inWorld   = [] { return false; };
		env.current   = [](const std::string&) { return std::optional<OpGrid::Size>(); };
		env.occupied  = [](const CheatDispatch::Context&) { return std::optional<int>(); };
		env.resize    = [](const CheatDispatch::Context&, uint32_t, int, int) { return false; };
		OpGrid::SetEnv(std::move(env));
	}
}
