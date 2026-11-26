#include "rules_model.h"
#include "ini_file.h"

#include <algorithm>
#include <cctype>

namespace RulesModel
{
	namespace
	{
		// Every row is an admins-only deny (denied for everyone but a proven admin). Mod names are the PluginInfo names, section ids the
		// ones each mod declares. Information-only features have no row.
		constexpr DefaultRow kDefaultRows[] = {
			{ "BetterCheats", "stack-size" },
			{ "BetterCheats", "inventory-slot-size" },
			{ "BetterCheats", "player-stats" },
			{ "BetterCheats", "movement" },
			{ "BetterCheats", "weapons" },
			{ "BetterCheats", "grenade-probe" },
			{ "BetterCheats", "mining-tools" },
			{ "BetterCheats", "inventory" },
			{ "BetterCheats", "item-spawner" },
			{ "BetterCheats", "skills" },
			{ "BetterCheats", "build-free" },
			{ "BetterCheats", "build-unlock" },
			{ "BetterCheats", "build-anywhere" },
			{ "BetterCheats", "deconstruct-windows" },
			{ "BetterCheats", "enemies" },
			{ "BetterCheats", "machine-power" },
			{ "BetterCheats", "corporations" },
			{ "BetterCheats", "rupture-wave" },
			{ "Best Builder", "build-anywhere" },
			{ "Best Builder", "deconstruct-windows" },
			{ "Best Builder", "no-build-cost" },
			{ "Best Builder", "no-stability-check" },
			{ "Best Builder", "unlock-all-recipes" },
			{ "Best Builder", "unlock-all-buildings" },
			{ "CameraControls", "vitals" },
			{ "WaveOptimiser", "heat-tools" },
			{ "BetterDrone", "mining-laser" },
			{ "BetterDrone", "map-reveal" },
			{ "BetterDrone", "drone-limits" },
			{ "BetterDrone", "always-allow" },
			{ "BetterDrone", "drone-interact" },
		};

		// The loader takes at most 2048 records, the header counts as one, and every default row is one.
		constexpr size_t kMaxRecords   = 2048;
		constexpr size_t kMaxRules     = kMaxRecords - 1 - sizeof(kDefaultRows) / sizeof(kDefaultRows[0]);
		constexpr size_t kMaxMessage   = 200;
		constexpr size_t kMaxNameBytes = 128;
		constexpr size_t kMaxPluginNameBytes = 64;
		constexpr int    kMaxVersionDigits = 9;   // keeps each number inside 32 bits

		using IniFile::Lower;
		using IniFile::Trim;

		bool HasControl(const std::string& s)
		{
			return std::any_of(s.begin(), s.end(), [](unsigned char c) { return c < 0x20 || c == 0x7F; });
		}

		bool SamePlugin(const std::string& a, const std::string& b) { return Lower(a) == Lower(b); }

		// A mod name for a minimum row. "loader" is the loader's own row in the
		// text the loader takes, so no mod can use it.
		bool ValidModMinName(const std::string& s)
		{
			return ValidPluginName(s) && !SamePlugin(s, "loader");
		}

		// "wire" is stored in lower case, as the loader's text spells it.
		std::string ModMinValue(const std::string& v)
		{
			return Lower(v) == "wire" ? std::string("wire") : v;
		}

		std::vector<Rule>::iterator FindRule(Model& m, const std::string& plugin, const std::string& id)
		{
			return std::find_if(m.rules.begin(), m.rules.end(),
				[&](const Rule& r) { return SamePlugin(r.plugin, plugin) && r.id == id; });
		}

		bool ParseDeny(const std::string& value, bool& deny)
		{
			const std::string v = Lower(value);
			if (v == "allow") { deny = false; return true; }
			if (v == "deny")  { deny = true;  return true; }
			return false;
		}

