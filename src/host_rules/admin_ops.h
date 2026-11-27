#pragma once

#include "rules_model.h"
#include "player_list.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// One dispatcher in front of every change an admin can make to the host's rules
// and player list. RCON, the admin panel and the in-game admin tool all call it,
// so a change is validated, applied, counted and logged the same way whoever
// made it.
//
// An op is {caller, name, args}. A batch of ops is applied as one Commit (one
// save, one push to the players). Ops:
//
//   rule.set <key> allow|admins|deny|unset     policy.default allow|deny
//   policy.message <text|off>            min.loader <version>|off
//   min.mod <plugin> <version>|wire|off  player.set <id> <flags>
//   player.remove <id>                   player.kick <id> <reason>
//   admin.clear                          rules.reload      players.reload
//   server.stacksize <0..9999|off>
//
// admin.clear and the two reloads are for RCON and the panel only.
namespace AdminOps
{
	enum class CallerKind { Rcon, Panel, InGame };

	struct Caller
	{
		CallerKind  kind = CallerKind::Rcon;
		// InGame only: the player ID the server read itself (empty while it
		// cannot be read yet) and the player's name, for the audit line.
		std::string playerId;
		std::string name;

		static Caller FromRcon()  { return Caller{ CallerKind::Rcon,  {}, {} }; }
		static Caller FromPanel() { return Caller{ CallerKind::Panel, {}, {} }; }
		static Caller FromPlayer(const std::string& id, const std::string& name)
		{ return Caller{ CallerKind::InGame, id, name }; }
	};

	struct Op
	{
		std::string              name;
		std::vector<std::string> args;   // the last one may hold spaces (text, flags, reason)
	};

	struct Result
	{
		bool        ok       = false;
		std::string text;       // the reason when refused, a short note when applied
		uint64_t    revision = 0;   // the server's revision after the call
		bool        stale    = false;   // refused because the base revision was old
	};

	// The refusal text for a stale base revision. Callers read Result::stale, not this text.
	constexpr const char* kStaleText = "Changed elsewhere, refreshed";

	// Which parts of the state a change touched.
	enum Section : unsigned
	{
		kSectionRules   = 1,   // [Policy] and [Rules]
		kSectionMins    = 2,   // [MinVersions] and the loader minimum
		kSectionPlayers = 4,   // [Players]
		kSectionMods    = 8,   // the known-features catalog (not part of the ops revision)
		kSectionServer  = 16,  // the server-wide stack size
	};

	// What the dispatcher needs from the host. The server's own set is made by
	// HostBackend(); tests bring their own.
	struct Backend
	{
		std::function<RulesModel::Model()>                                       getModel;
		std::function<bool(const RulesModel::Model&, std::string&)>              commitModel;
		std::function<std::vector<PlayerList::Entry>()>                          players;
		std::function<bool(const std::string&)>                                  validId;
		std::function<bool(PlayerList::Entry&, const std::string&, std::string&)> applyFlags;
		std::function<bool(const std::string&, const std::string&, std::string&)> setPlayer;
		std::function<bool(const std::string&, std::string&)>                    removePlayer;
		std::function<bool(int&, std::string&)>                                  clearAdmins;
		std::function<void()>                                                    refreshPlayers;
		std::function<bool(std::string&)>                                        reloadRules;
		std::function<bool(std::string&)>                                        reloadPlayers;
		std::function<bool(const std::string&)>                                  connected;
		std::function<bool(const std::string&, const std::string&, std::string&)> kick;
		std::function<void(const std::string&)>                                  audit;
		// The server-wide stack size (0 = the game's own sizes). A backend with no
		// setStackSize does not offer it: the op is refused as not available.
		std::function<int()>                                                     getStackSize;
		std::function<bool(int, const std::string&, std::string&)>               setStackSize;   // size, source for the log, error
	};

	// Check the caller, validate every op, then apply them all (nothing is
	// applied when one is refused). baseRevision 0 means "no base"; any other
	// value that is not the current revision is refused with
	// kStaleText (and Result::stale). Call on the game thread.
	Result Apply(Backend& backend, const Caller& caller, const std::vector<Op>& ops, uint64_t baseRevision = 0);

	// The same on the server: hops to the game thread, uses the real rules,
	// player list and kick, and logs the audit line.
	Result Run(const Caller& caller, const std::vector<Op>& ops, uint64_t baseRevision = 0);
	Result Run(const Caller& caller, const Op& op);
	Backend& HostBackend();

	// The one connected player a kick by ID means: its position in connectedIds. An ID is not
	// proof of a player, so when two connections share it nothing is chosen and error says so
	// (the same words the admin panel uses).
	constexpr const char* kAmbiguousIdText = "more than one connected player has that ID";
	bool UniqueConnected(const std::vector<std::string>& connectedIds, const std::string& id, size_t& position, std::string& error);

	// The server-wide counter. It goes up once for every applied batch from any
	// front end, and for a reload or an edit made outside AdminOps (NoteChanged).
	uint64_t Revision();

	// Called after every change with the new revision and the Section bits it
	// touched. Runs on the thread that made the change (the game thread, for
	// Run). Keep it short and do not call Apply from it.
	using Listener = std::function<void(uint64_t revision, unsigned sections)>;
	int  AddListener(Listener listener);   // returns a handle
	void RemoveListener(int handle);

	// A change made without Apply (the admin claim adding the first admin).
	void NoteChanged(unsigned sections);

	// The known-features catalog changed. Listeners hear it with the current
	// revision and kSectionMods; the revision does not move, so staged applies
	// stay valid.
	void NoteCatalogChanged();
}
