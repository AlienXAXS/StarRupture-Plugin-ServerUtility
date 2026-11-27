#pragma once

// The cheat.weapon op, apart from the game: it checks the request, asks the game glue to put a
// damage or fire-rate multiplier on the caller's own weapon attributes (or take it off), and builds
// the reply. Each of the two is a personal Toggle in the revert ledger (the glue makes the entries):
// feature kFeature, slots kSlotDamage and kSlotRate. They revert when the player leaves, when the
// feature is denied for them and at server stop. The game side comes in through Env, so this runs
// in unit tests. Game thread.
#include "cheat_dispatch.h"

#include <cstdint>
#include <functional>
#include <string>

namespace OpWeapon
{
	// The ledger feature (the rules feature id) and the slots of the two multipliers.
	constexpr const char* kFeature    = "weapons";
	constexpr const char* kSlotDamage = "damage";
	constexpr const char* kSlotRate   = "rate";

	// BetterCheats' slider ranges.
	constexpr double kDamageMin = 0.1, kDamageMax = 25.0;
	constexpr double kRateMin   = 0.1, kRateMax   = 5.0;

	constexpr const char* kTextOff      = "Not available on this server's game build.";
	constexpr const char* kTextNoWorld  = "Not in a world right now.";
	constexpr const char* kTextPickOne  = "Pick at least one setting.";
	constexpr const char* kTextFailed   = "Couldn't change your weapon on this server.";
	constexpr const char* kTextBoostOff = "Weapon boost turned off.";
	constexpr const char* kTextRange    = "Damage must be 0.1 to 25, and fire rate 0.1 to 5.";
	constexpr const char* kTextNoWeapon = "No weapon found on you.";

	enum class Slot { Damage, Rate };

	struct Env
	{
		// Are the weapon members usable on this server right now (every key resolved).
		std::function<bool()> available;
		// Is a world loaded.
		std::function<bool()> inWorld;
		// Does the caller have a weapon attribute set to write to right now.
		std::function<bool(const CheatDispatch::Context& ctx)> hasWeapon;
		// The multiplier in force for this player on that slot; 1 when it is off.
		std::function<float(const std::string& playerId, Slot slot)> current;
		// Put the multiplier on (on = true, mult in range) or take it off through the ledger. Off
		// puts the prior value back. False when it failed. reqId is for the audit line.
		std::function<bool(const CheatDispatch::Context& ctx, uint32_t reqId, Slot slot, bool on, float mult)> set;
		std::function<void(const std::string&)> log;
	};

	void SetEnv(Env env);

	// The op's `available` hook for the dispatch table: false with no env or when the env says off.
	bool Available();

	CheatDispatch::Result Handle(const CheatDispatch::Context& ctx, const CheatWire::Request& req);

	// The op's `cheat.state` hook: weapon=<damage>,<rate> for this player, or weapon=off when both
	// are off. Empty with no env.
	CheatDispatch::Fields StateFields(const std::string& playerId);
}
