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

	// What SetRules takes: default=, message=, one Plugin.feature=allow|deny per rule.
	std::string RulesText(const Model& m);
	// What SetVersionMinimums will take: one Plugin=version per line, the loader
	// minimum first as $loader=version. Not read by any loader yet.
	std::string VersionMinimumsText(const Model& m);

	// "Plugin.feature" or "Plugin.*": split and checked.
	bool ParseKey(const std::string& key, std::string& plugin, std::string& id, std::string& error);
	// 0.11.0, v0.11.0, 1.27.0-dev.1: at least major.minor, up to four numbers.
	bool ValidVersion(const std::string& version);

	// Changes. Each returns false with an error and leaves the model as it was.
	bool SetRule(Model& m, const std::string& key, bool deny, std::string& error);
	bool UnsetRule(Model& m, const std::string& key, std::string& error);
	bool SetDefault(Model& m, bool deny);
	bool SetMessage(Model& m, const std::string& text, std::string& error);
	bool SetMinLoader(Model& m, const std::string& versionOrEmpty, std::string& error);
	bool SetModMin(Model& m, const std::string& plugin, const std::string& versionOrEmpty, std::string& error);
}
