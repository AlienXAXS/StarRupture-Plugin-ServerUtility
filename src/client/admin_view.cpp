#include "client/admin_view.h"

#include <algorithm>
#include <cctype>
#include <set>

#include "rcon/commands/word_split.h"

using namespace AdminWire;

namespace AdminView
{
	static std::vector<std::string> Split(const std::string& s, char sep)
	{
		std::vector<std::string> r;
		std::string cur;
		for (char c : s)
		{
			if (c == sep) { if (!cur.empty()) r.push_back(cur); cur.clear(); }
			else cur += c;
		}
		if (!cur.empty()) r.push_back(cur);
		return r;
	}

	// Applies the tokens of one flags text to f (delta semantics, as the server does).
	static void ApplyTokens(PlayerFlags& f, const std::string& text)
	{
		std::string rest = text;
		for (;;)
		{
			const size_t a = rest.find_first_not_of(' ');
			if (a == std::string::npos) break;
			rest.erase(0, a);
			if (rest.compare(0, 6, "label=") == 0)
			{
				f.label = rest.substr(6);   // runs to the end
				return;
			}
			// A key with a space is sent as sections="Best Builder.x;Other.y"; TakeWord drops the quotes.
			const std::string tok = WordSplit::TakeWord(rest);
			if (tok == "admin") f.admin = true;
			else if (tok == "-admin") f.admin = false;
			else if (tok == "exemptversion") f.exemptVersion = true;
			else if (tok == "-exemptversion") f.exemptVersion = false;
			else if (tok == "exemptrules") f.exemptRules = true;
			else if (tok == "-exemptrules") f.exemptRules = false;
			else if (tok == "-sections") f.sections.clear();
			else if (tok.compare(0, 9, "sections=") == 0) f.sections = Split(tok.substr(9), ';');
		}
	}

	PlayerFlags ParseFlags(const std::string& text)
	{
		PlayerFlags f;
		ApplyTokens(f, text);
		return f;
	}

	std::string FormatFlags(const PlayerFlags& f)
	{
		std::string r;
		auto add = [&](const std::string& t) { if (!r.empty()) r += ' '; r += t; };
		if (f.admin) add("admin");
		if (f.exemptVersion) add("exemptversion");
		if (f.exemptRules) add("exemptrules");
		if (!f.sections.empty()) add(WordSplit::SectionsFlag("sections", f.sections));
		if (!f.label.empty()) add("label=" + f.label);
		return r;
	}

	std::string FormatWireFlags(const PlayerFlags& f)
	{
		std::string r;
		auto add = [&](const std::string& t) { if (!r.empty()) r += ' '; r += t; };
		add(f.admin ? "admin" : "-admin");
		add(f.exemptVersion ? "exemptversion" : "-exemptversion");
		add(f.exemptRules ? "exemptrules" : "-exemptrules");
		add(f.sections.empty() ? std::string("-sections") : WordSplit::SectionsFlag("sections", f.sections));
		return r;
	}

	static PlayerInfo* FindMutable(Snapshot& s, const std::string& id)
	{
		for (PlayerInfo& p : s.players) if (p.id == id) return &p;
		return nullptr;
	}

	const PlayerInfo* FindPlayer(const Snapshot& s, const std::string& id)
	{
		for (const PlayerInfo& p : s.players) if (p.id == id) return &p;
		return nullptr;
	}

	static void SetRule(Snapshot& s, const std::string& key, const std::string& value)
	{
		auto it = std::find_if(s.rules.begin(), s.rules.end(), [&](const Rule& r) { return r.key == key; });
		if (value == "allow" || value == "deny")
		{
			if (it != s.rules.end()) it->value = value;
			else s.rules.push_back({ key, value });
		}
		else if (it != s.rules.end()) s.rules.erase(it);
	}

	static void SetMin(Snapshot& s, const std::string& plugin, const std::string& version)
	{
		auto it = std::find_if(s.mins.begin(), s.mins.end(), [&](const MinEntry& m) { return m.plugin == plugin; });
		if (!version.empty() && version != "off")
		{
			if (it != s.mins.end()) it->version = version;
			else s.mins.push_back({ plugin, version });
		}
		else if (it != s.mins.end()) s.mins.erase(it);
	}

