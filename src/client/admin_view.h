#pragma once

// Pure helpers behind the in-game admin window: the view of the server state with
// the staged changes applied, change detection per row, player flag text, filters.
// No ImGui, no game types.

#include "admin_wire/admin_wire.h"

#include <map>
#include <string>
#include <vector>

namespace AdminView
{
	// The player list flags text as booleans plus the section list and label.
	struct PlayerFlags
	{
		bool admin = false;
		bool exemptVersion = false;
		bool exemptRules = false;
		std::vector<std::string> sections;   // rule keys: Plugin.feature or Plugin.*
		std::string label;
	};

	PlayerFlags ParseFlags(const std::string& text);
	// Stored form, as the server lists it: set flags only ("admin sections=a;b label=x").
	std::string FormatFlags(const PlayerFlags& f);
	// player.set argument: the full wanted state, because the server applies it as a delta
	// ("-admin exemptversion -exemptrules sections=a;b"). The label is left out, so it stays.
	std::string FormatWireFlags(const PlayerFlags& f);

	// The snapshot with every op applied in order, as the server would. Ops that do not
	// change the snapshot (player.kick, catalog.push) and unknown ops are skipped.
	AdminWire::Snapshot ApplyStaged(const AdminWire::Snapshot& base, const std::vector<AdminWire::Op>& ops);

	// Value of a rule: "allow", "admins", "deny" or "" for none. Key "Plugin.feature" or "Plugin.*".
	std::string RuleValue(const AdminWire::Snapshot& s, const std::string& key);
	// True when the server denies key by a built-in default row and no rule names it.
	// The mod name matches without regard to case, the feature id exactly, as the loader does.
	bool IsDefaultDenied(const AdminWire::Snapshot& s, const std::string& key);
	// Minimum version: plugin empty = the loader. "" = no minimum (Off).
	std::string MinValue(const AdminWire::Snapshot& s, const std::string& plugin);
	const AdminWire::PlayerInfo* FindPlayer(const AdminWire::Snapshot& s, const std::string& id);

	// True when both ops set the same thing (a later one replaces the earlier one).
	bool SameTarget(const AdminWire::Op& a, const AdminWire::Op& b);
	// Indices of staged ops with the same target as op, highest first (safe to Unstage in order).
	std::vector<size_t> StagedWithTarget(const std::vector<AdminWire::Op>& staged, const AdminWire::Op& op);
	// True when op would leave the base snapshot as it is.
	bool IsNoop(const AdminWire::Snapshot& base, const AdminWire::Op& op);

	// Row-changed tests, base against effective.
	bool RuleChanged(const AdminWire::Snapshot& base, const AdminWire::Snapshot& eff, const std::string& key);
	bool MinChanged(const AdminWire::Snapshot& base, const AdminWire::Snapshot& eff, const std::string& plugin);
	bool PlayerChanged(const AdminWire::Snapshot& base, const AdminWire::Snapshot& eff, const std::string& id);
	bool ModChanged(const AdminWire::Snapshot& base, const AdminWire::Snapshot& eff, const AdminWire::ModInfo& mod);   // whole mod or any feature rule

	// Rule keys that match no known feature (not a mod's "Plugin.*" or "Plugin.feature").
	std::vector<std::string> UnknownRuleKeys(const AdminWire::Snapshot& s);
	// Every key the Sections picker offers: "Plugin.*" then each "Plugin.feature", per mod.
	std::vector<std::string> SectionKeys(const AdminWire::Snapshot& s);
	// Players that reported a mod, counted by plugin name.
	std::map<std::string, int> PlayersPerMod(const AdminWire::Snapshot& s);

	// Case-insensitive substring; empty filter matches all.
	bool MatchesFilter(const std::string& text, const std::string& filter);
	bool ModMatches(const AdminWire::ModInfo& mod, const std::string& filter);
	bool PlayerMatches(const AdminWire::PlayerInfo& p, const std::string& filter);
	// Indices of the players that match the filter: online first, then saved, each in list order.
	std::vector<size_t> PlayerOrder(const AdminWire::Snapshot& s, const std::string& filter);
	// Plugins the Versions tab lists: every catalog mod, then plugins that only have a minimum.
	std::vector<std::string> MinPlugins(const AdminWire::Snapshot& s);
	// "1 mod", "2 mods": the noun takes an "s" unless the count is one.
	std::string CountLabel(size_t count, const char* noun);
}
