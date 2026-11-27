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

		// The anywhere parts. The placement helper's two collision checks answer with an
		// EAuAPlacementConditionResult byte, as BetterCheats reads them: 1 Valid, 40 IsColliding,
		// 43 BuildingColliding. GetSnappedSocket returns the socket pointer and takes ten arguments
		// (the last six on the stack); IsSocketFree is the stability subsystem's slot check.
		using MainMeshFn   = uint8_t(__fastcall*)(void* self, const void* placementData);
		using BoxFn        = uint8_t(__fastcall*)(void* self, const void* min, const void* max,
			const void* ignoredActors, const void* ignoredClasses, bool flag);
		using SnappedFn    = void*(__fastcall*)(void* self, const void* data, void* a3, void* a4, void* a5,
			void* a6, void* a7, void* a8, void* a9, void* a10);
		using SocketFreeFn = bool(__fastcall*)(void* self, void* building, const void* socket);

		constexpr uint8_t kIsColliding       = 40;
		constexpr uint8_t kBuildingColliding = 43;

		uintptr_t g_addrMainMesh = 0, g_addrBox = 0, g_addrSnapped = 0, g_addrSocketFree = 0;
		MainMeshFn   g_origMainMesh   = nullptr;
		BoxFn        g_origBox        = nullptr;
		SnappedFn    g_origSnapped    = nullptr;
		SocketFreeFn g_origSocketFree = nullptr;
		HookHandle   g_hookMainMesh = nullptr, g_hookBox = nullptr, g_hookSnapped = nullptr, g_hookSocketFree = nullptr;
		// Which anywhere parts the glue asked hooks for: the collision pair serves both.
		bool g_windowsOn = false, g_benchesOn = false;

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
		std::string OwnerId(void* object)
		{
			if (!OnGameThread())
				return {};
			void* controller = ControllerOf_SEH(object);
			return controller ? PlayerIds::OfController(controller) : std::string();
		}

		bool PartHeld(const std::string& id, OpBuild::Part part)
		{
			if (id.empty() || !OpBuild::PartOn(id, part))
				return false;
			return CheatLedgerGame::Get().Has(CheatLedger::Kind::Toggle, id, OpBuild::FeatureOf(part), OpBuild::PartName(part));
		}

		bool OnForOwner(void* object, OpBuild::Part part)
		{
			return PartHeld(OwnerId(object), part);
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

		// ---- The anywhere parts. A habitat's built-in machines are not registered on the wall slot they
		// fill, so a window or bench next to them is refused by the two collision checks; a bench
		// into a slot that holds a window is refused earlier, by the slot check. For a player with the
		// part on, a window or bench snapped to a habitat or the Hub counts as valid after the
		// collision checks, and a bench's snap counts a habitat's slot as free.

		bool IsWindowId(SDK::ECrBuildingID id)
		{
			return id == SDK::ECrBuildingID::ViewportLeft  || id == SDK::ECrBuildingID::ViewportMiddle
			    || id == SDK::ECrBuildingID::ViewportRight || id == SDK::ECrBuildingID::ViewportSingle;
		}

		// The habitat machines, placed into wall slots like windows. Personal storage is a custom
		// building that registers on its slot, so it is not here.
		bool IsBenchId(SDK::ECrBuildingID id)
		{
			switch (id)
			{
			case SDK::ECrBuildingID::Analyzer:
			case SDK::ECrBuildingID::Armory:
			case SDK::ECrBuildingID::CloningBed:
			case SDK::ECrBuildingID::FoodProcessor:
			case SDK::ECrBuildingID::ItemPrinter:
			case SDK::ECrBuildingID::RecipeTable:
			case SDK::ECrBuildingID::Recycler:
			case SDK::ECrBuildingID::ResearchTerminal:
			case SDK::ECrBuildingID::SuitWorkshop:
			case SDK::ECrBuildingID::UpgradeStation:
				return true;
			default:
				return false;
			}
		}

		// A habitat or the Hub. Calls into the game: inside __try only.
		bool IsHabitatActor(SDK::AActor* actor)
		{
			if (!actor || !actor->IsA(SDK::ACrBuildingActorBase::StaticClass()))
				return false;
			const SDK::ECrBuildingID id = static_cast<SDK::ACrBuildingActorBase*>(actor)->GetBuildingID();
			return id == SDK::ECrBuildingID::HabitatBig || id == SDK::ECrBuildingID::HabitatSmall
			    || id == SDK::ECrBuildingID::Hub;
		}

		bool IsHabitat_SEH(void* actor)
		{
			__try
			{
				return IsHabitatActor(static_cast<SDK::AActor*>(actor));
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// The building id of a placement data asset. May fault: SEH, no objects needing unwinding.
		bool DataId_SEH(const void* data, SDK::ECrBuildingID* out)
		{
			__try
			{
				const SDK::ECrBuildingID* id = data ? ServerUtilityFields::PlacementBuildingID.In(data) : nullptr;
				if (!id)
					return false;
				*out = *id;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// The building the placement helper is placing: its building data's id.
		bool PieceId_SEH(void* helper, SDK::ECrBuildingID* out)
		{
			__try
			{
				SDK::UCrBuildingData** data = ServerUtilityFields::CrAPHelperBuildingData.In(helper);
				const SDK::ECrBuildingID* id = (data && *data)
					? ServerUtilityFields::PlacementBuildingID.In(static_cast<const void*>(*data)) : nullptr;
				if (!id)
					return false;
				*out = *id;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// Is the helper's piece snapped to a habitat or the Hub: 1 yes, 0 no, -1 when this build's
		// helper has no snapped-actors member (the part then leaves collision alone).
		int SnappedToHabitat_SEH(void* helper)
		{
			__try
			{
				IPluginHooks* hooks = GetHooks();
				auto* snapped = ServerUtilityFields::CrAPHelperSnappedActors.In(
					hooks ? hooks->ObjectProperties : nullptr, static_cast<SDK::UObject*>(helper));
				if (!snapped)
					return -1;
				for (int32_t i = 0; i < snapped->Num(); ++i)
					if (IsHabitatActor((*snapped)[i]))
						return 1;
				return 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return 0;
			}
		}

		// Valid for a window or bench snapped to a habitat, when its owner has that part on. Only the
		// two colliding verdicts are touched; anything else the game found stands.
		uint8_t RelaxCollision(void* helper, uint8_t result)
		{
			if ((result != kBuildingColliding && result != kIsColliding) || !OnGameThread())
				return result;
			SDK::ECrBuildingID piece{};
			if (!PieceId_SEH(helper, &piece))
				return result;
			const bool window = IsWindowId(piece);
			if (!window && !IsBenchId(piece))
				return result;
			if (!PartHeld(OwnerId(helper), window ? OpBuild::Part::Windows : OpBuild::Part::Benches))
				return result;
			const int snapped = SnappedToHabitat_SEH(helper);
			if (snapped < 0)
			{
				static bool s_logged = false;
				if (!s_logged)
				{
					s_logged = true;
					LOG_WARN("[cheat.build] The placement helper has no snapped-actors member on this game build; windows and benches anywhere leave collision alone");
				}
			}
			return snapped > 0 ? static_cast<uint8_t>(kPlacementValid) : result;
		}

		uint8_t __fastcall Detour_MainMesh(void* self, const void* placementData)
		{
			return RelaxCollision(self, g_origMainMesh(self, placementData));
		}

		uint8_t __fastcall Detour_Box(void* self, const void* min, const void* max, const void* ignoredActors,
			const void* ignoredClasses, bool flag)
		{
			return RelaxCollision(self, g_origBox(self, min, max, ignoredActors, ignoredClasses, flag));
		}

		// IsSocketFree belongs to the stability subsystem, which has no player. Its only caller is
		// GetSnappedSocket, so the player is found there: this is set only while a bench's snap of a
		// player who has the part on runs, on the thread that runs it. Anything else is untouched.
		thread_local bool t_snappingBench = false;

		struct SnapScope
		{
			explicit SnapScope(bool on) { t_snappingBench = on; }
			~SnapScope() { t_snappingBench = false; }
		};

		bool BenchSnap(void* method, const void* data)
		{
			SDK::ECrBuildingID piece{};
			if (!OnGameThread() || !DataId_SEH(data, &piece) || !IsBenchId(piece))
				return false;
			return PartHeld(OwnerId(method), OpBuild::Part::Benches);
		}

		void* __fastcall Detour_Snapped(void* self, const void* data, void* a3, void* a4, void* a5, void* a6,
			void* a7, void* a8, void* a9, void* a10)
		{
			SnapScope scope(BenchSnap(self, data));
			return g_origSnapped(self, data, a3, a4, a5, a6, a7, a8, a9, a10);
		}

		bool __fastcall Detour_SocketFree(void* self, void* building, const void* socket)
		{
			const bool isFree = g_origSocketFree(self, building, socket);
			if (isFree || !t_snappingBench)
				return isFree;
			return IsHabitat_SEH(building);
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

		// The collision pair serves windows and benches: it stays while either part wants it.
		void RemoveCollision()
		{
			RemoveOne(g_hookMainMesh);
			RemoveOne(g_hookBox);
		}

		// Both checks or neither: one alone would relax only half the verdict.
		bool InstallCollision()
		{
			if (InstallOne(g_addrMainMesh, reinterpret_cast<void*>(&Detour_MainMesh), reinterpret_cast<void**>(&g_origMainMesh), g_hookMainMesh)
				&& InstallOne(g_addrBox, reinterpret_cast<void*>(&Detour_Box), reinterpret_cast<void**>(&g_origBox), g_hookBox))
				return true;
			RemoveCollision();
			return false;
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
			else if (part == OpBuild::Part::Windows)
			{
				g_windowsOn = false;
				if (!g_benchesOn)
					RemoveCollision();
			}
			else if (part == OpBuild::Part::Benches)
			{
				g_benchesOn = false;
				RemoveOne(g_hookSnapped);
				RemoveOne(g_hookSocketFree);
				if (!g_windowsOn)
					RemoveCollision();
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
			else if (part == OpBuild::Part::Windows)
				ok = g_windowsOn = InstallCollision();
			else if (part == OpBuild::Part::Benches)
				ok = g_benchesOn = InstallCollision()
					&& InstallOne(g_addrSnapped, reinterpret_cast<void*>(&Detour_Snapped), reinterpret_cast<void**>(&g_origSnapped), g_hookSnapped)
					&& InstallOne(g_addrSocketFree, reinterpret_cast<void*>(&Detour_SocketFree), reinterpret_cast<void**>(&g_origSocketFree), g_hookSocketFree);
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
			if (part == OpBuild::Part::Windows)
				return g_windowsOn && g_hookMainMesh != nullptr && g_hookBox != nullptr;
			if (part == OpBuild::Part::Benches)
				return g_benchesOn && g_hookMainMesh != nullptr && g_hookBox != nullptr
					&& g_hookSnapped != nullptr && g_hookSocketFree != nullptr;
			return false;
		}

		// A part works when its rows resolved, the loader has its hook calls and the player lookup (the
		// ID of the player behind a hooked object) is on.
		bool PartAvailable(OpBuild::Part part)
		{
			if (!HookApiPresent() || !ServerUtilityFields::PlayerLookup.On())
				return false;
			if (part == OpBuild::Part::Cost)
				return ServerUtilityFields::CheatBuildCost.On() && g_addrCost != 0;
			if (part == OpBuild::Part::Stability)
				return ServerUtilityFields::CheatBuildStability.On() && g_addrCustom != 0 && g_addrPillar != 0;
			if (part == OpBuild::Part::Windows)
				return ServerUtilityFields::CheatBuildWindows.On() && g_addrMainMesh != 0 && g_addrBox != 0;
			if (part == OpBuild::Part::Benches)
				return ServerUtilityFields::CheatBuildBenches.On() && g_addrMainMesh != 0 && g_addrBox != 0
					&& g_addrSnapped != 0 && g_addrSocketFree != 0;
			return false;
		}

		// ---- Ledger accessors for the free parts. A part's slot value is whether it is on for the
		// entry's player; there is no game object to write, the hooks read it.

		bool PartOfSlot(const CheatLedger::Entry& e, OpBuild::Part* out)
		{
			for (OpBuild::Part p : { OpBuild::Part::Cost, OpBuild::Part::Stability, OpBuild::Part::Windows, OpBuild::Part::Benches })
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
		g_addrMainMesh = PluginMap::Function(scanner, self, ServerUtilityFields::CheckMainMeshCollision,
			nullptr, PLUGIN_SCAN_FUNCTION_START, PLUGIN_SCAN_FLAG_OPTIONAL);
		g_addrBox = PluginMap::Function(scanner, self, ServerUtilityFields::GetBoxCollisionResult,
			nullptr, PLUGIN_SCAN_FUNCTION_START, PLUGIN_SCAN_FLAG_OPTIONAL);
		g_addrSnapped = PluginMap::Function(scanner, self, ServerUtilityFields::GetSnappedSocket,
			nullptr, PLUGIN_SCAN_FUNCTION_START, PLUGIN_SCAN_FLAG_OPTIONAL);
		g_addrSocketFree = PluginMap::Function(scanner, self, ServerUtilityFields::IsSocketFree,
			nullptr, PLUGIN_SCAN_FUNCTION_START, PLUGIN_SCAN_FLAG_OPTIONAL);
	}

	// Takes every hook out. Safe to call twice.
	void Shutdown()
	{
		RemoveHook(OpBuild::Part::Cost);
		RemoveHook(OpBuild::Part::Stability);
		RemoveHook(OpBuild::Part::Windows);
		RemoveHook(OpBuild::Part::Benches);
	}

	void Init()
	{
		for (OpBuild::Part p : { OpBuild::Part::Cost, OpBuild::Part::Stability, OpBuild::Part::Windows, OpBuild::Part::Benches })
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
