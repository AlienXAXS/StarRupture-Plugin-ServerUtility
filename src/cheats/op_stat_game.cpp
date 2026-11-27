#include "op_stat_game.h"
#include "op_stat.h"
#include "op_power.h"
#include "cheat_ledger_game.h"
#include "host_rules/ingame_admin.h"
#include "host_rules/player_list.h"
#include "plugin_helpers.h"
#include "server_utility_fields.h"
#include "plugin_map.h"

#include <Windows.h>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace OpStatGame
{
	namespace
	{
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

		bool Available()
		{
			return ServerUtilityFields::CheatStat.On();
		}

		// One stat: how to reach its attribute set from the player character and the set's attributes.
		// The members come through the loader's map (server_utility_fields.h); a member that did not
		// resolve gives null, and Available() is off then. `minAttr` is null on the stats that floor at zero.
		struct Row
		{
			const char*                                         name;
			SDK::UObject*                                       (*set)(SDK::UObject* pawn);
			SDK::FGameplayAttributeData*                        (*current)(SDK::UObject* set);
			SDK::FGameplayAttributeData*                        (*maxAttr)(SDK::UObject* set);
			SDK::FGameplayAttributeData*                        (*minAttr)(SDK::UObject* set);
		};

#define SU_STAT_SET(Name) \
	[](SDK::UObject* pawn) -> SDK::UObject* { auto* at = ServerUtilityFields::Stat##Name##Set.In(pawn); return at ? *at : nullptr; }
#define SU_STAT_ATTR(Field) \
	[](SDK::UObject* set) -> SDK::FGameplayAttributeData* { return ServerUtilityFields::Field.In(set); }
#define SU_STAT_ROW_RES(Name, label) \
	{ label, SU_STAT_SET(Name), SU_STAT_ATTR(Stat##Name##Current), SU_STAT_ATTR(Stat##Name##Max), nullptr }
#define SU_STAT_ROW_AFF(Name, label) \
	{ label, SU_STAT_SET(Name), SU_STAT_ATTR(Stat##Name##Current), SU_STAT_ATTR(Stat##Name##Max), SU_STAT_ATTR(Stat##Name##Min) }

		const Row kRows[] = {
			SU_STAT_ROW_RES(Health, "health"),
			SU_STAT_ROW_RES(Energy, "energy"),
			SU_STAT_ROW_RES(Shield, "shield"),
			SU_STAT_ROW_RES(Hydration, "hydration"),
			SU_STAT_ROW_RES(Calories, "calories"),
			SU_STAT_ROW_RES(Oxygen, "oxygen"),
			SU_STAT_ROW_AFF(Toxicity, "toxicity"),
			SU_STAT_ROW_AFF(Radiation, "radiation"),
			SU_STAT_ROW_AFF(Heat, "heat"),
			SU_STAT_ROW_AFF(Drain, "drain"),
			SU_STAT_ROW_AFF(Corrosion, "corrosion"),
			SU_STAT_ROW_AFF(Infection, "infection"),
			SU_STAT_ROW_AFF(Temperature, "temperature"),
		};
#undef SU_STAT_ROW_AFF
#undef SU_STAT_ROW_RES
#undef SU_STAT_ATTR
#undef SU_STAT_SET

		const Row* RowOf(std::string_view stat)
		{
			for (const Row& r : kRows)
			{
				if (stat == r.name)
					return &r;
			}
			return nullptr;
		}

		// The row of a ledger slot ("lock:<stat>").
		const Row* RowOfSlot(const std::string& slot)
		{
			const size_t prefix = std::strlen(OpStat::kSlotPrefix);
			if (slot.size() <= prefix || slot.compare(0, prefix, OpStat::kSlotPrefix) != 0)
				return nullptr;
			return RowOf(std::string_view(slot).substr(prefix));
		}

		// The caller's attribute set for a stat: the player character's. Used inside the call that
		// fetched it, never kept (the ledger holds a weak reference). Null when the caller has no player
		// character or the member did not resolve. No C++ objects with destructors in this frame: SEH is legal.
		SDK::UObject* FindSet_SEH(void* controller, const Row& row)
		{
			__try
			{
				auto* pc = static_cast<SDK::APlayerController*>(controller);
				if (!pc)
					return nullptr;
				SDK::APawn* pawn = pc->K2_GetPawn();
				if (!pawn || !pawn->IsA(SDK::ACrCharacterPlayerBase::StaticClass()))
					return nullptr;
				return row.set(pawn);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return nullptr;
			}
		}

		// The stat's base and current value. False on a fault or an unresolved member.
		bool ReadPair_SEH(SDK::UObject* set, const Row& row, float* base, float* current)
		{
			__try
			{
				const SDK::FGameplayAttributeData* attr = row.current(set);
				if (!attr)
					return false;
				*base    = attr->BaseValue;
				*current = attr->CurrentValue;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// The stat's own limits: Min (zero where the set has none) to Max, as BetterCheats reads them.
		bool ReadRange_SEH(SDK::UObject* set, const Row& row, float* lo, float* hi)
		{
			__try
			{
				const SDK::FGameplayAttributeData* hiAttr = row.maxAttr(set);
				if (!hiAttr)
					return false;
				const SDK::FGameplayAttributeData* loAttr = row.minAttr ? row.minAttr(set) : nullptr;
				if (row.minAttr && !loAttr)
					return false;
				*lo = loAttr ? loAttr->CurrentValue : 0.0f;
				*hi = hiAttr->CurrentValue;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// Writes both values, as BetterCheats does, so a gameplay effect that recomputes from the base
		// does not put the old number back.
		bool WritePair_SEH(SDK::UObject* set, const Row& row, float base, float current)
		{
			__try
			{
				SDK::FGameplayAttributeData* attr = row.current(set);
				if (!attr)
					return false;
				attr->BaseValue    = base;
				attr->CurrentValue = current;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// ---- Ledger accessors. obj is the entry's attribute set, validated by the ledger glue. The
		// value is {base, current}, so a revert puts both back exactly.

		CheatLedger::Value ReadLock(SDK::UObject* obj, const CheatLedger::Entry& e)
		{
			const Row* row = RowOfSlot(e.slot);
			if (!obj || !row || !OnGameThread())
				return {};
			float base = 0.0f, current = 0.0f;
			if (!ReadPair_SEH(obj, *row, &base, &current))
				return {};
			return CheatLedger::Value(std::vector<float>{ base, current });
		}

		bool WriteLock(SDK::UObject* obj, const CheatLedger::Entry& e, const CheatLedger::Value& v)
		{
			const Row* row = RowOfSlot(e.slot);
			const auto* pair = std::get_if<std::vector<float>>(&v);
			if (!obj || !row || !pair || pair->size() != 2 || !OnGameThread())
				return false;
			return WritePair_SEH(obj, *row, (*pair)[0], (*pair)[1]);
		}

		// The player's current attribute set for a lock's stat, for the ledger to move a held lock onto
		// after a respawn.
		CheatLedger::Weak RebindLock(void* controller, const CheatLedger::Entry& e)
		{
			const Row* row = RowOfSlot(e.slot);
			if (!row || !OnGameThread())
				return {};
			return CheatLedgerGame::MakeWeak(FindSet_SEH(controller, *row));
		}

		// ---- OpStat::Env

		bool InWorld() { return World() != nullptr; }

		std::vector<std::string> Locked(const std::string& playerId)
		{
			std::vector<std::string> out;
			for (const std::string& stat : OpStat::Stats())
			{
				if (CheatLedgerGame::Get().Has(CheatLedger::Kind::Toggle, playerId, OpStat::kFeature, OpStat::SlotFor(stat)))
					out.push_back(stat);
			}
			return out;
		}

		std::optional<OpStat::Range> RangeOf(const CheatDispatch::Context& ctx, const std::string& stat)
		{
			const Row* row = RowOf(stat);
			if (!row || !World())
				return std::nullopt;
			SDK::UObject* set = FindSet_SEH(ctx.controller, *row);
			float lo = 0.0f, hi = 0.0f;
			if (!set || !ReadRange_SEH(set, *row, &lo, &hi))
				return std::nullopt;
			return OpStat::Range{ lo, hi };
		}

		std::optional<float> Read(const CheatDispatch::Context& ctx, const std::string& stat)
		{
			const Row* row = RowOf(stat);
			if (!row || !World())
				return std::nullopt;
			SDK::UObject* set = FindSet_SEH(ctx.controller, *row);
			float base = 0.0f, current = 0.0f;
			if (!set || !ReadPair_SEH(set, *row, &base, &current))
				return std::nullopt;
			return current;
		}

		// A lock is a pinned Toggle of this player on the stat's attribute set; the ledger tick writes
		// it back, and leave, deny, world end and server stop put the value held before it back. The
		// lock follows the player: after a respawn the tick moves it to the new pawn's set, with that
		// set's own value as the prior, and the read-back stays on throughout. A value for a stat that is already locked moves the
		// lock to that value, so the pin does not undo it. A value without a lock is a one-shot grant
		// with no entry. A lock off releases the entry first, then a value, if any, is written once.
		bool Apply(const CheatDispatch::Context& ctx, uint32_t reqId, const std::string& stat,
		           std::optional<float> value, std::optional<bool> lock)
		{
			const Row* row = RowOf(stat);
			if (!row || !OnGameThread() || !Available())
				return false;

			const std::string id = ctx.playerId ? *ctx.playerId : std::string();
			CheatLedger::Ledger& ledger = CheatLedgerGame::Get();
			const std::string slot = OpStat::SlotFor(stat);
			const bool held = ledger.Has(CheatLedger::Kind::Toggle, id, OpStat::kFeature, slot);

			if (lock && !*lock)
			{
				ledger.Release(CheatLedger::Kind::Toggle, id, OpStat::kFeature, slot);
				if (!value)
					return true;
			}
			const bool pinned = lock ? *lock : held;
			if (!pinned && !value)
				return false;

			SDK::UObject* set = FindSet_SEH(ctx.controller, *row);
			if (!set)
				return false;
			float base = 0.0f, current = 0.0f;
			if (!ReadPair_SEH(set, *row, &base, &current))
				return false;

			PlayerList::Entry who;
			PlayerList::Find(id, who);

			if (!pinned)
			{
				if (!WritePair_SEH(set, *row, *value, *value))
					return false;
				ledger.Grant(id, who.label, OpStat::kFeature, slot, "set " + stat + " to " + OpPower::FormatValue(*value), reqId);
				return true;
			}

			const float target = value ? *value : current;
			CheatLedger::Request req;
			req.playerId    = id;
			req.playerName  = who.label;
			req.conn        = CheatLedgerGame::MakeWeak(static_cast<SDK::UObject*>(ctx.controller));
			req.feature     = OpStat::kFeature;
			req.slot        = slot;
			req.kind        = CheatLedger::Kind::Toggle;
			req.pin         = true;
			req.follow      = true;
			req.target      = CheatLedgerGame::MakeWeak(set);
			req.value       = CheatLedger::Value(std::vector<float>{ target, target });
			req.reqId       = reqId;
			req.ownerProven = InGameAdmin::IsProven(ctx.controller, id);
			return ledger.Apply(req);
		}
	}

	void Init()
	{
		CheatLedgerGame::RegisterAccessor(OpStat::kFeature, OpStat::kSlotPrefix, { &ReadLock, &WriteLock, &RebindLock });

		OpStat::Env env;
		env.available = &Available;
		env.inWorld   = &InWorld;
		env.locked    = &Locked;
		env.range     = &RangeOf;
		env.read      = &Read;
		env.apply     = &Apply;
		env.log       = [](const std::string& line) { LOG_INFO("[cheat.stat] %s", line.c_str()); };
		OpStat::SetEnv(std::move(env));
	}
}
