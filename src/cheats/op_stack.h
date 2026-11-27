#pragma once

// The cheat.stack op, apart from the game: it checks the request, asks the game glue to set or
// clear the server-wide stack size, and builds the reply. The size is a server-wide Override in
// the revert ledger (the glue makes the entry), so it is put back on size=0, deny and server stop.
// The game side comes in through Env, so this runs in unit tests. Also here: the decision the
// glue's tick takes about the item data (pure, so it is tested too). Game thread.
#include "cheat_dispatch.h"

#include <cstdint>
#include <functional>
#include <string>

namespace OpStack
{
	// The largest size accepted (the SDK stack limits helper's cap).
	constexpr int kMaxStack = 9999;

	constexpr const char* kTextOff    = "Not available on this server's game build.";
	constexpr const char* kTextCleared = "Stack size back to the server's setting.";
	constexpr const char* kTextFailed = "Couldn't change the stack size on this server.";

	// "Stack size set to <n>."
	std::string TextSet(int size);

	struct Env
	{
		// Are the item members usable on this server right now (every key resolved).
		std::function<bool()> available;
		// The size in effect, 0 = the game's own sizes.
		std::function<int()> current;
		// Make size (1..kMaxStack) the server's stack size through the ledger, or put the game's
		// sizes back. Each returns false when it failed. reqId is for the audit line.
		std::function<bool(const CheatDispatch::Context& ctx, uint32_t reqId, int size)> set;
		std::function<bool(const CheatDispatch::Context& ctx, uint32_t reqId)> clear;
		std::function<void(const std::string&)> log;
	};

	void SetEnv(Env env);

	// The op's `available` hook for the dispatch table: false with no env or when the env says off.
	bool Available();

	// A size as plain digits, 0..kMaxStack. False for anything else.
	bool ParseSize(const std::string& text, int& out);

	CheatDispatch::Result Handle(const CheatDispatch::Context& ctx, const CheatWire::Request& req);

	enum class Action { None, Rescan, Apply, Restore };

	struct TickState
	{
		bool featureAvailable = false;   // the game build's version map has the stack fields
		int  target           = 0;       // the wanted size, 0 = the game's own
		int  lastApplied      = 0;       // the size last written to the items, 0 = nothing applied
		                                 // (pass 0 on a guard tick to have it written again)
		int  itemsTracked     = 0;       // items the limits hold
		int  itemsRaised      = 0;       // items still above their own size (as of the last write)
		bool worldChanged     = false;   // the world is not the one the items were found in
		bool catalogueChanged = false;   // the game's item list has a different length than at the last scan
	};

	// What the tick does next.
	Action Decide(const TickState& s);
}
