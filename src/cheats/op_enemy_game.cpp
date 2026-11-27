#include "op_enemy_game.h"
#include "op_enemy.h"
#include "plugin_helpers.h"

namespace OpEnemyGame
{
	namespace
	{
		// The Mass enemy calls are not written yet: the feature reports off, so `cap` lists the op
		// off and a request is refused with "Not available on this server's game build."
		bool Available() { return false; }

		bool InWorld() { return false; }

		bool AiDisabled() { return false; }

		bool OneHit() { return false; }

		int Tracked() { return 0; }

		bool SetAi(const CheatDispatch::Context&, uint32_t, bool) { return false; }

		bool SetOneHit(const CheatDispatch::Context&, uint32_t, bool) { return false; }
	}

	void Init()
	{
		OpEnemy::Env env;
		env.available  = &Available;
		env.inWorld    = &InWorld;
		env.aiDisabled = &AiDisabled;
		env.oneHit     = &OneHit;
		env.tracked    = &Tracked;
		env.setAi      = &SetAi;
		env.setOneHit  = &SetOneHit;
		env.log        = [](const std::string& line) { LOG_INFO("[cheat.enemy] %s", line.c_str()); };
		OpEnemy::SetEnv(std::move(env));
	}
}