		// A [Rules] value: allow, deny or admins.
		bool ParseRuleValue(const std::string& value, bool& deny, bool& adminsOnly)
		{
			adminsOnly = Lower(value) == "admins";
			if (adminsOnly) { deny = true; return true; }
			return ParseDeny(value, deny);
		}

		const char* RuleValueText(const Rule& r) { return r.adminsOnly ? "admins" : r.deny ? "deny" : "allow"; }

		// The ServerUtility mod's own client and admin features are never ruled: no row names the mod.
		bool IsOwnMod(const std::string& plugin) { return SamePlugin(plugin, "ServerUtility"); }

		bool Split(const std::string& line, std::string& key, std::string& value)
		{
			const size_t eq = line.find('=');
			if (eq == std::string::npos) return false;
			key = Trim(line.substr(0, eq));
			value = Trim(line.substr(eq + 1));
			return !key.empty();
		}
	}

	const std::vector<DefaultRow>& DefaultRows()
	{
		static const std::vector<DefaultRow> rows(std::begin(kDefaultRows), std::end(kDefaultRows));
		return rows;
	}

	bool IsDefaultRow(const std::string& plugin, const std::string& id)
	{
		return std::any_of(DefaultRows().begin(), DefaultRows().end(),
			[&](const DefaultRow& d) { return SamePlugin(d.plugin, plugin) && id == d.id; });
	}

	std::vector<DefaultRow> ActiveDefaultRows(const Model& m)
	{
		std::vector<DefaultRow> out;
		for (const DefaultRow& d : DefaultRows())
		{
			const bool named = std::any_of(m.rules.begin(), m.rules.end(),
				[&](const Rule& r) { return SamePlugin(r.plugin, d.plugin) && r.id == d.id; });
			if (!named) out.push_back(d);
		}
		return out;
	}

	bool ValidName(const std::string& s)
	{
		return !s.empty() && s.size() <= kMaxNameBytes && s[0] != '$' && s[0] != '#' && s[0] != ';' && !HasControl(s)
		       && s.find_first_of(" =") == std::string::npos;
	}

	// A mod name, as the mod loader allows it: letters, digits, '_' and '-',
	// with single spaces between words ("Best Builder"), 1 to 64 bytes.
	bool ValidPluginName(const std::string& s)
	{
		if (s.empty() || s.size() > kMaxPluginNameBytes || s.front() == ' ' || s.back() == ' ')
			return false;
		for (size_t i = 0; i < s.size(); ++i)
		{
			const unsigned char c = static_cast<unsigned char>(s[i]);
			const bool word = (c < 0x80 && std::isalnum(c)) || c == '_' || c == '-';
			if (!word && !(c == ' ' && s[i - 1] != ' '))
				return false;
		}
		return true;
	}

	bool ParseKey(const std::string& key, std::string& plugin, std::string& id, std::string& error)
	{
		const size_t dot = key.find('.');
		if (dot == std::string::npos) { error = "'" + key + "' is not Plugin.feature or Plugin.*"; return false; }
		plugin = key.substr(0, dot);
		id = key.substr(dot + 1);
		if (id.find('.') != std::string::npos)
		{
			error = "'" + key + "' has more than one dot; a rule key splits at the first dot, so a mod name with a dot cannot be used";
			return false;
		}
		if (!ValidPluginName(plugin)) { error = "'" + plugin + "' is not a usable mod name"; return false; }
		if (!ValidName(id))     { error = "'" + id + "' is not a usable feature name"; return false; }
		return true;
	}

	bool ValidVersion(const std::string& version)
	{
		const char* p = version.c_str();
		if (*p == 'v' || *p == 'V') ++p;
		int parts = 0;
		while (parts < 4 && *p >= '0' && *p <= '9')
		{
			int digits = 0;
			while (*p >= '0' && *p <= '9') { ++p; ++digits; }
			if (digits > kMaxVersionDigits)
				return false;
			++parts;
			// A dot belongs to the version only when another number follows it.
			if (*p == '.' && p[1] >= '0' && p[1] <= '9') ++p; else break;
		}
		return parts >= 2 && (*p == '\0' || *p == '-' || *p == '+') && !HasControl(version);
	}

