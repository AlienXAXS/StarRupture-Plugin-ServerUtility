#pragma once

// The cheat.corp op, apart from the game: it checks the request, asks the game glue to set the
// corporations data points, and builds the reply. The game side comes in through Env, so this runs
// in unit tests. Server-wide (the rules feature is an admins-only default) and a grant: the value
// stays, and the glue logs it. Game thread.
#include "cheat_dispatch.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace OpCorp
{
	// The largest value accepted.
	constexpr int kMaxPoints = 999999999;

	constexpr const char* kTextOff      = "Not available on this server's game build.";
	constexpr const char* kTextNoWorld  = "Not in a world right now.";
	constexpr const char* kTextBounds   = "Data points must be 0 to 999,999,999.";
	constexpr const char* kTextFailed   = "Couldn't change the data points on this server.";

	// "Data points set to <n>."
	std::string TextSet(int points);

	struct Env
	{
		// Are the corporation members usable on this server right now (every key resolved).
		std::function<bool()> available;
		// The data points now; nothing when there is no world or the game could not be asked.
		std::function<std::optional<int>()> current;
		// Sets the value and logs the grant. False when the write failed. reqId is for the audit line.
		std::function<bool(const CheatDispatch::Context& ctx, uint32_t reqId, int points)> set;
		std::function<void(const std::string&)> log;
	};

	void SetEnv(Env env);

	// The op's `available` hook for the dispatch table: false with no env or when the env says off.
	bool Available();

	// A value as plain digits, 0..kMaxPoints. False for anything else.
	bool ParsePoints(const std::string& text, int& out);

	CheatDispatch::Result Handle(const CheatDispatch::Context& ctx, const CheatWire::Request& req);

	// The op's `cheat.state` hook: points=<n>, the data points now. No field when there is no world.
	CheatDispatch::Fields StateFields(const std::string& playerId);
}
