#pragma once

// The cheat.mine op, apart from the game: it checks the request, asks the game glue to turn the
// caller's drill overheating off (the loose tag Cheat.UnlimitedWeaponHeat on their pawn) and/or set
// their mining boost multiplier (or put either back), and builds the reply. Each of the two is a
// personal Toggle in the revert ledger (the glue makes the entries): feature kFeature, slots
// kSlotHeat and kSlotBoost. They revert when the player leaves, when the feature is denied for them
// and at server stop. The game side comes in through Env, so this runs in unit tests. Game thread.
#include "cheat_dispatch.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace OpMine
{
	// The ledger feature (the rules feature id) and the slots of the two toggles.
	constexpr const char* kFeature   = "mining-tools";
	constexpr const char* kSlotHeat  = "heat";
	constexpr const char* kSlotBoost = "boost";

	// The boost range (draft; the contract says 1 to 10, BetterCheats' slider goes 0.1 to 10).
	constexpr double kBoostMin = 1.0, kBoostMax = 10.0;

	constexpr const char* kTextOff      = "Not available on this server's game build.";
	constexpr const char* kTextNoWorld  = "Not in a world right now.";
	constexpr const char* kTextPickOne  = "Pick at least one setting.";
	constexpr const char* kTextFailed   = "Couldn't change your mining on this server.";
	constexpr const char* kTextRange    = "Boost must be 1 to 10.";
	constexpr const char* kTextBothOff  = "Mining heat and boost back to normal.";
	constexpr const char* kTextHeatOnly = "Mining heat off.";

	// What is in force for one player. boost is 1 when the boost is off.
	struct State
	{
		bool heat = false;
		float boost = 1.0f;
	};

	struct Env
	{
		// Are the mining members usable on this server right now (every key resolved).
		std::function<bool()> available;
		// Is a world loaded.
		std::function<bool()> inWorld;
		// What is in force for this player.
		std::function<State(const std::string& playerId)> current;
		// Change the caller's settings through the ledger: heat (true = overheating off, false = put
		// back) and/or boost (a multiplier from 1 to 10; 1 puts it back). An empty one is left as it
		// is. False when it failed (no pawn, a write failed). reqId is for the audit line.
		std::function<bool(const CheatDispatch::Context& ctx, uint32_t reqId, std::optional<bool> heat, std::optional<float> boost)> apply;
		std::function<void(const std::string&)> log;
	};

	void SetEnv(Env env);

	// The op's `available` hook for the dispatch table: false with no env or when the env says off.
	bool Available();

	CheatDispatch::Result Handle(const CheatDispatch::Context& ctx, const CheatWire::Request& req);

	// The op's `cheat.state` hook: mine=<boost>|off and heat=on|off for this player. Empty with no env.
	CheatDispatch::Fields StateFields(const std::string& playerId);
}
