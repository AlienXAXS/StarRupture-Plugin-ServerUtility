#pragma once

// The cheat.grid op, apart from the game: it checks the request, refuses a grid smaller than the
// slots the player's items fill, asks the game glue to resize the caller's own inventory grid, and
// builds the reply. A resize is a grant: it stays, the glue logs it in the ledger, and nothing ever
// reverts it on leave, deny or stop. The game side comes in through Env, so this runs in unit
// tests. Game thread.
#include "cheat_dispatch.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace OpGrid
{
	// The ledger feature (the rules feature id).
	constexpr const char* kFeature = "inventory";

	constexpr int kMin = 8, kMax = 40;

	constexpr const char* kTextOff     = "Not available on this server's game build.";
	constexpr const char* kTextNoWorld = "Not in a world right now.";
	constexpr const char* kTextRange   = "Grid must be 8 to 40 on each side.";
	constexpr const char* kTextNoCheck = "Cannot check your slots right now.";
	constexpr const char* kTextFailed  = "Couldn't change your inventory on this server.";

	struct Size
	{
		int cols = 0;
		int rows = 0;
		bool operator==(const Size& o) const { return cols == o.cols && rows == o.rows; }
	};

	struct Env
	{
		// Are the inventory members usable on this server right now (every key resolved).
		std::function<bool()> available;
		// Is a world loaded.
		std::function<bool()> inWorld;
		// The grid a player has now; nullopt when it can't be read.
		std::function<std::optional<Size>(const std::string& playerId)> current;
		// How many slots the caller's items fill; nullopt when that can't be read.
		std::function<std::optional<int>(const CheatDispatch::Context& ctx)> occupied;
		// Resize the caller's grid. Records a ledger Grant and one log line on success. False when
		// it failed. reqId is for the audit line.
		std::function<bool(const CheatDispatch::Context& ctx, uint32_t reqId, int cols, int rows)> resize;
		std::function<void(const std::string&)> log;
	};

	void SetEnv(Env env);

	// The op's `available` hook for the dispatch table: false with no env or when the env says off.
	bool Available();

	CheatDispatch::Result Handle(const CheatDispatch::Context& ctx, const CheatWire::Request& req);

	// The op's `cheat.state` hook: grid=<cols>x<rows> for this player. Empty with no env or when
	// the grid can't be read.
	CheatDispatch::Fields StateFields(const std::string& playerId);
}
