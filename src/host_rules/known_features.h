#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

// The remembered catalog of mod features: every feature list a player's mod
// loader reported, kept across restarts in Plugins\ServerUtility\known-features.txt.
// The model (Catalog) has no game or file dependency; the free functions at the
// bottom hold the one instance the plugin uses.
namespace KnownFeatures
{
	// Where a row came from. "published" is for a later signed catalog.
	extern const char* const kSourceReported;
	extern const char* const kSourceDeclared;   // pushed by the admin client from the loader's declared list

	struct Entry
	{
		std::string           plugin;
		std::string           id;
		std::string           title;       // empty when no report carried one
		std::string           version;     // newest seen; empty when none was reported
		std::set<std::string> offBuilds;   // game builds that reported it off
		std::string           lastSeen;    // YYYY-MM-DD
		std::string           source;
		uint64_t              declaredSeq = 0;   // order this process merged a declared row in (not saved); 0 = none
	};

	// Limits: rows one client's report may add, and rows the catalog keeps.
	constexpr size_t kMaxRowsPerReport = 64;
	constexpr size_t kMaxRows          = 512;

	// One feature as a report or a catalog names it.
	struct Report
	{
		std::string plugin, id, title, version;
		std::string build;                 // game build of the reporter; empty = unknown
		bool        off = false;
		bool        recordOff = true;      // false: keep the state out of offBuilds (no known build)
		std::string source;                // empty = reported
	};

	// -1, 0 or 1. A leading v is ignored. The numbers compare first; when they are
	// equal a pre-release (after a -) is below the release and pre-releases follow
	// SemVer 2.0.0 section 11 (dev.9 < rc.2 < rc.10 < none); a + part is ignored. A
	// version with no numbers is older than any with some.
	int CompareVersions(const std::string& a, const std::string& b);

	// "<Plugin>.<id>=on|off" lines after the first ("loader=... interface=..."
	// line), as the mod loader lists them. A line that does not fit is skipped.
	std::vector<Report> ParseReportText(const std::string& text, const std::string& build);

	// Rows the admin client pushes (catalog.push), one per feature, tab separated:
	// "<plugin>	<version>	<id>	<on|off>	<title>". Rows that do not fit are
	// skipped; at most kMaxRowsPerReport are read.
	std::vector<Report> ParseDeclaredRows(const std::vector<std::string>& rows);

	// The version a mod shows: that of its declared row last seen newest (by date, then by
	// the order this process merged them), else the highest by CompareVersions. rows are one plugin's entries; empty when none
	// has a version.
	std::string ModVersion(const std::vector<Entry>& rows);

	class Catalog
	{
	public:
		// True when the catalog changed. A reported row raises the version to the
		// newest seen; a declared row (what an admin's loader runs now) sets it, down
		// as well as up. Rows with a bad plugin name or feature id,
		// are refused; a new row in a full catalog replaces the reported row with
		// the oldest lastSeen (declared rows stay, and are refused when all are declared); a title is cut to 80
		// bytes. today is YYYY-MM-DD. *visible, when given, is set when something a
		// window shows changed (a new row, version, title or off build) - a newer
		// lastSeen alone is a change to save, not to show.
		bool Merge(const Report& r, const std::string& today, bool* visible = nullptr);
		// The plugins whose version could not be read since the last call, each named
		// once for the life of the catalog. Their rows are kept, with no version.
		std::vector<std::string> TakeVersionWarnings();
		// Sorted by plugin, then id (case ignored).
		std::vector<Entry> Entries() const;
		size_t Size() const { return m_rows.size(); }

		// One text line per feature, after a header line.
		std::string Serialize() const;
		// Merges the lines in; returns how many it skipped as unreadable.
		int Parse(const std::string& text);

	private:
		bool EvictOldestReported();
		std::map<std::string, Entry> m_rows;   // lower-case "plugin.id"
		std::set<std::string> m_versionWarned;   // lower-case plugin names
		uint64_t m_declaredSeq = 0;   // the last declaredSeq given out
		bool     m_loading = false;   // Parse is merging saved rows: they get no declaredSeq
		std::vector<std::string> m_newVersionWarn;
	};

	// At most one write per interval. Not thread safe: one owner.
	class Debounce
	{
	public:
		static constexpr uint64_t kIntervalMs = 10000;
		bool Due(bool dirty, uint64_t nowMs) const { return dirty && (!m_wrote || nowMs - m_lastMs >= kIntervalMs); }
		void Wrote(uint64_t nowMs) { m_wrote = true; m_lastMs = nowMs; }
	private:
		bool     m_wrote = false;
		uint64_t m_lastMs = 0;
	};

	// At most one announcement of a catalog change per interval. The first change goes out at
	// once; changes inside the interval are held and go out as one when it ends (Due, from the
	// tick). Not thread safe: one owner (the game thread).
	class PushGate
	{
	public:
		static constexpr uint64_t kIntervalMs = 1000;
		// A change happened. True: announce it now.
		bool Changed(uint64_t nowMs)
		{
			if (!m_sent || nowMs - m_lastMs >= kIntervalMs)
			{
				m_sent = true; m_lastMs = nowMs; m_held = false;
				return true;
			}
			m_held = true;
			return false;
		}
		// True when a held change is due now.
		bool Due(uint64_t nowMs)
		{
			if (!m_held || nowMs - m_lastMs < kIntervalMs) return false;
			m_held = false; m_lastMs = nowMs;
			return true;
		}
		bool Held() const { return m_held; }
	private:
		bool     m_sent = false;
		bool     m_held = false;
		uint64_t m_lastMs = 0;
	};

	// False when the file does not exist or could not be read (text left empty).
	bool LoadFile(const std::wstring& path, std::string& text);

	// Rule keys ("Plugin.feature" or "Plugin.*") that no remembered feature matches.
	std::vector<std::string> UnmatchedKeys(const std::vector<std::string>& ruleKeys, const std::vector<Entry>& known);
	// One warning line per unmatched key, at most `cap`, then "and N more".
	std::string UnmatchedWarnings(const std::vector<std::string>& unmatched, size_t cap);

	// ---- the plugin's catalog ----
	// Load the file once. An empty path keeps the catalog in memory only.
	void Init(const std::wstring& path);
	// Where a catalog warning goes (one line); none by default. The lock is not held when it runs.
	using WarnSink = void (*)(const std::string&);
	void SetWarnSink(WarnSink sink);
	// Merge a mod loader's feature text (game thread: where reports arrive).
	void NoteReportText(const std::string& text);
	// Merge rows pushed by an admin client's mod loader. True when something a
	// window shows changed (not for a newer lastSeen alone).
	bool NoteDeclaredRows(const std::vector<std::string>& rows);
	// Write the file when something changed and 10 s passed. Call from the tick;
	// it does no I/O unless a write is due.
	void Tick(uint64_t nowMs);
	// Write any change now (shutdown).
	void Flush();
	// A copy of every remembered feature, safe from any thread. The wire snapshot
	// pages through this.
	std::vector<Entry> Snapshot();
}
