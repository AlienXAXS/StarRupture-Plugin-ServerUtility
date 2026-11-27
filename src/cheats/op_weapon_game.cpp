#include "op_weapon_game.h"
#include "op_weapon.h"
#include "cheat_ledger_game.h"
#include "host_rules/ingame_admin.h"
#include "host_rules/player_list.h"
#include "plugin_helpers.h"
#include "server_utility_fields.h"
#include "plugin_map.h"

#include <Windows.h>
#include <algorithm>
#include <map>
#include <string>
#include <variant>

namespace OpWeaponGame
{
	namespace
	{
		// The multiplier each player asked for, by player and slot. Only the value the player sees
		// in `cheat.state`: the ledger holds the entry and the attribute values, and a stale value
		// here is never read while the ledger has no entry. Game thread.
		std::map<std::string, float> g_mult;

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
			return ServerUtilityFields::CheatWeapon.On();
		}

		const char* SlotName(OpWeapon::Slot slot)
		{
			return slot == OpWeapon::Slot::Damage ? OpWeapon::kSlotDamage : OpWeapon::kSlotRate;
		}

		std::string MultKey(const std::string& playerId, OpWeapon::Slot slot)
		{
			return playerId + "|" + SlotName(slot);
		}

		// The attribute of a slot on a weapon attribute set; null when the member did not resolve.
		SDK::FGameplayAttributeData* Attr(SDK::UCrWeaponAttributeSet* set, OpWeapon::Slot slot)
		{
			return slot == OpWeapon::Slot::Damage ? ServerUtilityFields::WeaponDamageMod.In(set)
			                                      : ServerUtilityFields::WeaponFireRateMod.In(set);
		}

		// The caller's weapon attribute set: the player character's, which the game applies to
		// whatever weapon is equipped. Used inside the call that fetched it, never kept. Null when the
		// caller has no player character. No C++ objects with destructors in this frame: SEH is legal.
		SDK::UCrWeaponAttributeSet* FindSet_SEH(void* controller)
		{
			__try
			{
				auto* pc = static_cast<SDK::APlayerController*>(controller);
				if (!pc)
					return nullptr;
				SDK::APawn* pawn = pc->K2_GetPawn();
				if (!pawn || !pawn->IsA(SDK::ACrCharacterPlayerBase::StaticClass()))
					return nullptr;
				SDK::UCrWeaponAttributeSet** setAt = ServerUtilityFields::CharacterWeaponAttributes.In(pawn);
				return setAt ? *setAt : nullptr;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return nullptr;
			}
		}

