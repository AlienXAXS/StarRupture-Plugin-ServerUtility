#include "op_enemy_game.h"
#include "op_enemy.h"
#include "cheat_ledger_game.h"
#include "host_rules/ingame_admin.h"
#include "host_rules/player_list.h"
#include "plugin_helpers.h"
#include "server_utility_fields.h"
#include "plugin_map.h"
#include "object_ref.h"
#include "SDK/MassCommon_structs.hpp"

#include <Windows.h>
#include <atomic>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace OpEnemyGame
{
	namespace
	{
		using WeakAssignFn = void(__fastcall*)(SDK::FWeakObjectPtr* self, void* const* objectPtrAddr);
		using GetHandleFn  = SDK::FMassEntityHandle*(__fastcall*)(SDK::UMassActorSubsystem* self, SDK::FMassEntityHandle* result, SDK::FWeakObjectPtr actorKey);
		using FragmentFn   = void*(__fastcall*)(void* entityManager, SDK::FMassEntityHandle entity, const SDK::UScriptStruct* fragmentType);

		WeakAssignFn g_weakAssign = nullptr;
		GetHandleFn  g_getHandle  = nullptr;
		FragmentFn   g_fragment   = nullptr;

		// How often the live enemies are looked up again among the game's objects.
		constexpr ULONGLONG kScanMs = 1000;

		// One live enemy actor. The actor and its agent are weak references: they are validated
		// before each use and never kept as plain pointers. The attack ranges are what the actor held
		// before Disable AI zeroed them; aiApplied says they are cached and the AI is off.
		struct Tracked
		{
			ObjectRef<SDK::AMassEnemyCharacterBase> actor;
			ObjectRef<SDK::UMassAgentComponent>     agent;
			float distance      = 0.0f;
			float cone          = 0.0f;
			float building      = 0.0f;
			bool  aiApplied     = false;
			bool  agentDisabled = false;
		};

		std::vector<Tracked>                     g_enemies;
		ObjectRef<SDK::UWorld>                   g_world;
		ObjectRef<SDK::UMassEntitySubsystem>     g_entitySubsystem;
		ObjectRef<SDK::UMassActorSubsystem>      g_actorSubsystem;
		ObjectRef<SDK::UScriptStruct>            g_healthStruct;
		ULONGLONG                                g_nextScan = 0;
		std::atomic<bool>                        g_ai{ false };
		std::atomic<bool>                        g_oneHit{ false };
		bool                                     g_tickRegistered = false;

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
			return ServerUtilityFields::CheatEnemy.On() && g_weakAssign && g_getHandle && g_fragment
				&& PluginMap::MassEntityManagerField.offset >= 0;
		}

		// ---- Game access. Each helper takes one live object fetched this tick and touches it inside the
		// call; the frames hold no C++ objects with destructors, so SEH is legal. A bad address ends in
		// false instead of taking the server down.

		bool ReadRanges_SEH(SDK::AMassEnemyCharacterBase* enemy, float out[3])
		{
			__try
			{
				const float* distance = ServerUtilityFields::EnemyAllowedAttackDistance.In(enemy);
				const float* cone     = ServerUtilityFields::EnemyAllowedAttackConeHalfAngle.In(enemy);
				const float* building = ServerUtilityFields::EnemyAllowedBuildingAttackDistance.In(enemy);
				if (!distance || !cone || !building)
					return false;
				out[0] = *distance;
				out[1] = *cone;
				out[2] = *building;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		bool WriteRanges_SEH(SDK::AMassEnemyCharacterBase* enemy, float distanceValue, float coneValue, float buildingValue)
		{
			__try
			{
				float* distance = ServerUtilityFields::EnemyAllowedAttackDistance.In(enemy);
				float* cone     = ServerUtilityFields::EnemyAllowedAttackConeHalfAngle.In(enemy);
				float* building = ServerUtilityFields::EnemyAllowedBuildingAttackDistance.In(enemy);
				if (!distance || !cone || !building)
					return false;
				*distance = distanceValue;
				*cone     = coneValue;
				*building = buildingValue;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		bool IsPooled_SEH(SDK::AMassEnemyCharacterBase* enemy)
		{
			__try
			{
				const bool* pooled = ServerUtilityFields::EnemyIsInPool.In(enemy);
				return !pooled || *pooled;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return true;
			}
		}

		// Stops or starts the AI controller's behaviour tree ticking. False when the enemy has no AI controller.
		bool BrainTick_SEH(SDK::AMassEnemyCharacterBase* enemy, bool enable)
		{
			__try
			{
				SDK::AController** controller = ServerUtilityFields::PawnController.In(enemy);
				if (!controller || !*controller || !(*controller)->IsA(SDK::AAIController::StaticClass()))
					return false;
				SDK::UBrainComponent** brain = ServerUtilityFields::AIControllerBrainComponent.In(*controller);
				if (!brain || !*brain)
					return false;
				(*brain)->SetComponentTickEnabled(enable);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		SDK::UMassAgentComponent* FindAgent_SEH(SDK::AMassEnemyCharacterBase* enemy)
		{
			__try
			{
				SDK::UClass* agentClass = SDK::UMassAgentComponent::StaticClass();
				return agentClass ? static_cast<SDK::UMassAgentComponent*>(enemy->GetComponentByClass(agentClass)) : nullptr;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return nullptr;
			}
		}

		bool AgentEnable_SEH(SDK::UMassAgentComponent* agent, bool enable)
		{
			__try
			{
				if (enable)
					agent->Enable();
				else
					agent->Disable();
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// Looks the two Mass subsystems of the world up. Either is null when the world has none.
		void FindSubsystems_SEH(SDK::UWorld* world, SDK::UMassEntitySubsystem** entity, SDK::UMassActorSubsystem** actor)
		{
			*entity = nullptr;
			*actor  = nullptr;
			__try
			{
				*entity = static_cast<SDK::UMassEntitySubsystem*>(
					SDK::USubsystemBlueprintLibrary::GetWorldSubsystem(world, SDK::UMassEntitySubsystem::StaticClass()));
				*actor = static_cast<SDK::UMassActorSubsystem*>(
					SDK::USubsystemBlueprintLibrary::GetWorldSubsystem(world, SDK::UMassActorSubsystem::StaticClass()));
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				*entity = nullptr;
				*actor  = nullptr;
			}
		}

		SDK::UScriptStruct* FindHealthStruct_SEH(const std::string& name)
		{
			__try
			{
				return static_cast<SDK::UScriptStruct*>(
					SDK::UObject::FindObjectFastImpl(name, SDK::EClassCastFlags::ScriptStruct));
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return nullptr;
			}
		}

		// Lowers the enemy's health to 1 through its Mass health fragment (never raises it). The entity
		// handle comes from the actor subsystem and the fragment address from the entity manager, both
		// fetched fresh here: the fragment's memory is freed when the entity dies, so the address is
		// never kept past this call. A destroyed entity has handle index 0 and is skipped.
		bool Pin_SEH(SDK::AMassEnemyCharacterBase* enemy, SDK::UMassActorSubsystem* actorSubsystem, void* manager, SDK::UScriptStruct* healthStruct)
		{
			__try
			{
				SDK::FWeakObjectPtr key{};
				void* actorPtr = enemy;
				g_weakAssign(&key, &actorPtr);

				SDK::FMassEntityHandle handle{};
				g_getHandle(actorSubsystem, &handle, key);
				if (handle.Index == 0)
					return false;

				void* fragment = g_fragment(manager, handle, healthStruct);
				float* hp = fragment ? ServerUtilityFields::EnemyHealthHP.In(fragment) : nullptr;
				if (!hp)
					return false;
				if (*hp > 1.0f)
					*hp = 1.0f;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// ---- Disable AI

		// Puts one enemy back: its attack ranges, its behaviour tree and its Mass agent.
		void RestoreOne(Tracked& t, SDK::AMassEnemyCharacterBase* enemy)
		{
			if (enemy)
			{
				WriteRanges_SEH(enemy, t.distance, t.cone, t.building);
				BrainTick_SEH(enemy, true);
				SDK::UMassAgentComponent* agent = t.agent.Get();
				if (agent && t.agentDisabled)
					AgentEnable_SEH(agent, true);
			}
			t.aiApplied     = false;
			t.agentDisabled = false;
		}

		void RestoreAi()
		{
			if (!OnGameThread())
				return;
			for (Tracked& t : g_enemies)
			{
				if (t.aiApplied)
					RestoreOne(t, t.actor.Get());
			}
		}

		// First time Disable AI reaches this enemy: remember its ranges, stop its Mass agent once (the
		// call does real work, so it is not repeated) and its behaviour tree, and zero the ranges.
		void ApplyAi(Tracked& t, SDK::AMassEnemyCharacterBase* enemy)
		{
			float ranges[3] = {};
			if (!ReadRanges_SEH(enemy, ranges))
				return;
			t.distance = ranges[0];
			t.cone     = ranges[1];
			t.building = ranges[2];

			SDK::UMassAgentComponent* agent = t.agent.Get();
			if (!agent)
			{
				agent = FindAgent_SEH(enemy);
				t.agent.Set(agent);
			}
			if (agent)
				t.agentDisabled = AgentEnable_SEH(agent, false);
			BrainTick_SEH(enemy, false);
			t.aiApplied = WriteRanges_SEH(enemy, 0.0f, 0.0f, 0.0f);
		}

		// ---- Tracking

		// Looks the live, non-pooled enemy actors up among the game's objects, keeping the state of the
		// ones already tracked. An enemy that left the list while its AI was off (it went back into the
		// pool) is put back first, since the pool hands the same actor out again. Game thread.
		void Rescan(SDK::UWorld* world, bool ai, bool oneHit)
		{
			SDK::UClass* enemyClass = SDK::AMassEnemyCharacterBase::StaticClass();
			auto& gobjects = SDK::UObject::GObjects;
			if (!enemyClass || !gobjects)
				return;

			std::unordered_map<SDK::AMassEnemyCharacterBase*, size_t> previous;
			for (size_t i = 0; i < g_enemies.size(); ++i)
			{
				if (SDK::AMassEnemyCharacterBase* actor = g_enemies[i].actor.Get())
					previous.emplace(actor, i);
			}
			std::vector<char> kept(g_enemies.size(), 0);

			std::vector<Tracked> found;
			const int32_t count = gobjects->Num();
			for (int32_t i = 0; i < count; ++i)
			{
				SDK::UObject* obj = gobjects->GetByIndex(i);
				if (!obj || !obj->Class || obj->IsDefaultObject() || !obj->IsA(enemyClass))
					continue;
				auto* enemy = static_cast<SDK::AMassEnemyCharacterBase*>(obj);
				if (IsPooled_SEH(enemy))
					continue;

				auto it = previous.find(enemy);
				if (it != previous.end())
				{
					kept[it->second] = 1;
					found.push_back(g_enemies[it->second]);
				}
				else
				{
					Tracked t;
					t.actor.Set(enemy);
					found.push_back(t);
				}
			}

			for (size_t i = 0; i < g_enemies.size(); ++i)
			{
				if (!kept[i] && g_enemies[i].aiApplied)
					RestoreOne(g_enemies[i], g_enemies[i].actor.Get());
			}
			g_enemies = std::move(found);

			if (ai)
			{
				// The controller can be replaced while the enemy lives: stop its tree again.
				for (Tracked& t : g_enemies)
				{
					if (t.aiApplied)
					{
						if (SDK::AMassEnemyCharacterBase* enemy = t.actor.Get())
							BrainTick_SEH(enemy, false);
					}
				}
			}

			if (oneHit)
			{
				if (!g_entitySubsystem.Get() || !g_actorSubsystem.Get())
				{
					SDK::UMassEntitySubsystem* entity = nullptr;
					SDK::UMassActorSubsystem*  actor  = nullptr;
					FindSubsystems_SEH(world, &entity, &actor);
					g_entitySubsystem.Set(entity);
					g_actorSubsystem.Set(actor);
				}
				if (!g_healthStruct.Get())
					g_healthStruct.Set(FindHealthStruct_SEH(std::string("MassEnemyHealthFragment")));
			}
		}

		void OnTick(float)
		{
			SDK::UWorld* world = World();
			if (!world)
				return;

			// A new world has new actors and subsystems: nothing tracked carries over, and the settings
			// the ledger holds apply to the enemies found there.
			if (!(g_world == ObjectRef<SDK::UWorld>(world)))
			{
				g_world.Set(world);
				g_enemies.clear();
				g_entitySubsystem.Reset();
				g_actorSubsystem.Reset();
				g_nextScan = 0;
			}

			const bool ai     = g_ai.load();
			const bool oneHit = g_oneHit.load();
			if (!ai)
				RestoreAi();
			if (!ai && !oneHit)
			{
				g_enemies.clear();
				return;
			}
			if (!Available())
				return;

			const ULONGLONG now = GetTickCount64();
			if (now >= g_nextScan)
			{
				g_nextScan = now + kScanMs;
				Rescan(world, ai, oneHit);
			}

			SDK::UMassActorSubsystem* actorSubsystem = oneHit ? g_actorSubsystem.Get() : nullptr;
			void* manager = oneHit ? PluginMap::MassEntityManagerOf(g_entitySubsystem.Get()) : nullptr;
			SDK::UScriptStruct* healthStruct = oneHit ? g_healthStruct.Get() : nullptr;
			const bool pin = actorSubsystem && manager && healthStruct;

			for (Tracked& t : g_enemies)
			{
				SDK::AMassEnemyCharacterBase* enemy = t.actor.Get();
				if (!enemy)
					continue;
				if (ai)
				{
					if (t.aiApplied)
						WriteRanges_SEH(enemy, 0.0f, 0.0f, 0.0f);
					else
						ApplyAi(t, enemy);
				}
				if (pin)
					Pin_SEH(enemy, actorSubsystem, manager, healthStruct);
			}
		}

		// ---- The ledger's way to read and write the two settings: the flag itself. Turning Disable AI
		// off puts the enemies back at once on the game thread; the tick does it otherwise.

		CheatLedger::Value ReadAi(SDK::UObject*, const CheatLedger::Entry&)
		{
			return CheatLedger::Value(std::in_place_type<bool>, g_ai.load());
		}

		bool WriteAi(SDK::UObject*, const CheatLedger::Entry&, const CheatLedger::Value& v)
		{
			const bool* on = std::get_if<bool>(&v);
			if (!on)
				return false;
			g_ai = *on;
			if (!*on)
				RestoreAi();
			return true;
		}

		CheatLedger::Value ReadOneHit(SDK::UObject*, const CheatLedger::Entry&)
		{
			return CheatLedger::Value(std::in_place_type<bool>, g_oneHit.load());
		}

		bool WriteOneHit(SDK::UObject*, const CheatLedger::Entry&, const CheatLedger::Value& v)
		{
			const bool* on = std::get_if<bool>(&v);
			if (!on)
				return false;
			g_oneHit = *on;
			return true;
		}

		// ---- OpEnemy::Env

		bool InWorld() { return World() != nullptr; }

		bool AiDisabled()
		{
			return CheatLedgerGame::Get().Has(CheatLedger::Kind::Override, std::string(), OpEnemy::kFeature, OpEnemy::kSlotAi);
		}

		bool OneHit()
		{
			return CheatLedgerGame::Get().Has(CheatLedger::Kind::Override, std::string(), OpEnemy::kFeature, OpEnemy::kSlotOneHit);
		}

		int TrackedCount() { return static_cast<int>(g_enemies.size()); }

		// Turns one setting on through the ledger (a server-wide Override that holds the flag), or off by
		// releasing it, which writes the flag back.
		bool Set(const CheatDispatch::Context& ctx, uint32_t reqId, const char* slot, bool on)
		{
			if (!on)
			{
				CheatLedgerGame::Get().Release(CheatLedger::Kind::Override, std::string(), OpEnemy::kFeature, slot);
				return true;
			}

			const std::string id = ctx.playerId ? *ctx.playerId : std::string();
			PlayerList::Entry who;
			PlayerList::Find(id, who);

			CheatLedger::Request req;
			req.playerId    = id;
			req.playerName  = who.label;
			req.conn        = CheatLedgerGame::MakeWeak(static_cast<SDK::UObject*>(ctx.controller));
			req.feature     = OpEnemy::kFeature;
			req.slot        = slot;
			req.kind        = CheatLedger::Kind::Override;
			req.value       = CheatLedger::Value(std::in_place_type<bool>, true);
			req.reqId       = reqId;
			req.ownerProven = InGameAdmin::IsProven(ctx.controller, id);
			return CheatLedgerGame::Get().Apply(req);
		}

		bool SetAi(const CheatDispatch::Context& ctx, uint32_t reqId, bool on)
		{
			return Set(ctx, reqId, OpEnemy::kSlotAi, on);
		}

		bool SetOneHit(const CheatDispatch::Context& ctx, uint32_t reqId, bool on)
		{
			return Set(ctx, reqId, OpEnemy::kSlotOneHit, on);
		}
	}

	void Resolve(IPluginSelf* self, IPluginHookScanner* scanner)
	{
		auto fn = [&](const char* key) { return PluginMap::Function(scanner, self, key, nullptr, PLUGIN_SCAN_FUNCTION_START, PLUGIN_SCAN_FLAG_OPTIONAL); };
		g_weakAssign = reinterpret_cast<WeakAssignFn>(fn(ServerUtilityFields::WeakObjectPtrAssign));
		g_getHandle  = reinterpret_cast<GetHandleFn>(fn(ServerUtilityFields::GetEntityHandleFromActor));
		g_fragment   = reinterpret_cast<FragmentFn>(fn(ServerUtilityFields::InternalGetFragmentDataPtr));
	}

	void Init()
	{
		CheatLedgerGame::RegisterAccessor(OpEnemy::kFeature, OpEnemy::kSlotAi, { &ReadAi, &WriteAi });
		CheatLedgerGame::RegisterAccessor(OpEnemy::kFeature, OpEnemy::kSlotOneHit, { &ReadOneHit, &WriteOneHit });

		OpEnemy::Env env;
		env.available  = &Available;
		env.inWorld    = &InWorld;
		env.aiDisabled = &AiDisabled;
		env.oneHit     = &OneHit;
		env.tracked    = &TrackedCount;
		env.setAi      = &SetAi;
		env.setOneHit  = &SetOneHit;
		env.log        = [](const std::string& line) { LOG_INFO("[cheat.enemy] %s", line.c_str()); };
		OpEnemy::SetEnv(std::move(env));

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
		if (hooks && hooks->Engine && g_tickRegistered)
			hooks->Engine->UnregisterOnTick(OnTick);
		g_tickRegistered = false;

		// Shutdown also runs at process exit: the enemies are touched on the game thread only.
		g_ai = false;
		g_oneHit = false;
		RestoreAi();
	}
}
