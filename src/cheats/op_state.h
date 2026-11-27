#pragma once

// The cheat.state op, apart from the game: a read-back of what is on for the caller. It adds one
// field per op the caller may use (the rules allow that op's feature for this player) that is on
// in `cap`, taken from the op's State hook. It is not an action: no ledger entry, no cheats.log
// line. Game thread.
#include "cheat_dispatch.h"

#include <functional>
#include <string>

namespace OpState
{
	constexpr const char* kOp      = "cheat.state";
	constexpr int         kVersion = 1;

	// The reply for a cheat.state request. `allowed` says whether the rules let this player use an
	// op's feature. A request with any field is refused as unreadable.
	CheatDispatch::Result Handle(const CheatWire::Request& req,
	                             const std::function<bool(const CheatDispatch::OpEntry&)>& allowed,
	                             const std::string& playerId);

	// The longest value each planned key can take, for the 1024-byte size check. Every op slice
	// adds its key here.
	CheatDispatch::Fields WorstCaseFields();
}
