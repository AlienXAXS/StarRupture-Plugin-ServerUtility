#include "player_list.h"
#include "ini_file.h"
#include "rules_model.h"
#include "../plugin_helpers.h"
#include "../rcon/commands/word_split.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <mutex>

namespace
{
	using IniFile::Lower;
	using IniFile::Trim;
	using IniFile::IsCommentOrBlank;

	std::mutex                      g_mutex;
	std::vector<PlayerList::Entry>  g_entries;
	// False when the file exists but could not be read: nothing is written
	// then, so a bad read never replaces the host's file with an empty list.
	bool                            g_loadOk = true;

	std::string CleanLabel(std::string s)
	{
		for (char& c : s)
			if (c == '|' || static_cast<unsigned char>(c) < 0x20) c = ' ';
		return Trim(s);
	}

	bool IsAnyHeader(const std::string& line)
	{
		const std::string t = Trim(line);
		return !t.empty() && t[0] == '[';
	}

	bool IsPlayersHeader(const std::string& line)
	{
		return Lower(Trim(line)) == "[players]";
	}

	// strict: unknown flags are an error (typed commands); not strict: they are
	// skipped (a hand-edited file may carry flags from a newer version).
	bool ApplyFlagsImpl(PlayerList::Entry& e, const std::string& flags, std::string& error, bool strict)
	{
		// Words as the console takes them: a double-quoted part may hold spaces,
		// so a mod name with a space is written Sections="Best Builder.x;Other.y".
		std::string rest = flags;
		while (true)
		{
			const size_t a = rest.find_first_not_of(' ');
			if (a == std::string::npos) break;
			rest.erase(0, a);

			if (Lower(rest.substr(0, 6)) == "label=")
			{
				e.label = CleanLabel(rest.substr(6));
				break;
			}

			const std::string tok = WordSplit::TakeWord(rest);
			if (tok.empty()) continue;
			const bool remove = tok[0] == '-';
			const std::string low = Lower(remove ? tok.substr(1) : tok);

			if (low == "admin")              e.admin = !remove;
			else if (low == "exemptversion") e.exemptVersion = !remove;
			else if (low == "exemptrules")   e.exemptRules = !remove;
			else if (low == "label" && remove) e.label.clear();
			else if (low == "sections" && remove) e.sections.clear();
			else if (!remove && low.rfind("sections=", 0) == 0)
			{
				e.sections.clear();
				const std::string list = tok.substr(tok.size() - low.size() + 9);
				size_t s = 0;
				while (s <= list.size())
				{
					size_t sep = list.find(';', s);
					if (sep == std::string::npos) sep = list.size();
					const std::string key = Trim(list.substr(s, sep - s));
					if (key.empty()) { s = sep + 1; continue; }
					if (!RulesModel::ValidSectionKey(key))
					{
						if (strict) { error = "a section key may not hold control characters, quotes, | = ; [ ], or start with #"; return false; }
					}
					else e.sections.push_back(key);
					s = sep + 1;
				}
			}
			else if (strict)
			{
				error = "unknown flag '" + tok + "' (admin, exemptversion, exemptrules, sections=<key>;<key>, label=<text>)";
				return false;
			}
		}
		return true;
	}

	std::vector<PlayerList::Entry> ParseSection(const std::vector<std::string>& lines)
	{
		std::vector<PlayerList::Entry> out;
		bool inSection = false;
		for (const std::string& line : lines)
		{
			if (IsAnyHeader(line)) { inSection = IsPlayersHeader(line); continue; }
			if (!inSection || IsCommentOrBlank(line)) continue;

			PlayerList::Entry e;
			if (!PlayerList::ParseLine(line, e)) continue;

			auto dup = std::find_if(out.begin(), out.end(), [&](const PlayerList::Entry& o) { return o.id == e.id; });
			if (dup != out.end()) *dup = e; else out.push_back(std::move(e));
		}
		return out;
	}

	// Write the file with a fresh [Players] section and every other line as it
	// was. Temp file first, then a rename over the old file.
	bool SaveFile(const std::vector<PlayerList::Entry>& entries, std::string& error)
	{
		const std::wstring& path = IniFile::Path();
		if (path.empty()) { error = "no rules file path"; return false; }
		if (!g_loadOk) { error = "the rules file could not be read, so it was not changed"; return false; }

		std::lock_guard<std::mutex> fileLock(IniFile::FileMutex());
		std::vector<std::string> lines;
		bool exists = false, crlf = true;
		if (!IniFile::ReadLines(path, lines, exists, crlf)) { error = "the rules file could not be read"; return false; }

		std::vector<std::string> body;
		for (const PlayerList::Entry& e : entries) body.push_back(PlayerList::FormatLine(e));
		IniFile::ReplaceSection(lines, "Players", body);

		if (!IniFile::WriteLines(path, lines, crlf)) { error = "could not write the rules file"; return false; }
		return true;
	}

