#include "player_list.h"
#include "../plugin_helpers.h"

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
	std::mutex                      g_mutex;
	std::wstring                    g_path;
	std::vector<PlayerList::Entry>  g_entries;
	// False when the file exists but could not be read: nothing is written
	// then, so a bad read never replaces the host's file with an empty list.
	bool                            g_loadOk = true;

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

	std::string CleanLabel(std::string s)
	{
		for (char& c : s)
			if (c == '|' || static_cast<unsigned char>(c) < 0x20) c = ' ';
		return Trim(s);
	}

	bool IsCommentOrBlank(const std::string& line)
	{
		const std::string t = Trim(line);
		return t.empty() || t[0] == ';' || t[0] == '#';
	}

	bool IsPlayersHeader(const std::string& line)
	{
		return Lower(Trim(line)) == "[players]";
	}

	bool IsAnyHeader(const std::string& line)
	{
		const std::string t = Trim(line);
		return !t.empty() && t[0] == '[';
	}

	// strict: unknown flags are an error (typed commands); not strict: they are
	// skipped (a hand-edited file may carry flags from a newer version).
	bool ApplyFlagsImpl(PlayerList::Entry& e, const std::string& flags, std::string& error, bool strict)
	{
		const size_t n = flags.size();
		size_t pos = 0;
		while (pos < n)
		{
			while (pos < n && flags[pos] == ' ') ++pos;
			if (pos >= n) break;

			size_t end = flags.find(' ', pos);
			if (end == std::string::npos) end = n;

			const std::string tok = flags.substr(pos, end - pos);
			const bool remove = tok[0] == '-';
			const std::string low = Lower(remove ? tok.substr(1) : tok);

			if (!remove && low.rfind("label=", 0) == 0)
			{
				e.label = CleanLabel(flags.substr(pos + (tok.size() - low.size()) + 6));
				break;
			}

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
					if (!key.empty()) e.sections.push_back(key);
					s = sep + 1;
				}
			}
			else if (strict)
			{
				error = "unknown flag '" + tok + "' (admin, exemptversion, exemptrules, sections=<key>;<key>, label=<text>)";
				return false;
			}
			pos = end;
		}
		return true;
	}

	bool ReadFileLines(const std::wstring& path, std::vector<std::string>& lines, bool& exists, bool& crlf)
	{
		lines.clear();
		crlf = true;

		const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
		                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE)
		{
			const DWORD err = GetLastError();
			exists = !(err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND);
			return !exists;
		}
		exists = true;

		std::string data;
		char buf[8192];
		DWORD got = 0;
		bool ok = true;
		while (true)
		{
			if (!ReadFile(h, buf, sizeof(buf), &got, nullptr)) { ok = false; break; }
			if (got == 0) break;
			data.append(buf, got);
		}
		CloseHandle(h);
		if (!ok) return false;

		if (data.compare(0, 3, "\xEF\xBB\xBF") == 0) data.erase(0, 3);
		crlf = data.find("\r\n") != std::string::npos || data.find('\n') == std::string::npos;

		size_t s = 0;
		while (s < data.size())
		{
			size_t e = data.find('\n', s);
			if (e == std::string::npos) e = data.size();
			std::string line = data.substr(s, e - s);
			if (!line.empty() && line.back() == '\r') line.pop_back();
			lines.push_back(std::move(line));
			s = e + 1;
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
		if (g_path.empty()) { error = "no rules file path"; return false; }
		if (!g_loadOk) { error = "the rules file could not be read, so it was not changed"; return false; }

		std::vector<std::string> old;
		bool exists = false, crlf = true;
		if (!ReadFileLines(g_path, old, exists, crlf)) { error = "the rules file could not be read"; return false; }

		std::vector<std::string> out;
		bool wrote = false;
		bool inSection = false;
		auto emit = [&]()
		{
			for (const PlayerList::Entry& e : entries) out.push_back(PlayerList::FormatLine(e));
			wrote = true;
		};
		std::vector<std::string> keep; // comment lines of the old section
		for (const std::string& line : old)
		{
			if (IsAnyHeader(line))
			{
				if (inSection) { out.insert(out.end(), keep.begin(), keep.end()); keep.clear(); emit(); }
				inSection = IsPlayersHeader(line) && !wrote;
				out.push_back(line);
				continue;
			}
			if (!inSection) { out.push_back(line); continue; }
			if (IsCommentOrBlank(line)) keep.push_back(line);
		}
		if (inSection) { out.insert(out.end(), keep.begin(), keep.end()); emit(); }
		if (!wrote)
		{
			if (!out.empty() && !Trim(out.back()).empty()) out.push_back({});
			out.push_back("[Players]");
			emit();
		}

		const std::wstring dir = g_path.substr(0, g_path.find_last_of(L"\\/"));
		CreateDirectoryW(dir.c_str(), nullptr);

		std::string text;
		for (const std::string& l : out) { text += l; text += crlf ? "\r\n" : "\n"; }

		const std::wstring tmp = g_path + L".tmp";
		const HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE) { error = "could not write the rules file"; return false; }
		DWORD put = 0;
		const bool wroteOk = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &put, nullptr) && put == text.size()
		                     && FlushFileBuffers(h);
		CloseHandle(h);
		if (!wroteOk || !MoveFileExW(tmp.c_str(), g_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			DeleteFileW(tmp.c_str());
			error = "could not write the rules file";
			return false;
		}
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
			for (const std::string& s : e.sections) { if (!list.empty()) list += ';'; list += s; }
			add("Sections=" + list);
		}
		return e.id + "=" + e.label + (flags.empty() ? "" : "|" + flags);
	}

	bool ApplyFlags(Entry& entry, const std::string& flags, std::string& error)
	{
		return ApplyFlagsImpl(entry, flags, error, true);
	}

	void Init()
	{
		wchar_t module[MAX_PATH] = {};
		HMODULE self = nullptr;
		if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                       reinterpret_cast<LPCWSTR>(&PlayerList::Init), &self)
		    && GetModuleFileNameW(self, module, MAX_PATH) > 0)
		{
			std::wstring dir(module);
			dir.resize(dir.find_last_of(L"\\/"));
			std::lock_guard<std::mutex> lk(g_mutex);
			g_path = dir + L"\\config\\HostRules.ini";
		}

		std::string error;
		if (!Reload(error))
			LOG_WARN("[HostRules] Player list not read: %s", error.c_str());
	}

	bool Reload(std::string& error)
	{
		std::lock_guard<std::mutex> lk(g_mutex);
		if (g_path.empty()) { error = "the rules file path is unknown"; return false; }

		std::vector<std::string> lines;
		bool exists = false, crlf = true;
		if (!ReadFileLines(g_path, lines, exists, crlf))
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

	bool Find(const std::string& id, Entry& out)
	{
		std::lock_guard<std::mutex> lk(g_mutex);
		for (const Entry& e : g_entries)
			if (e.id == id) { out = e; return true; }
		return false;
	}

	bool Set(const std::string& id, const std::string& flags, std::string& error)
	{
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

	bool Remove(const std::string& id, std::string& error)
	{
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