	bool ValidSectionKey(const std::string& key)
	{
		if (key.empty() || key[0] == '#') return false;
		for (const char ch : key)
		{
			const unsigned char c = static_cast<unsigned char>(ch);
			if (c < 0x20 || c == 0x7f || c == '"' || c == '|' || c == '=' || c == ';' || c == '[' || c == ']')
				return false;
		}
		return true;
	}

	bool ValidModMinValue(const std::string& value)
	{
		return ValidVersion(value) || Lower(value) == "wire";
	}

	void Parse(const std::vector<std::string>& lines, Model& out, std::vector<std::string>& warnings)
	{
		out = Model{};
		enum { None, Policy, Rules, Mins } section = None;
		for (const std::string& raw : lines)
		{
			const std::string line = Trim(raw);
			if (line.empty() || line[0] == ';' || line[0] == '#') continue;
			if (line[0] == '[')
			{
				const std::string h = Lower(line);
				section = h == "[policy]" ? Policy : h == "[rules]" ? Rules : h == "[minversions]" ? Mins : None;
				continue;
			}
			if (section == None) continue;

			std::string key, value, error;
			std::vector<std::string>& kept = section == Policy ? out.policyKept : section == Rules ? out.rulesKept : out.modMinKept;
			const char* name = section == Policy ? "[Policy]" : section == Rules ? "[Rules]" : "[MinVersions]";
			auto reject = [&](const std::string& why)
			{
				warnings.push_back(std::string(name) + " '" + line + "': " + why + " (ignored, kept in the file)");
				kept.push_back(line);
			};
			if (!Split(line, key, value)) { reject("not key=value"); continue; }

			if (section == Policy)
			{
				const std::string k = Lower(key);
				bool deny = false;
				if (k == "default")
				{
					if (ParseDeny(value, deny)) out.defaultDeny = deny; else reject("Default is allow or deny");
				}
				else if (k == "message")
				{
					if (value.size() <= kMaxMessage && !HasControl(value)) out.message = value;
					else reject("Message must be one line of up to 200 bytes");
				}
				else if (k == "minloaderversion")
				{
					if (value.empty()) {}
					else if (ValidVersion(value)) out.minLoaderVersion = value;
					else reject("not a version");
				}
				else if (k == "adminprompt") kept.push_back(line);   // read by a later part
				else reject("unknown key");
			}
			else if (section == Rules)
			{
				Rule r;
				bool deny = false, adminsOnly = false;
				if (!ParseKey(key, r.plugin, r.id, error)) { reject(error); continue; }
				if (!ParseRuleValue(value, deny, adminsOnly)) { reject("the value is allow, deny or admins"); continue; }
				r.deny = deny;
				r.adminsOnly = adminsOnly;
				auto it = FindRule(out, r.plugin, r.id);
				if (it != out.rules.end())
				{
					warnings.push_back("[Rules] '" + key + "' is listed twice: the stricter value wins");
					// deny beats admins, admins beats allow.
					if (deny && !adminsOnly) { it->deny = true; it->adminsOnly = false; }
					else if (adminsOnly && !it->deny) { it->deny = true; it->adminsOnly = true; }
				}
				else out.rules.push_back(r);
			}
			else
			{
				if (!ValidModMinName(key) || !ValidModMinValue(value)) { reject("expected Plugin=version or Plugin=wire"); continue; }
				const std::string stored = ModMinValue(value);
				auto it = std::find_if(out.modMins.begin(), out.modMins.end(), [&](const ModMin& m) { return SamePlugin(m.plugin, key); });
				if (it != out.modMins.end()) it->version = stored; else out.modMins.push_back({ key, stored });
			}
		}
	}

