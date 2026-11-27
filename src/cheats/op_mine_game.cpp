#include "op_mine_game.h"
#include "op_mine.h"

namespace OpMineGame
{
	namespace
	{
		// Stub: the game side is not written yet, so the op reports off in `cap`.
		bool Off() { return false; }
		OpMine::State NoState(const std::string&) { return {}; }
	}

	void Init()
	{
		OpMine::Env env;
		env.available = &Off;
		env.inWorld   = &Off;
		env.current   = &NoState;
		env.apply     = [](const CheatDispatch::Context&, uint32_t, std::optional<bool>, std::optional<float>) { return false; };
		OpMine::SetEnv(std::move(env));
	}
}
