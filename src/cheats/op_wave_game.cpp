#include "op_wave_game.h"
#include "op_wave.h"
#include "cheat_ledger_game.h"
#include "host_rules/ingame_admin.h"
#include "host_rules/player_list.h"
#include "admin/endpoints/wave_common.h"
#include "plugin_helpers.h"
#include "game_access.h"
#include "plugin_map.h"
#include "server_utility_fields.h"

#include <Windows.h>
#include <cstdint>
#include <optional>
#include <string>

namespace OpWaveGame
{
	namespace
	{
		using GameAccess::OnGameThread;

		constexpr const char* kFeature = "rupture-wave";
		constexpr const char* kSlot    = "pause";
		constexpr const char* kClass   = "CrEnviroWaveSubsystem";

		// Looks the wave subsystem up and calls one of its functions by name; params is the call's
		// parameter block, or null for a function without parameters. The subsystem pointer lives
		// only inside this call. No C++ objects with destructors in this frame: SEH is legal.
		bool Invoke_SEH(IPluginHooks* hooks, const char* function, void* params)
		{
			__try
			{
				SDK::UCrEnviroWaveSubsystem* subsystem = GetWaveSubsystem();
				if (!subsystem) return false;
				void* resolved = hooks->ObjectWalker->ResolveUFunction(kClass, function);
				return resolved && hooks->ObjectWalker->InvokeResolvedUFunction(subsystem, resolved, params);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		bool Invoke(const char* function, void* params)
		{
			IPluginHooks* hooks = GetHooks();
			if (!hooks || !hooks->ObjectWalker || !hooks->ObjectWalker->ResolveUFunction
				|| !hooks->ObjectWalker->InvokeResolvedUFunction || !OnGameThread())
				return false;
			return Invoke_SEH(hooks, function, params);
		}

		// A bool-returning call without parameters (IsWaveInProgress, IsWavePaused).
		std::optional<bool> ReadBool(const char* function, const PluginFieldSpec& size, const PluginFieldSpec& result)
		{
			PluginMap::ParamsBlock block = PluginMap::ParamsBlockOf(size, { &result });
			if (!block || result.resolvedSize != 1 || !Invoke(function, block.Data()))
				return std::nullopt;
			const uint8_t* value = block.Param("ReturnValue");
			if (!value) return std::nullopt;
			return *value != 0;
		}

		std::optional<bool> InProgress()
		{
			return ReadBool("IsWaveInProgress", ServerUtilityFields::IsWaveInProgressSize, ServerUtilityFields::IsWaveInProgressResult);
		}

		std::optional<bool> Paused()
		{
			return ReadBool("IsWavePaused", ServerUtilityFields::IsWavePausedSize, ServerUtilityFields::IsWavePausedResult);
		}

		bool Available()
		{
			return ServerUtilityFields::CheatWave.On();
		}

		std::optional<OpWave::State> State()
		{
			const std::optional<bool> running = InProgress();
			const std::optional<bool> paused  = Paused();
			if (!running || !paused) return std::nullopt;
			return OpWave::State{ *running, *paused };
		}

		bool Start(int type)
		{
			PluginMap::ParamsBlock block = PluginMap::ParamsBlockOf(ServerUtilityFields::StartWaveSize, { &ServerUtilityFields::StartWaveType });
			uint8_t* wave = block.Param("WaveType");
			if (!block || !wave || ServerUtilityFields::StartWaveType.resolvedSize != 1) return false;
			*wave = static_cast<uint8_t>(type);
			return Invoke("StartWave", block.Data());
		}

		// The ledger's way to read and write the pause: the live IsWavePaused, and Pause or Resume.
		CheatLedger::Value ReadPause(SDK::UObject*, const CheatLedger::Entry&)
		{
			const std::optional<bool> paused = Paused();
			return paused ? CheatLedger::Value(std::in_place_type<bool>, *paused) : CheatLedger::Value();
		}

		bool WritePause(SDK::UObject*, const CheatLedger::Entry&, const CheatLedger::Value& v)
		{
			const bool* pause = std::get_if<bool>(&v);
			return pause && Invoke(*pause ? "PauseCurrentWave" : "ResumeCurrentWave", nullptr);
		}

		bool Pause(const CheatDispatch::Context& ctx, uint32_t reqId)
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
			req.value       = CheatLedger::Value(std::in_place_type<bool>, true);
			req.reqId       = reqId;
			req.ownerProven = InGameAdmin::IsProven(ctx.controller, id);
			return CheatLedgerGame::Get().Apply(req);
		}

		// Puts the pause back through the ledger, which resumes the wave while it is still paused
		// by us; a pause that came from somewhere else (RCON, the panel) is resumed directly.
		bool Resume(const CheatDispatch::Context&, uint32_t)
		{
			CheatLedgerGame::Get().Release(CheatLedger::Kind::Override, std::string(), kFeature, kSlot);
			const std::optional<bool> paused = Paused();
			if (!paused) return false;
			return !*paused || Invoke("ResumeCurrentWave", nullptr);
		}

		bool Cancel(const CheatDispatch::Context& ctx, uint32_t reqId)
		{
			Resume(ctx, reqId);
			return Invoke("CancelCurrentWave", nullptr);
		}
	}

	void Init()
	{
		CheatLedgerGame::RegisterAccessor(kFeature, kSlot, { &ReadPause, &WritePause });

		OpWave::Env env;
		env.available = &Available;
		env.state     = &State;
		env.start     = &Start;
		env.pause     = &Pause;
		env.resume    = &Resume;
		env.cancel    = &Cancel;
		env.log       = [](const std::string& line) { LOG_INFO("[cheat.wave] %s", line.c_str()); };
		OpWave::SetEnv(std::move(env));
	}
}