	std::vector<std::string> PolicyLines(const Model& m)
	{
		std::vector<std::string> out;
		out.push_back(std::string("Default=") + (m.defaultDeny ? "deny" : "allow"));
		if (!m.message.empty()) out.push_back("Message=" + m.message);
		out.push_back("MinLoaderVersion=" + m.minLoaderVersion);
		out.insert(out.end(), m.policyKept.begin(), m.policyKept.end());
		return out;
	}

	std::vector<std::string> RuleLines(const Model& m)
	{
		std::vector<std::string> out;
		for (const Rule& r : m.rules) out.push_back(r.plugin + "." + r.id + "=" + RuleValueText(r));
		out.insert(out.end(), m.rulesKept.begin(), m.rulesKept.end());
		return out;
	}

	std::vector<std::string> ModMinLines(const Model& m)
	{
		std::vector<std::string> out;
		for (const ModMin& r : m.modMins) out.push_back(r.plugin + "=" + r.version);
		out.insert(out.end(), m.modMinKept.begin(), m.modMinKept.end());
		return out;
	}

	std::string RulesText(const Model& m, bool remote)
	{
		std::string t = std::string("default=") + (m.defaultDeny ? "deny" : "allow") + "\n";
		if (!m.message.empty()) t += "message=" + m.message + "\n";
		// The loader gets plain allow or deny: "admins only" is a deny here, and a proven admin's
		// own text (PlayerRulesText) turns it into an allow.
		for (const Rule& r : m.rules)
			if (!IsOwnMod(r.plugin)) t += r.plugin + "." + r.id + "=" + (r.deny ? "deny" : "allow") + "\n";
		if (remote)
			for (const DefaultRow& d : ActiveDefaultRows(m))
				t += std::string(d.plugin) + "." + d.id + "=deny\n";
		return t;
	}

	PlayerFlags FlagsFor(bool admin, bool exemptRules, const std::vector<std::string>& sections, bool proven)
	{
		PlayerFlags f;
		if (proven)
		{
			f.admin = admin;
			f.exemptRules = exemptRules;
			f.sections = sections;
		}
		return f;
	}

	bool NeedsOwnRules(const PlayerFlags& f)
	{
		return f.admin || f.exemptRules || !f.sections.empty();
	}

	std::string PlayerRulesText(const Model& m, const PlayerFlags& f, bool remote)
	{
		if (!NeedsOwnRules(f))
			return RulesText(m, remote);

		// Nothing denied: the default, the message and no rows.
		if (f.exemptRules)
		{
			std::string t = "default=allow\n";
			if (!m.message.empty()) t += "message=" + m.message + "\n";
			return t;
		}

		std::vector<Rule> rules = m.rules;
		// A proven admin: "admins only" rows are allows. The built-in default rows are left out
		// (RulesText below adds none for an admin), so the host's own rules decide for them.
		if (f.admin)
			for (Rule& r : rules)
				if (r.adminsOnly) { r.deny = false; r.adminsOnly = false; }
		std::vector<Rule> own;   // the player's keys, checked and split
		for (const std::string& key : f.sections)
		{
			Rule r;
			std::string error;
			if (ParseKey(key, r.plugin, r.id, error)) own.push_back(r);
		}

		// A whole-mod key opens every denied row of that mod first. Allow rows stay,
		// so an exact allow that opened a default row keeps doing so.
		for (const Rule& o : own)
		{
			if (o.id != "*") continue;
			rules.erase(std::remove_if(rules.begin(), rules.end(),
				[&](const Rule& r) { return SamePlugin(r.plugin, o.plugin) && (r.deny || r.id == "*"); }), rules.end());
			rules.push_back({ o.plugin, "*", false });
		}
		for (const Rule& o : own)
		{
			if (o.id == "*") continue;
			const bool covered = std::any_of(rules.begin(), rules.end(),
				[&](const Rule& r) { return SamePlugin(r.plugin, o.plugin) && r.id == "*" && !r.deny; });
			auto it = std::find_if(rules.begin(), rules.end(),
				[&](const Rule& r) { return SamePlugin(r.plugin, o.plugin) && r.id == o.id; });
			if (it != rules.end()) it->deny = false;
			// A default row stays denied under a covering Plugin.*, so the exact key is written.
			else if (!covered || m.defaultDeny || IsDefaultRow(o.plugin, o.id)) rules.push_back({ o.plugin, o.id, false });
		}

		Model own2 = m;
		own2.rules = rules;
		return RulesText(own2, remote && !f.admin);
	}

