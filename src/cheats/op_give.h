#pragma once

// The cheat.give op, apart from the game: it checks the request, asks the game glue to find the
// item and add it, and builds the reply. The game side comes in through Env, so this runs in unit
// tests. A give is a grant: no ledger entry. Game thread.
#include "cheat_dispatch.h"

#include <functional>
#include <string>

namespace OpGive
{
	constexpr int    kMinCount     = 1;
	constexpr int    kMaxCount     = 9999;
	constexpr size_t kMaxItemBytes = 256;

	constexpr const char* kTextBadCount    = "Count must be 1 to 9999.";
	constexpr const char* kTextNoItem      = "No such item on this server.";
	constexpr const char* kTextOff         = "Item giving needs a newer version map on this server.";
	constexpr const char* kTextNoCharacter = "You have no character to give items to.";
	constexpr const char* kTextNothingAdded = "Nothing could be added. Your inventory may be full.";
	constexpr const char* kTextFailed      = "The server could not give that item.";

	struct GiveResult
	{
		enum class Outcome { Gave, NoItem, NoCharacter, NothingAdded, Failed };
		Outcome     outcome = Outcome::Failed;
		std::string name;     // the item's display name
		int         given = 0;
	};

	struct Env
	{
		// Is AddNewItem usable on this server right now (the version map has its row).
		std::function<bool()> available;
		// Find the item, add it to the caller's inventory, report what was added. reqId is for the audit line.
		std::function<GiveResult(const CheatDispatch::Context& ctx, const std::string& item, int count, uint32_t reqId)> give;
		std::function<void(const std::string&)> log;
	};

	void SetEnv(Env env);

	// The op's `available` hook for the dispatch table: false with no env or when the env says off.
	bool Available();

	CheatDispatch::Result Handle(const CheatDispatch::Context& ctx, const CheatWire::Request& req);
}
