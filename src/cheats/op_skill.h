#pragma once

// The cheat.skill op, apart from the game: it checks the request, asks the game glue to set the
// caller's skill level (and zero its experience) on one skill or all of them, and builds the reply.
// A grant: the levels stay, and the glue logs them; nothing goes in the revert ledger. The game side
// comes in through Env, so this runs in unit tests. Game thread.
#include "cheat_dispatch.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace OpSkill
{
	// The most skills one request may touch, and the level range.
	constexpr int kMaxSkills = 16;
	constexpr int kMaxLevel  = 100;

	constexpr const char* kTextOff      = "Not available on this server's game build.";
	constexpr const char* kTextNoWorld  = "Not in a world right now.";
	constexpr const char* kTextRange    = "Level must be 0 to 100.";
	constexpr const char* kTextNoSkill  = "No such skill on this server.";
	constexpr const char* kTextFailed   = "Couldn't change your skills on this server.";

	// "Skill <index> set to <level>." and "All skills set to <level>."
	std::string TextOne(int index, int level);
	std::string TextAll(int level);

	struct Env
	{
		// Are the skill members usable on this server right now (every key resolved).
		std::function<bool()> available;
		// How many skills the caller's controller holds; nothing when there is no world or controller.
		std::function<std::optional<int>(const CheatDispatch::Context& ctx)> skillCount;
		// Sets one skill's level and its experience to 0. False when the write failed. reqId is for the
		// audit line. The index is below skillCount.
		std::function<bool(const CheatDispatch::Context& ctx, uint32_t reqId, int index, int level)> setSkill;
		std::function<void(const std::string&)> log;
	};

	void SetEnv(Env env);

	// The op's `available` hook for the dispatch table: false with no env or when the env says off.
	bool Available();

	// A whole number as plain digits (at most three). False for anything else.
	bool ParseWhole(const std::string& text, int& out);

	CheatDispatch::Result Handle(const CheatDispatch::Context& ctx, const CheatWire::Request& req);
}