	static void ApplyOp(Snapshot& s, const Op& op)
	{
		if (OpArgCount(op.name) != static_cast<int>(op.args.size())) return;
		if (op.name == "rule.set") SetRule(s, op.args[0], op.args[1]);
		else if (op.name == "policy.default") { if (op.args[0] == "allow" || op.args[0] == "deny") s.defaultPolicy = op.args[0]; }
		else if (op.name == "policy.message") s.message = op.args[0];
		else if (op.name == "min.loader") SetMin(s, std::string(), op.args[0]);
		else if (op.name == "min.mod") SetMin(s, op.args[0], op.args[1]);
		else if (op.name == "player.set")
		{
			PlayerInfo* p = FindMutable(s, op.args[0]);
			if (!p)
			{
				s.players.push_back(PlayerInfo());
				p = &s.players.back();
				p->id = op.args[0];
			}
			PlayerFlags f = ParseFlags(p->flags);
			ApplyTokens(f, op.args[1]);
			p->flags = FormatFlags(f);
		}
		else if (op.name == "player.remove")
		{
			PlayerInfo* p = FindMutable(s, op.args[0]);
			if (!p) return;
			if (p->online) p->flags.clear();
			else s.players.erase(s.players.begin() + (p - s.players.data()));
		}
	}

	Snapshot ApplyStaged(const Snapshot& base, const std::vector<Op>& ops)
	{
		Snapshot s = base;
		for (const Op& op : ops) ApplyOp(s, op);
		return s;
	}

	std::string RuleValue(const Snapshot& s, const std::string& key)
	{
		for (const Rule& r : s.rules) if (r.key == key) return r.value;
		return std::string();
	}

	// "plugin.id" with the plugin lower-cased, split at the first dot.
	static std::string RuleKeyForMatch(const std::string& key)
	{
		const size_t dot = key.find('.');
		if (dot == std::string::npos) return key;
		std::string plugin = key.substr(0, dot);
		for (char& c : plugin) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return plugin + key.substr(dot);
	}

	bool IsDefaultDenied(const Snapshot& s, const std::string& key)
	{
		const std::string k = RuleKeyForMatch(key);
		bool isDefault = false;
		for (const std::string& d : s.defaults) if (RuleKeyForMatch(d) == k) { isDefault = true; break; }
		if (!isDefault) return false;
		for (const Rule& r : s.rules) if (RuleKeyForMatch(r.key) == k) return false;
		return true;
	}

	std::string MinValue(const Snapshot& s, const std::string& plugin)
	{
		for (const MinEntry& m : s.mins) if (m.plugin == plugin) return m.version;
		return std::string();
	}

	bool SameTarget(const Op& a, const Op& b)
	{
		auto kind = [](const Op& o) -> std::string
		{
			if (o.name == "player.set" || o.name == "player.remove") return "player";
			return o.name;
		};
		if (kind(a) != kind(b)) return false;
		if (a.name == "rule.set" || a.name == "min.mod" || a.name == "player.set" || a.name == "player.remove")
			return !a.args.empty() && !b.args.empty() && a.args[0] == b.args[0];
		return true;
	}

	std::vector<size_t> StagedWithTarget(const std::vector<Op>& staged, const Op& op)
	{
		std::vector<size_t> r;
		for (size_t i = staged.size(); i-- > 0;) if (SameTarget(staged[i], op)) r.push_back(i);
		return r;
	}

	bool IsNoop(const Snapshot& base, const Op& op)
	{
		if (OpArgCount(op.name) != static_cast<int>(op.args.size())) return false;
		if (op.name == "rule.set") return RuleValue(base, op.args[0]) == (op.args[1] == "unset" ? std::string() : op.args[1]);
		if (op.name == "policy.default") return base.defaultPolicy == op.args[0];
		if (op.name == "policy.message") return base.message == op.args[0];
		if (op.name == "min.loader") return MinValue(base, std::string()) == (op.args[0] == "off" ? std::string() : op.args[0]);
		if (op.name == "min.mod") return MinValue(base, op.args[0]) == (op.args[1] == "off" ? std::string() : op.args[1]);
		if (op.name == "player.set")
		{
			const PlayerInfo* p = FindPlayer(base, op.args[0]);
			if (!p) return false;
			PlayerFlags f = ParseFlags(p->flags);
			PlayerFlags g = f;
			ApplyTokens(g, op.args[1]);
			return FormatFlags(f) == FormatFlags(g);
		}
		return false;
	}

