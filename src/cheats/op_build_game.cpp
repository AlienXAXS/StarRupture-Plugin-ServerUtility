#include "op_build_game.h"
#include "op_build.h"
#include "cheat_ledger_game.h"
#include "host_rules/ingame_admin.h"
#include "host_rules/player_ids.h"
#include "host_rules/player_list.h"
#include "plugin_helpers.h"
#include "SDK/Engine_classes.hpp"
#include "SDK/Chimera_classes.hpp"
#include "plugin_map.h"
#include "server_utility_fields.h"

#include <Windows.h>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

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

		// ---- The free-building hooks. Each part is a post-hook on the game's own placement check: the
		// original always runs first (so the HUD and every other plugin's hook on the same function
		// still see the real result) and the result is then forced for a player who has the part on.

		// UCrBuildingComponent::GetPlacementResourceConditionResult(this): the enum result of the
		// resource check. 1 is Valid, as BetterCheats forces it.
		using CostFn = int64_t(__fastcall*)(void* self);
		// ACrAPHelperActorCustom / ACrAPHelperDynamicPillar::CheckStability(this, placementData).
		using StabilityFn = bool(__fastcall*)(void* self, const void* placementData);

		constexpr int64_t kPlacementValid = 1;

		uintptr_t g_addrCost = 0, g_addrCustom = 0, g_addrPillar = 0;
		CostFn      g_origCost   = nullptr;
		StabilityFn g_origCustom = nullptr, g_origPillar = nullptr;
		HookHandle  g_hookCost = nullptr, g_hookCustom = nullptr, g_hookPillar = nullptr;

		bool HookApiPresent()
		{
			IPluginHooks* hooks = GetHooks();
			return hooks && hooks->Hooks && hooks->Hooks->Install && hooks->Hooks->Remove;
		}

		// The player controller behind a hooked object: a placement component's outer is its actor, an
		// actor's owner is the next link, and a pawn's controller is the player's. Follows at most
		// eight links. The pointer is used inside the hook's frame only. May fault: SEH here, and no
		// objects that need unwinding in this frame.
		void* ControllerOf_SEH(void* object)
		{
			__try
			{
				SDK::UObject* cur = static_cast<SDK::UObject*>(object);
				for (int hop = 0; cur && hop < 8; ++hop)
				{
					if (cur->IsA(SDK::APlayerController::StaticClass()))
						return cur;
					if (cur->IsA(SDK::APawn::StaticClass()))
					{
						SDK::AController** at = ServerUtilityFields::PawnController.In(cur);
						SDK::AController* controller = at ? *at : nullptr;
						return controller && controller->IsA(SDK::APlayerController::StaticClass()) ? controller : nullptr;
					}
					SDK::UObject* next = nullptr;
					if (cur->IsA(SDK::AActor::StaticClass()))
					{
						SDK::AActor** owner = ServerUtilityFields::ActorOwner.In(cur);
						next = owner ? *owner : nullptr;
					}
					if (!next)
					{
						SDK::UObject** outer = ServerUtilityFields::ObjectOuter.In(cur);
						next = outer ? *outer : nullptr;
					}
					cur = next;
				}
				return nullptr;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return nullptr;
			}
		}

		// Is the part on for the player who owns the hooked object. False, so the original's result
		// stands, when the owner cannot be resolved. Needs the ledger entry as well as the part's
		// bit: a world end drops the entries without telling OpBuild, and the entry is the truth.
		bool OnForOwner(void* object, OpBuild::Part part)
		{
			if (!OnGameThread())
				return false;
			void* controller = ControllerOf_SEH(object);
			if (!controller)
				return false;
			const std::string id = PlayerIds::OfController(controller);
			if (id.empty() || !OpBuild::PartOn(id, part))
				return false;
			return CheatLedgerGame::Get().Has(CheatLedger::Kind::Toggle, id, OpBuild::FeatureOf(part), OpBuild::PartName(part));
		}

		int64_t __fastcall Detour_Cost(void* self)
		{
			const int64_t result = g_origCost(self);
			return OnForOwner(self, OpBuild::Part::Cost) ? kPlacementValid : result;
		}

		bool __fastcall Detour_StabilityCustom(void* self, const void* placementData)
		{
			const bool result = g_origCustom(self, placementData);
			return result || OnForOwner(self, OpBuild::Part::Stability);
		}

		bool __fastcall Detour_StabilityPillar(void* self, const void* placementData)
		{
			const bool result = g_origPillar(self, placementData);
			return result || OnForOwner(self, OpBuild::Part::Stability);
		}

		bool InstallOne(uintptr_t address, void* detour, void** original, HookHandle& handle)
		{
			if (handle)
				return true;
			if (!address || !HookApiPresent())
				return false;
			handle = GetHooks()->Hooks->Install(address, detour, original);
			return handle != nullptr;
		}

		void RemoveOne(HookHandle& handle)
		{
			if (!handle)
				return;
			if (HookApiPresent())
				GetHooks()->Hooks->Remove(handle);
			handle = nullptr;
		}

		void RemoveHook(OpBuild::Part part)
		{
			if (part == OpBuild::Part::Cost)
				RemoveOne(g_hookCost);
			else if (part == OpBuild::Part::Stability)
			{
				RemoveOne(g_hookCustom);
				RemoveOne(g_hookPillar);
			}
		}

		// Idempotent. The stability part needs both of its hooks: when the second fails the first is
		// taken out again and the part is not installed.
		bool InstallHook(OpBuild::Part part)
		{
			bool ok = false;
			if (part == OpBuild::Part::Cost)
				ok = InstallOne(g_addrCost, reinterpret_cast<void*>(&Detour_Cost), reinterpret_cast<void**>(&g_origCost), g_hookCost);
			else if (part == OpBuild::Part::Stability)
				ok = InstallOne(g_addrCustom, reinterpret_cast<void*>(&Detour_StabilityCustom), reinterpret_cast<void**>(&g_origCustom), g_hookCustom)
					&& InstallOne(g_addrPillar, reinterpret_cast<void*>(&Detour_StabilityPillar), reinterpret_cast<void**>(&g_origPillar), g_hookPillar);
			if (!ok)
			{
				RemoveHook(part);
				LOG_ERROR("[cheat.build] The %s hook could not be installed", OpBuild::PartName(part));
			}
			return ok;
		}

		bool HookInstalled(OpBuild::Part part)
		{
			if (part == OpBuild::Part::Cost)
				return g_hookCost != nullptr;
			if (part == OpBuild::Part::Stability)
				return g_hookCustom != nullptr && g_hookPillar != nullptr;
			return false;
		}

		// A part works when its rows resolved, the loader has its hook calls and the player lookup (the
		// ID of the player behind a hooked object) is on. Windows and benches are not hooked yet.
		bool PartAvailable(OpBuild::Part part)
		{
			if (!HookApiPresent() || !ServerUtilityFields::PlayerLookup.On())
				return false;
			if (part == OpBuild::Part::Cost)
				return ServerUtilityFields::CheatBuildCost.On() && g_addrCost != 0;
			if (part == OpBuild::Part::Stability)
				return ServerUtilityFields::CheatBuildStability.On() && g_addrCustom != 0 && g_addrPillar != 0;
			return false;
		}

		// ---- Ledger accessors for the free parts. A part's slot value is whether it is on for the
		// entry's player; there is no game object to write, the hooks read it.

		bool PartOfSlot(const CheatLedger::Entry& e, OpBuild::Part* out)
		{
			for (OpBuild::Part p : { OpBuild::Part::Cost, OpBuild::Part::Stability })
				if (e.feature == OpBuild::FeatureOf(p) && e.slot == OpBuild::PartName(p))
				{
					*out = p;
					return true;
				}
			return false;
		}

		CheatLedger::Value ReadPart(SDK::UObject*, const CheatLedger::Entry& e)
		{
			OpBuild::Part p;
			if (!PartOfSlot(e, &p))
				return {};
			return CheatLedger::Value(std::in_place_type<bool>, OpBuild::PartOn(e.playerId, p));
		}

		// On installs the part's hook with the first player (SetPart does it) and fails when it did not
		// install; off puts it back and removes the hook with the last player.
		bool WritePart(SDK::UObject*, const CheatLedger::Entry& e, const CheatLedger::Value& v)
		{
			OpBuild::Part p;
			const bool* on = std::get_if<bool>(&v);
			if (!on || !PartOfSlot(e, &p) || !OnGameThread())
				return false;
			OpBuild::SetPart(e.playerId, p, *on);
			if (*on && !HookInstalled(p))
			{
				OpBuild::SetPart(e.playerId, p, false);
				return false;
			}
			return true;
		}

		bool Apply(const CheatDispatch::Context& ctx, uint32_t reqId, const std::vector<OpBuild::Part>& parts, bool on)
		{
			if (!OnGameThread())
				return false;
			const std::string id = ctx.playerId ? *ctx.playerId : std::string();
			CheatLedger::Ledger& ledger = CheatLedgerGame::Get();

			if (!on)
			{
				for (OpBuild::Part p : parts)
					ledger.Release(CheatLedger::Kind::Toggle, id, OpBuild::FeatureOf(p), OpBuild::PartName(p));
				return true;
			}

			PlayerList::Entry who;
			PlayerList::Find(id, who);
			std::vector<OpBuild::Part> added;
			for (OpBuild::Part p : parts)
			{
				const char* feature = OpBuild::FeatureOf(p);
				const char* slot = OpBuild::PartName(p);
				const bool held = ledger.Has(CheatLedger::Kind::Toggle, id, feature, slot);
				// A world end drops entries without a write: clear a stale bit so the prior value captured is "off".
				if (!held)
					OpBuild::SetPart(id, p, false);

				CheatLedger::Request req;
				req.playerId    = id;
				req.playerName  = who.label;
				req.conn        = CheatLedgerGame::MakeWeak(static_cast<SDK::UObject*>(ctx.controller));
				req.feature     = feature;
				req.slot        = slot;
				req.kind        = CheatLedger::Kind::Toggle;
				req.value       = CheatLedger::Value(std::in_place_type<bool>, true);
				req.reqId       = reqId;
				req.ownerProven = InGameAdmin::IsProven(ctx.controller, id);
				if (!ledger.Apply(req))
				{
					for (OpBuild::Part a : added)
						ledger.Release(CheatLedger::Kind::Toggle, id, OpBuild::FeatureOf(a), OpBuild::PartName(a));
					return false;
				}
				if (!held)
					added.push_back(p);
			}
			return true;
		}
	}

	void Resolve(IPluginSelf* self, IPluginHookScanner* scanner)
	{
		g_check = reinterpret_cast<CheckFn>(PluginMap::Function(scanner, self, ServerUtilityFields::CheckAvailableBuildings,
			nullptr, PLUGIN_SCAN_FUNCTION_START, PLUGIN_SCAN_FLAG_OPTIONAL));
		g_addrCost = PluginMap::Function(scanner, self, ServerUtilityFields::PlacementResourceCondition,
			nullptr, PLUGIN_SCAN_FUNCTION_START, PLUGIN_SCAN_FLAG_OPTIONAL);
		g_addrCustom = PluginMap::Function(scanner, self, ServerUtilityFields::CheckStabilityCustom,
			nullptr, PLUGIN_SCAN_FUNCTION_START, PLUGIN_SCAN_FLAG_OPTIONAL);
		g_addrPillar = PluginMap::Function(scanner, self, ServerUtilityFields::CheckStabilityPillar,
			nullptr, PLUGIN_SCAN_FUNCTION_START, PLUGIN_SCAN_FLAG_OPTIONAL);
	}

	// Takes every hook out. Safe to call twice.
	void Shutdown()
	{
		RemoveHook(OpBuild::Part::Cost);
		RemoveHook(OpBuild::Part::Stability);
	}

	void Init()
	{
		for (OpBuild::Part p : { OpBuild::Part::Cost, OpBuild::Part::Stability })
			CheatLedgerGame::RegisterAccessor(OpBuild::FeatureOf(p), OpBuild::PartName(p), { &ReadPart, &WritePart });

		OpBuild::Env env;
		env.available       = &PartAvailable;
		env.unlockAvailable = &UnlockAvailable;
		env.inWorld         = &InWorld;
		env.apply           = &Apply;
		env.installHook     = &InstallHook;
		env.removeHook      = &RemoveHook;
		env.unlocked        = &Unlocked;
		env.unlock          = &Unlock;
		env.log             = [](const std::string& line) { LOG_INFO("[cheat.build] %s", line.c_str()); };
		OpBuild::SetEnv(std::move(env));
	}
}
