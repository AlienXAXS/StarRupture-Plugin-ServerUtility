#pragma once

#include <string>
#include <vector>

// The host's list of players with special handling: the [Players] section of
// Plugins\config\HostRules.ini. One line per player:
//
//   <player id>=<label>|<flags>
//
// The ID is the string `player ids` prints. Flags, space separated and not
// case sensitive: Admin, ExemptVersion, ExemptRules, Sections=<key>;<key>.
// Admin implies ExemptVersion and ExemptRules. Other sections of the file
// are left as they are. Thread safe.
namespace PlayerList
{
	struct Entry
	{
		std::string              id;
		std::string              label;
		bool                     admin         = false;
		bool                     exemptVersion = false;
		bool                     exemptRules   = false;
		std::vector<std::string> sections;

		bool ExemptsVersion() const { return admin || exemptVersion; }
		bool ExemptsRules()   const { return admin || exemptRules; }
		// Flags that give something or loosen a rule: the player must prove the pairing secret first, an ID alone is not proof.
		bool NeedsProof()     const { return admin || exemptRules || !sections.empty(); }
	};

	// Pure helpers (no file or engine access).
	bool        ParseLine(const std::string& line, Entry& out);
	std::string FormatLine(const Entry& entry);
	// "<name>=a;b", or <name>="a b.x;c" when a key holds a space; empty when there are no usable keys.
	std::string SectionsFlag(const char* name, const std::vector<std::string>& keys);
	// Apply "admin -exemptversion sections=a;b label=Some Name". A label= runs to
	// the end of the text. Returns false and sets error for an unknown flag.
	bool        ApplyFlags(Entry& entry, const std::string& flags, std::string& error);
	// A usable ID: not empty, no "=", no control characters, not a comment or section line.
	bool        IsValidId(const std::string& id);

	// File-backed list.
	void               Init();                          // find the file, read it
	bool               Reload(std::string& error);
	std::vector<Entry> All();
	bool               Find(const std::string& id, Entry& out);
	// Create the entry if needed, apply flags, write the file.
	bool               Set(const std::string& id, const std::string& flags, std::string& error);
	bool               Remove(const std::string& id, std::string& error);
	// Take the Admin flag off every entry. Returns how many changed.
	bool               ClearAdmins(int& changed, std::string& error);
}
