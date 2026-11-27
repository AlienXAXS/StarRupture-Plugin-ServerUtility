#include "op_build_game.h"
#include "op_build.h"
#include "cheat_ledger_game.h"
#include "host_rules/player_list.h"
#include "plugin_helpers.h"
#include "SDK/Engine_classes.hpp"
#include "SDK/Chimera_classes.hpp"
#include "plugin_map.h"
#include "server_utility_fields.h"

#include <Windows.h>
#include <cstdint>
#include <string>

namespace OpBuildGame
{
	namespace
	{
		constexpr const char* kSlot = "unlock";

		// ACrTechnologyKeeper::CheckAvailableBuildings(keeper, corporation, reputation), as BetterCheats
		// calls it with no corporation: with the unlock flag set it rebuilds the buildings on offer.
		using CheckFn = void(__fastcall*)(void* keeper, void* corporation, int64_t reputation);
		CheckFn g_check = nullptr;

		bool OnGameThread()
		{
			IPluginHooks* hooks = GetHooks();
			return hooks && hooks->Engine && hooks->Engine->IsGameThread();
		}

		// Null off the game thread: every engine and UObject access in this file starts here.
		SDK::UWorld* World()
		{
			if (!OnGameThread())
				return nullptr;
			return static_cast<SDK::UWorld*>(PluginSdkRuntime::GetWorld());
		}

		bool InWorld() { return World() != nullptr; }

		// Every row the unlock needs resolved from the loader's map: the keeper member, the flag and
		// the call. None of them is compiled in, so a build with no row for one reports the rule off.
		bool UnlockAvailable()
		{
			return ServerUtilityFields::CheatBuildUnlock.On() && g_check
				&& ServerUtilityFields::TechnologyKeeper.offset >= 0
				&& ServerUtilityFields::UnlockAllBuildings.offset >= 0;
		}

		// The server's technology keeper: found again from the world's game state on every call, never
		// kept. Only valid inside the caller's frame. Null when there is no world, the game state is
		// not the game's own class or it has no keeper. May fault: call it inside __try only.
		SDK::ACrTechnologyKeeper* Keeper()
		{
			auto* world = static_cast<SDK::UWorld*>(PluginSdkRuntime::GetWorld());
			if (!world || !world->GameState || !world->GameState->IsA(SDK::ACrGameStateBase::StaticClass()))
				return nullptr;
			SDK::ACrTechnologyKeeper** at = ServerUtilityFields::TechnologyKeeper.In(world->GameState);
			return at ? *at : nullptr;
		}

		// The unlock flag. False when there is no keeper or the flag row did not resolve, or on a
		// fault. No C++ objects needing unwinding in this frame: SEH is legal.
		bool ReadFlag_SEH(bool* out)
		{
			__try
			{
				SDK::ACrTechnologyKeeper* keeper = Keeper();
				if (!keeper) return false;
				const bool* flag = ServerUtilityFields::UnlockAllBuildings.In(keeper);
				if (!flag) return false;
				*out = *flag;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// Sets the flag to true. This is the only write; the flag is never written false here.
		bool WriteFlag_SEH()
		{
			__try
			{
				SDK::ACrTechnologyKeeper* keeper = Keeper();
				if (!keeper) return false;
				bool* flag = ServerUtilityFields::UnlockAllBuildings.In(keeper);
				if (!flag) return false;
				*flag = true;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// The game's own refresh of the buildings on offer, with the flag already set.
		bool CallCheck_SEH()
		{
			__try
			{
				SDK::ACrTechnologyKeeper* keeper = Keeper();
				if (!keeper || !g_check) return false;
				g_check(keeper, nullptr, 0);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		bool Unlocked()
		{
			bool on = false;
			return UnlockAvailable() && World() && ReadFlag_SEH(&on) && on;
		}

		// Sets the flag, reads it back (it has to read true) and has the game rebuild its available
		// buildings. A Grant in the ledger, one log line; never reverted.
		bool Unlock(const CheatDispatch::Context& ctx, uint32_t reqId)
		{
			if (!UnlockAvailable() || !World())
				return false;
			bool on = false;
			if (!WriteFlag_SEH() || !ReadFlag_SEH(&on) || !on)
				return false;
			if (!CallCheck_SEH())
				return false;

			const std::string id = ctx.playerId ? *ctx.playerId : std::string();
			PlayerList::Entry who;
			PlayerList::Find(id, who);
			CheatLedgerGame::Get().Grant(id, who.label, OpBuild::kFeatureUnlock, kSlot, "all buildings unlocked", reqId);
			LOG_INFO("[cheat.build] req %u: %s unlocked all buildings", reqId, id.c_str());
			return true;
		}
	}

	void Resolve(IPluginSelf* self, IPluginHookScanner* scanner)
	{
		g_check = reinterpret_cast<CheckFn>(PluginMap::Function(scanner, self, ServerUtilityFields::CheckAvailableBuildings,
			nullptr, PLUGIN_SCAN_FUNCTION_START, PLUGIN_SCAN_FLAG_OPTIONAL));
	}

	void Init()
	{
		OpBuild::Env env;
		env.available       = [](OpBuild::Part) { return false; };
		env.unlockAvailable = &UnlockAvailable;
		env.inWorld         = &InWorld;
		env.apply           = [](const CheatDispatch::Context&, uint32_t, const std::vector<OpBuild::Part>&, bool) { return false; };
		env.installHook     = [](OpBuild::Part) { return false; };
		env.removeHook      = [](OpBuild::Part) {};
		env.unlocked        = &Unlocked;
		env.unlock          = &Unlock;
		env.log             = [](const std::string& line) { LOG_INFO("[cheat.build] %s", line.c_str()); };
		OpBuild::SetEnv(std::move(env));
	}
}
