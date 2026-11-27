#include "cheat_ledger.h"

#include <cstdio>
#include <utility>

namespace CheatLedger
{
	std::string SlotPrefix(const std::string& slot)
	{
		const size_t colon = slot.find(':');
		return colon == std::string::npos ? slot : slot.substr(0, colon + 1);
	}

	std::string ToText(const Value& v)
	{
		char buf[64];
		switch (v.index())
		{
		case 1: return std::to_string(std::get<int32_t>(v));
		case 2: std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(std::get<float>(v))); return buf;
		case 3: return std::get<bool>(v) ? "true" : "false";
		case 4:
		{
			std::string s = "[";
			for (float f : std::get<std::vector<float>>(v))
			{
				std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(f));
				s += (s.size() > 1 ? "," : "") + std::string(buf);
			}
			return s + "]";
		}
		default: return "none";
		}
	}

	std::string Ledger::KeyOf(Kind kind, const std::string& playerId, const std::string& feature, const std::string& slot)
	{
		// An Override is server-wide: one entry per feature and slot, whoever set it.
		return (kind == Kind::Toggle ? "p|" + playerId : std::string("s|")) + "|" + feature + "|" + slot;
	}

	void Ledger::Log(const std::string& line) const
	{
		if (m_env.log)
			m_env.log(line);
	}

	std::string Ledger::Describe(const Entry& e) const
	{
		return "'" + e.playerName + "' (" + e.playerId + ") " + e.feature + "/" + e.slot;
	}

	const Entry* Ledger::Find(Kind kind, const std::string& playerId, const std::string& feature, const std::string& slot) const
	{
		const auto it = m_entries.find(KeyOf(kind, playerId, feature, slot));
		return it == m_entries.end() ? nullptr : &it->second;
	}

	Ledger::Map::iterator Ledger::Drop(Map::iterator it, const char* why)
	{
		Log("drop " + Describe(it->second) + ": " + why + ", nothing written");
		it = m_entries.erase(it);
		CountFollowers();
		return it;
	}

	void Ledger::CountFollowers()
	{
		int n = 0;
		for (const auto& kv : m_entries)
			n += kv.second.follow ? 1 : 0;
		m_followers.store(n);
	}

	bool Ledger::PutBack(const Entry& e, const char* why, bool quiet)
	{
		const Value live = m_env.read(e);
		if (live == e.applied)
		{
			if (m_env.write(e, e.prior))
			{
				Log("restore " + Describe(e) + ": " + ToText(e.applied) + " -> " + ToText(e.prior) + " (" + why + ")");
				return true;
			}
			if (!quiet)
				Log("restore " + Describe(e) + ": could not write " + ToText(e.prior) + ", left as is (" + why + ")");
			return false;
		}
		Log("restore " + Describe(e) + ": changed by the game or someone else (now " + ToText(live) + ", SU wrote " +
			ToText(e.applied) + "), left as is (" + why + ")");
		return true;
	}

	Ledger::Map::iterator Ledger::Restore(Map::iterator it, const char* why)
	{
		const Entry& e = it->second;
		if (e.parked)
			return Drop(it, "the character was already put back");
		if (e.target.Valid() && !m_env.alive(e.target))
			return Drop(it, "its target is gone");

		PutBack(e, why);
		it = m_entries.erase(it);
		CountFollowers();
		return it;
	}

	bool Ledger::Apply(const Request& req)
	{
		const std::string key = KeyOf(req.kind, req.playerId, req.feature, req.slot);
		auto it = m_entries.find(key);

		// A new target (a respawned pawn) is a new capture: the old one belonged to an object that is gone.
		// So is a parked entry's: its character was put back, the prior it holds is stale.
		if (it != m_entries.end() && (it->second.parked || !(it->second.target == req.target)))
		{
			const Entry& old = it->second;
			// A live old target still holds our value: put it back before the entry moves on.
			if (!old.parked && old.target.Valid() && m_env.alive(old.target))
			{
				PutBack(old, "the target changed");
				m_entries.erase(it);
			}
			else
			{
				Drop(it, old.parked ? "the character was already put back" : "the target changed");
			}
			it = m_entries.end();
		}

		Entry e;
		if (it == m_entries.end())
		{
			e.playerId = req.playerId; e.playerName = req.playerName; e.conn = req.conn;
			e.feature = req.feature; e.slot = req.slot; e.kind = req.kind; e.pin = req.pin; e.follow = req.follow;
			e.target = req.target; e.reqId = req.reqId; e.ownerProven = req.ownerProven;
			e.prior = m_env.read(e);
			if (std::holds_alternative<std::monostate>(e.prior))
			{
				Log("refuse " + Describe(e) + ": the current value cannot be read");
				return false;
			}
		}
		else
		{
			e = it->second;
		}

		// What the slot holds right before the write: the prior just read for a new entry.
		const Value before = it == m_entries.end() ? e.prior : m_env.read(e);
		if (!m_env.write(e, req.value))
		{
			// A write can fail part-way (a slot with several members), which nothing later would put
			// right: a new entry does not exist yet, and the release of an existing one leaves a value
			// that is not the one it wrote. So when the slot no longer holds what it held, put that back.
			const bool intact = std::holds_alternative<std::monostate>(before) || m_env.read(e) == before;
			const bool putBack = intact || m_env.write(e, before);
			Log("refuse " + Describe(e) + ": the value could not be written" +
				(putBack ? "" : ", and the earlier value could not be put back"));
			return false;
		}

		const bool isNew = it == m_entries.end();
		const bool changed = isNew || !(e.applied == req.value) || e.playerId != req.playerId;
		// The last admin to set an Override takes it over; the first prior stays.
		e.playerId = req.playerId; e.playerName = req.playerName; e.conn = req.conn;
		e.pin = req.pin; e.follow = req.follow; e.followStuck = false; e.settling = Weak(); e.reqId = req.reqId; e.ownerProven = req.ownerProven;
		e.applied = req.value;
		e.asked = req.asked;
		e.appliedAtMs = m_env.nowMs ? m_env.nowMs() : 0;
		if (isNew)
			e.seq = ++m_seq;

		if (changed)
			Log(std::string(isNew ? "apply " : "reapply ") + Describe(e) + ": " + ToText(e.prior) + " -> " + ToText(e.applied) +
				" req " + std::to_string(e.reqId));

		m_entries[key] = e;
		CountFollowers();
		return true;
	}

	bool Ledger::Release(Kind kind, const std::string& playerId, const std::string& feature, const std::string& slot)
	{
		const auto it = m_entries.find(KeyOf(kind, playerId, feature, slot));
		if (it == m_entries.end())
			return false;
		Restore(it, "released");
		return true;
	}

	void Ledger::Grant(const std::string& playerId, const std::string& playerName, const std::string& feature,
	                   const std::string& slot, const std::string& what, uint32_t reqId)
	{
		Log("grant '" + playerName + "' (" + playerId + ") " + feature + "/" + slot + ": " + what + " req " + std::to_string(reqId));
	}

	void Ledger::RestorePlayer(const std::string& playerId)
	{
		for (auto it = m_entries.begin(); it != m_entries.end();)
			it = (it->second.kind == Kind::Toggle && it->second.playerId == playerId) ? Restore(it, "player left") : std::next(it);
	}

	void Ledger::SweepRejoined(const std::string& playerId)
	{
		for (auto it = m_entries.begin(); it != m_entries.end();)
		{
			const Entry& e = it->second;
			it = (e.kind == Kind::Toggle && e.playerId == playerId && !m_env.alive(e.conn))
				? Restore(it, "player rejoined, the earlier leave was missed") : std::next(it);
		}
	}

	void Ledger::Reconcile()
	{
		for (auto it = m_entries.begin(); it != m_entries.end();)
			it = m_env.allowed(it->second) == Allowance::Denied ? Restore(it, "feature denied") : std::next(it);
	}

	void Ledger::ReconcilePlayer(const std::string& playerId)
	{
		for (auto it = m_entries.begin(); it != m_entries.end();)
			it = (it->second.playerId == playerId && m_env.allowed(it->second) == Allowance::Denied)
				? Restore(it, "feature denied") : std::next(it);
	}

	void Ledger::OnWorldEnd()
	{
		for (auto it = m_entries.begin(); it != m_entries.end();)
		{
			const Entry& e = it->second;
			if (e.kind == Kind::Toggle)
				it = Drop(it, "the world ended");
			else if (e.target.Valid() && !m_env.alive(e.target))
				it = Drop(it, "the world ended and its target is gone");
			else
				++it;
		}
	}

	void Ledger::RestoreAll()
	{
		if (!m_env.isGameThread())
		{
			if (!m_entries.empty())
				Log("stop: not on the game thread, " + std::to_string(m_entries.size()) + " entries left as is");
			return;
		}
		for (auto it = m_entries.begin(); it != m_entries.end();)
			it = Restore(it, "server stop");
	}

	void Ledger::Park(Entry& e)
	{
		e.settling = Weak();
		if (e.parked)
			return;
		// The character is alive and parked: put it back before letting it go. A dead one has
		// nothing to put back, and nothing is written to it. A failed write is retried next tick,
		// so the prior is never lost to a half-reverted character.
		if (e.target.Valid() && m_env.alive(e.target))
		{
			if (!PutBack(e, "possessed a vehicle", e.followStuck))
			{
				if (!e.followStuck)
				{
					e.followStuck = true;
					Log("follow " + Describe(e) + ": the character could not be put back yet, retrying");
				}
				return;
			}
		}
		else
		{
			Log("follow " + Describe(e) + ": possessed a vehicle, the character is gone, nothing written");
		}
		e.parked = true;
		e.followStuck = false;
	}

	Value Ledger::ComposeFor(const Entry& e, const Value& prior, const Value& fallback) const
	{
		if (std::holds_alternative<std::monostate>(e.asked) || !m_env.compose || std::holds_alternative<std::monostate>(prior))
			return fallback;
		Value v = m_env.compose(e, prior);
		return std::holds_alternative<std::monostate>(v) ? fallback : v;
	}

	// The pin read something other than what SU wrote. For a composed entry the game recomputed the
	// value (our own write is applied exactly, so it never lands here): the game's value is the new
	// prior and applied is composed again from it. True when handled, written or not (a failed write
	// is tried again next tick, with the same game value). False for an entry that is not composed.
	bool Ledger::Rebase(Entry& e, const Value& live)
	{
		if (std::holds_alternative<std::monostate>(e.asked) || !m_env.compose || std::holds_alternative<std::monostate>(live))
			return false;
		const Value next = m_env.compose(e, live);
		if (std::holds_alternative<std::monostate>(next))
			return false;
		if (!(next == live) && !m_env.write(e, next))
			return true;
		if (!(live == e.prior))
			Log("rebase " + Describe(e) + ": the game changed it to " + ToText(live) + ", now " + ToText(next));
		e.prior = live;
		e.applied = next;
		e.appliedAtMs = m_env.nowMs ? m_env.nowMs() : 0;
		return true;
	}

	void Ledger::FollowTick()
	{
		if (!m_env.stance || !m_env.rebind)
			return;
		// One read of the pawn per controller per tick: a player with many follow entries (a lock
		// per stat) is asked once.
		std::vector<std::pair<Weak, Stance>> stances;
		for (auto& kv : m_entries)
		{
			Entry& e = kv.second;
			if (!e.follow || e.kind != Kind::Toggle)
				continue;
			Stance stance;
			bool known = false;
			for (const auto& seen : stances)
			{
				if (seen.first == e.conn)
				{
					stance = seen.second;
					known = true;
					break;
				}
			}
			if (!known)
			{
				stance = m_env.stance(e);
				stances.emplace_back(e.conn, stance);
			}
			// A vehicle gets no write; the character goes back to what it was.
			if (stance.where == Stance::Where::Elsewhere)
			{
				Park(e);
				continue;
			}
			const Weak target = stance.where == Stance::Where::Character ? m_env.rebind(e, stance.character) : Weak();
			// No pawn yet: the entry waits, still held. Same object, never parked: nothing to move.
			if (!target.Valid() || (!e.parked && target == e.target))
			{
				e.settling = Weak();
				continue;
			}

			// A new pawn's attribute and movement data may not be initialised on the first tick it
			// is seen, and zero is a real value for many stats, so there is no telling a blank pawn
			// from a set-up one by its values. Rebind only once the same pawn has stood for a settle
			// period; until then nothing is read from or written to it. The same goes for a character
			// the player returns to: the game resets it on return.
			if (m_env.nowMs)
			{
				const uint64_t t = m_env.nowMs();
				if (!(e.settling == target))
				{
					e.settling = target;
					e.settleSinceMs = t;
					continue;
				}
				if (t - e.settleSinceMs < kFollowSettleMs)
					continue;
			}

			Entry moved = e;
			moved.target = target;
			moved.prior = m_env.read(moved);
			// A composed value is made from this pawn's own prior; any other is carried over.
			moved.applied = ComposeFor(moved, moved.prior, e.applied);
			if (std::holds_alternative<std::monostate>(moved.prior) || !m_env.write(moved, moved.applied))
			{
				if (!e.followStuck)
				{
					e.followStuck = true;
					Log("follow " + Describe(e) + ": the new pawn's value could not be read or written yet, retrying");
				}
				continue;
			}
			// The old character, if it is still there, holds our value: put it back before the entry
			// moves on, so no character is left with a value and no prior (a return to it would capture
			// our own value as its prior). Done once the new one took the value, so a retry leaves the
			// old one alone. A parked entry holds nothing; a dead one has nothing to put back.
			if (!e.parked && e.target.Valid() && m_env.alive(e.target))
				PutBack(e, "the player moved to another character");
			Log("follow " + Describe(e) + ": " + (e.parked ? "character again, " : "new pawn, ") + ToText(moved.prior) + " -> " + ToText(moved.applied));
			moved.followStuck = false;
			moved.parked = false;
			moved.settling = Weak();
			moved.appliedAtMs = m_env.nowMs ? m_env.nowMs() : 0;
			e = moved;
		}
	}

	void Ledger::Tick()
	{
		if (m_reconcileDue.exchange(false))
			Reconcile();
		if (m_followers.load() != 0)
			FollowTick();

		for (auto it = m_entries.begin(); it != m_entries.end();)
		{
			Entry& e = it->second;
			// A parked entry holds nothing on any object.
			if (!e.pin || e.parked)
			{
				++it;
				continue;
			}
			if (e.target.Valid() && !m_env.alive(e.target))
			{
				// A follow entry waits for the player's next pawn; anything else goes with its target.
				if (e.follow)
					++it;
				else
					it = Drop(it, "its target is gone");
				continue;
			}
			const Value live = m_env.read(e);
			if (!(live == e.applied))
			{
				if (!Rebase(e, live))
					m_env.write(e, e.applied);
			}
			++it;
		}
	}
}
