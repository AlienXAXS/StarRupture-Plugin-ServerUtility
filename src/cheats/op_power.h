#pragma once

// The cheat.power op, apart from the game: it checks the request, asks the game glue to find,
// set or reset a building's power value, and builds the reply. A building is named by its config
// asset name. The edit is a server-wide Override in the revert ledger, one entry per building (the
// glue makes it): it stays until reset, a deny or server stop. The game side comes in through
// Env, so this runs in unit tests. Game thread.
#include "cheat_dispatch.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace OpPower
{
	// The ledger feature (the rules feature id) and the slot of one building: "power:<asset>".
	constexpr const char* kFeature = "machine-power";
	std::string Slot(const std::string& asset);

	constexpr double kMinPower = 0.01;
	constexpr double kMaxPower = 1000000.0;
	constexpr size_t kMaxName   = 128;   // bytes of a building asset name
	constexpr size_t kMaxListed = 20;    // buildings in one list reply, at most (the message limit may cut it sooner)

	constexpr const char* kTextOff      = "Not available on this server's game build.";
	constexpr const char* kTextNoWorld  = "Not in a world right now.";
	constexpr const char* kTextNoSuch   = "No such building on this server.";
	constexpr const char* kTextBounds   = "Power must be 0.01 to 1,000,000.";
	constexpr const char* kTextFailed   = "Could not change power on this server.";
	constexpr const char* kTextResetAll = "All building power values reset.";

	// "<building> power set to <value>." and "<building> power back to <default>."
	std::string TextSet(const std::string& building, float value);
	std::string TextReset(const std::string& building, float defaultValue);

	// A number as plain text: two decimals at most, no trailing zeros ("500", "0.5", "33.33").
	std::string FormatValue(float value);

	struct Building
	{
		std::string asset;          // the config asset name
		std::string display;        // the name players see
		std::string type;           // what the building is (producer, consumer ...)
		float       value        = 0;   // in effect
		float       defaultValue = 0;   // the game's own (pristine)
	};

	enum class Lookup { NoWorld, NotFound, Found };

	struct Env
	{
		// Is the feature usable on this server right now (every key resolved).
		std::function<bool()> available;
		// The building with this asset name.
		std::function<Lookup(const std::string& asset, Building& out)> find;
		// Every building the server found, or nothing when there is no world.
		std::function<std::optional<std::vector<Building>>()> list;
		// How many buildings have an edited value now.
		std::function<int()> edited;
		// Set (value is in bounds), reset one, or reset all, through the ledger. False when it
		// failed. A reset of a building that has no edit is true. reqId is for the audit line.
		std::function<bool(const CheatDispatch::Context& ctx, uint32_t reqId, const std::string& asset, float value)> set;
		std::function<bool(const CheatDispatch::Context& ctx, uint32_t reqId, const std::string& asset)> reset;
		std::function<bool(const CheatDispatch::Context& ctx, uint32_t reqId)> resetAll;
		std::function<void(const std::string&)> log;
	};

	void SetEnv(Env env);

	// The op's `available` hook for the dispatch table: false with no env or when the env says off.
	bool Available();

	// A value as plain text (optional '-', digits, optional '.' and digits; no exponent). False for
	// anything else; the range is not checked here.
	bool ParseValue(const std::string& text, double& out);

	CheatDispatch::Result Handle(const CheatDispatch::Context& ctx, const CheatWire::Request& req);

	// The op's `cheat.state` hook: power=<n>|off. n = buildings with an edited value.
	CheatDispatch::Fields StateFields(const std::string& playerId);
}
