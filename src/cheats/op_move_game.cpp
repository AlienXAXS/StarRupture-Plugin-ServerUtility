#include "op_move_game.h"
#include "op_move.h"

namespace OpMoveGame
{
	namespace
	{
		// Stub: the game side is not written yet, so the op reports off in `cap`.
		bool Off() { return false; }
	}

	void Init()
	{
		OpMove::Env env;
		env.available    = &Off;
		env.inWorld      = &Off;
		env.hasCharacter = [](const CheatDispatch::Context&) { return false; };
		env.flying       = [](const std::string&) { return false; };
		env.speed        = [](const std::string&) { return 1.0f; };
		env.setFly       = [](const CheatDispatch::Context&, uint32_t, bool) { return false; };
		env.setSpeed     = [](const CheatDispatch::Context&, uint32_t, bool, float) { return false; };
		OpMove::SetEnv(std::move(env));
	}
}
