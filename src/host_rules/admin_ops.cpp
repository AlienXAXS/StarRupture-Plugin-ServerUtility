#include "admin_ops.h"
#include "ini_file.h"
#include "line_text.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <utility>

namespace
{
	std::atomic<uint64_t> g_revision{ 1 };

	std::mutex                                   g_listenMutex;
	std::vector<std::pair<int, AdminOps::Listener>> g_listeners;
	int                                          g_nextHandle = 1;

	using IniFile::Lower;
	using IniFile::Trim;

	using LineText::OneLine;

	std::string CallerText(const AdminOps::Caller& c)
	{
		switch (c.kind)
		{
		case AdminOps::CallerKind::Rcon:  return "(RCON)";
		case AdminOps::CallerKind::Panel: return "(panel)";
		default:
			return "'" + (c.name.empty() ? std::string("unknown") : OneLine(c.name)) + "' (in game)";
		}
	}

	int AdminCount(const std::vector<PlayerList::Entry>& list)
	{
		int n = 0;
		for (const auto& e : list) if (e.admin) ++n;
		return n;
	}

	std::vector<PlayerList::Entry>::iterator FindEntry(std::vector<PlayerList::Entry>& list, const std::string& id)
	{
		return std::find_if(list.begin(), list.end(), [&](const PlayerList::Entry& e) { return e.id == id; });
	}

	// One validated op, ready to apply.
	struct Planned
	{
		enum class Kind { Model, Set, Remove, Clear, Kick, ReloadRules, ReloadPlayers } kind = Kind::Model;
		std::string id, flags, reason;
		std::string audit;      // what the audit line says
		unsigned    sections = 0;
	};

	bool Need(const AdminOps::Op& op, size_t count, std::string& error)
	{
		if (op.args.size() == count) return true;
		error = "wrong number of arguments for " + op.name;
		return false;
	}

