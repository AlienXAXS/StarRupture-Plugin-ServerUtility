#include "op_skill_game.h"
#include "op_skill.h"

namespace OpSkillGame
{
	void Init()
	{
		// Stub: the game side is not written yet, so the op reports off in `cap`.
		OpSkill::Env env;
		env.available  = [] { return false; };
		env.skillCount = [](const CheatDispatch::Context&) { return std::optional<int>(); };
		env.setSkill   = [](const CheatDispatch::Context&, uint32_t, int, int) { return false; };
		OpSkill::SetEnv(std::move(env));
	}
}
