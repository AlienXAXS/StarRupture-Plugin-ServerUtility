#pragma once

// The cheat.build op, apart from the game. It has three rules. `free` (cost, stability) and
// `anywhere` (windows, benches) are personal Toggles in the revert ledger (the glue makes the
// entries): each part has its own slot kSlot*, and the feature id of its rule. They revert when the
// player leaves, when the rule's feature is denied for them and at server stop. Nothing is written
// to the game: post-hooks on the server's own placement checks read PartOn for the requesting
// player and let the check pass. `unlock` is a server-wide Grant: the glue sets the technology
// keeper's unlock flag once and logs it; it is never reverted. The game side comes in through Env,
// so this runs in unit tests.
#include "cheat_dispatch.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace OpBuild
{
	enum class Part { Cost, Stability, Windows, Benches };
	constexpr int kPartCount = 4;

	// The part's name on the wire (`part=`, `parts=`, `build=`), which is also its ledger slot.
	const char* PartName(Part p);

	// The ledger feature of each rule (the rules feature ids).
	constexpr const char* kFeatureFree     = "build-free";
	constexpr const char* kFeatureAnywhere = "build-anywhere";
	constexpr const char* kFeatureUnlock   = "build-unlock";

	// The rule's feature a part belongs to: Cost and Stability are `free`, Windows and Benches `anywhere`.
	const char* FeatureOf(Part p);

	constexpr const char* kTextOff         = "Not available on this server's game build.";
	constexpr const char* kTextNoWorld     = "Not in a world right now.";
	constexpr const char* kTextFailed      = "Couldn't change building on this server.";
	constexpr const char* kTextFreeOn      = "Free building is on for your builds.";
	constexpr const char* kTextFreeOff     = "Free building is off.";
	constexpr const char* kTextAnywhereOn  = "Building anywhere is on for your builds.";
	constexpr const char* kTextAnywhereOff = "Building anywhere is off.";
	constexpr const char* kTextUnlocked    = "All buildings are unlocked on this server.";
	constexpr const char* kTextNoRelock    = "Unlocked buildings can't be locked again.";

	struct Env
	{
		// Is the part's hook usable on this server right now (its rows resolved).
		std::function<bool(Part)> available;
		// Can the server unlock buildings (the unlock flag row resolved).
		std::function<bool()> unlockAvailable;
		std::function<bool()> inWorld;
		// Puts the caller's parts in place through the ledger (on=false puts them back). The glue
		// makes one Toggle per part. False when it failed. reqId is for the audit line.
		std::function<bool(const CheatDispatch::Context& ctx, uint32_t reqId, const std::vector<Part>& parts, bool on)> apply;
		// Install the post-hook of a part (idempotent); called when the first player turns it on.
		std::function<bool(Part)> installHook;
		// Remove it again (idempotent); called when the last player turns it off.
		std::function<void(Part)> removeHook;
		// Is the unlock flag already set on the server.
		std::function<bool()> unlocked;
		// Sets the unlock flag, refreshes the available buildings and logs a ledger Grant. False when it failed.
		std::function<bool(const CheatDispatch::Context& ctx, uint32_t reqId)> unlock;
		std::function<void(const std::string&)> log;
	};

	void SetEnv(Env env);

	// The op's `available` hook for the dispatch table: some rule works on this server.
	bool Available();

	// The feature id the rules decide on for this request: by `rule=` (free, anywhere, unlock), and
	// build-free when it is missing or unknown (the handler then refuses it as unreadable).
	const char* FeatureFor(const CheatWire::Request& req);

	CheatDispatch::Result Handle(const CheatDispatch::Context& ctx, const CheatWire::Request& req);

	// The op's `cheat.state` hook. `allowed` says whether the rules let this player use a feature
	// id. build=<parts>|none is sent when build-free or build-anywhere is allowed and lists only the
	// parts that are on and whose own feature is allowed; unlock=on|off goes to every caller.
	CheatDispatch::Fields StateFieldsFor(const std::string& playerId, const std::function<bool(const char*)>& allowed);

	// Is the part on for the player. Thread-safe and cheap: the hooks call it on every check; with
	// nobody active for that part it is one atomic load.
	bool PartOn(const std::string& playerId, Part part);

	// Records a player's part (false clears it). The glue calls it when its ledger entry is applied
	// and when it is reverted (leave, deny, stop). Installs the part's hook when the first player
	// turns it on and removes it when the last one turns it off.
	void SetPart(const std::string& playerId, Part part, bool on);
}
