#include "op_move_game.h"
#include "op_move.h"
#include "cheat_ledger_game.h"
#include "host_rules/ingame_admin.h"
#include "host_rules/player_list.h"
#include "plugin_helpers.h"
#include "server_utility_fields.h"
#include "plugin_map.h"
#include "object_ref.h"

#include <Windows.h>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace OpMoveGame
{
	namespace
	{
		constexpr const char* kMoveClass      = "CharacterMovementComponent";

		// UCharacterMovementComponent's own default, in cm/s, for a component that ships with no fly
		// speed of its own. Only the base a multiplier is applied to; the prior put back is the real value.
		constexpr float kFallbackMaxFlySpeed = 600.0f;
		// Flying braking ships at 0 on most components, which means coasting; BetterCheats holds it at
		// four times the top speed so the player stops when the keys are let go.
		constexpr float kBrakingFactor = 4.0f;

		using MoveRef = ObjectRef<SDK::UCharacterMovementComponent>;

		// The multiplier each player asked for. Only the value the player sees in `cheat.state`: the
		// ledger holds the entry and the values, and this is never read while the ledger has no entry.
		// Game thread.
		std::map<std::string, float> g_speed;

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
			return ServerUtilityFields::CheatMove.On();
		}

		// The caller's player character's movement component, as a weak reference. False when the
		// controller has no player character or the character has no component. No C++ objects needing
		// unwinding in this frame: SEH is legal.
		bool Capture_SEH(void* controller, MoveRef* out)
		{
			__try
			{
				auto* pc = static_cast<SDK::APlayerController*>(controller);
				if (!pc)
					return false;
				SDK::APawn* pawn = pc->K2_GetPawn();
				if (!pawn || !pawn->IsA(SDK::ACrCharacterPlayerBase::StaticClass()))
					return false;
				SDK::UCrCharacterMovementComponent** at = ServerUtilityFields::CharacterMovement.In(pawn);
				if (!at || !*at)
					return false;
				out->Set(*at);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// ---- Calls through the loader's ProcessEvent path

		// Calls one reflected function. params is the call's parameter block. No C++ objects needing
		// unwinding in this frame: SEH is legal.
		bool Invoke_SEH(IPluginHooks* hooks, SDK::UObject* object, const char* cls, const char* function, void* params)
		{
			__try
			{
				void* resolved = hooks->ObjectWalker->ResolveUFunction(cls, function);
				return resolved && hooks->ObjectWalker->InvokeResolvedUFunction(object, resolved, params);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		bool Invoke(SDK::UObject* object, const char* cls, const char* function, void* params)
		{
			IPluginHooks* hooks = GetHooks();
			if (!object || !hooks || !hooks->ObjectWalker || !hooks->ObjectWalker->ResolveUFunction
				|| !hooks->ObjectWalker->InvokeResolvedUFunction || !OnGameThread())
				return false;
			return Invoke_SEH(hooks, object, cls, function, params);
		}

		bool SetMode(SDK::UCharacterMovementComponent* comp, uint8_t mode)
		{
			using namespace ServerUtilityFields;
			if (!comp || SetModeMode.resolvedSize != 1 || SetModeCustom.resolvedSize != 1)
				return false;
			PluginMap::ParamsBlock block = PluginMap::ParamsBlockOf(SetModeSize, { &SetModeMode, &SetModeCustom });
			if (!block)
				return false;
			*block.Param("NewMovementMode") = mode;
			*block.Param("NewCustomMode")   = 0;
			return Invoke(comp, kMoveClass, "SetMovementMode", block.Data());
		}

		// ---- Members

		// The cheat-flying flag and the movement mode.
		bool ReadFlyMembers_SEH(SDK::UCharacterMovementComponent* comp, bool* flag, uint8_t* mode)
		{
			__try
			{
				const uint8_t* modeAt = reinterpret_cast<const uint8_t*>(ServerUtilityFields::MovementMode.In(comp));
				if (!modeAt || ServerUtilityFields::MovementCheatFlying.offset < 0)
					return false;
				*flag = ServerUtilityFields::MovementCheatFlying.Get(comp);
				*mode = *modeAt;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		bool WriteFlag_SEH(SDK::UCharacterMovementComponent* comp, bool flag)
		{
			__try
			{
				return ServerUtilityFields::MovementCheatFlying.Set(comp, flag);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// The two flying values: top speed and braking.
		bool ReadSpeed_SEH(SDK::UCharacterMovementComponent* comp, float* maxSpeed, float* braking)
		{
			__try
			{
				const float* speed = ServerUtilityFields::MovementMaxFlySpeed.In(comp);
				const float* brake = ServerUtilityFields::MovementBrakingFlying.In(comp);
				if (!speed || !brake)
					return false;
				*maxSpeed = *speed;
				*braking  = *brake;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		bool WriteSpeed_SEH(SDK::UCharacterMovementComponent* comp, float maxSpeed, float braking)
		{
			__try
			{
				float* speed = ServerUtilityFields::MovementMaxFlySpeed.In(comp);
				float* brake = ServerUtilityFields::MovementBrakingFlying.In(comp);
				if (!speed || !brake)
					return false;
				*speed = maxSpeed;
				*brake = braking;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// ---- Ledger accessors. obj is the entry's movement component, validated by the ledger glue.

		// The fly slot's value is {cheat-flying flag, movement mode}, so a revert puts both back
		// exactly. Collision is never touched.
		CheatLedger::Value ReadFly(SDK::UObject* obj, const CheatLedger::Entry&)
		{
			if (!obj || !OnGameThread())
				return {};
			auto* comp = static_cast<SDK::UCharacterMovementComponent*>(obj);
			bool flag = false;
			uint8_t mode = 0;
			if (!ReadFlyMembers_SEH(comp, &flag, &mode))
				return {};
			return CheatLedger::Value(std::vector<float>{ flag ? 1.0f : 0.0f, static_cast<float>(mode) });
		}

		// Set semantics: a call is made only for a part that differs, so the pin on the tick costs
		// nothing while the game holds the values.
		bool WriteFly(SDK::UObject* obj, const CheatLedger::Entry&, const CheatLedger::Value& value)
		{
			const auto* v = std::get_if<std::vector<float>>(&value);
			if (!obj || !v || v->size() != 2 || !OnGameThread())
				return false;
			auto* comp = static_cast<SDK::UCharacterMovementComponent*>(obj);
			const bool    wantFlag = (*v)[0] != 0.0f;
			const uint8_t wantMode = static_cast<uint8_t>((*v)[1]);

			bool flag = false;
			uint8_t mode = 0;
			if (!ReadFlyMembers_SEH(comp, &flag, &mode))
				return false;

			if (flag != wantFlag && !WriteFlag_SEH(comp, wantFlag))
				return false;
			if (mode != wantMode && !SetMode(comp, wantMode))
				return false;
			return true;
		}

		// The speed slot's value is {max fly speed, flying braking}.
		CheatLedger::Value ReadSpeed(SDK::UObject* obj, const CheatLedger::Entry&)
		{
			if (!obj || !OnGameThread())
				return {};
			float maxSpeed = 0.0f, braking = 0.0f;
			if (!ReadSpeed_SEH(static_cast<SDK::UCharacterMovementComponent*>(obj), &maxSpeed, &braking))
				return {};
			return CheatLedger::Value(std::vector<float>{ maxSpeed, braking });
		}

		bool WriteSpeed(SDK::UObject* obj, const CheatLedger::Entry&, const CheatLedger::Value& value)
		{
			const auto* v = std::get_if<std::vector<float>>(&value);
			if (!obj || !v || v->size() != 2 || !OnGameThread())
				return false;
			return WriteSpeed_SEH(static_cast<SDK::UCharacterMovementComponent*>(obj), (*v)[0], (*v)[1]);
		}

		// ---- OpMove::Env

		bool InWorld() { return World() != nullptr; }

		bool HasCharacter(const CheatDispatch::Context& ctx)
		{
			if (!OnGameThread() || !Available())
				return false;
			MoveRef ref;
			return Capture_SEH(ctx.controller, &ref) && ref.Get() != nullptr;
		}

		bool Flying(const std::string& playerId)
		{
			return CheatLedgerGame::Get().Has(CheatLedger::Kind::Toggle, playerId, OpMove::kFeature, OpMove::kSlotFly);
		}

		float Speed(const std::string& playerId)
		{
			if (!CheatLedgerGame::Get().Has(CheatLedger::Kind::Toggle, playerId, OpMove::kFeature, OpMove::kSlotSpeed))
				return 1.0f;
			const auto it = g_speed.find(playerId);
			return it != g_speed.end() ? it->second : 1.0f;
		}

		CheatLedger::Request BaseRequest(const CheatDispatch::Context& ctx, uint32_t reqId, const std::string& id, const char* slot)
		{
			PlayerList::Entry who;
			PlayerList::Find(id, who);

			CheatLedger::Request req;
			req.playerId    = id;
			req.playerName  = who.label;
			req.conn        = CheatLedgerGame::MakeWeak(static_cast<SDK::UObject*>(ctx.controller));
			req.feature     = OpMove::kFeature;
			req.slot        = slot;
			req.kind        = CheatLedger::Kind::Toggle;
			req.pin         = true;
			req.reqId       = reqId;
			req.ownerProven = InGameAdmin::IsProven(ctx.controller, id);
			return req;
		}

		// Flying on or back through the ledger. Both are the caller's own pinned Toggles on the
		// movement component. Off needs no pawn: a character that is gone has no live target, and the
		// release then writes nothing, so a respawned pawn never gets the old values. On fetches the
		// component again and holds it as a weak reference; a new pawn is a new target, so the ledger
		// takes a fresh prior from it.
		bool SetFly(const CheatDispatch::Context& ctx, uint32_t reqId, bool on)
		{
			if (!OnGameThread() || !Available() || !World())
				return false;
			const std::string id = ctx.playerId ? *ctx.playerId : std::string();
			CheatLedger::Ledger& ledger = CheatLedgerGame::Get();
			if (!on)
			{
				ledger.Release(CheatLedger::Kind::Toggle, id, OpMove::kFeature, OpMove::kSlotFly);
				return true;
			}

			MoveRef ref;
			if (!Capture_SEH(ctx.controller, &ref))
				return false;
			SDK::UCharacterMovementComponent* comp = ref.Get();
			if (!comp)
				return false;

			constexpr uint8_t kFlying = static_cast<uint8_t>(SDK::EMovementMode::MOVE_Flying);
			CheatLedger::Request req = BaseRequest(ctx, reqId, id, OpMove::kSlotFly);
			req.target = CheatLedgerGame::MakeWeak(static_cast<SDK::UObject*>(comp));
			req.value  = CheatLedger::Value(std::vector<float>{ 1.0f, static_cast<float>(kFlying) });
			return ledger.Apply(req);
		}

		// The speed multiplier on or back through the ledger: the component's fly speed is the value
		// the game held before the first apply times the multiplier, and the flying braking four times
		// that. A new multiplier is taken from the same held value, so it does not stack. Like
		// BetterCheats' client speed, this scales the flying speed only, not walking.
		bool SetSpeed(const CheatDispatch::Context& ctx, uint32_t reqId, bool on, float mult)
		{
			if (!OnGameThread() || !Available() || !World())
				return false;
			const std::string id = ctx.playerId ? *ctx.playerId : std::string();
			CheatLedger::Ledger& ledger = CheatLedgerGame::Get();
			if (!on)
			{
				ledger.Release(CheatLedger::Kind::Toggle, id, OpMove::kFeature, OpMove::kSlotSpeed);
				g_speed.erase(id);
				return true;
			}

			MoveRef ref;
			if (!Capture_SEH(ctx.controller, &ref))
				return false;
			SDK::UCharacterMovementComponent* comp = ref.Get();
			if (!comp)
				return false;

			const CheatLedger::Weak target = CheatLedgerGame::MakeWeak(static_cast<SDK::UObject*>(comp));
			float base = 0.0f, braking = 0.0f;
			const CheatLedger::Entry* held = ledger.Find(CheatLedger::Kind::Toggle, id, OpMove::kFeature, OpMove::kSlotSpeed);
			bool fromHeld = false;
			if (held && held->target == target)
			{
				if (const auto* prior = std::get_if<std::vector<float>>(&held->prior))
				{
					if (prior->size() == 2)
					{
						base = (*prior)[0];
						fromHeld = true;
					}
				}
			}
			if (!fromHeld && !ReadSpeed_SEH(comp, &base, &braking))
				return false;
			if (base <= 0.0f)
				base = kFallbackMaxFlySpeed;

			const float top = base * mult;
			CheatLedger::Request req = BaseRequest(ctx, reqId, id, OpMove::kSlotSpeed);
			req.target = target;
			req.value  = CheatLedger::Value(std::vector<float>{ top, top * kBrakingFactor });
			if (!ledger.Apply(req))
				return false;
			g_speed[id] = mult;
			return true;
		}
	}

	void Init()
	{
		CheatLedgerGame::RegisterAccessor(OpMove::kFeature, OpMove::kSlotFly, { &ReadFly, &WriteFly });
		CheatLedgerGame::RegisterAccessor(OpMove::kFeature, OpMove::kSlotSpeed, { &ReadSpeed, &WriteSpeed });

		OpMove::Env env;
		env.available    = &Available;
		env.inWorld      = &InWorld;
		env.hasCharacter = &HasCharacter;
		env.flying       = &Flying;
		env.speed        = &Speed;
		env.setFly       = &SetFly;
		env.setSpeed     = &SetSpeed;
		env.log          = [](const std::string& line) { LOG_INFO("[cheat.move] %s", line.c_str()); };
		OpMove::SetEnv(std::move(env));
	}
}