	bool RuleChanged(const Snapshot& base, const Snapshot& eff, const std::string& key)
	{
		return RuleValue(base, key) != RuleValue(eff, key);
	}

	bool MinChanged(const Snapshot& base, const Snapshot& eff, const std::string& plugin)
	{
		return MinValue(base, plugin) != MinValue(eff, plugin);
	}

	bool PlayerChanged(const Snapshot& base, const Snapshot& eff, const std::string& id)
	{
		const PlayerInfo* a = FindPlayer(base, id);
		const PlayerInfo* b = FindPlayer(eff, id);
		if (!a || !b) return a != b;
		return FormatFlags(ParseFlags(a->flags)) != FormatFlags(ParseFlags(b->flags));
	}

	bool ModChanged(const Snapshot& base, const Snapshot& eff, const ModInfo& mod)
	{
		if (RuleChanged(base, eff, mod.plugin + ".*")) return true;
		for (const Feature& f : mod.features)
			if (RuleChanged(base, eff, mod.plugin + "." + f.id)) return true;
		return false;
	}

	std::vector<std::string> UnknownRuleKeys(const Snapshot& s)
	{
		std::set<std::string> known;
		for (const ModInfo& m : s.mods)
		{
			known.insert(m.plugin + ".*");
			for (const Feature& f : m.features) known.insert(m.plugin + "." + f.id);
		}
		std::vector<std::string> r;
		for (const Rule& rule : s.rules) if (!known.count(rule.key)) r.push_back(rule.key);
		return r;
	}

	std::vector<std::string> SectionKeys(const Snapshot& s)
	{
		std::vector<std::string> r;
		for (const ModInfo& m : s.mods)
		{
			r.push_back(m.plugin + ".*");
			for (const Feature& f : m.features) r.push_back(m.plugin + "." + f.id);
		}
		return r;
	}

	std::map<std::string, int> PlayersPerMod(const Snapshot& s)
	{
		std::map<std::string, int> r;
		for (const PlayerInfo& p : s.players)
		{
			std::set<std::string> seen;
			for (const PlayerMod& m : p.mods) if (seen.insert(m.name).second) ++r[m.name];
		}
		return r;
	}

	static std::string Lower(const std::string& s)
	{
		std::string r = s;
		for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return r;
	}

	bool MatchesFilter(const std::string& text, const std::string& filter)
	{
		if (filter.empty()) return true;
		return Lower(text).find(Lower(filter)) != std::string::npos;
	}

	bool ModMatches(const ModInfo& mod, const std::string& filter)
	{
		if (MatchesFilter(mod.plugin, filter)) return true;
		for (const Feature& f : mod.features)
			if (MatchesFilter(f.id, filter) || MatchesFilter(f.label, filter)) return true;
		return false;
	}

	bool PlayerMatches(const PlayerInfo& p, const std::string& filter)
	{
		return MatchesFilter(p.name, filter) || MatchesFilter(p.id, filter) || MatchesFilter(ParseFlags(p.flags).label, filter);
	}

	std::vector<size_t> PlayerOrder(const Snapshot& s, const std::string& filter)
	{
		std::vector<size_t> r;
		for (int pass = 0; pass < 2; ++pass)
			for (size_t i = 0; i < s.players.size(); ++i)
				if (s.players[i].online == (pass == 0) && PlayerMatches(s.players[i], filter)) r.push_back(i);
		return r;
	}

	std::vector<std::string> MinPlugins(const Snapshot& s)
	{
		std::vector<std::string> r;
		std::set<std::string> seen;
		for (const ModInfo& m : s.mods) if (seen.insert(m.plugin).second) r.push_back(m.plugin);
		for (const MinEntry& e : s.mins) if (!e.plugin.empty() && seen.insert(e.plugin).second) r.push_back(e.plugin);
		return r;
	}

	std::string CountLabel(size_t count, const char* noun)
	{
		return std::to_string(count) + " " + noun + (count == 1 ? "" : "s");
	}
}
