#include "known_features.h"

#include "rules_model.h"
#include "ini_file.h"
#include "../admin_wire/admin_wire.h"

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
	const char* const kSourceDeclared = "declared";

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

		constexpr size_t kMaxTitle = 80;

		// The row as the catalog keeps it: a title cut to kMaxTitle bytes on a UTF-8 boundary.
		void Fit(Report& r)
		{
			r.title = AdminWire::CleanText(r.title, kMaxTitle);
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

		// The pre-release part of a version: what follows the first '-', up to a '+'.
		// Empty when there is none.
		std::string PreRelease(const std::string& v)
		{
			const size_t at = v.find_first_of("-+");
			if (at == std::string::npos || v[at] == '+') return std::string();
			const size_t end = v.find('+', at);
			return v.substr(at + 1, end == std::string::npos ? std::string::npos : end - at - 1);
		}

		bool AllDigits(const std::string& s)
		{
			return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
		}

		// SemVer 2.0.0 section 11 on two pre-release parts, both non-empty: identifiers
		// split on '.', numbers by value, a number below text, text by byte, and when
		// all else is equal the shorter list is lower.
		int ComparePreRelease(const std::string& a, const std::string& b)
		{
			const std::vector<std::string> x = AdminWire::Split(a, '.'), y = AdminWire::Split(b, '.');
			for (size_t i = 0; i < x.size() && i < y.size(); ++i)
			{
				const bool nx = AllDigits(x[i]), ny = AllDigits(y[i]);
				int c;
				if (nx && ny)
				{
					const size_t zx = x[i].find_first_not_of('0'), zy = y[i].find_first_not_of('0');
					const std::string px = zx == std::string::npos ? std::string() : x[i].substr(zx);
					const std::string py = zy == std::string::npos ? std::string() : y[i].substr(zy);
					c = px.size() != py.size() ? (px.size() < py.size() ? -1 : 1) : px.compare(py);
				}
				else if (nx != ny)
					c = nx ? -1 : 1;
				else
					c = x[i].compare(y[i]);
				if (c != 0) return c < 0 ? -1 : 1;
			}
			return x.size() == y.size() ? 0 : (x.size() < y.size() ? -1 : 1);
		}

		bool ValidDate(const std::string& d)
		{
			if (d.size() != 10) return false;
			for (size_t i = 0; i < 10; ++i)
				if (i == 4 || i == 7 ? d[i] != '-' : !std::isdigit(static_cast<unsigned char>(d[i]))) return false;
			return true;
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
		if (x.empty() != y.empty()) return x.empty() ? -1 : 1;
		if (x.empty()) return 0;
		const std::string pa = PreRelease(a), pb = PreRelease(b);
		if (pa.empty() != pb.empty()) return pa.empty() ? 1 : -1;   // a release is above its pre-releases
		return pa.empty() ? 0 : ComparePreRelease(pa, pb);
	}

	std::string ModVersion(const std::vector<Entry>& rows)
	{
		const Entry* best = nullptr;
		for (const Entry& e : rows)
		{
			if (e.version.empty()) continue;
			if (!best) { best = &e; continue; }
			const bool eDecl = e.source == kSourceDeclared, bDecl = best->source == kSourceDeclared;
			if (eDecl != bDecl) { if (eDecl) best = &e; continue; }
			if (eDecl && e.lastSeen != best->lastSeen) { if (e.lastSeen > best->lastSeen) best = &e; continue; }
			if (eDecl && e.declaredSeq != best->declaredSeq) { if (e.declaredSeq > best->declaredSeq) best = &e; continue; }
			if (CompareVersions(e.version, best->version) > 0) best = &e;
		}
		return best ? best->version : std::string();
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

	std::vector<Report> ParseDeclaredRows(const std::vector<std::string>& rows)
	{
		std::vector<Report> out;
		for (const std::string& row : rows)
		{
			if (out.size() >= kMaxRowsPerReport) break;
			const std::vector<std::string> f = AdminWire::Split(row, '	');
			if (f.size() != 5 || (f[3] != "on" && f[3] != "off")) continue;
			Report r;
			r.plugin = f[0]; r.version = f[1]; r.id = f[2]; r.off = f[3] == "off"; r.title = f[4];
			r.recordOff = false;   // the push carries no game build
			r.source = kSourceDeclared;
			Fit(r);
			out.push_back(std::move(r));
		}
		return out;
	}

	// A full catalog drops the reported row seen longest ago, so a new feature can
	// still get in; rows a plugin declared stay. False when every row is declared.
	bool Catalog::EvictOldestReported()
	{
		auto oldest = m_rows.end();
		for (auto it = m_rows.begin(); it != m_rows.end(); ++it)
		{
			if (it->second.source != kSourceReported) continue;
			if (oldest == m_rows.end() || it->second.lastSeen < oldest->second.lastSeen) oldest = it;
		}
		if (oldest == m_rows.end()) return false;
		m_rows.erase(oldest);
		return true;
	}

	bool Catalog::Merge(const Report& rIn, const std::string& today, bool* visible)
	{
		Report r = rIn;
		bool seen = false;
		bool& shown = visible ? *visible : seen;
		shown = false;
		Fit(r);
		Entry e;
		e.plugin = Clean(r.plugin);
		e.id = Clean(r.id);
		if (!RulesModel::ValidPluginName(e.plugin) || !RulesModel::ValidName(e.id) || !ValidDate(today))
			return false;
		e.title = Clean(r.title);
		e.version = Clean(r.version);
		if (!e.version.empty() && !RulesModel::ValidVersion(e.version))
		{
			// A plugin built without a tag reports "dev": keep the row, without a version.
			e.version.clear();
			if (m_versionWarned.insert(Lower(e.plugin)).second)
				m_newVersionWarn.push_back(e.plugin);
		}
		if (r.off && r.recordOff)
			e.offBuilds.insert(r.build.empty() ? kUnknownBuild : CleanBuild(r.build));
		e.lastSeen = today;
		e.source = r.source.empty() ? kSourceReported : Clean(r.source);
		if (e.source == kSourceDeclared && !m_loading)
			e.declaredSeq = ++m_declaredSeq;

		const std::string key = Lower(e.plugin + "." + e.id);
		auto it = m_rows.find(key);
		if (it == m_rows.end())
		{
			if (m_rows.size() >= kMaxRows && !EvictOldestReported())
				return false;
			m_rows.emplace(key, std::move(e));
			shown = true;
			return true;
		}

		Entry& cur = it->second;
		bool changed = false;
		if (cur.source == kSourceReported && e.source != kSourceReported)
		{
			cur.source = e.source;   // a loader declared what a player had only reported
			changed = true;
		}
		if (e.declaredSeq != 0)
			cur.declaredSeq = e.declaredSeq;
		// A declared row is what an admin's loader is running now, so it replaces the
		// stored version, older or not. A reported row has no version and only fills in.
		const bool running = e.source == kSourceDeclared && !e.version.empty() && e.version != cur.version;
		const int cmp = running ? 1 : CompareVersions(e.version, cur.version);
		if (cmp > 0)
		{
			cur.version = e.version;
			changed = shown = true;
		}
		if (!e.title.empty() && e.title != cur.title && (cmp > 0 || cur.title.empty()))
		{
			cur.title = e.title;
			changed = shown = true;
		}
		for (const std::string& b : e.offBuilds)
			if (cur.offBuilds.insert(b).second) changed = shown = true;
		if (e.lastSeen > cur.lastSeen)
		{
			cur.lastSeen = e.lastSeen;
			changed = true;   // kept on disk, but nothing a window shows
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

	std::vector<std::string> Catalog::TakeVersionWarnings()
	{
		std::vector<std::string> out;
		out.swap(m_newVersionWarn);
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
		m_loading = true;
		std::istringstream lines(text);
		std::string line;
		bool first = true;
		while (std::getline(lines, line))
		{
			if (!line.empty() && line.back() == '\r') line.pop_back();
			if (first) { first = false; if (line.rfind("known-features", 0) == 0) continue; }
			if (line.empty()) continue;

			const std::vector<std::string> f = AdminWire::Split(line, '\t');
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
				for (const std::string& b : AdminWire::Split(f[5], ','))
				{
					r.off = true;
					r.build = b;
					Merge(r, f[6]);
				}
		}
		m_loading = false;
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

	std::string UnmatchedWarnings(const std::vector<std::string>& unmatched, size_t cap)
	{
		std::string out;
		for (size_t i = 0; i < unmatched.size() && i < cap; ++i)
			out += "  warning: no remembered feature matches the rule " + unmatched[i] + "\n";
		if (unmatched.size() > cap)
			out += "  ... and " + std::to_string(unmatched.size() - cap) + " more\n";
		return out;
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
		WarnSink    g_warn = nullptr;

		// Logs, once per plugin, a version the catalog could not read. Lock not held.
		void WarnVersions(const std::vector<std::string>& plugins)
		{
			if (!g_warn) return;
			for (const std::string& p : plugins)
				g_warn("[KnownFeatures] " + p + " reports a version that is not a version number; its features are listed without one");
		}

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
		std::vector<std::string> warn;
		{
			std::lock_guard<std::mutex> lk(g_mutex);
			g_path = path;
			g_catalog = Catalog();
			g_dirty = false;
			if (read)
				g_catalog.Parse(text);
			warn = g_catalog.TakeVersionWarnings();
		}
		WarnVersions(warn);
	}

	void SetWarnSink(WarnSink sink)
	{
		std::lock_guard<std::mutex> lk(g_mutex);
		g_warn = sink;
	}

	void NoteReportText(const std::string& text)
	{
		const std::vector<Report> reports = ParseReportText(text, std::string());
		const std::string today = Today();
		std::vector<std::string> warn;
		{
			std::lock_guard<std::mutex> lk(g_mutex);
			const size_t n = std::min(reports.size(), kMaxRowsPerReport);
			for (size_t i = 0; i < n; ++i)
				g_dirty |= g_catalog.Merge(reports[i], today);
			warn = g_catalog.TakeVersionWarnings();
		}
		WarnVersions(warn);
	}

	bool NoteDeclaredRows(const std::vector<std::string>& rows)
	{
		const std::vector<Report> reports = ParseDeclaredRows(rows);
		const std::string today = Today();
		bool shown = false;
		std::vector<std::string> warn;
		{
			std::lock_guard<std::mutex> lk(g_mutex);
			for (const Report& r : reports)
			{
				bool one = false;
				g_dirty |= g_catalog.Merge(r, today, &one);
				shown |= one;
			}
			warn = g_catalog.TakeVersionWarnings();
		}
		WarnVersions(warn);
		return shown;
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