	// Validate one op against the working copies, change the copies and describe
	// what to do for real. The in-game flag turns on the guards.
	bool Plan(AdminOps::Backend& be, bool inGame, const AdminOps::Op& op, RulesModel::Model& model,
	          std::vector<PlayerList::Entry>& list, Planned& out, std::string& error)
	{
		using namespace AdminOps;
		const std::string& n = op.name;

		if (n == "rule.set")
		{
			if (!Need(op, 2, error)) return false;
			const std::string value = Lower(op.args[1]);
			if (value == "unset")
			{
				if (!RulesModel::UnsetRule(model, op.args[0], error)) return false;
			}
			else if (value == "allow" || value == "deny")
			{
				if (!RulesModel::SetRule(model, op.args[0], value == "deny", error)) return false;
			}
			else if (value == "admins")
			{
				if (!RulesModel::SetRuleAdminsOnly(model, op.args[0], error)) return false;
			}
			else { error = "value must be allow, admins, deny or unset"; return false; }
			out.audit = op.args[0] + "=" + value;
			out.sections = kSectionRules;
			return true;
		}
		if (n == "policy.default")
		{
			if (!Need(op, 1, error)) return false;
			const std::string value = Lower(op.args[0]);
			if (value != "allow" && value != "deny") { error = "default must be allow or deny"; return false; }
			RulesModel::SetDefault(model, value == "deny");
			out.audit = "policy.default=" + value;
			out.sections = kSectionRules;
			return true;
		}
		if (n == "policy.message")
		{
			if (!Need(op, 1, error)) return false;
			const std::string text = Lower(op.args[0]) == "off" ? std::string() : op.args[0];
			if (!RulesModel::SetMessage(model, text, error)) return false;
			out.audit = "policy.message=" + (text.empty() ? std::string("off") : OneLine(text));
			out.sections = kSectionRules;
			return true;
		}
		if (n == "min.loader")
		{
			if (!Need(op, 1, error)) return false;
			const std::string version = Lower(op.args[0]) == "off" ? std::string() : op.args[0];
			if (!RulesModel::SetMinLoader(model, version, error)) return false;
			out.audit = "min.loader=" + (version.empty() ? std::string("off") : version);
			out.sections = kSectionMins;
			return true;
		}
		if (n == "min.mod")
		{
			if (!Need(op, 2, error)) return false;
			const std::string version = Lower(op.args[1]) == "off" ? std::string() : op.args[1];
			if (!RulesModel::SetModMin(model, op.args[0], version, error)) return false;
			out.audit = "min.mod " + op.args[0] + "=" + (version.empty() ? std::string("off") : version);
			out.sections = kSectionMins;
			return true;
		}

		if (n == "player.set")
		{
			if (!Need(op, 2, error)) return false;
			const std::string id = Trim(op.args[0]);
			if (!be.validId(id)) { error = "not a usable player ID"; return false; }

			auto it = FindEntry(list, id);
			PlayerList::Entry updated;
			if (it != list.end()) updated = *it; else updated.id = id;
			if (!be.applyFlags(updated, op.args[1], error)) return false;

			if (inGame && AdminCount(list) > 0)
			{
				const bool wasAdmin = it != list.end() && it->admin;
				if (wasAdmin && !updated.admin && AdminCount(list) == 1)
				{
					error = "that would remove the last Admin; use RCON for that";
					return false;
				}
			}
			if (it == list.end()) list.push_back(updated); else *it = updated;
			out.kind = Planned::Kind::Set;
			out.id = id;
			out.flags = op.args[1];
			out.audit = "player.set " + id + " " + OneLine(op.args[1]);
			out.sections = kSectionPlayers;
			return true;
		}
		if (n == "player.remove")
		{
			if (!Need(op, 1, error)) return false;
			const std::string id = Trim(op.args[0]);
			auto it = FindEntry(list, id);
			if (it == list.end()) { error = "no entry with that ID"; return false; }
			if (inGame && it->admin && AdminCount(list) == 1)
			{
				error = "that would remove the last Admin; use RCON for that";
				return false;
			}
			list.erase(it);
			out.kind = Planned::Kind::Remove;
			out.id = id;
			out.audit = "player.remove " + id;
			out.sections = kSectionPlayers;
			return true;
		}
		if (n == "player.kick")
		{
			if (!Need(op, 2, error)) return false;
			const std::string id = Trim(op.args[0]);
			if (inGame)
			{
				auto it = FindEntry(list, id);
				if (it != list.end() && it->admin) { error = "an Admin cannot be kicked; remove the Admin flag first (RCON: admin remove)"; return false; }
			}
			if (!be.connected(id)) { error = "player not found"; return false; }
			out.kind = Planned::Kind::Kick;
			out.id = id;
			out.reason = op.args[1];
			out.audit = "player.kick " + id + (op.args[1].empty() ? std::string() : " (" + OneLine(op.args[1]) + ")");
			return true;
		}
		if (n == "admin.clear")
		{
			if (!Need(op, 0, error)) return false;
			if (inGame) { error = "not available in game; use RCON"; return false; }
			for (auto& e : list) e.admin = false;
			out.kind = Planned::Kind::Clear;
			out.audit = "admin.clear";
			out.sections = kSectionPlayers;
			return true;
		}
		if (n == "rules.reload" || n == "players.reload")
		{
			if (!Need(op, 0, error)) return false;
			if (inGame) { error = "not available in game; use RCON"; return false; }
			const bool rules = n == "rules.reload";
			out.kind = rules ? Planned::Kind::ReloadRules : Planned::Kind::ReloadPlayers;
			out.audit = n;
			out.sections = rules ? (kSectionRules | kSectionMins) : kSectionPlayers;
			return true;
		}

		error = "unknown op " + OneLine(n);
		return false;
	}

	void Notify(uint64_t revision, unsigned sections)
	{
		std::vector<AdminOps::Listener> copy;
		{
			std::lock_guard<std::mutex> lk(g_listenMutex);
			for (const auto& l : g_listeners) copy.push_back(l.second);
		}
		for (const auto& l : copy) l(revision, sections);
	}

	void Bump(unsigned sections)
	{
		Notify(++g_revision, sections);
	}
}