		// The attribute's current and base value. False on a fault or an unresolved member.
		bool ReadAttr_SEH(SDK::UCrWeaponAttributeSet* set, OpWeapon::Slot slot, float* current, float* base)
		{
			__try
			{
				const SDK::FGameplayAttributeData* attr = Attr(set, slot);
				if (!attr)
					return false;
				*current = attr->CurrentValue;
				*base    = attr->BaseValue;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// Writes the attribute's current value. Base stays as the game has it: the game's own effects
		// aggregate from it, and the pin in the ledger tick puts this value back when they overwrite it.
		bool WriteCurrent_SEH(SDK::UCrWeaponAttributeSet* set, OpWeapon::Slot slot, float value)
		{
			__try
			{
				SDK::FGameplayAttributeData* attr = Attr(set, slot);
				if (!attr)
					return false;
				attr->CurrentValue = value;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// BetterCheats' compose: the multiplier acts as one more mod stacked on the base, so a x2 on
		// +15% attachments gives 2.15, not 2.30. Floored at 0 and clamped to the slider range.
		float Compose(float game, float base, float mult, OpWeapon::Slot slot)
		{
			const float lo = static_cast<float>(slot == OpWeapon::Slot::Damage ? OpWeapon::kDamageMin : OpWeapon::kRateMin);
			const float hi = static_cast<float>(slot == OpWeapon::Slot::Damage ? OpWeapon::kDamageMax : OpWeapon::kRateMax);
			const float final = (std::max)(game + base * (mult - 1.0f), 0.0f);
			return std::clamp(final, lo, hi);
		}

		OpWeapon::Slot SlotOf(const CheatLedger::Entry& e)
		{
			return e.slot == OpWeapon::kSlotDamage ? OpWeapon::Slot::Damage : OpWeapon::Slot::Rate;
		}

		// ---- Ledger accessors. obj is the entry's weapon attribute set, validated by the ledger glue.

		CheatLedger::Value ReadSlot(SDK::UObject* obj, const CheatLedger::Entry& e)
		{
			if (!obj || !OnGameThread())
				return {};
			float current = 0.0f, base = 0.0f;
			if (!ReadAttr_SEH(static_cast<SDK::UCrWeaponAttributeSet*>(obj), SlotOf(e), &current, &base))
				return {};
			return CheatLedger::Value(std::in_place_type<float>, current);
		}

		bool WriteSlot(SDK::UObject* obj, const CheatLedger::Entry& e, const CheatLedger::Value& v)
		{
			const float* value = std::get_if<float>(&v);
			if (!obj || !value || !OnGameThread())
				return false;
			return WriteCurrent_SEH(static_cast<SDK::UCrWeaponAttributeSet*>(obj), SlotOf(e), *value);
		}

		// ---- OpWeapon::Env

		bool InWorld() { return World() != nullptr; }

		bool HasWeapon(const CheatDispatch::Context& ctx)
		{
			return OnGameThread() && FindSet_SEH(ctx.controller) != nullptr;
		}

		float Current(const std::string& playerId, OpWeapon::Slot slot)
		{
			if (!CheatLedgerGame::Get().Has(CheatLedger::Kind::Toggle, playerId, OpWeapon::kFeature, SlotName(slot)))
				return 1.0f;
			const auto it = g_mult.find(MultKey(playerId, slot));
			return it == g_mult.end() ? 1.0f : it->second;
		}

		// On: puts the multiplier on the attribute through the ledger (a Toggle of this player; the
		// pin keeps it there). A change while it is already on composes from the value the game held
		// before the first apply, so the multipliers do not stack. Off: releases the Toggle, which
		// puts that prior value back.
		bool Set(const CheatDispatch::Context& ctx, uint32_t reqId, OpWeapon::Slot slot, bool on, float mult)
		{
			if (!OnGameThread() || !Available())
				return false;

			const std::string id = ctx.playerId ? *ctx.playerId : std::string();
			CheatLedger::Ledger& ledger = CheatLedgerGame::Get();
			const char* slotName = SlotName(slot);

			if (!on)
			{
				ledger.Release(CheatLedger::Kind::Toggle, id, OpWeapon::kFeature, slotName);
				g_mult.erase(MultKey(id, slot));
				return true;
			}

			SDK::UCrWeaponAttributeSet* set = FindSet_SEH(ctx.controller);
			if (!set)
				return false;
			const CheatLedger::Weak target = CheatLedgerGame::MakeWeak(static_cast<SDK::UObject*>(set));

			float current = 0.0f, base = 0.0f;
			if (!ReadAttr_SEH(set, slot, &current, &base))
				return false;

			float game = current;
			const CheatLedger::Entry* held = ledger.Find(CheatLedger::Kind::Toggle, id, OpWeapon::kFeature, slotName);
			if (held && held->target == target)
			{
				if (const float* prior = std::get_if<float>(&held->prior))
					game = *prior;
			}

			PlayerList::Entry who;
			PlayerList::Find(id, who);

			CheatLedger::Request req;
			req.playerId    = id;
			req.playerName  = who.label;
			req.conn        = CheatLedgerGame::MakeWeak(static_cast<SDK::UObject*>(ctx.controller));
			req.feature     = OpWeapon::kFeature;
			req.slot        = slotName;
			req.kind        = CheatLedger::Kind::Toggle;
			req.pin         = true;
			req.target      = target;
			req.value       = CheatLedger::Value(std::in_place_type<float>, Compose(game, base, mult, slot));
			req.reqId       = reqId;
			req.ownerProven = InGameAdmin::IsProven(ctx.controller, id);
			if (!ledger.Apply(req))
				return false;

			g_mult[MultKey(id, slot)] = mult;
			return true;
		}
	}

	void Init()
	{
		CheatLedgerGame::RegisterAccessor(OpWeapon::kFeature, OpWeapon::kSlotDamage, { &ReadSlot, &WriteSlot });
		CheatLedgerGame::RegisterAccessor(OpWeapon::kFeature, OpWeapon::kSlotRate, { &ReadSlot, &WriteSlot });

		OpWeapon::Env env;
		env.available = &Available;
		env.inWorld   = &InWorld;
		env.hasWeapon = &HasWeapon;
		env.current   = &Current;
		env.set       = &Set;
		env.log       = [](const std::string& line) { LOG_INFO("[cheat.weapon] %s", line.c_str()); };
		OpWeapon::SetEnv(std::move(env));
	}
}
