#include "op_mine_game.h"
#include "op_mine.h"
#include "cheat_ledger_game.h"
#include "host_rules/ingame_admin.h"
#include "host_rules/player_list.h"
#include "plugin_helpers.h"
#include "game_access.h"
#include "server_utility_fields.h"
#include "plugin_map.h"
#include "object_ref.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace OpMineGame
{
	namespace
	{
		using GameAccess::OnGameThread;
		using GameAccess::World;
		using GameAccess::Invoke;

		constexpr const char* kLibraryClass   = "AbilitySystemBlueprintLibrary";
		constexpr const char* kComponentClass = "AbilitySystemComponent";
		constexpr const wchar_t* kHeatTag     = L"Cheat.UnlimitedWeaponHeat";
		constexpr size_t kTagCap              = 64;

		using PawnRef = ObjectRef<SDK::ACrCharacterPlayerBase>;
		using SetRef  = ObjectRef<SDK::UCrMiningBoostAttributeSet>;

		// The boost each player asked for. Only the value the player sees in `cheat.state`: the
		// ledger holds the entry and the attribute values, and this is never read while the ledger
		// has no entry. Game thread.
		std::map<std::string, float> g_boost;

		SDK::FName g_heatName;
		bool       g_heatNameSet = false;

		bool Available()
		{
			return ServerUtilityFields::CheatMine.On();
		}

		// ---- Boost

		// A player character and its mining boost attribute set, as weak references. False when the
		// character is null or has no set. No C++ objects needing unwinding in this frame: SEH is legal.
		bool CaptureFrom_SEH(SDK::UObject* character, PawnRef* pawnOut, SetRef* setOut)
		{
			__try
			{
				SDK::UCrMiningBoostAttributeSet** setAt = ServerUtilityFields::CharacterMiningBoost.In(character);
				if (!setAt || !*setAt)
					return false;
				pawnOut->Set(static_cast<SDK::ACrCharacterPlayerBase*>(character));
				setOut->Set(*setAt);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// The caller's player character and its mining boost attribute set. False when the controller
		// has no player character or the character has no set.
		bool Capture(void* controller, PawnRef* pawnOut, SetRef* setOut)
		{
			return CaptureFrom_SEH(GameAccess::CharacterOf(controller), pawnOut, setOut);
		}

		// The boost's four values: current base, current current, max base, max current.
		bool ReadBoost_SEH(SDK::UCrMiningBoostAttributeSet* set, float* out)
		{
			__try
			{
				const SDK::FGameplayAttributeData* cur = ServerUtilityFields::MiningBoostCurrent.In(set);
				const SDK::FGameplayAttributeData* max = ServerUtilityFields::MiningBoostMax.In(set);
				if (!cur || !max)
					return false;
				out[0] = cur->BaseValue;
				out[1] = cur->CurrentValue;
				out[2] = max->BaseValue;
				out[3] = max->CurrentValue;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		bool WriteBoost_SEH(SDK::UCrMiningBoostAttributeSet* set, const float* in)
		{
			__try
			{
				SDK::FGameplayAttributeData* cur = ServerUtilityFields::MiningBoostCurrent.In(set);
				SDK::FGameplayAttributeData* max = ServerUtilityFields::MiningBoostMax.In(set);
				if (!cur || !max)
					return false;
				cur->BaseValue    = in[0];
				cur->CurrentValue = in[1];
				max->BaseValue    = in[2];
				max->CurrentValue = in[3];
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		CheatLedger::Value ReadBoostSlot(SDK::UObject* obj, const CheatLedger::Entry&)
		{
			if (!obj || !OnGameThread())
				return {};
			float v[4] = {};
			if (!ReadBoost_SEH(static_cast<SDK::UCrMiningBoostAttributeSet*>(obj), v))
				return {};
			return CheatLedger::Value(std::in_place_type<std::vector<float>>, v, v + 4);
		}

		bool WriteBoostSlot(SDK::UObject* obj, const CheatLedger::Entry&, const CheatLedger::Value& value)
		{
			const auto* v = std::get_if<std::vector<float>>(&value);
			if (!obj || !v || v->size() != 4 || !OnGameThread())
				return false;
			return WriteBoost_SEH(static_cast<SDK::UCrMiningBoostAttributeSet*>(obj), v->data());
		}

		// The boost values to hold for a boost multiplier: current at the value, and the max raised to
		// it when it is below it. base is the game's four values (see ReadBoost_SEH).
		std::vector<float> ComposeBoostValue(const float* base, float value)
		{
			std::vector<float> applied = { value, value, base[2], base[3] };
			if (base[3] < value)
				applied[2] = applied[3] = value;
			return applied;
		}

		// The value to hold for a prior (the game's four values) and the boost the player asked for.
		CheatLedger::Value ComposeBoostSlot(SDK::UObject*, const CheatLedger::Entry& e, const CheatLedger::Value& prior)
		{
			const auto* p = std::get_if<std::vector<float>>(&prior);
			const float* value = std::get_if<float>(&e.asked);
			if (!p || p->size() != 4 || !value)
				return {};
			return CheatLedger::Value(std::in_place_type<std::vector<float>>, ComposeBoostValue(p->data(), *value));
		}

		// Only the max pair is the game's to change. GAS derives the current pair from the BaseValue SU
		// writes, so a change there (a drain) is built on our write, not the game's own value.
		bool BoostRebases(const CheatLedger::Entry&, size_t member)
		{
			return member >= 2;
		}

		// ---- Heat tag

		// The ability library's default object, which its static functions are called on.
		SDK::UObject* LibraryObject_SEH()
		{
			__try
			{
				return SDK::UAbilitySystemBlueprintLibrary::GetDefaultObj();
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return nullptr;
			}
		}

		bool MakeName_SEH(SDK::FName* out)
		{
			__try
			{
				*out = SDK::BasicFilesImplUtils::StringToName(kHeatTag);
				return !out->IsNone();
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// Fills tag (kTagCap bytes, zeroed here) with the heat tag, as the game reflects its layout.
		bool MakeTag(uint8_t* tag)
		{
			using namespace ServerUtilityFields;
			if (GameplayTagSize.resolvedSize == 0 || GameplayTagSize.resolvedSize > kTagCap
				|| GameplayTagName.resolvedSize != sizeof(SDK::FName))
				return false;
			if (!g_heatNameSet)
			{
				if (!MakeName_SEH(&g_heatName))
					return false;
				g_heatNameSet = true;
			}
			std::memset(tag, 0, kTagCap);
			SDK::FName* at = GameplayTagName.In(tag);
			if (!at)
				return false;
			*at = g_heatName;
			return true;
		}

		// How many copies of the heat tag the pawn carries; nullopt when the call could not be made.
		std::optional<int32_t> HeatCount(SDK::AActor* pawn)
		{
			using namespace ServerUtilityFields;
			SDK::UObject* library = LibraryObject_SEH();
			if (!pawn || !library || GetAscActor.resolvedSize != sizeof(void*) || GetAscResult.resolvedSize != sizeof(void*)
				|| TagCountResult.resolvedSize != sizeof(int32_t) || TagCountTag.resolvedSize != GameplayTagSize.resolvedSize)
				return std::nullopt;

			PluginMap::ParamsBlock getAsc = PluginMap::ParamsBlockOf(GetAscSize, { &GetAscActor, &GetAscResult });
			if (!getAsc)
				return std::nullopt;
			*reinterpret_cast<SDK::AActor**>(getAsc.Param("Actor")) = pawn;
			if (!Invoke(library, kLibraryClass, "GetAbilitySystemComponent", getAsc.Data()))
				return std::nullopt;
			SDK::UObject* asc = *reinterpret_cast<SDK::UObject**>(getAsc.Param("ReturnValue"));
			if (!asc)
				return std::nullopt;

			uint8_t tag[kTagCap];
			if (!MakeTag(tag))
				return std::nullopt;
			PluginMap::ParamsBlock count = PluginMap::ParamsBlockOf(TagCountSize, { &TagCountTag, &TagCountResult });
			if (!count)
				return std::nullopt;
			std::memcpy(count.Param("GameplayTag"), tag, GameplayTagSize.resolvedSize);
			if (!Invoke(asc, kComponentClass, "GetGameplayTagCount", count.Data()))
				return std::nullopt;
			return *reinterpret_cast<const int32_t*>(count.Param("ReturnValue"));
		}

		// Adds or removes the heat tag on the pawn through the library's loose tag calls, replicating.
		bool SetHeatTag(SDK::AActor* pawn, bool present)
		{
			using namespace ServerUtilityFields;
			const PluginFieldSpec& size    = present ? AddTagsSize : RemoveTagsSize;
			const PluginFieldSpec& actor   = present ? AddTagsActor : RemoveTagsActor;
			const PluginFieldSpec& tags    = present ? AddTagsTags : RemoveTagsTags;
			const PluginFieldSpec& replic  = present ? AddTagsReplicate : RemoveTagsReplicate;
			SDK::UObject* library = LibraryObject_SEH();
			if (!pawn || !library || actor.resolvedSize != sizeof(void*) || replic.resolvedSize != 1
				|| TagContainerTags.resolvedSize != sizeof(SDK::TArray<SDK::FGameplayTag>)
				|| TagContainerTags.offset < 0
				|| static_cast<size_t>(TagContainerTags.offset) + TagContainerTags.resolvedSize > tags.resolvedSize
				|| GameplayTagSize.resolvedSize == 0 || GameplayTagSize.resolvedSize > kTagCap)
				return false;

			uint8_t tag[kTagCap];
			if (!MakeTag(tag))
				return false;
			PluginMap::ParamsBlock block = PluginMap::ParamsBlockOf(size, { &actor, &tags, &replic });
			if (!block)
				return false;
			*reinterpret_cast<SDK::AActor**>(block.Param("Actor")) = pawn;
			*block.Param("bShouldReplicate") = 1;
			uint8_t* container = block.Param("GameplayTags");
			std::memset(container, 0, tags.resolvedSize);
			// A one-element array on this stack frame: the call only reads it, and it is gone when the
			// call returns, so nothing of ours is left in the game's memory.
			SDK::TArray<SDK::FGameplayTag>* array = TagContainerTags.In(container);
			if (!array)
				return false;
			*array = SDK::TArray<SDK::FGameplayTag>(static_cast<SDK::FGameplayTag*>(static_cast<void*>(tag)), 1, 1);
			return Invoke(library, kLibraryClass, present ? "AddLooseGameplayTags" : "RemoveLooseGameplayTags", block.Data());
		}

		CheatLedger::Value ReadHeatSlot(SDK::UObject* obj, const CheatLedger::Entry&)
		{
			if (!obj || !OnGameThread())
				return {};
			const std::optional<int32_t> count = HeatCount(static_cast<SDK::AActor*>(obj));
			if (!count)
				return {};
			return CheatLedger::Value(std::in_place_type<bool>, *count > 0);
		}

		// Set semantics, so a re-apply or a restore never stacks tags: on adds the tag only when it
		// is not there, off removes it only when it is.
		bool WriteHeatSlot(SDK::UObject* obj, const CheatLedger::Entry&, const CheatLedger::Value& value)
		{
			const bool* want = std::get_if<bool>(&value);
			if (!obj || !want || !OnGameThread())
				return false;
			auto* pawn = static_cast<SDK::AActor*>(obj);
			const std::optional<int32_t> before = HeatCount(pawn);
			if (!before)
				return false;
			if (*want == (*before > 0))
				return true;
			if (!SetHeatTag(pawn, *want))
				return false;
			const std::optional<int32_t> after = HeatCount(pawn);
			return after && (*want ? *after > 0 : true);
		}

		// ---- OpMine::Env

		bool InWorld() { return World() != nullptr; }

		OpMine::State Current(const std::string& playerId)
		{
			OpMine::State state;
			CheatLedger::Ledger& ledger = CheatLedgerGame::Get();
			state.heat = ledger.Has(CheatLedger::Kind::Toggle, playerId, OpMine::kFeature, OpMine::kSlotHeat);
			if (ledger.Has(CheatLedger::Kind::Toggle, playerId, OpMine::kFeature, OpMine::kSlotBoost))
			{
				const auto it = g_boost.find(playerId);
				if (it != g_boost.end())
					state.boost = it->second;
			}
			return state;
		}

		// The player's current mining boost set, and their current character for the heat tag, for the
		// ledger to move a held toggle onto after a respawn.
		CheatLedger::Weak RebindBoost(SDK::UObject* character, const CheatLedger::Entry&)
		{
			PawnRef pawnRef;
			SetRef setRef;
			if (!OnGameThread() || !CaptureFrom_SEH(character, &pawnRef, &setRef) || !setRef.Get())
				return {};
			return CheatLedgerGame::MakeWeak(static_cast<SDK::UObject*>(setRef.Get()));
		}

		CheatLedger::Weak RebindHeat(SDK::UObject* character, const CheatLedger::Entry&)
		{
			PawnRef pawnRef;
			SetRef setRef;
			if (!OnGameThread() || !CaptureFrom_SEH(character, &pawnRef, &setRef) || !pawnRef.Get())
				return {};
			return CheatLedgerGame::MakeWeak(static_cast<SDK::UObject*>(pawnRef.Get()));
		}

		CheatLedger::Request BaseRequest(const CheatDispatch::Context& ctx, uint32_t reqId, const std::string& id, const char* slot)
		{
			PlayerList::Entry who;
			PlayerList::Find(id, who);

			CheatLedger::Request req;
			req.playerId    = id;
			req.playerName  = who.label;
			req.conn        = CheatLedgerGame::MakeWeak(static_cast<SDK::UObject*>(ctx.controller));
			req.feature     = OpMine::kFeature;
			req.slot        = slot;
			req.kind        = CheatLedger::Kind::Toggle;
			req.follow      = true;
			req.reqId       = reqId;
			req.ownerProven = InGameAdmin::IsProven(ctx.controller, id);
			return req;
		}

		// Overheating off or back, and/or the boost set or back, through the ledger. Both are the
		// caller's own Toggles. The pawn and the attribute set are fetched again here and held only as
		// weak references; a pawn that is gone leaves a release a no-op that succeeds. Both follow the
		// player: after a respawn the ledger tick moves them to the new pawn with a fresh prior, and the
		// read-back stays on throughout.
		bool Apply(const CheatDispatch::Context& ctx, uint32_t reqId, std::optional<bool> heat, std::optional<float> boost)
		{
			if (!OnGameThread() || !Available() || !World())
				return false;

			const std::string id = ctx.playerId ? *ctx.playerId : std::string();
			CheatLedger::Ledger& ledger = CheatLedgerGame::Get();

			// Putting back needs no pawn.
			const bool boostOn = boost && *boost > 1.0f;
			const bool heatOn  = heat && *heat;
			if (boost && !boostOn)
			{
				ledger.Release(CheatLedger::Kind::Toggle, id, OpMine::kFeature, OpMine::kSlotBoost);
				g_boost.erase(id);
			}
			if (heat && !heatOn)
				ledger.Release(CheatLedger::Kind::Toggle, id, OpMine::kFeature, OpMine::kSlotHeat);
			if (!boostOn && !heatOn)
				return true;

			PawnRef pawnRef;
			SetRef setRef;
			if (!Capture(ctx.controller, &pawnRef, &setRef))
				return false;
			SDK::ACrCharacterPlayerBase* pawn = pawnRef.Get();
			SDK::UCrMiningBoostAttributeSet* set = setRef.Get();
			if (!pawn || !set)
				return false;

			if (boostOn)
			{
				float live[4] = {};
				if (!ReadBoost_SEH(set, live))
					return false;

				const CheatLedger::Weak target = CheatLedgerGame::MakeWeak(static_cast<SDK::UObject*>(set));
				// The max is raised to the value when it is below it, from what the game held before
				// the first apply, so a changed multiplier does not stack on the earlier raise and a
				// release puts both back exactly.
				float base[4] = { live[0], live[1], live[2], live[3] };
				const CheatLedger::Entry* held = ledger.Find(CheatLedger::Kind::Toggle, id, OpMine::kFeature, OpMine::kSlotBoost);
				if (held && held->target == target)
				{
					if (const auto* prior = std::get_if<std::vector<float>>(&held->prior))
						if (prior->size() == 4)
							for (int i = 0; i < 4; ++i)
								base[i] = (*prior)[i];
				}
				const float value = *boost;

				CheatLedger::Request req = BaseRequest(ctx, reqId, id, OpMine::kSlotBoost);
				req.pin    = true;
				req.target = target;
				req.value  = CheatLedger::Value(std::in_place_type<std::vector<float>>, ComposeBoostValue(base, value));
				req.asked  = CheatLedger::Value(std::in_place_type<float>, value);
				if (!ledger.Apply(req))
					return false;
				g_boost[id] = value;
			}

			if (heatOn)
			{
				CheatLedger::Request req = BaseRequest(ctx, reqId, id, OpMine::kSlotHeat);
				req.pin    = false;
				req.target = CheatLedgerGame::MakeWeak(static_cast<SDK::UObject*>(pawn));
				req.value  = CheatLedger::Value(std::in_place_type<bool>, true);
				if (!ledger.Apply(req))
					return false;
			}
			return true;
		}
	}

	void Init()
	{
		CheatLedgerGame::RegisterAccessor(OpMine::kFeature, OpMine::kSlotHeat, { &ReadHeatSlot, &WriteHeatSlot, &RebindHeat });
		CheatLedgerGame::RegisterAccessor(OpMine::kFeature, OpMine::kSlotBoost, { &ReadBoostSlot, &WriteBoostSlot, &RebindBoost, &ComposeBoostSlot, &BoostRebases });

		OpMine::Env env;
		env.available = &Available;
		env.inWorld   = &InWorld;
		env.current   = &Current;
		env.apply     = &Apply;
		env.log       = [](const std::string& line) { LOG_INFO("[cheat.mine] %s", line.c_str()); };
		OpMine::SetEnv(std::move(env));
	}
}
