#include "rules_model.h"

#include <algorithm>
#include <cctype>

namespace RulesModel
{
	namespace
	{
		constexpr size_t kMaxRules     = 2047;
		constexpr size_t kMaxMessage   = 200;
		constexpr size_t kMaxNameBytes = 128;

		std::string Lower(std::string s)
		{
			std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return s;
		}

		std::string Trim(const std::string& s)
		{
			const size_t a = s.find_first_not_of(" \t\r\n");
			if (a == std::string::npos) return {};
			return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
		}

		bool HasControl(const std::string& s)
		{
			return std::any_of(s.begin(), s.end(), [](unsigned char c) { return c < 0x20 || c == 0x7F; });
		}

		bool SamePlugin(const std::string& a, const std::string& b) { return Lower(a) == Lower(b); }

		bool ValidName(const std::string& s)
		{
			return !s.empty() && s.size() <= kMaxNameBytes && s[0] != '$' && !HasControl(s)
			       && s.find_first_of(" =") == std::string::npos;
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

		bool Split(const std::string& line, std::string& key, std::string& value)
		{
			const size_t eq = line.find('=');
			if (eq == std::string::npos) return false;
			key = Trim(line.substr(0, eq));
			value = Trim(line.substr(eq + 1));
			return !key.empty();
		}
	}

	bool ParseKey(const std::string& key, std::string& plugin, std::string& id, std::string& error)
	{
		const size_t dot = key.find('.');
		if (dot == std::string::npos) { error = "'" + key + "' is not Plugin.feature or Plugin.*"; return false; }
		plugin = key.substr(0, dot);
		id = key.substr(dot + 1);
		if (!ValidName(plugin)) { error = "'" + plugin + "' is not a usable mod name"; return false; }
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
			while (*p >= '0' && *p <= '9') ++p;
			++parts;
			if (*p == '.') ++p; else break;
		}
		return parts >= 2 && (*p == '\0' || *p == '-' || *p == '+') && !HasControl(version);
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
				bool deny = false;
				if (!ParseKey(key, r.plugin, r.id, error)) { reject(error); continue; }
				if (!ParseDeny(value, deny)) { reject("the value is allow or deny"); continue; }
				r.deny = deny;
				auto it = FindRule(out, r.plugin, r.id);
				if (it != out.rules.end())
				{
					warnings.push_back("[Rules] '" + key + "' is listed twice: deny wins");
					it->deny = it->deny || deny;
				}
				else out.rules.push_back(r);
			}
			else
			{
				if (!ValidName(key) || !ValidVersion(value)) { reject("expected Plugin=version"); continue; }
				auto it = std::find_if(out.modMins.begin(), out.modMins.end(), [&](const ModMin& m) { return SamePlugin(m.plugin, key); });
				if (it != out.modMins.end()) it->version = value; else out.modMins.push_back({ key, value });
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
		for (const Rule& r : m.rules) out.push_back(r.plugin + "." + r.id + "=" + (r.deny ? "deny" : "allow"));
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

	std::string RulesText(const Model& m)
	{
		std::string t = std::string("default=") + (m.defaultDeny ? "deny" : "allow") + "\n";
		if (!m.message.empty()) t += "message=" + m.message + "\n";
		for (const Rule& r : m.rules) t += r.plugin + "." + r.id + "=" + (r.deny ? "deny" : "allow") + "\n";
		return t;
	}

	std::string VersionMinimumsText(const Model& m)
	{
		std::string t;
		if (!m.minLoaderVersion.empty()) t += "$loader=" + m.minLoaderVersion + "\n";
		for (const ModMin& r : m.modMins) t += r.plugin + "=" + r.version + "\n";
		return t;
	}

	bool SetRule(Model& m, const std::string& key, bool deny, std::string& error)
	{
		Rule r;
		if (!ParseKey(key, r.plugin, r.id, error)) return false;
		r.deny = deny;
		auto it = FindRule(m, r.plugin, r.id);
		if (it != m.rules.end()) { it->deny = deny; return true; }
		if (m.rules.size() >= kMaxRules) { error = "too many rules (most 2047)"; return false; }
		m.rules.push_back(r);
		return true;
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
		if (!ValidName(plugin)) { error = "'" + plugin + "' is not a usable mod name"; return false; }
		if (!v.empty() && !ValidVersion(v)) { error = "'" + v + "' is not a version (for example 0.11.0)"; return false; }
		auto it = std::find_if(m.modMins.begin(), m.modMins.end(), [&](const ModMin& e) { return SamePlugin(e.plugin, plugin); });
		if (v.empty())
		{
			if (it == m.modMins.end()) { error = "no minimum set for " + plugin; return false; }
			m.modMins.erase(it);
			return true;
		}
		if (it != m.modMins.end()) it->version = v; else m.modMins.push_back({ plugin, v });
		return true;
	}
}