	std::string VersionMinimumsText(const Model& m)
	{
		std::string t;
		if (!m.minLoaderVersion.empty()) t += "loader=" + m.minLoaderVersion + "\n";
		for (const ModMin& r : m.modMins) t += r.plugin + "=" + r.version + "\n";
		return t;
	}

	namespace
	{
		bool SetRuleValue(Model& m, const std::string& key, bool deny, bool adminsOnly, std::string& error)
		{
			Rule r;
			if (!ParseKey(key, r.plugin, r.id, error)) return false;
			r.deny = deny;
			r.adminsOnly = adminsOnly;
			auto it = FindRule(m, r.plugin, r.id);
			if (it != m.rules.end()) { it->deny = deny; it->adminsOnly = adminsOnly; return true; }
			if (m.rules.size() >= kMaxRules) { error = "too many rules (most " + std::to_string(kMaxRules) + ")"; return false; }
			m.rules.push_back(r);
			return true;
		}
	}

	bool SetRule(Model& m, const std::string& key, bool deny, std::string& error)
	{
		return SetRuleValue(m, key, deny, false, error);
	}

	bool SetRuleAdminsOnly(Model& m, const std::string& key, std::string& error)
	{
		return SetRuleValue(m, key, true, true, error);
	}

	bool UnsetRule(Model& m, const std::string& key, std::string& error)
	{
		std::string plugin, id;
		if (!ParseKey(key, plugin, id, error)) return false;
		auto it = FindRule(m, plugin, id);
		if (it == m.rules.end()) { error = "no rule for " + key; return false; }
		m.rules.erase(it);
		return true;
	}

	bool SetDefault(Model& m, bool deny) { m.defaultDeny = deny; return true; }

	bool SetMessage(Model& m, const std::string& text, std::string& error)
	{
		const std::string t = Trim(text);
		if (t.size() > kMaxMessage || HasControl(t)) { error = "the message is one line of up to 200 bytes"; return false; }
		m.message = t;
		return true;
	}

	bool SetMinLoader(Model& m, const std::string& v, std::string& error)
	{
		if (!v.empty() && !ValidVersion(v)) { error = "'" + v + "' is not a version (for example 1.27.0)"; return false; }
		m.minLoaderVersion = v;
		return true;
	}

	bool SetModMin(Model& m, const std::string& plugin, const std::string& v, std::string& error)
	{
		if (!ValidModMinName(plugin)) { error = "'" + plugin + "' is not a usable mod name"; return false; }
		if (!v.empty() && !ValidModMinValue(v)) { error = "'" + v + "' is not a version (for example 0.11.0) or wire"; return false; }
		auto it = std::find_if(m.modMins.begin(), m.modMins.end(), [&](const ModMin& e) { return SamePlugin(e.plugin, plugin); });
		if (v.empty())
		{
			if (it == m.modMins.end()) { error = "no minimum set for " + plugin; return false; }
			m.modMins.erase(it);
			return true;
		}
		if (it != m.modMins.end()) it->version = ModMinValue(v); else m.modMins.push_back({ plugin, ModMinValue(v) });
		return true;
	}

	HeldStep NextHeldStep(bool alive, bool idRead, unsigned long long heldMs)
	{
		if (!alive) return HeldStep::Drop;
		if (idRead) return HeldStep::Act;
		return heldMs > kLateIdGraceMs ? HeldStep::ActNoId : HeldStep::Wait;
	}
}
