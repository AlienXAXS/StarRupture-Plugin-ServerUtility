#include "cheat_ledger.h"

#include <cstdio>

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

	Ledger::Map::iterator Ledger::Restore(Map::iterator it, const char* why)
	{
		const Entry& e = it->second;
		if (e.target.Valid() && !m_env.alive(e.target))
			return Drop(it, "its target is gone");

		const Value live = m_env.read(e);
		if (live == e.applied)
		{
			if (m_env.write(e, e.prior))
				Log("restore " + Describe(e) + ": " + ToText(e.applied) + " -> " + ToText(e.prior) + " (" + why + ")");
			else
				Log("restore " + Describe(e) + ": could not write " + ToText(e.prior) + ", left as is (" + why + ")");
		}
		else
		{
			Log("restore " + Describe(e) + ": changed by the game or someone else (now " + ToText(live) + ", SU wrote " +
				ToText(e.applied) + "), left as is (" + why + ")");
		}
		it = m_entries.erase(it);
		CountFollowers();
		return it;
	}

	bool Ledger::Apply(const Request& req)
	{
		const std::string key = KeyOf(req.kind, req.playerId, req.feature, req.slot);
		auto it = m_entries.find(key);

		// A new target (a respawned pawn) is a new capture: the old one belonged to an object that is gone.
		if (it != m_entries.end() && !(it->second.target == req.target))
		{
			Drop(it, "the target changed");
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

		if (!m_env.write(e, req.value))
		{
			Log("refuse " + Describe(e) + ": the value could not be written");
			return false;
		}

		const bool isNew = it == m_entries.end();
		const bool changed = isNew || !(e.applied == req.value) || e.playerId != req.playerId;
		// The last admin to set an Override takes it over; the first prior stays.
		e.playerId = req.playerId; e.playerName = req.playerName; e.conn = req.conn;
		e.pin = req.pin; e.follow = req.follow; e.followStuck = false; e.settling = Weak(); e.reqId = req.reqId; e.ownerProven = req.ownerProven;
		e.applied = req.value;
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

	void Ledger::FollowTick()
	{
		if (!m_env.rebind)
			return;
		for (auto& kv : m_entries)
		{
			Entry& e = kv.second;
			if (!e.follow || e.kind != Kind::Toggle)
				continue;
			const Weak now = m_env.rebind(e);
			// No pawn yet: the entry waits, still held. Same object: nothing to move.
			if (!now.Valid() || now == e.target)
			{
				e.settling = Weak();
				continue;
			}

			// A new pawn's attribute and movement data may not be initialised on the first tick it
			// is seen, and zero is a real value for many stats, so there is no telling a blank pawn
			// from a set-up one by its values. Rebind only once the same pawn has stood for a settle
			// period; until then nothing is read from or written to it.
			if (m_env.nowMs)
			{
				const uint64_t t = m_env.nowMs();
				if (!(e.settling == now))
				{
					e.settling = now;
					e.settleSinceMs = t;
					continue;
				}
				if (t - e.settleSinceMs < kFollowSettleMs)
					continue;
			}

			Entry moved = e;
			moved.target = now;
			moved.prior = m_env.read(moved);
			if (std::holds_alternative<std::monostate>(moved.prior) || !m_env.write(moved, e.applied))
			{
				if (!e.followStuck)
				{
					e.followStuck = true;
					Log("follow " + Describe(e) + ": the new pawn's value could not be read or written yet, retrying");
				}
				continue;
			}
			Log("follow " + Describe(e) + ": new pawn, " + ToText(moved.prior) + " -> " + ToText(e.applied));
			moved.followStuck = false;
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
			const Entry& e = it->second;
			if (!e.pin)
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
			if (!(m_env.read(e) == e.applied))
				m_env.write(e, e.applied);
			++it;
		}
	}
}
