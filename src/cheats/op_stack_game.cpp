#include "op_stack_game.h"
#include "op_stack.h"
#include "cheat_ledger_game.h"
#include "host_rules/ingame_admin.h"
#include "host_rules/player_list.h"
#include "plugin_helpers.h"
#include "SDK/Engine_classes.hpp"
#include "SDK/AuItems_classes.hpp"
#include "item_stack_limits.h"
#include "plugin_map.h"
#include "server_utility_fields.h"

#include <Windows.h>
#include <atomic>
#include <string>
#include <vector>

static_assert(OpStack::kMaxStack == ItemStackLimits::kMaxStackCap,
              "the accepted size and the helper's cap must agree");

namespace OpStackGame
{
	namespace
	{
		constexpr const char* kFeature = "stack-size";
		constexpr const char* kSlot    = "stack";

		constexpr ULONGLONG kGuardMs  = 1000;   // the size is written again this often
		constexpr ULONGLONG kRescanMs = 5000;   // wait this long after a scan that found no items

		// The feature the version map can turn off on its own (a game build without the item
		// members): the plugin keeps running without it.
		PluginMap::Feature& Feature()
		{
			static PluginMap::Feature feature{ "cheat-stack", "Stack size", ItemStackLimits::Keys(), {} };
			return feature;
		}

		std::atomic<int>  g_target{ 0 };       // the wanted size, 0 = the game's own
		std::atomic<bool> g_available{ false };

		// Game thread only from here on.
		ItemStackLimits::Limits g_limits;
		ObjectRef<SDK::UWorld>  g_world;          // the world the items were found in
		int                     g_lastApplied = 0;
		int                     g_raised = 0;
		ULONGLONG               g_lastGuardMs = 0;
		ULONGLONG               g_nextRescanMs = 0;
		bool                    g_tickRegistered = false;
		int32_t                 g_scanCount = -1;   // length of the game's item list at the last scan
		bool                    g_warnedEmpty = false;

		bool OnGameThread()
		{
			IPluginHooks* hooks = GetHooks();
			return hooks && hooks->Engine && hooks->Engine->IsGameThread();
		}

		bool FeatureOn()
		{
			return Feature().On() && ItemStackLimits::Available();
		}

		SDK::UAuItemDataBaseSubsystem* Subsystem(SDK::UWorld* world)
		{
			return static_cast<SDK::UAuItemDataBaseSubsystem*>(
				SDK::USubsystemBlueprintLibrary::GetWorldSubsystem(world, SDK::UAuItemDataBaseSubsystem::StaticClass()));
		}

		// How many entries the game's item list holds now, -1 when it is not there.
		int32_t CatalogueCount(SDK::UWorld* world)
		{
			auto* subsystem = Subsystem(world);
			const auto* list = subsystem ? ServerUtilityFields::ItemDataBaseArray.In(subsystem) : nullptr;
			return list ? list->Num() : -1;
		}

		// Find every item the game has loaded in this world and hand them to the limits. The raw
		// pointers live in this function only.
		void Rescan(SDK::UWorld* world)
		{
			std::vector<SDK::UAuItemDataBase*> items;
			int32_t count = -1;
			auto* subsystem = Subsystem(world);
			const auto* list = subsystem ? ServerUtilityFields::ItemDataBaseArray.In(subsystem) : nullptr;
			if (list)
			{
				count = list->Num();
				items.reserve(static_cast<size_t>(count));
				for (int32_t i = 0; i < count; ++i)
					if ((*list)[i])
						items.push_back((*list)[i]);
			}

			g_limits.SetItems(items);
			g_world.Set(world);
			g_scanCount = count;
			g_lastApplied = 0;
			g_raised = g_limits.Raised();
			if (items.empty())
			{
				g_nextRescanMs = GetTickCount64() + kRescanMs;
				if (!g_warnedEmpty)
					LOG_WARN("[cheat.stack] No items found yet - trying again in a few seconds");
				g_warnedEmpty = true;
			}
			else
			{
				g_warnedEmpty = false;
				LOG_INFO("[cheat.stack] target %d, %d items", g_target.load(), g_limits.Tracked());
			}
		}

		// Every item back to the game's own max stack. A stack already above that size is not
		// touched (what the game does with it is not verified): see the hand-off, RISK.
		void Restore()
		{
			g_limits.SetTarget(0);
			const int changed = g_limits.RestoreAll();
			g_lastApplied = 0;
			g_raised = 0;
			if (changed > 0)
				LOG_INFO("[cheat.stack] game stack sizes put back on %d items", changed);
		}

