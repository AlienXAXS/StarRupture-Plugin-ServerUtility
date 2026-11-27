#pragma once

// The cheat.wave op, apart from the game: it checks the request, asks the game glue to start,
// pause, resume or cancel the wave, and builds the reply. The game side comes in through Env, so
// this runs in unit tests. A pause is a server-wide Override in the revert ledger (the glue makes
// the entry); start, resume and cancel are one-shot. Game thread.
#include "cheat_dispatch.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace OpWave
{
	// EEnviroWave: Heat is the default a start takes.
	constexpr int kTypeHeat = 1;
	constexpr int kTypeCold = 2;

	constexpr const char* kTextOff          = "Not available on this server's game build.";
	constexpr const char* kTextNoWorld      = "Not in a world right now.";
	constexpr const char* kTextStarted      = "Wave started.";
	constexpr const char* kTextPaused       = "Wave paused.";
	constexpr const char* kTextResumed      = "Wave resumed.";
	constexpr const char* kTextCancelled    = "Wave cancelled.";
	constexpr const char* kTextNoWave       = "No wave is running.";
	constexpr const char* kTextAlreadyWave  = "A wave is already running.";
	constexpr const char* kTextNotPaused    = "The wave is not paused.";
	constexpr const char* kTextAlreadyPause = "The wave is already paused.";
	constexpr const char* kTextFailed       = "Could not change the wave on this server.";

	struct State
	{
		bool inProgress = false;
		bool paused     = false;
	};

	struct Env
	{
		// Are the wave calls usable on this server right now (every key resolved).
		std::function<bool()> available;
		// The wave now; nothing when there is no world or the game could not be asked.
		std::function<std::optional<State>()> state;
		// Each returns false when the game call failed. reqId is for the audit line.
		std::function<bool(int type)> start;
		std::function<bool(const CheatDispatch::Context& ctx, uint32_t reqId)> pause;
		std::function<bool(const CheatDispatch::Context& ctx, uint32_t reqId)> resume;
		std::function<bool(const CheatDispatch::Context& ctx, uint32_t reqId)> cancel;
		std::function<void(const std::string&)> log;
	};

	void SetEnv(Env env);

	// The op's `available` hook for the dispatch table: false with no env or when the env says off.
	bool Available();

	CheatDispatch::Result Handle(const CheatDispatch::Context& ctx, const CheatWire::Request& req);
}
