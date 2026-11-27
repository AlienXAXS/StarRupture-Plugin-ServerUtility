#pragma once

// The cheat.enemy op, apart from the game: it checks the request, asks the game glue to turn
// Disable AI and One-Hit Kill on or off, and builds the reply. Both settings are server-wide
// Overrides in the revert ledger (the glue makes the entries): feature kFeature, slots kSlotAi and
// kSlotOneHit. They stay until changed, a deny or server stop. Disable AI is put back then;
// One-Hit Kill only stops pinning, it can't restore health already cut. The game side comes in
// through Env, so this runs in unit tests. Game thread.
#include "cheat_dispatch.h"

#include <cstdint>
#include <functional>
#include <string>

namespace OpEnemy
{
	// The ledger feature (the rules feature id) and the slots of the two settings.
	constexpr const char* kFeature    = "enemies";
	constexpr const char* kSlotAi     = "ai";
	constexpr const char* kSlotOneHit = "onehit";

	constexpr const char* kTextOff       = "Not available on this server's game build.";
	constexpr const char* kTextNoWorld   = "Not in a world right now.";
	constexpr const char* kTextPickOne   = "Pick at least one setting.";
	constexpr const char* kTextFailed    = "Could not change enemies on this server.";
	constexpr const char* kTextAiOff     = "Enemy AI turned off.";
	constexpr const char* kTextAiOn      = "Enemy AI turned on.";
	constexpr const char* kTextOneHitOn  = "One-hit kill on.";
	constexpr const char* kTextOneHitOff = "One-hit kill off.";

	struct Env
	{
		// Are the enemy members usable on this server right now (every key resolved).
		std::function<bool()> available;
		// Is a world loaded.
		std::function<bool()> inWorld;
		// Is Disable AI / One-Hit Kill in force (an Override in the ledger).
		std::function<bool()> aiDisabled;
		std::function<bool()> oneHit;
		// How many enemies the game side tracks now.
		std::function<int()> tracked;
		// Turn Disable AI / One-Hit Kill on or off through the ledger. Off puts the game back (AI) or
		// stops the pinning (One-Hit Kill). False when it failed. reqId is for the audit line.
		std::function<bool(const CheatDispatch::Context& ctx, uint32_t reqId, bool on)> setAi;
		std::function<bool(const CheatDispatch::Context& ctx, uint32_t reqId, bool on)> setOneHit;
		std::function<void(const std::string&)> log;
	};

	void SetEnv(Env env);

	// The op's `available` hook for the dispatch table: false with no env or when the env says off.
	bool Available();

	CheatDispatch::Result Handle(const CheatDispatch::Context& ctx, const CheatWire::Request& req);

	// The op's `cheat.state` hook: ai=on|off and onehit=on|off (on = the setting is in force).
	CheatDispatch::Fields StateFields(const std::string& playerId);
}
