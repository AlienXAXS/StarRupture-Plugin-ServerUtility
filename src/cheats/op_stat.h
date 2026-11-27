#pragma once

// The cheat.stat op, apart from the game: it checks the request, clamps a value to the server's own
// Min..Max for that stat, asks the game glue to set it and/or lock it for the caller, and builds the
// reply. A value alone is a one-shot grant (logged by the glue, no ledger entry). A lock is a
// personal Toggle in the revert ledger (the glue makes the entry): feature kFeature, slot
// "lock:<stat>", prior = the value held before the lock. SU's tick pins it from the ledger. It
// reverts when the player leaves, when the feature is denied for them and at server stop. The game
// side comes in through Env, so this runs in unit tests. Game thread.
#include "cheat_dispatch.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace OpStat
{
	// The ledger feature (the rules feature id) and the prefix of a lock's slot.
	constexpr const char* kFeature    = "player-stats";
	constexpr const char* kSlotPrefix = "lock:";

	// The stat names a request may carry, in the order cheat.state lists them.
	const std::vector<std::string>& Stats();

	// The ledger slot of one stat's lock, e.g. "lock:health".
	std::string SlotFor(const std::string& stat);

	constexpr const char* kTextOff     = "Not available on this server's game build.";
	constexpr const char* kTextNoWorld = "Not in a world right now.";
	constexpr const char* kTextPickOne = "Pick at least one setting.";
	constexpr const char* kTextFailed  = "Couldn't change your stats on this server.";

	// The server's own limits for one stat on this player's character.
	struct Range
	{
		float min = 0.0f;
		float max = 0.0f;
	};

	struct Env
	{
		// Are the attribute-set members usable on this server right now (every key resolved).
		std::function<bool()> available;
		// Is a world loaded.
		std::function<bool()> inWorld;
		// The stats this player has locked (names from Stats()).
		std::function<std::vector<std::string>(const std::string& playerId)> locked;
		// The Min..Max of a stat on the caller's character. Empty when there is no pawn or the
		// attribute set can't be read.
		std::function<std::optional<Range>(const CheatDispatch::Context& ctx, const std::string& stat)> range;
		// The stat's current value on the caller's character. Empty when it can't be read.
		std::function<std::optional<float>(const CheatDispatch::Context& ctx, const std::string& stat)> read;
		// Set the value (already clamped) and/or change the lock. A lock on without a value pins the
		// current value; a lock off puts the prior back. False when it failed. reqId is for the audit line.
		std::function<bool(const CheatDispatch::Context& ctx, uint32_t reqId, const std::string& stat, std::optional<float> value, std::optional<bool> lock)> apply;
		std::function<void(const std::string&)> log;
	};

	void SetEnv(Env env);

	// The op's `available` hook for the dispatch table: false with no env or when the env says off.
	bool Available();

	CheatDispatch::Result Handle(const CheatDispatch::Context& ctx, const CheatWire::Request& req);

	// The op's `cheat.state` hook: lock=<stat>,<stat>|none for this player. Empty with no env.
	CheatDispatch::Fields StateFields(const std::string& playerId);
}
