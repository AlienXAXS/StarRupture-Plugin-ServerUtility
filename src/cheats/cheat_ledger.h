#pragma once

// The revert ledger of the server-side cheats, apart from the game. It remembers what a cheat
// changed (the value before and the value SU wrote) so the change can be put back, and decides
// when to put it back. The game comes in through Env, so this runs in unit tests.
//
// Two kinds of entry:
//   Toggle   a player's own switch (fly, a stat lock). Reverts when the player leaves, when the
//            feature is denied for them and at server stop, and on nothing else. A Toggle that
//            follows the player (follow) belongs to the player, not to the object it is written
//            on: when that object dies or the player's pawn changes (a respawn), the entry stays
//            and the next Tick writes its value to the new pawn's object, capturing that object's
//            own prior value first. Until then the entry is held, so a read-back still says on.
//            While the player possesses something that is not their character (a vehicle), the
//            character, still alive, is put back to its prior at once and the entry is parked:
//            nothing is written until a character has stood for a settle period, when the prior is
//            captured afresh and the held value written. Leave, deny and stop on a parked entry
//            write nothing: the character was already put back.
//   Override a server-wide setting (stack size, a paused wave). Stays when the admin who set it
//            leaves or at world end; reverts when the feature is denied for that admin (connected or
//            not: the entry keeps the proof state the owner last had), when an admin or RCON
//            releases it and at server stop.
// A grant (items, data points, unlocks, skill levels) gets no entry: Grant() only logs it.
//
// Restore writes the prior value only while the live value is still the one SU wrote. Otherwise
// the game or someone else changed it since, and it is left as is.
//
// The ledger keeps no pointer to a game object. A Weak is the object's slot index, serial and
// class, plus its address as a bare number that is only compared to the slot, never used as a
// pointer; the glue validates it before every use. Game thread only, except MarkReconcileDue.
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace CheatLedger
{
	// A reference to a game object that is safe to keep past the tick (see ObjectRef for the check).
	struct Weak
	{
		int32_t   index  = -1;
		int32_t   serial = 0;
		uint64_t  cls    = 0;
		uint64_t  addr   = 0;
		bool Valid() const { return index >= 0; }
		bool operator==(const Weak& o) const { return index == o.index && serial == o.serial && cls == o.cls && addr == o.addr; }
	};

	using Value = std::variant<std::monostate, int32_t, float, bool, std::vector<float>>;
	std::string ToText(const Value& v);

	// An entry whose slot has a per-object tail ("power:<asset>") is routed by the slot up to and
	// including the first ':' ("power:"). A slot without a ':' has no prefix; returns it whole.
	std::string SlotPrefix(const std::string& slot);

	// How long a respawned pawn must stand before a follow entry moves onto it.
	constexpr uint64_t kFollowSettleMs = 1000;

	enum class Kind { Toggle, Override };

	// Where a follow entry's player stands now, as the glue sees it. Read once per controller per
	// tick, whatever number of entries the player has.
	struct Stance
	{
		enum class Where
		{
			NoPawn,       // no pawn (a spectator pawn counts as none), or a character not reachable yet: the entry waits
			Elsewhere,    // possessing something that is not their character (a vehicle)
			Character     // their live player character
		};
		Where where = Where::NoPawn;
		Weak  character;   // Where::Character: that character, for this tick only
	};

	struct Entry
	{
		uint64_t    seq = 0;                     // ledger order, for the log
		std::string playerId, playerName;        // the player; for an Override the admin who set it last
		Weak        conn;                        // that player's controller
		std::string feature, slot;               // "movement" / "fly", "stack-size" / "stack"
		Kind        kind = Kind::Toggle;
		bool        pin  = false;                // Tick writes `applied` back while it differs
		bool        follow = false;              // Toggle only: the target is the player's pawn or a part of it; Tick moves it to the new one
		bool        followStuck = false;         // the last attempt to move it failed (logged once)
		bool        parked = false;              // follow: the player is in a vehicle; the character was put back and holds nothing of ours
		Weak        settling;                    // follow: the new pawn being waited on, until the game has set it up
		uint64_t    settleSinceMs = 0;           // when `settling` was first seen
		Weak        target;                      // what the value lives on (an Override may have none)
		Value       prior, applied;              // prior is captured once, on the first apply
		uint64_t    appliedAtMs = 0;
		uint32_t    reqId = 0;
		bool        ownerProven = false;         // the owner's admin proof as last seen; decides a deny once they are gone
	};

	struct Request
	{
		std::string playerId, playerName;
		Weak        conn;
		std::string feature, slot;
		Kind        kind = Kind::Toggle;
		bool        pin  = false;
		bool        follow = false;
		Weak        target;
		Value       value;                       // what to write
		uint32_t    reqId = 0;
		bool        ownerProven = false;
	};

	enum class Allowance { Allowed, Denied };

	struct Env
	{
		// The live value of the entry's slot; monostate when it cannot be read.
		std::function<Value(const Entry&)>                read;
		// Write a value to the entry's slot; false when it could not be written.
		std::function<bool(const Entry&, const Value&)>   write;
		std::function<bool(const Weak&)>                  alive;
		// For a follow entry: where the player stands now. Game thread. The follow step calls it once
		// per controller per tick and shares the answer between that controller's entries.
		std::function<Stance(const Entry&)>               stance;
		// For a follow entry whose player stands on their character: the object the entry's value
		// belongs on (the character's component or set), as a weak reference; invalid while the
		// character has none yet. Game thread.
		std::function<Weak(const Entry&, const Weak&)>    rebind;
		// Whether the feature is still allowed for the entry's owner. While the owner is connected
		// the glue refreshes e.ownerProven; once they are gone it decides from the stored value.
		std::function<Allowance(Entry&)>                  allowed;
		std::function<uint64_t()>                         nowMs;
		std::function<bool()>                             isGameThread;
		// One line per commit, never per tick.
		std::function<void(const std::string&)>           log;
	};

	class Ledger
	{
	public:
		explicit Ledger(Env env) : m_env(std::move(env)) {}

		// Capture the prior value (first apply only), write the value, record it. False when the
		// prior cannot be read or the write fails; nothing is recorded then.
		bool Apply(const Request& req);

		// Put one entry back (a toggle switched off, an override released). playerId is ignored
		// for an Override. False when there is no such entry.
		bool Release(Kind kind, const std::string& playerId, const std::string& feature, const std::string& slot);

		// Whether there is such an entry (an Override ignores playerId).
		bool Has(Kind kind, const std::string& playerId, const std::string& feature, const std::string& slot) const
		{
			return m_entries.count(KeyOf(kind, playerId, feature, slot)) != 0;
		}

		// A grant: logged, no entry.
		void Grant(const std::string& playerId, const std::string& playerName, const std::string& feature,
		           const std::string& slot, const std::string& what, uint32_t reqId);

		// The player left: their Toggles are restored. Overrides stay.
		void RestorePlayer(const std::string& playerId);

		// The player joined: a Toggle of theirs whose controller is gone (a leave was missed) is
		// restored where its target is alive, else dropped.
		void SweepRejoined(const std::string& playerId);

		// Rules or a player's flags changed: every entry (or the player's) the authority now
		// denies is restored.
		void Reconcile();
		void ReconcilePlayer(const std::string& playerId);

		// World end: Toggles are dropped without a write (the pawns are gone). Overrides stay (not
		// restored), unless their target is gone too.
		void OnWorldEnd();

		// Server stop. Does nothing (and says so) off the game thread.
		void RestoreAll();

		// Any thread: asks for a Reconcile at the next Tick.
		void MarkReconcileDue() { m_reconcileDue.store(true); }

		// Each game tick: a due Reconcile, the follow entries moved to the player's new pawn, then
		// the pins. Writes nothing and logs nothing when nothing changed; with no follow entry the
		// follow step is one atomic read.
		void Tick();

		size_t Size() const { return m_entries.size(); }
		const Entry* Find(Kind kind, const std::string& playerId, const std::string& feature, const std::string& slot) const;

	private:
		using Map = std::map<std::string, Entry>;
		static std::string KeyOf(Kind kind, const std::string& playerId, const std::string& feature, const std::string& slot);
		void Log(const std::string& line) const;
		std::string Describe(const Entry& e) const;
		// Restore (when the live value is still ours) and erase.
		Map::iterator Restore(Map::iterator it, const char* why);
		// Erase with a log line and no write.
		Map::iterator Drop(Map::iterator it, const char* why);

		// Move each follow entry whose player now has another pawn (or whose target died) onto the
		// new object: capture its prior, write the held value. While the player is in a vehicle the
		// character is put back and the entry is parked until a character settles again.
		void FollowTick();
		// Put back the live value if it is still the one SU wrote. False when the write failed.
		bool PutBack(const Entry& e, const char* why, bool quiet = false);
		// The player entered a vehicle: put the character back and hold the entry.
		void Park(Entry& e);
		void CountFollowers();

		Env               m_env;
		Map               m_entries;
		uint64_t          m_seq = 0;
		std::atomic<bool> m_reconcileDue{ false };
		std::atomic<int>  m_followers{ 0 };       // entries with follow set; lets Tick skip the step
	};
}