	// Run a change on a copy; keep it only when the file was written.
	template <typename Fn>
	bool Change(std::string& error, Fn fn)
	{
		std::lock_guard<std::mutex> lk(g_mutex);
		std::vector<PlayerList::Entry> next = g_entries;
		if (!fn(next)) return false;
		if (!SaveFile(next, error)) return false;
		g_entries = std::move(next);
		return true;
	}
}

namespace PlayerList
{
	bool IsValidId(const std::string& id)
	{
		if (id.empty() || id.size() > 200) return false;
		if (id[0] == ';' || id[0] == '#' || id[0] == '[') return false;
		if (id.find('=') != std::string::npos) return false;
		return std::none_of(id.begin(), id.end(), [](unsigned char c) { return c < 0x20; });
	}

	bool ParseLine(const std::string& line, Entry& out)
	{
		const size_t eq = line.find('=');
		if (eq == std::string::npos) return false;

		Entry e;
		e.id = Trim(line.substr(0, eq));
		if (!IsValidId(e.id)) return false;

		const std::string value = line.substr(eq + 1);
		const size_t bar = value.rfind('|');
		std::string error;
		if (bar == std::string::npos)
			e.label = CleanLabel(value);
		else
		{
			e.label = CleanLabel(value.substr(0, bar));
			ApplyFlagsImpl(e, Trim(value.substr(bar + 1)), error, false);
		}
		out = std::move(e);
		return true;
	}

	std::string FormatLine(const Entry& e)
	{
		std::string flags;
		auto add = [&](const std::string& f) { if (!flags.empty()) flags += ' '; flags += f; };
		if (e.admin)         add("Admin");
		if (e.exemptVersion) add("ExemptVersion");
		if (e.exemptRules)   add("ExemptRules");
		if (!e.sections.empty())
		{
			std::string list;
			for (const std::string& s : e.sections)
			{
				if (!RulesModel::ValidSectionKey(s)) continue;   // never write a key that could break the line
				if (!list.empty()) list += ';';
				list += s;
			}
			// A key with a space (a mod name like "Best Builder") needs the quotes to stay one word.
			if (!list.empty()) add(list.find(' ') == std::string::npos ? "Sections=" + list : "Sections=\"" + list + "\"");
		}
		return e.id + "=" + e.label + (flags.empty() ? "" : "|" + flags);
	}

	bool ApplyFlags(Entry& entry, const std::string& flags, std::string& error)
	{
		return ApplyFlagsImpl(entry, flags, error, true);
	}

	void Init()
	{
		std::string error;
		if (!Reload(error))
			LOG_WARN("[HostRules] Player list not read: %s", error.c_str());
	}

	bool Reload(std::string& error)
	{
		std::lock_guard<std::mutex> lk(g_mutex);
		const std::wstring& path = IniFile::Path();
		if (path.empty()) { error = "the rules file path is unknown"; return false; }

		std::lock_guard<std::mutex> fileLock(IniFile::FileMutex());   // same order as SaveFile
		std::vector<std::string> lines;
		bool exists = false, crlf = true;
		if (!IniFile::ReadLines(path, lines, exists, crlf))
		{
			g_loadOk = false;
			error = "the rules file could not be read";
			return false;
		}
		g_loadOk = true;
		g_entries = ParseSection(lines);

		int admins = 0;
		for (const Entry& e : g_entries) if (e.admin) ++admins;
		LOG_INFO("[HostRules] Player list: %d entries, %d admins", static_cast<int>(g_entries.size()), admins);
		return true;
	}

	std::vector<Entry> All()
	{
		std::lock_guard<std::mutex> lk(g_mutex);
		return g_entries;
	}

	bool Find(const std::string& rawId, Entry& out)
	{
		const std::string id = Trim(rawId);
		std::lock_guard<std::mutex> lk(g_mutex);
		for (const Entry& e : g_entries)
			if (e.id == id) { out = e; return true; }
		return false;
	}

	bool Set(const std::string& rawId, const std::string& flags, std::string& error)
	{
		const std::string id = Trim(rawId);
		if (!IsValidId(id)) { error = "not a usable player ID"; return false; }

		return Change(error, [&](std::vector<Entry>& list)
		{
			auto it = std::find_if(list.begin(), list.end(), [&](const Entry& e) { return e.id == id; });
			if (it == list.end()) { list.emplace_back(); list.back().id = id; it = list.end() - 1; }
			Entry updated = *it;
			if (!ApplyFlags(updated, flags, error)) return false;
			*it = updated;
			return true;
		});
	}

	bool Remove(const std::string& rawId, std::string& error)
	{
		const std::string id = Trim(rawId);
		return Change(error, [&](std::vector<Entry>& list)
		{
			auto it = std::find_if(list.begin(), list.end(), [&](const Entry& e) { return e.id == id; });
			if (it == list.end()) { error = "no entry with that ID"; return false; }
			list.erase(it);
			return true;
		});
	}

	bool ClearAdmins(int& changed, std::string& error)
	{
		changed = 0;
		return Change(error, [&](std::vector<Entry>& list)
		{
			for (Entry& e : list)
				if (e.admin) { e.admin = false; ++changed; }
			return true;
		});
	}
}
