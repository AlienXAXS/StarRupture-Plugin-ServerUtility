#pragma once

// The cheat.plant op, apart from the game: it checks the request, asks the game glue to put the
// caller's plant pickup multiplier in place (or back to normal), and builds the reply. The
// multiplier is a personal Toggle in the revert ledger (the glue makes the entry): feature
// kFeature, slot kSlot. It reverts when the player leaves, when the feature is denied for them and
// at server stop. Nothing is written to the game; a post-hook on the plant's depletion reads the
// multiplier through MultiplierFor and adds the extra items. The game side comes in through Env,
// so this runs in unit tests.
#include "cheat_dispatch.h"

#include <cstdint>
#include <functional>
#include <string>

namespace OpPlant
{
	// The ledger feature (the rules feature id, shared with cheat.grid) and the slot of the toggle.
	constexpr const char* kFeature = "inventory";
	constexpr const char* kSlot    = "plant";

	constexpr int kMultMin = 1, kMultMax = 5;

	constexpr const char* kTextOff     = "Not available on this server's game build.";
	constexpr const char* kTextNoWorld = "Not in a world right now.";
	constexpr const char* kTextFailed  = "Couldn't change your plant pickup on this server.";
	constexpr const char* kTextRange   = "Plant pickup must be 1 to 5.";
	constexpr const char* kTextNormal  = "Plant pickup back to normal.";

	struct Env
	{
		// Is the plant depletion hook usable on this server right now (its row resolved).
		std::function<bool()> available;
		// Is a world loaded.
		std::function<bool()> inWorld;
		// Puts the caller's multiplier in place through the ledger (1 puts it back). The glue calls
		// SetMultiplier when it applies or reverts. False when it failed. reqId is for the audit line.
		std::function<bool(const CheatDispatch::Context& ctx, uint32_t reqId, int mult)> apply;
		// Install the post-hook on the plant depletion (idempotent); called when the first player
		// turns the multiplier on. False when it could not be installed.
		std::function<bool()> installHook;
		// Remove it again (idempotent); called when the last multiplier goes back to normal.
		std::function<void()> removeHook;
		std::function<void(const std::string&)> log;
	};

	void SetEnv(Env env);

	// The op's `available` hook for the dispatch table: false with no env or when the env says off.
	bool Available();

	CheatDispatch::Result Handle(const CheatDispatch::Context& ctx, const CheatWire::Request& req);

	// The op's `cheat.state` hook: plant=<n>|off for this player. Empty with no env.
	CheatDispatch::Fields StateFields(const std::string& playerId);

	// The multiplier in force for a player (1 when none). Thread-safe and cheap: the hook calls it
	// on every depletion; with nobody active it is one atomic load.
	int MultiplierFor(const std::string& playerId);

	// Records a player's multiplier (1 clears it). The glue calls it when its ledger entry is
	// applied and when the entry is reverted (leave, deny, stop). Installs the hook when the first
	// player goes above 1 and removes it when the last one goes back.
	void SetMultiplier(const std::string& playerId, int mult);
}
