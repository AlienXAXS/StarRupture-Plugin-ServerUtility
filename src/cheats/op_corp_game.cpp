#include "op_corp_game.h"
#include "op_corp.h"
#include "cheat_ledger_game.h"
#include "host_rules/player_list.h"
#include "plugin_helpers.h"
#include "SDK/Engine_classes.hpp"
#include "SDK/Chimera_classes.hpp"
#include "plugin_map.h"
#include "server_utility_fields.h"

#include <Windows.h>
#include <cstdint>
#include <optional>
#include <string>

namespace OpCorpGame
{
	namespace
	{
		bool OnGameThread()
		{
			IPluginHooks* hooks = GetHooks();
			return hooks && hooks->Engine && hooks->Engine->IsGameThread();
		}

		// The data points of the world's corporations owner, or written when write is set. The
		// owner pointer lives only inside this call. No C++ objects with destructors in this frame:
		// SEH is legal. Returns false when there is no world or owner, or on a fault.
		bool Access_SEH(bool write, int32_t value, int32_t* out)
		{
			__try
			{
				auto* world = static_cast<SDK::UWorld*>(PluginSdkRuntime::GetWorld());
				if (!world || !world->GameState) return false;

				// On a server the game state is the game's own class; checked so the owner member is
				// never read at an offset another class does not have.
				if (!world->GameState->IsA(SDK::ACrGameStateBase::StaticClass())) return false;

				SDK::ACrCorporationsOwner** ownerAt = ServerUtilityFields::CorporationsOwner.In(world->GameState);
				SDK::ACrCorporationsOwner* owner = ownerAt ? *ownerAt : nullptr;
				if (!owner) return false;

				int32_t* points = ServerUtilityFields::CorporationsDataPoints.In(owner);
				if (!points) return false;
				if (write) *points = value;
				*out = *points;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		bool Available()
		{
			return ServerUtilityFields::CheatCorp.On();
		}

		std::optional<int> Current()
		{
			int32_t now = 0;
			if (!Available() || !OnGameThread() || !Access_SEH(false, 0, &now)) return std::nullopt;
			return now;
		}

		bool Set(const CheatDispatch::Context& ctx, uint32_t reqId, int points)
		{
			int32_t now = 0;
			if (!Available() || !OnGameThread() || !Access_SEH(true, points, &now)) return false;

			const std::string id = ctx.playerId ? *ctx.playerId : std::string();
			PlayerList::Entry who;
			PlayerList::Find(id, who);
			const std::string what = std::to_string(points) + " data points";
			CheatLedgerGame::Get().Grant(id, who.label, "corporations", "points", what, reqId);
			LOG_INFO("[cheat.corp] req %u: %s set %s", reqId, id.c_str(), what.c_str());
			return true;
		}
	}

	void Init()
	{
		OpCorp::Env env;
		env.available = &Available;
		env.current   = &Current;
		env.set       = &Set;
		env.log       = [](const std::string& line) { LOG_INFO("[cheat.corp] %s", line.c_str()); };
		OpCorp::SetEnv(std::move(env));
	}
}
