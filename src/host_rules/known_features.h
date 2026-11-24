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

	struct Entry
	{
		std::string           plugin;
		std::string           id;
		std::string           title;       // empty when no report carried one
		std::string           version;     // newest seen; empty when none was reported
		std::set<std::string> offBuilds;   // game builds that reported it off
		std::string           lastSeen;    // YYYY-MM-DD
		std::string           source;
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
		std::string source;                // empty = reported
	};

	// -1, 0 or 1. A leading v and anything from a - or + on are ignored; a
	// version with no numbers is older than any with some.
	int CompareVersions(const std::string& a, const std::string& b);

	// "<Plugin>.<id>=on|off" lines after the first ("loader=... interface=..."
	// line), as the mod loader lists them. A line that does not fit is skipped.
	std::vector<Report> ParseReportText(const std::string& text, const std::string& build);

	// Rows the admin client pushes (catalog.push), one per feature, tab separated:
	// "<plugin>	<version>	<id>	<on|off>	<title>". Rows that do not fit are
	// skipped; at most kMaxRowsPerReport are read.
	std::vector<Report> ParseDeclaredRows(const std::vector<std::string>& rows);

	class Catalog
	{
	public:
		// True when the catalog changed. Rows with a bad plugin name or feature id,
		// and new rows past kMaxRows, are refused. today is YYYY-MM-DD.
		bool Merge(const Report& r, const std::string& today);
		// Sorted by plugin, then id (case ignored).
		std::vector<Entry> Entries() const;
		size_t Size() const { return m_rows.size(); }

		// One text line per feature, after a header line.
		std::string Serialize() const;
		// Merges the lines in; returns how many it skipped as unreadable.
		int Parse(const std::string& text);

	private:
		std::map<std::string, Entry> m_rows;   // lower-case "plugin.id"
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

	// False when the file does not exist or could not be read (text left empty).
	bool LoadFile(const std::wstring& path, std::string& text);

	// Rule keys ("Plugin.feature" or "Plugin.*") that no remembered feature matches.
	std::vector<std::string> UnmatchedKeys(const std::vector<std::string>& ruleKeys, const std::vector<Entry>& known);
	// One warning line per unmatched key, at most `cap`, then "and N more".
	std::string UnmatchedWarnings(const std::vector<std::string>& unmatched, size_t cap);

	// ---- the plugin's catalog ----
	// Load the file once. An empty path keeps the catalog in memory only.
	void Init(const std::wstring& path);
	// Merge a mod loader's feature text (game thread: where reports arrive).
	void NoteReportText(const std::string& text);
	// Merge rows pushed by an admin client's mod loader. True when the catalog changed.
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
