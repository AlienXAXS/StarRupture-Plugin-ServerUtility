#pragma once

// The cheat.move op, apart from the game: it checks the request, asks the game glue to put the
// caller's character in fly mode and/or raise its speed (or put either back), and builds the reply.
// Each of the two is a personal Toggle in the revert ledger (the glue makes the entries): feature
// kFeature, slots kSlotFly and kSlotSpeed. They revert when the player leaves, when the feature is
// denied for them and at server stop, and on nothing else: a respawn keeps them (SU moves them to the
// new pawn). Flying sets the movement mode only and never touches collision. The game side comes in
// through Env, so this runs in unit tests. Game thread.
#include "cheat_dispatch.h"

#include <cstdint>
#include <functional>
#include <string>

namespace OpMove
{
	// The ledger feature (the rules feature id) and the slots of the two toggles.
	constexpr const char* kFeature   = "movement";
	constexpr const char* kSlotFly   = "fly";
	constexpr const char* kSlotSpeed = "speed";

	// BetterCheats' slider range. 1 (or "off") is normal speed.
	constexpr double kSpeedMin = 1.0, kSpeedMax = 25.0;

	constexpr const char* kTextOff         = "Not available on this server's game build.";
	constexpr const char* kTextNoWorld     = "Not in a world right now.";
	constexpr const char* kTextPickOne     = "Pick at least one setting.";
	constexpr const char* kTextFailed      = "Couldn't change your movement on this server.";
	constexpr const char* kTextRange       = "Speed must be 1 to 25.";
	constexpr const char* kTextNoCharacter = "No character found on you.";
	constexpr const char* kTextFlyOn       = "Flying on.";
	constexpr const char* kTextFlyOff      = "Flying off.";
	constexpr const char* kTextSpeedOff    = "Speed back to normal.";

	struct Env
	{
		// Are the movement members usable on this server right now (every key resolved).
		std::function<bool()> available;
		// Is a world loaded.
		std::function<bool()> inWorld;
		// Does the caller have a character with a movement component to write to right now.
		std::function<bool(const CheatDispatch::Context& ctx)> hasCharacter;
		// Is this player flying through a ledger entry.
		std::function<bool(const std::string& playerId)> flying;
		// The speed multiplier in force for this player; 1 when it is off.
		std::function<float(const std::string& playerId)> speed;
		// Turn flying on or off through the ledger. Off puts the prior mode back. False when it
		// failed. reqId is for the audit line.
		std::function<bool(const CheatDispatch::Context& ctx, uint32_t reqId, bool on)> setFly;
		// Put the speed multiplier on (on = true, mult in range) or take it off through the ledger.
		// Off puts the prior value back. False when it failed.
		std::function<bool(const CheatDispatch::Context& ctx, uint32_t reqId, bool on, float mult)> setSpeed;
		std::function<void(const std::string&)> log;
	};

	void SetEnv(Env env);

	// The op's `available` hook for the dispatch table: false with no env or when the env says off.
	bool Available();

	CheatDispatch::Result Handle(const CheatDispatch::Context& ctx, const CheatWire::Request& req);

	// The op's `cheat.state` hook: fly=on|off and speed=<mult>|off for this player. Empty with no env.
	CheatDispatch::Fields StateFields(const std::string& playerId);
}
