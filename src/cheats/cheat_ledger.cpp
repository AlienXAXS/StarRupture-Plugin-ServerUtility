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

	bool Ledger::MixPrior(const Entry& e, const Value& live, Value& out) const
	{
		const auto* l = std::get_if<std::vector<float>>(&live);
		const auto* a = std::get_if<std::vector<float>>(&e.applied);
		const auto* p = std::get_if<std::vector<float>>(&e.prior);
		if (!l || !a || !p || l->size() != a->size() || p->size() != a->size())
			return false;
		std::vector<float> mix = *p;
		for (size_t i = 0; i < mix.size(); ++i)
			if ((*l)[i] != (*a)[i] && (!m_env.rebases || m_env.rebases(e, live, i)))
				mix[i] = (*l)[i];
		out = std::move(mix);
		return true;
	}

	static bool SameSlot(const Entry& a, const Entry& b)
	{
		return a.target == b.target && a.feature == b.feature && a.slot == b.slot;
	}

	// One pending write per object and slot: a later one is the newer intent and replaces the earlier.
	void Ledger::Defer(const Entry& e, const Value& restore, const Value& expect)
	{
		for (auto& p : m_pending)
		{
			if (SameSlot(p.e, e))
			{
				p = Pending{ e, restore, expect, 0 };
				return;
			}
		}
		m_pending.push_back(Pending{ e, restore, expect, 0 });
	}

	bool Ledger::IsPending(const Entry& e) const
	{
		for (const auto& p : m_pending)
			if (SameSlot(p.e, e))
				return true;
		return false;
	}

	void Ledger::ForgetPending(const Entry& e)
	{
		for (auto it = m_pending.begin(); it != m_pending.end();)
		{
			if (SameSlot(it->e, e))
			{
				Log("drop pending write " + Describe(it->e) + ": its character is gone, nothing written");
				it = m_pending.erase(it);
			}
			else
				++it;
		}
	}

	void Ledger::RetryPending()
	{
		for (auto it = m_pending.begin(); it != m_pending.end();)
		{
			Pending& p = *it;
			// A target that does not resolve is gone. An Override with no target never had one to lose.
			if (p.e.target.Valid() && !m_env.alive(p.e.target))
			{
				it = m_pending.erase(it);
				continue;
			}
			const Value live = m_env.read(p.e);
			if (!std::holds_alternative<std::monostate>(p.expect) && !(live == p.expect))
			{
				Log("restore " + Describe(p.e) + ": changed by the game or someone else (now " + ToText(live) + "), left as is (retry)");
				it = m_pending.erase(it);
				continue;
			}
			if (m_env.write(p.e, p.restore))
			{
				Log("restore " + Describe(p.e) + ": " + ToText(live) + " -> " + ToText(p.restore) + " (retried)");
				it = m_pending.erase(it);
				continue;
			}
			// A retry that fails part-way: undo it, or stop caring what the slot held and write the value anyway.
			if (!(m_env.read(p.e) == live) && !m_env.write(p.e, live))
				p.expect = Value();
			if (++p.tries >= kPendingMaxTries)
			{
				Log("restore " + Describe(p.e) + ": could not write " + ToText(p.restore) + " after " + std::to_string(p.tries) +
					" tries, left as is (now " + ToText(m_env.read(p.e)) + ")");
				it = m_pending.erase(it);
				continue;
			}
			++it;
		}
	}

	// Put back an entry that is leaving its target. A write still waiting on the slot is finished
	// first; when it cannot be, the put-back takes its place.
	void Ledger::PutBackEntry(const Entry& e, const char* why)
	{
		if (IsPending(e))
		{
			RetryPending();
			if (IsPending(e))
			{
				Log("restore " + Describe(e) + ": an earlier write to it is still being put right, " + ToText(e.prior) + " is written when it works (" + why + ")");
				Defer(e, e.prior, Value());
				return;
			}
		}
		PutBack(e, why, false, true);
	}

	bool Ledger::WriteSafe(const Entry& e, const Value& v, bool* undoFailed)
	{
		if (undoFailed)
			*undoFailed = false;
		const Value before = m_env.read(e);
		if (m_env.write(e, v))
			return true;
		// A write can fail part-way (a slot with several members), which nothing later would put
		// right: the value read next would be taken for a prior or for the game's change. So when the
		// slot no longer holds what it held, put that back; when that fails too, keep trying.
		if (!std::holds_alternative<std::monostate>(before) && !(m_env.read(e) == before) && !m_env.write(e, before))
		{
			Defer(e, before, Value());
			if (undoFailed)
				*undoFailed = true;
		}
		return false;
	}

	bool Ledger::PutBack(const Entry& e, const char* why, bool quiet, bool retry)
	{
		const Value live = m_env.read(e);
		Value restore = e.prior;
		if (!(live == e.applied))
		{
			// Only a composed list is put back when the live value is not ours: the members the game
			// changed keep its value, the rest return to the original.
			if (std::holds_alternative<std::monostate>(e.asked) || !MixPrior(e, live, restore) || restore == live)
			{
				Log("restore " + Describe(e) + ": changed by the game or someone else (now " + ToText(live) + ", SU wrote " +
					ToText(e.applied) + "), left as is (" + why + ")");
				return true;
			}
		}
		bool undoFailed = false;
		if (WriteSafe(e, restore, &undoFailed))
		{
			Log("restore " + Describe(e) + ": " + ToText(live) + " -> " + ToText(restore) + " (" + why + ")");
			return true;
		}
		if (!quiet)
			Log("restore " + Describe(e) + ": could not write " + ToText(restore) + ", left as is (" + why + ")");
		// The slot has one pending write: when the undo failed, it holds a half value and is replaced by
		// the put-back, which then writes whatever the slot holds.
		if (retry)
			Defer(e, restore, undoFailed ? Value() : live);
		return false;
	}

	Ledger::Map::iterator Ledger::Restore(Map::iterator it, const char* why)
	{
		const Entry& e = it->second;
		if (e.parked)
			return Drop(it, "the character was already put back");
		if (e.noPawn)
			return Drop(it, "the player has no pawn, the character is gone");
		if (e.target.Valid() && !m_env.alive(e.target))
			return Drop(it, "its target is gone");

		PutBackEntry(e, why);
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
			// A live old target still holds our value: put it back before the entry moves on. Without a
			// pawn the player has a corpse for a target until the entry moves: it gets no write.
			if (!old.parked && !old.noPawn && old.target.Valid() && m_env.alive(old.target))
			{
				PutBackEntry(old, "the target changed");
				m_entries.erase(it);
			}
			else
			{
				Drop(it, old.parked ? "the character was already put back" : "the target changed");
			}
			it = m_entries.end();
			CountFollowers();
		}

		Entry e;
		if (it == m_entries.end())
		{
			e.playerId = req.playerId; e.playerName = req.playerName; e.conn = req.conn;
			e.feature = req.feature; e.slot = req.slot; e.kind = req.kind; e.pin = req.pin; e.follow = req.follow;
			e.target = req.target; e.reqId = req.reqId; e.ownerProven = req.ownerProven;
		}
		else
		{
			e = it->second;
		}

		// A write still waiting on this object and slot (an earlier put-back) leaves its value unreliable.
		if (IsPending(e))
		{
			RetryPending();
			if (IsPending(e))
			{
				Log("refuse " + Describe(e) + ": an earlier write to it is still being put right");
				return false;
			}
		}
		if (it == m_entries.end())
		{
			e.prior = m_env.read(e);
			if (std::holds_alternative<std::monostate>(e.prior))
			{
				Log("refuse " + Describe(e) + ": the current value cannot be read");
				return false;
			}
		}

		bool undoFailed = false;
		if (!WriteSafe(e, req.value, &undoFailed))
		{
			Log("refuse " + Describe(e) + ": the value could not be written" +
				(undoFailed ? ", and the earlier value could not be put back" : ""));
			return false;
		}

		const bool isNew = it == m_entries.end();
		const bool changed = isNew || !(e.applied == req.value) || e.playerId != req.playerId;
		// The last admin to set an Override takes it over; the first prior stays.
		e.playerId = req.playerId; e.playerName = req.playerName; e.conn = req.conn;
		e.pin = req.pin; e.follow = req.follow; e.followStuck = false; e.noPawn = false; e.rebaseLogged = false; e.settling = Weak(); e.reqId = req.reqId; e.ownerProven = req.ownerProven;
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
		// A write waiting on an object of the world that ended has nothing left to write to. One with no
		// target is a server-wide setting and goes on.
		for (auto it = m_pending.begin(); it != m_pending.end();)
		{
			if (it->e.target.Valid())
			{
				Log("drop pending write " + Describe(it->e) + ": the world ended, nothing written");
				it = m_pending.erase(it);
			}
			else
				++it;
		}
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
		// Each write still waiting gets one more try, and what cannot be written is said.
		const std::vector<Pending> waiting = m_pending;
		RetryPending();
		for (const auto& w : waiting)
			if (IsPending(w.e))
				Log("stop: could not write " + ToText(w.restore) + " to " + Describe(w.e) + ", left as is");
		m_pending.clear();
	}

	void Ledger::Park(Entry& e)
	{
		e.settling = Weak();
		if (e.parked)
			return;
		// An earlier write to the character is still being put right: wait for it, adding nothing.
		if (IsPending(e))
			return;
		// The character is alive and parked: put it back before letting it go. A dead one has
		// nothing to put back, and nothing is written to it. A failed write is retried next tick,
		// so the prior is never lost to a half-reverted character.
		if (!e.noPawn && e.target.Valid() && m_env.alive(e.target))
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
	// value: its members that differ from applied (our own write is applied exactly, so a member
	// still at it is ours) become the new prior, and applied is composed again from that. True when
	// handled, written or not (a failed write is tried again next tick). False when the entry is not
	// composed, or when no member the game owns changed (the pin writes applied back).
	bool Ledger::Rebase(Entry& e, const Value& live)
	{
		if (std::holds_alternative<std::monostate>(e.asked) || !m_env.compose || std::holds_alternative<std::monostate>(live))
			return false;
		Value prior;
		if (!MixPrior(e, live, prior))
			prior = live;
		else if (prior == e.prior)
			return false;
		const Value next = m_env.compose(e, prior);
		if (std::holds_alternative<std::monostate>(next))
			return false;
		if (!(next == live) && !WriteSafe(e, next))
			return true;
		// A value the game changes every tick would log, and so open the file, every tick: once per toggle.
		if (!(prior == e.prior) && !e.rebaseLogged)
		{
			Log("rebase " + Describe(e) + ": the game changed it to " + ToText(live) + ", now " + ToText(next));
			e.rebaseLogged = true;
		}
		e.prior = prior;
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
		for (auto next = m_entries.begin(); next != m_entries.end();)
		{
			// Restoring erases only this entry, so the iterator to the next one stays good.
			const auto it = next++;
			Entry& e = it->second;
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
			// The controller is gone, not the character: the same as a leave that was missed.
			if (stance.where == Stance::Where::Gone)
			{
				Restore(it, "player left");
				continue;
			}
			// A vehicle gets no write; the character goes back to what it was.
			if (stance.where == Stance::Where::Elsewhere)
			{
				Park(e);
				continue;
			}
			// No pawn: the old target is a corpse from here until the entry moves, and is left alone.
			if (stance.where == Stance::Where::NoPawn && !e.noPawn)
			{
				e.noPawn = true;
				ForgetPending(e);
			}
			const Weak target = stance.where == Stance::Where::Character ? m_env.rebind(e, stance.character) : Weak();
			// No pawn yet: the entry waits, still held. Same object, never parked: nothing to move.
			if (!target.Valid() || (!e.parked && target == e.target))
			{
				if (target.Valid())
					e.noPawn = false;
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
			// An earlier write to that pawn is still being put right: its value is not a prior yet.
			if (IsPending(moved))
				continue;
			moved.prior = m_env.read(moved);
			// A composed value is made from this pawn's own prior; any other is carried over.
			moved.applied = ComposeFor(moved, moved.prior, e.applied);
			if (std::holds_alternative<std::monostate>(moved.prior) || !WriteSafe(moved, moved.applied))
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
			// old one alone. A parked entry holds nothing; a dead one or a corpse (no pawn) has nothing to put back.
			if (!e.parked && !e.noPawn && e.target.Valid() && m_env.alive(e.target))
				PutBackEntry(e, "the player moved to another character");
			Log("follow " + Describe(e) + ": " + (e.parked ? "character again, " : "new pawn, ") + ToText(moved.prior) + " -> " + ToText(moved.applied));
			moved.followStuck = false;
			moved.parked = false;
			moved.noPawn = false;
			moved.rebaseLogged = false;
			moved.settling = Weak();
			moved.appliedAtMs = m_env.nowMs ? m_env.nowMs() : 0;
			e = moved;
		}
	}

	void Ledger::Tick()
	{
		// First, so a player who just lost their pawn is marked noPawn and their pendings are forgotten
		// before anything below writes to the corpse.
		if (m_followers.load() != 0)
			FollowTick();
		if (m_reconcileDue.exchange(false))
			Reconcile();
		RetryPending();

		for (auto it = m_entries.begin(); it != m_entries.end();)
		{
			Entry& e = it->second;
			// A parked entry holds nothing on any object; a player without a pawn has a corpse for a target;
			// a write waiting to be put right leaves the value unreliable.
			if (!e.pin || e.parked || e.noPawn || IsPending(e))
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
					WriteSafe(e, e.applied);
			}
			++it;
		}
	}
}