namespace AdminOps
{
	Result Apply(Backend& be, const Caller& caller, const std::vector<Op>& ops, uint64_t baseRevision)
	{
		Result r;
		auto fail = [&](const std::string& text) { r.ok = false; r.text = text; r.revision = Revision(); return r; };

		const bool inGame = caller.kind == CallerKind::InGame;
		std::vector<PlayerList::Entry> list = be.players();

		// The caller is checked on every call: nothing from an earlier call counts.
		if (inGame)
		{
			const std::string id = Trim(caller.playerId);
			if (id.empty()) return fail("your player ID cannot be read yet");
			auto it = FindEntry(list, id);
			if (it == list.end() || !it->admin) return fail("you are not an admin on this server");
		}

		if (ops.empty()) return fail("nothing to do");
		if (baseRevision != 0 && baseRevision != Revision()) { fail(kStaleText); r.stale = true; return r; }

		// A reload re-reads a file and stands alone.
		for (const Op& op : ops)
			if ((op.name == "rules.reload" || op.name == "players.reload") && ops.size() > 1)
				return fail("a reload cannot be combined with other ops");

		// Check everything first; the working copies carry earlier ops into later ones.
		RulesModel::Model model = be.getModel();
		bool modelChanged = false;
		std::vector<Planned> plan;
		for (size_t i = 0; i < ops.size(); ++i)
		{
			Planned p;
			std::string error;
			if (!Plan(be, inGame, ops[i], model, list, p, error))
				return fail(ops.size() > 1 ? "op " + std::to_string(i + 1) + " (" + OneLine(ops[i].name) + "): " + error : error);
			if (p.kind == Planned::Kind::Model) modelChanged = true;
			plan.push_back(std::move(p));
		}

		const std::string who = CallerText(caller);
		unsigned sections = 0;
		std::string failure;
		auto applied = [&](const Planned& p)
		{
			sections |= p.sections;
			if (be.audit) be.audit("[Admin] " + who + " " + p.audit);
		};

		// One Commit for every rule, policy and minimum op in the batch.
		if (modelChanged)
		{
			std::string error;
			if (!be.commitModel(model, error)) return fail(error);
			for (const Planned& p : plan) if (p.kind == Planned::Kind::Model) applied(p);
		}

		bool playersTouched = false;
		std::string text = "Saved.";
		for (const Planned& p : plan)
		{
			std::string error;
			switch (p.kind)
			{
			case Planned::Kind::Set:
				if (be.setPlayer(p.id, p.flags, error)) { applied(p); playersTouched = true; }
				else if (failure.empty()) failure = error;
				break;
			case Planned::Kind::Remove:
				if (be.removePlayer(p.id, error)) { applied(p); playersTouched = true; }
				else if (failure.empty()) failure = error;
				break;
			case Planned::Kind::Clear:
			{
				int changed = 0;
				if (be.clearAdmins(changed, error))
				{
					applied(p);
					playersTouched = true;
					text = "Took Admin off " + std::to_string(changed) + " entries.";
				}
				else if (failure.empty()) failure = error;
				break;
			}
			case Planned::Kind::ReloadRules:
				if (be.reloadRules(error)) { applied(p); text = "Host rules reloaded."; }
				else if (failure.empty()) failure = error;
				break;
			case Planned::Kind::ReloadPlayers:
				if (be.reloadPlayers(error)) { applied(p); playersTouched = true; text = "Player list reloaded."; }
				else if (failure.empty()) failure = error;
				break;
			default:
				break;
			}
		}
		if (playersTouched) be.refreshPlayers();

		for (const Planned& p : plan)
		{
			if (p.kind != Planned::Kind::Kick) continue;
			std::string error;
			if (be.kick(p.id, p.reason, error)) { applied(p); text = "Kicked."; }
			else if (failure.empty()) failure = error;
		}

		// Whatever was applied counts as a change, even when a later step failed.
		if (sections != 0) Bump(sections);

		r.ok = failure.empty();
		r.text = r.ok ? text : failure;
		r.revision = Revision();
		return r;
	}

	bool UniqueConnected(const std::vector<std::string>& connectedIds, const std::string& id, size_t& position, std::string& error)
	{
		bool found = false;
		for (size_t i = 0; i < connectedIds.size(); ++i)
		{
			if (connectedIds[i] != id) continue;
			if (found) { error = kAmbiguousIdText; return false; }
			found = true;
			position = i;
		}
		if (!found) { error = "player not found"; return false; }
		return true;
	}

	uint64_t Revision() { return g_revision.load(); }

	int AddListener(Listener listener)
	{
		std::lock_guard<std::mutex> lk(g_listenMutex);
		const int handle = g_nextHandle++;
		g_listeners.emplace_back(handle, std::move(listener));
		return handle;
	}

	void RemoveListener(int handle)
	{
		std::lock_guard<std::mutex> lk(g_listenMutex);
		g_listeners.erase(std::remove_if(g_listeners.begin(), g_listeners.end(),
		                  [&](const auto& l) { return l.first == handle; }), g_listeners.end());
	}

	void NoteChanged(unsigned sections)
	{
		Bump(sections);
	}

	void NoteCatalogChanged()
	{
		Notify(g_revision.load(), kSectionMods);
	}
}
