#include "op_skill_game.h"
#include "op_skill.h"
#include "cheat_ledger_game.h"
#include "host_rules/player_list.h"
#include "plugin_helpers.h"
#include "game_access.h"
#include "server_utility_fields.h"
#include "plugin_map.h"

#include <Windows.h>
#include <cstdint>
#include <optional>
#include <string>

namespace OpSkillGame
{
	namespace
	{
		using GameAccess::OnGameThread;
		using GameAccess::World;

		bool Available()
		{
			return ServerUtilityFields::CheatSkill.On();
		}

		// How many skills the requesting player's controller holds. The controller comes with the
		// request and is used inside the call that gets it, never kept. False when it is not a player
		// controller or the array or the struct size did not resolve. No C++ objects with destructors in
		// this frame: SEH is legal.
		bool CountSkills_SEH(void* controller, int* count)
		{
			__try
			{
				auto* pc = static_cast<SDK::UObject*>(controller);
				if (!pc || !pc->IsA(SDK::ACrPlayerControllerBase::StaticClass()))
					return false;
				const auto* skills = ServerUtilityFields::ControllerPlayerSkills.In(pc);
				if (!skills || ServerUtilityFields::SkillDataSize.resolvedSize == 0)
					return false;
				*count = skills->Num();
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// Sets one skill's level and zeroes its experience. The index is bounded by the live array count
		// and the array is stepped by the struct size the game reflects. Reads the skill id for the audit
		// line (-1 when it did not read). False when the controller, the array or the index is not
		// usable, or on a fault. No C++ objects with destructors in this frame: SEH is legal.
		bool WriteSkill_SEH(void* controller, int index, int level, int* skillId)
		{
			__try
			{
				auto* pc = static_cast<SDK::UObject*>(controller);
				if (!pc || !pc->IsA(SDK::ACrPlayerControllerBase::StaticClass()))
					return false;
				auto* skills = ServerUtilityFields::ControllerPlayerSkills.In(pc);
				if (!skills)
					return false;
				uint8_t* element = PluginMap::ElementAt(*skills, index, ServerUtilityFields::SkillDataSize);
				if (!element)
					return false;
				auto* levelAt = ServerUtilityFields::SkillDataLevel.In(element);
				auto* expAt   = ServerUtilityFields::SkillDataExperience.In(element);
				if (!levelAt || !expAt)
					return false;
				*levelAt = level;
				*expAt   = 0.0f;
				const auto* skillAt = ServerUtilityFields::SkillDataSkill.In(static_cast<const void*>(element));
				*skillId = skillAt ? static_cast<int>(*skillAt) : -1;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		std::optional<int> SkillCount(const CheatDispatch::Context& ctx)
		{
			if (!World())
				return std::nullopt;
			int count = 0;
			if (!CountSkills_SEH(ctx.controller, &count))
				return std::nullopt;
			return count;
		}

		// A grant, no ledger entry: the level stays. Logged like the other grants.
		bool SetSkill(const CheatDispatch::Context& ctx, uint32_t reqId, int index, int level)
		{
			if (!OnGameThread() || !Available())
				return false;
			int skillId = -1;
			if (!WriteSkill_SEH(ctx.controller, index, level, &skillId))
				return false;

			const std::string id = ctx.playerId ? *ctx.playerId : std::string();
			PlayerList::Entry who;
			PlayerList::Find(id, who);
			const std::string what = "skill " + std::to_string(index) + " (id " + std::to_string(skillId) + ") to level "
				+ std::to_string(level);
			CheatLedgerGame::Get().Grant(id, who.label, "skills", "level", what, reqId);
			return true;
		}
	}

	void Init()
	{
		OpSkill::Env env;
		env.available  = &Available;
		env.skillCount = &SkillCount;
		env.setSkill   = &SetSkill;
		env.log        = [](const std::string& line) { LOG_INFO("[cheat.skill] %s", line.c_str()); };
		OpSkill::SetEnv(std::move(env));
	}
}