		// Game thread, once per frame. Writes only a change, no file access.
		void OnTick(float)
		{
			g_available.store(FeatureOn());

			const ULONGLONG now = GetTickCount64();
			const int target = g_target.load();
			const bool guard = now - g_lastGuardMs >= kGuardMs;
			if (guard) g_lastGuardMs = now;

			auto* world = static_cast<SDK::UWorld*>(PluginSdkRuntime::GetWorld());

			OpStack::TickState s;
			s.featureAvailable = g_available.load();
			s.target           = target;
			s.lastApplied      = (guard && target > 0) ? 0 : g_lastApplied;
			s.itemsTracked     = g_limits.Tracked();
			s.itemsRaised      = g_raised;
			s.worldChanged     = world && !(ObjectRef<SDK::UWorld>(world) == g_world);
			// Items the game adds to its list after a scan: looked at once a second, only while a size is set.
			s.catalogueChanged = guard && world && target > 0 && g_limits.Tracked() > 0 && CatalogueCount(world) != g_scanCount;

			switch (OpStack::Decide(s))
			{
			case OpStack::Action::Rescan:
				if (world && now >= g_nextRescanMs)
					Rescan(world);
				break;
			case OpStack::Action::Apply:
				g_limits.SetTarget(target);
				g_limits.Apply();
				g_lastApplied = target;
				g_raised = g_limits.Raised();
				break;
			case OpStack::Action::Restore:
				Restore();
				break;
			case OpStack::Action::None:
				break;
			}
		}

		// The ledger's way to read and write the size: the wanted size, which the tick applies.
		CheatLedger::Value ReadSize(SDK::UObject*, const CheatLedger::Entry&)
		{
			return CheatLedger::Value(std::in_place_type<int32_t>, g_target.load());
		}

		bool WriteSize(SDK::UObject*, const CheatLedger::Entry&, const CheatLedger::Value& v)
		{
			const int32_t* size = std::get_if<int32_t>(&v);
			if (!size || *size < 0 || *size > OpStack::kMaxStack)
				return false;
			g_target.store(*size);
			return true;
		}

		bool Available()
		{
			return FeatureOn();
		}

		int Current()
		{
			return g_target.load();
		}

		bool Set(const CheatDispatch::Context& ctx, uint32_t reqId, int size)
		{
			const std::string id = ctx.playerId ? *ctx.playerId : std::string();
			PlayerList::Entry who;
			PlayerList::Find(id, who);

			CheatLedger::Request req;
			req.playerId    = id;
			req.playerName  = who.label;
			req.conn        = CheatLedgerGame::MakeWeak(static_cast<SDK::UObject*>(ctx.controller));
			req.feature     = kFeature;
			req.slot        = kSlot;
			req.kind        = CheatLedger::Kind::Override;
			req.value       = CheatLedger::Value(std::in_place_type<int32_t>, static_cast<int32_t>(size));
			req.reqId       = reqId;
			req.ownerProven = InGameAdmin::IsProven(ctx.controller, id);
			return CheatLedgerGame::Get().Apply(req);
		}

		// Puts the size back through the ledger, which writes the prior (the game's own sizes).
		bool Clear(const CheatDispatch::Context&, uint32_t)
		{
			CheatLedgerGame::Get().Release(CheatLedger::Kind::Override, std::string(), kFeature, kSlot);
			return g_target.load() == 0;
		}
	}

	void Resolve(IPluginSelf* self, IPluginHookScanner* scanner)
	{
		PluginMap::DeclareFeatures(scanner, self, Feature());
		if (!ItemStackLimits::Resolve(scanner, self))
			LOG_WARN("[cheat.stack] UAuItemDataBase::MaxStack/StackingType not in this game build's version map - cheat.stack is off");
		g_available.store(FeatureOn());
	}

	void Init()
	{
		CheatLedgerGame::RegisterAccessor(kFeature, kSlot, { &ReadSize, &WriteSize });

		OpStack::Env env;
		env.available = &Available;
		env.current   = &Current;
		env.set       = &Set;
		env.clear     = &Clear;
		env.log       = [](const std::string& line) { LOG_INFO("[cheat.stack] %s", line.c_str()); };
		OpStack::SetEnv(std::move(env));

		IPluginHooks* hooks = GetHooks();
		if (hooks && hooks->Engine)
		{
			hooks->Engine->RegisterOnTick(OnTick);
			g_tickRegistered = true;
		}
	}

	void Shutdown()
	{
		IPluginHooks* hooks = GetHooks();
		if (!hooks || !hooks->Engine)
			return;
		if (g_tickRegistered)
			hooks->Engine->UnregisterOnTick(OnTick);
		g_tickRegistered = false;

		// Shutdown also runs at process exit: the items are touched on the game thread only.
		if (OnGameThread())
			Restore();
		else
			LOG_WARN("[cheat.stack] Shutdown is not on the game thread - stack sizes left as they are");
	}
}
