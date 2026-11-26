#pragma once

#include <string>
#include <utility>
#include <vector>

// The host's rules: the [Policy], [Rules] and [MinVersions] sections of
// Plugins\config\HostRules.ini, and the text the mod loader takes from them.
// Pure code (no file, engine or loader access) so it can be tested alone.
namespace RulesModel
{
	// id "*" = the whole mod. adminsOnly (file value "admins") is a deny that a proven admin does not
	// get, so deny is true whenever adminsOnly is: anything that reads only deny sees the plain deny.
	struct Rule   { std::string plugin; std::string id; bool deny = false; bool adminsOnly = false; };
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
	// beats Plugin.* and [Policy] Default for everyone but a proven admin (who gets none, so the
	// host's other rules decide); only an exact [Rules] line (allow, deny or admins), or a
	// player's own exact Sections= key, takes its place. ExemptRules players get none. Mod names match without case, section ids exactly (as the loader does).
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
		bool exemptRules = false;               // the ExemptRules flag (Admin does not imply it)
		bool admin       = false;               // a proven admin: "admins only" rows are allows for them
		std::vector<std::string> sections;      // Plugin.feature or Plugin.* keys allowed for this player
	};
	// The flags a connection gets from its list entry. An ID alone is not proof, so a connection
	// that has not proved the pairing secret as that ID gets none: the default rules apply.
	PlayerFlags FlagsFor(bool admin, bool exemptRules, const std::vector<std::string>& sections, bool proven);
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
	// The same with the third value: "admins only" (deny for everyone but a proven admin).
	bool SetRuleAdminsOnly(Model& m, const std::string& key, std::string& error);
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

	// A listed admin whose admin check is pending gets the admin's rule set at once
	// (admins-only rows allowed), so they never see those features switch off and
	// on again at join. The hold starts at the join and ends at the first of:
	// the check passed (the normal set takes over, the same text), no pass within
	// kProofHoldMs of the client's feature list arriving (the client needs the list
	// before it can answer, so the time is counted from it), kProofHoldCapMs from the
	// join when no list ever arrives, the player is no longer a listed admin, the
	// check was refused, the client has no ServerUtility client part, or the player
	// left. Then the player is on the server rules like anyone else, so the
	// admins-only rows are denied. A rules reload does not end the hold: the hold is
	// checked again and the admin's set is sent again with the new rules. Ending it
	// there would bring back the off and on switch for a real admin.
	constexpr unsigned long long kProofHoldMs   = 8000;
	constexpr unsigned long long kProofHoldCapMs = 30000;
	constexpr unsigned long long kNoFeatures     = ~0ull;   // "since the feature list": it has not arrived

	// What the host knows about one held connection. listedAdmin: the list entry has Admin.
	// secretOnFile: the ID has paired before, so the client can answer a challenge by itself
	// (a first pairing waits for a person to type a code: nothing is pending, no hold).
	// clientHasTool: the client reported the ServerUtility client part (known once its feature
	// list has arrived; not asked before).
	struct HoldInputs { bool listedAdmin = false, secretOnFile = false, proven = false, clientHasTool = false; };
	// True while the hold applies. sinceJoinMs: time since the join; sinceFeaturesMs: time since
	// the feature list arrived, or kNoFeatures.
	bool ProofHoldOn(const HoldInputs& in, unsigned long long sinceJoinMs, unsigned long long sinceFeaturesMs);
	// The flags a held connection gets: the admin's own, nothing else (a proven
	// admin with ExemptRules or Sections= gets those when the check passes).
	PlayerFlags HoldFlags();
	// "Admins only (default)" is true of a built-in default row only while the host's
	// own rules leave it to admins: not under default=deny and not under a Plugin.*=deny.
	bool DefaultRowAdminsOnly(const Model& m, const std::string& plugin);

	// Per-connection records, keyed by the connection and never by the player's
	// ID: two connections can read the same ID (a player and one who copied it),
	// and when one of them leaves the other keeps its record.
	template <class Key, class Value>
	struct PerConnection
	{
		struct Row { Key key; Value value; };
		std::vector<Row> rows;

		Value* Find(const Key& key)
		{
			for (Row& r : rows)
				if (r.key == key) return &r.value;
			return nullptr;
		}
		Value& Ensure(const Key& key)
		{
			if (Value* v = Find(key)) return *v;
			rows.push_back({ key, Value() });
			return rows.back().value;
		}
		void Erase(const Key& key)
		{
			for (size_t i = 0; i < rows.size(); ++i)
				if (rows[i].key == key) { rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(i)); return; }
		}
	};

	enum class HoldStep
	{
		NotHeld,   // no hold for this connection (never began, or already over)
		Held,      // the hold applies: send the held set
		Ended      // the hold ended just now and its record is gone: send the normal set
	};

	// The join holds, one row per held connection. The host's join, feature list, rules refresh,
	// refusal and leave paths all go through this, so the tests run what ships.
	template <class Key>
	class JoinHolds
	{
	public:
		// The player joined (PostLogin) and is held.
		void Begin(const Key& key, unsigned long long nowMs)
		{
			Row* r = Find(key);
			if (!r) { m_rows.push_back({ key, nowMs, 0, false }); return; }
			*r = { key, nowMs, 0, false };
		}
		bool Has(const Key& key) const { return Find(key) != nullptr; }
		size_t Size() const { return m_rows.size(); }
		// The connection's feature list arrived. Only the first one counts.
		void FeaturesArrived(const Key& key, unsigned long long nowMs)
		{
			Row* r = Find(key);
			if (r && !r->featuresSeen) { r->featuresSeen = true; r->featuresMs = nowMs; }
		}
		// Decide for one connection now. A hold that is over is dropped here.
		HoldStep Step(const Key& key, unsigned long long nowMs, const HoldInputs& in)
		{
			Row* r = Find(key);
			if (!r) return HoldStep::NotHeld;
			if (ProofHoldOn(in, nowMs - r->joinedMs, r->featuresSeen ? nowMs - r->featuresMs : kNoFeatures))
				return HoldStep::Held;
			Drop(key);
			return HoldStep::Ended;
		}
		// End the hold for any other reason (a refused check, the player left). True when there was one.
		bool End(const Key& key)
		{
			if (!Find(key)) return false;
			Drop(key);
			return true;
		}
		// The connections whose time is up, for the host to end (their inputs may say more).
		std::vector<Key> Expired(unsigned long long nowMs) const
		{
			std::vector<Key> out;
			for (const Row& r : m_rows)
				if (nowMs - r.joinedMs >= kProofHoldCapMs || (r.featuresSeen && nowMs - r.featuresMs >= kProofHoldMs))
					out.push_back(r.key);
			return out;
		}

	private:
		struct Row { Key key; unsigned long long joinedMs; unsigned long long featuresMs; bool featuresSeen; };
		Row* Find(const Key& key)
		{
			for (Row& r : m_rows) if (r.key == key) return &r;
			return nullptr;
		}
		const Row* Find(const Key& key) const
		{
			for (const Row& r : m_rows) if (r.key == key) return &r;
			return nullptr;
		}
		void Drop(const Key& key)
		{
			for (size_t i = 0; i < m_rows.size(); ++i)
				if (m_rows[i].key == key) { m_rows.erase(m_rows.begin() + static_cast<std::ptrdiff_t>(i)); return; }
		}
		std::vector<Row> m_rows;
	};
}
