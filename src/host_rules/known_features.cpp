#include "known_features.h"

#include "rules_model.h"
#include "ini_file.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

namespace KnownFeatures
{
	const char* const kSourceReported = "reported";

	namespace
	{
		const char* const kHeader = "known-features 1";
		const char* const kUnknownBuild = "unknown";

		using IniFile::Lower;

		// One line, no tab: what the file format needs.
		std::string Clean(const std::string& s)
		{
			std::string out;
			for (const char c : s)
				out += (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) ? ' ' : c;
			const size_t a = out.find_first_not_of(' ');
			if (a == std::string::npos) return std::string();
			return out.substr(a, out.find_last_not_of(' ') - a + 1);
		}

		std::string CleanBuild(const std::string& s)
		{
			std::string out = Clean(s);
			std::replace(out.begin(), out.end(), ',', ' ');
			return out;
		}

		// Numbers of a version, at most four; empty when it has none.
		std::vector<long> Numbers(const std::string& v)
		{
			std::vector<long> n;
			size_t i = (!v.empty() && (v[0] == 'v' || v[0] == 'V')) ? 1 : 0;
			while (i < v.size() && n.size() < 4 && std::isdigit(static_cast<unsigned char>(v[i])))
			{
				long x = 0;
				int digits = 0;
				while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i])))
				{
					if (digits++ < 9) x = x * 10 + (v[i] - '0');
					++i;
				}
				n.push_back(x);
				if (i + 1 < v.size() && v[i] == '.' && std::isdigit(static_cast<unsigned char>(v[i + 1]))) ++i; else break;
			}
			return n;
		}

		bool ValidDate(const std::string& d)
		{
			if (d.size() != 10) return false;
			for (size_t i = 0; i < 10; ++i)
				if (i == 4 || i == 7 ? d[i] != '-' : !std::isdigit(static_cast<unsigned char>(d[i]))) return false;
			return true;
		}

		std::vector<std::string> Split(const std::string& s, char sep)
		{
			std::vector<std::string> parts;
			size_t start = 0;
			for (;;)
			{
				const size_t at = s.find(sep, start);
				parts.push_back(s.substr(start, at == std::string::npos ? std::string::npos : at - start));
				if (at == std::string::npos) return parts;
				start = at + 1;
			}
		}

		std::string Today()
		{
			const std::time_t t = std::time(nullptr);
			std::tm tm = {};
			localtime_s(&tm, &t);
			char buf[16];
			std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
			return buf;
		}
	}

	int CompareVersions(const std::string& a, const std::string& b)
	{
		const std::vector<long> x = Numbers(a), y = Numbers(b);
		for (size_t i = 0; i < 4; ++i)
		{
			const long p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
			if (p != q) return p < q ? -1 : 1;
		}
		return x.empty() == y.empty() ? 0 : (x.empty() ? -1 : 1);
	}

	std::vector<Report> ParseReportText(const std::string& text, const std::string& build)
	{
		std::vector<Report> out;
		std::istringstream lines(text);
		std::string line;
		bool first = true;
		while (std::getline(lines, line))
		{
			if (!line.empty() && line.back() == '\r') line.pop_back();
			if (first) { first = false; continue; }   // loader=<tag> interface=<n>

			const size_t eq = line.find('=');
			if (eq == std::string::npos) continue;
			const std::string key = line.substr(0, eq), state = line.substr(eq + 1);
			const size_t dot = key.find('.');
			if (dot == std::string::npos || dot == 0 || dot + 1 >= key.size()) continue;
			if (state != "on" && state != "off") continue;

			Report r;
			r.plugin = key.substr(0, dot);
			r.id = key.substr(dot + 1);
			r.off = state == "off";
			r.build = build;
			out.push_back(std::move(r));
		}
		return out;
	}

	bool Catalog::Merge(const Report& r, const std::string& today)
	{
		Entry e;
		e.plugin = Clean(r.plugin);
		e.id = Clean(r.id);
		if (!RulesModel::ValidPluginName(e.plugin) || !RulesModel::ValidName(e.id) || !ValidDate(today))
			return false;
		e.title = Clean(r.title);
		e.version = Clean(r.version);
		if (r.off)
			e.offBuilds.insert(r.build.empty() ? kUnknownBuild : CleanBuild(r.build));
		e.lastSeen = today;
		e.source = r.source.empty() ? kSourceReported : Clean(r.source);

		const std::string key = Lower(e.plugin + "." + e.id);
		auto it = m_rows.find(key);
		if (it == m_rows.end())
		{
			if (m_rows.size() >= kMaxRows)
				return false;
			m_rows.emplace(key, std::move(e));
			return true;
		}

		Entry& cur = it->second;
		bool changed = false;
		const int cmp = CompareVersions(e.version, cur.version);
		if (cmp > 0)
		{
			cur.version = e.version;
			changed = true;
		}
		if (!e.title.empty() && e.title != cur.title && (cmp > 0 || cur.title.empty()))
		{
			cur.title = e.title;
			changed = true;
		}
		for (const std::string& b : e.offBuilds)
			changed |= cur.offBuilds.insert(b).second;
		if (e.lastSeen > cur.lastSeen)
		{
			cur.lastSeen = e.lastSeen;
			changed = true;
		}
		return changed;
	}

	std::vector<Entry> Catalog::Entries() const
	{
		std::vector<Entry> out;
		out.reserve(m_rows.size());
		for (const auto& kv : m_rows)
			out.push_back(kv.second);
		return out;
	}

	std::string Catalog::Serialize() const
	{
		std::string out = std::string(kHeader) + "\n";
		for (const auto& kv : m_rows)
		{
			const Entry& e = kv.second;
			std::string builds;
			for (const std::string& b : e.offBuilds)
				builds += (builds.empty() ? "" : ",") + b;
			out += "feature\t" + e.plugin + "\t" + e.version + "\t" + e.id + "\t" + e.title + "\t" + builds + "\t" +
			       e.lastSeen + "\t" + e.source + "\n";
		}
		return out;
	}

	int Catalog::Parse(const std::string& text)
	{
		int skipped = 0;
		std::istringstream lines(text);
		std::string line;
		bool first = true;
		while (std::getline(lines, line))
		{
			if (!line.empty() && line.back() == '\r') line.pop_back();
			if (first) { first = false; if (line.rfind("known-features", 0) == 0) continue; }
			if (line.empty()) continue;

			const std::vector<std::string> f = Split(line, '\t');
			if (f.size() != 8 || f[0] != "feature" || f[1].empty() || f[3].empty() || !ValidDate(f[6]) || f[7].empty())
			{
				++skipped;
				continue;
			}

			Report r;
			r.plugin = f[1]; r.version = f[2]; r.id = f[3]; r.title = f[4]; r.source = f[7];
			if (f[5].empty())
				Merge(r, f[6]);
			else
				for (const std::string& b : Split(f[5], ','))
				{
					r.off = true;
					r.build = b;
					Merge(r, f[6]);
				}
		}
		return skipped;
	}

	bool LoadFile(const std::wstring& path, std::string& text)
	{
		text.clear();
		if (path.empty()) return false;
		std::ifstream f(std::filesystem::path(path), std::ios::binary);
		if (!f) return false;
		std::ostringstream ss;
		ss << f.rdbuf();
		text = ss.str();
		return true;
	}

	std::vector<std::string> UnmatchedKeys(const std::vector<std::string>& ruleKeys, const std::vector<Entry>& known)
	{
		std::vector<std::string> out;
		for (const std::string& key : ruleKeys)
		{
			const size_t dot = key.find('.');
			if (dot == std::string::npos) continue;
			const std::string plugin = Lower(key.substr(0, dot)), id = Lower(key.substr(dot + 1));
			const bool hit = std::any_of(known.begin(), known.end(), [&](const Entry& e)
			{
				return Lower(e.plugin) == plugin && (id == "*" || Lower(e.id) == id);
			});
			if (!hit) out.push_back(key);
		}
		return out;
	}

	// ---- the plugin's catalog ----
	namespace
	{
		std::mutex  g_mutex;
		Catalog     g_catalog;
		Debounce    g_debounce;
		std::wstring g_path;
		bool        g_dirty = false;

		// Writes the catalog; the lock is not held during the write.
		void Write(uint64_t nowMs)
		{
			std::string text;
			{
				std::lock_guard<std::mutex> lk(g_mutex);
				text = g_catalog.Serialize();
				g_dirty = false;
				g_debounce.Wrote(nowMs);
			}
			if (!IniFile::WriteTextFile(g_path, text))
			{
				std::lock_guard<std::mutex> lk(g_mutex);
				g_dirty = true;   // tried again after the next interval
			}
		}
	}

	void Init(const std::wstring& path)
	{
		std::string text;
		const bool read = LoadFile(path, text);
		std::lock_guard<std::mutex> lk(g_mutex);
		g_path = path;
		g_catalog = Catalog();
		g_dirty = false;
		if (read)
			g_catalog.Parse(text);
	}

	void NoteReportText(const std::string& text)
	{
		const std::vector<Report> reports = ParseReportText(text, std::string());
		const std::string today = Today();
		std::lock_guard<std::mutex> lk(g_mutex);
		const size_t n = std::min(reports.size(), kMaxRowsPerReport);
		for (size_t i = 0; i < n; ++i)
			g_dirty |= g_catalog.Merge(reports[i], today);
	}

	void Tick(uint64_t nowMs)
	{
		{
			std::lock_guard<std::mutex> lk(g_mutex);
			if (g_path.empty() || !g_debounce.Due(g_dirty, nowMs))
				return;
		}
		Write(nowMs);
	}

	void Flush()
	{
		{
			std::lock_guard<std::mutex> lk(g_mutex);
			if (g_path.empty() || !g_dirty)
				return;
		}
		Write(0);
	}

	std::vector<Entry> Snapshot()
	{
		std::lock_guard<std::mutex> lk(g_mutex);
		return g_catalog.Entries();
	}
}
