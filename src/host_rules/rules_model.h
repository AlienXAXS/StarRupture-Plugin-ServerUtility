#pragma once

#include <string>
#include <vector>

// The host's rules: the [Policy], [Rules] and [MinVersions] sections of
// Plugins\config\HostRules.ini, and the text the mod loader takes from them.
// Pure code (no file, engine or loader access) so it can be tested alone.
namespace RulesModel
{
	struct Rule   { std::string plugin; std::string id; bool deny = false; };   // id "*" = the whole mod
	struct ModMin { std::string plugin; std::string version; };

	struct Model
	{
		bool        defaultDeny = false;
		std::string message;
		std::string minLoaderVersion;   // empty = off
		std::vector<Rule>   rules;
		std::vector<ModMin> modMins;
		// Lines of each section that were not read (typos, keys of a later
		// version, AdminPrompt). Kept as they are when the file is rewritten.
		std::vector<std::string> policyKept, rulesKept, modMinKept;
	};

	// Read the three sections from the file's lines. Anything not understood
	// is kept and described in warnings.
	void Parse(const std::vector<std::string>& lines, Model& out, std::vector<std::string>& warnings);

	// The lines to write into each section (without the header).
	std::vector<std::string> PolicyLines(const Model& m);
	std::vector<std::string> RuleLines(const Model& m);
	std::vector<std::string> ModMinLines(const Model& m);

	// The built-in default rows: sections a server denies without being told to.
	// Each is an exact Plugin.section deny row in the text sent to players, so it
	// beats Plugin.* and [Policy] Default; only an exact [Rules] line (allow or
	// deny), or a player's own exact Sections= key, takes its place. Admins and
	// ExemptRules players get none. Mod names match without case, section ids exactly (as the loader does).
	// One list, in rules_model.cpp: add a row there.
	struct DefaultRow { const char* plugin; const char* id; };
	const std::vector<DefaultRow>& DefaultRows();
	// True when plugin.id is one of the default rows.
	bool IsDefaultRow(const std::string& plugin, const std::string& id);
	// The default rows in force: those no [Rules] line names exactly.
	std::vector<DefaultRow> ActiveDefaultRows(const Model& m);

	// What SetRules takes: default=, message=, one Plugin.feature=allow|deny per rule.
	// remote: the text goes to remote connections, so the default rows are added.
	// False (a set for the listen host's own player, or for no connection) adds none.
	std::string RulesText(const Model& m, bool remote);
	// What one player's entry changes in the rules that player is sent.
	struct PlayerFlags
	{
		bool exemptRules = false;               // Admin counts as this
		std::vector<std::string> sections;      // Plugin.feature or Plugin.* keys allowed for this player
	};
	// The flags a connection gets from its list entry. An ID alone is not proof, so a connection
	// that has not proved the pairing secret as that ID gets none: the default rules apply.
	PlayerFlags FlagsFor(bool exemptRules, const std::vector<std::string>& sections, bool proven);
	// True when the player needs a rules set of their own (a flag changes the result).
	bool NeedsOwnRules(const PlayerFlags& f);
	// The rules text for one player: the global rules with the flags applied.
	// It stands alone (default and message too) because the loader's per-player
	// set replaces the global one. ExemptRules: nothing denied. Sections: those
	// keys are allowed, ahead of any global rule for the same mod.
	std::string PlayerRulesText(const Model& m, const PlayerFlags& f, bool remote);

	// What SetVersionMinimums takes: loader=version first (when set), then one
	// Plugin=version line per mod minimum. Empty when every minimum is off.
	std::string VersionMinimumsText(const Model& m);

	// "Plugin.feature" or "Plugin.*": split and checked.
	bool ParseKey(const std::string& key, std::string& plugin, std::string& id, std::string& error);
	// A feature id, and a mod name as the mod loader allows it.
	bool ValidName(const std::string& s);
	bool ValidPluginName(const std::string& s);
	// 0.11.0, v0.11.0, 1.27.0-dev.1: at least major.minor, up to four numbers.
	bool ValidVersion(const std::string& version);
	// A key in a player's Sections= list: spaces are allowed (a mod name can hold
	// one); no control character, '"', "|", "=", ";", "[" or "]", and not
	// starting with "#". Anything else could end the
	// flag list or start a line of its own in the rules file.
	bool ValidSectionKey(const std::string& key);
	// A mod minimum: a version, or "wire" (the copy must be one the wire check accepts).
	bool ValidModMinValue(const std::string& value);

	// Changes. Each returns false with an error and leaves the model as it was.
	bool SetRule(Model& m, const std::string& key, bool deny, std::string& error);
	bool UnsetRule(Model& m, const std::string& key, std::string& error);
	bool SetDefault(Model& m, bool deny);
	bool SetMessage(Model& m, const std::string& text, std::string& error);
	bool SetMinLoader(Model& m, const std::string& versionOrEmpty, std::string& error);
	bool SetModMin(Model& m, const std::string& plugin, const std::string& versionOrEmpty, std::string& error);
	// A player's ID is read from the player state, which can arrive a moment after
	// the loader calls a plugin about that player. Work that needs the ID is held
	// and tried again each tick for this long; after that the player is treated
	// as having no entry in the player list.
	constexpr unsigned long long kLateIdGraceMs = 10000;

	enum class HeldStep
	{
		Drop,    // the controller is gone: forget it
		Wait,    // the ID is still unread and the grace has not run out
		Act,     // the ID reads: do the work
		ActNoId  // the grace ran out with no ID: do the work without one
	};
	// The next step for one held controller, from whether it still exists, whether
	// its ID reads now and how long it has been held.
	HeldStep NextHeldStep(bool alive, bool idRead, unsigned long long heldMs);
}
