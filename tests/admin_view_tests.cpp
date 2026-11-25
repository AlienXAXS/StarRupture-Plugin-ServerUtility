// Unit tests of the admin window helpers (no game needed).
// From a Visual Studio developer prompt, in the repo root:
//   cl /nologo /EHsc /std:c++17 /Isrc tests\admin_view_tests.cpp src\client\admin_view.cpp src\admin_wire\admin_wire.cpp /Fe:admin_view_tests.exe
//   admin_view_tests.exe
#include "client/admin_view.h"

#include <cstdio>

static int g_checks = 0, g_failed = 0;

#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failed; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

using namespace AdminWire;
using namespace AdminView;

static Op MakeOp(const char* name, std::vector<std::string> args)
{
	Op o;
	o.name = name;
	o.args = std::move(args);
	return o;
}

static Snapshot Base()
{
	Snapshot s;
	s.defaultPolicy = "allow";
	s.rules = { { "Alpha.*", "deny" }, { "Beta.fly", "allow" }, { "Gone.thing", "deny" } };
	s.mins = { { "", "1.20.0" }, { "Alpha", "wire" } };
	ModInfo a; a.plugin = "Alpha"; a.version = "1.0"; a.features = { { "fly", "Flying" }, { "build", "Fast build" } };
	ModInfo b; b.plugin = "Beta"; b.version = "2.0"; b.features = { { "fly", "Beta flying" } };
	s.mods = { a, b };
	PlayerInfo on; on.id = "p1"; on.name = "Ann"; on.online = true; on.flags = "admin sections=Alpha.*;Beta.fly label=Boss";
	on.mods = { { "Alpha", "1.0" }, { "Beta", "2.0" } };
	PlayerInfo off; off.id = "p2"; off.name = "Bob"; off.online = false; off.flags = "exemptversion";
	off.mods = { { "Alpha", "1.0" } };
	s.players = { on, off };
	return s;
}

int main()
{
	// Flag text round trip and delta form.
	{
		PlayerFlags f = ParseFlags("admin -exemptversion exemptrules sections=A.*;B.x label=Big Boss");
		CHECK(f.admin && !f.exemptVersion && f.exemptRules);
		CHECK(f.sections.size() == 2 && f.sections[1] == "B.x");
		CHECK(f.label == "Big Boss");
		CHECK(FormatFlags(f) == "admin exemptrules sections=A.*;B.x label=Big Boss");
		CHECK(FormatWireFlags(f) == "admin -exemptversion exemptrules sections=A.*;B.x");
		PlayerFlags none = ParseFlags("");
		CHECK(FormatFlags(none).empty());
		CHECK(FormatWireFlags(none) == "-admin -exemptversion -exemptrules -sections");
		CHECK(ParseFlags("sections=A.*").sections.size() == 1);
		CHECK(ParseFlags("admin -sections").sections.empty());
		// A key with a space travels quoted and survives parse, popup edit and write.
		PlayerFlags sp = ParseFlags("admin sections=\"Best Builder.x;Other.y\" label=Big Boss");
		CHECK(sp.sections.size() == 2 && sp.sections[0] == "Best Builder.x" && sp.sections[1] == "Other.y");
		CHECK(sp.label == "Big Boss");
		CHECK(FormatFlags(sp) == "admin sections=\"Best Builder.x;Other.y\" label=Big Boss");
		sp.sections.push_back("Third.z");
		CHECK(FormatWireFlags(sp) == "admin -exemptversion -exemptrules sections=\"Best Builder.x;Other.y;Third.z\"");
		CHECK(FormatWireFlags(ParseFlags(FormatWireFlags(sp))).find("\"Best Builder.x;Other.y;Third.z\"") != std::string::npos);
		PlayerFlags plain = ParseFlags("sections=A.x;B.y");
		CHECK(FormatWireFlags(plain).find("sections=A.x;B.y") != std::string::npos);
	}

	// Effective snapshot: rules, policy, minimums.
	{
		Snapshot base = Base();
		std::vector<Op> ops = {
			MakeOp("rule.set", { "Alpha.*", "unset" }),
			MakeOp("rule.set", { "Alpha.fly", "deny" }),
			MakeOp("rule.set", { "Beta.fly", "deny" }),
			MakeOp("policy.default", { "deny" }),
			MakeOp("policy.message", { "Closed" }),
			MakeOp("min.loader", { "off" }),
			MakeOp("min.mod", { "Alpha", "1.5" }),
			MakeOp("min.mod", { "Beta", "wire" }),
		};
		Snapshot eff = ApplyStaged(base, ops);
		CHECK(RuleValue(eff, "Alpha.*").empty());
		CHECK(RuleValue(eff, "Alpha.fly") == "deny");
		CHECK(RuleValue(eff, "Beta.fly") == "deny");
		CHECK(eff.defaultPolicy == "deny" && eff.message == "Closed");
		CHECK(MinValue(eff, "").empty());
		CHECK(MinValue(eff, "Alpha") == "1.5" && MinValue(eff, "Beta") == "wire");
		CHECK(RuleValue(base, "Alpha.*") == "deny");   // base untouched
		CHECK(RuleChanged(base, eff, "Alpha.*") && RuleChanged(base, eff, "Alpha.fly") && !RuleChanged(base, eff, "Gone.thing"));
		CHECK(MinChanged(base, eff, "") && MinChanged(base, eff, "Beta"));
		CHECK(ModChanged(base, eff, base.mods[0]) && ModChanged(base, eff, base.mods[1]));
		CHECK(!ModChanged(base, base, base.mods[0]));
	}

	// Players: delta applied on the stored flags, remove online vs saved, new player.
	{
		Snapshot base = Base();
		Snapshot e1 = ApplyStaged(base, { MakeOp("player.set", { "p1", "-admin exemptrules sections=Beta.fly" }) });
		PlayerFlags f = ParseFlags(FindPlayer(e1, "p1")->flags);
		CHECK(!f.admin && f.exemptRules && f.sections.size() == 1 && f.label == "Boss");
		CHECK(PlayerChanged(base, e1, "p1") && !PlayerChanged(base, e1, "p2"));

		Snapshot e2 = ApplyStaged(base, { MakeOp("player.remove", { "p1" }), MakeOp("player.remove", { "p2" }) });
		CHECK(FindPlayer(e2, "p1") && FindPlayer(e2, "p1")->flags.empty());   // online: stays, flags cleared
		CHECK(!FindPlayer(e2, "p2"));                                           // saved: gone
		CHECK(PlayerChanged(base, e2, "p1") && PlayerChanged(base, e2, "p2"));

		Snapshot e3 = ApplyStaged(base, { MakeOp("player.set", { "76561198000000001", "admin -exemptversion -exemptrules -sections" }) });
		CHECK(FindPlayer(e3, "76561198000000001") && FindPlayer(e3, "76561198000000001")->flags == "admin");
		CHECK(PlayerChanged(base, e3, "76561198000000001"));
		CHECK(e3.players.size() == 3);

		// kick and catalog.push leave the snapshot alone.
		Snapshot e4 = ApplyStaged(base, { MakeOp("catalog.push", { "Alpha" }), MakeOp("bogus", {}) });
		CHECK(e4.players.size() == 2 && e4.rules.size() == base.rules.size());
	}

	// Staged ops with the same target replace each other.
	{
		Op r1 = MakeOp("rule.set", { "A.x", "deny" });
		Op r2 = MakeOp("rule.set", { "A.y", "deny" });
		Op p1 = MakeOp("player.set", { "p1", "admin" });
		Op p1r = MakeOp("player.remove", { "p1" });
		CHECK(SameTarget(r1, MakeOp("rule.set", { "A.x", "allow" })));
		CHECK(!SameTarget(r1, r2));
		CHECK(SameTarget(p1, p1r));
		CHECK(!SameTarget(p1, MakeOp("player.set", { "p2", "admin" })));
		CHECK(SameTarget(MakeOp("policy.default", { "deny" }), MakeOp("policy.default", { "allow" })));
		CHECK(!SameTarget(MakeOp("min.loader", { "1" }), MakeOp("min.mod", { "A", "1" })));
		std::vector<Op> staged = { r1, p1, r2, p1r };
		std::vector<size_t> idx = StagedWithTarget(staged, MakeOp("player.set", { "p1", "x" }));
		CHECK(idx.size() == 2 && idx[0] == 3 && idx[1] == 1);
	}

	// No-op detection against the base.
	{
		Snapshot base = Base();
		CHECK(IsNoop(base, MakeOp("rule.set", { "Alpha.*", "deny" })));
		CHECK(!IsNoop(base, MakeOp("rule.set", { "Alpha.*", "allow" })));
		CHECK(IsNoop(base, MakeOp("rule.set", { "Alpha.fly", "unset" })));
		CHECK(!IsNoop(base, MakeOp("rule.set", { "Alpha.*", "unset" })));
		CHECK(IsNoop(base, MakeOp("policy.default", { "allow" })) && !IsNoop(base, MakeOp("policy.default", { "deny" })));
		CHECK(IsNoop(base, MakeOp("min.loader", { "1.20.0" })) && !IsNoop(base, MakeOp("min.loader", { "off" })));
		CHECK(IsNoop(base, MakeOp("min.mod", { "Beta", "off" })) && !IsNoop(base, MakeOp("min.mod", { "Alpha", "off" })));
		CHECK(IsNoop(base, MakeOp("player.set", { "p2", "-admin exemptversion -exemptrules -sections" })));
		CHECK(!IsNoop(base, MakeOp("player.set", { "p2", "admin exemptversion -exemptrules -sections" })));
		CHECK(!IsNoop(base, MakeOp("player.set", { "nobody", "-admin -exemptversion -exemptrules -sections" })));
	}

	// Catalog helpers and filters.
	{
		Snapshot base = Base();
		std::vector<std::string> unknown = UnknownRuleKeys(base);
		CHECK(unknown.size() == 1 && unknown[0] == "Gone.thing");
		std::vector<std::string> keys = SectionKeys(base);
		CHECK(keys.size() == 5 && keys[0] == "Alpha.*" && keys[1] == "Alpha.fly" && keys[3] == "Beta.*");
		std::map<std::string, int> n = PlayersPerMod(base);
		CHECK(n["Alpha"] == 2 && n["Beta"] == 1 && n.count("Gamma") == 0);
		CHECK(MatchesFilter("Fast Build", "build") && MatchesFilter("x", "") && !MatchesFilter("x", "y"));
		CHECK(ModMatches(base.mods[0], "FLYING") && ModMatches(base.mods[1], "beta") && !ModMatches(base.mods[1], "zzz"));
		CHECK(PlayerMatches(base.players[0], "ann") && PlayerMatches(base.players[0], "boss") && PlayerMatches(base.players[1], "p2"));
		CHECK(!PlayerMatches(base.players[1], "ann"));
	}

	// Player order and the Versions tab's plugin list.
	{
		Snapshot s;
		PlayerInfo a, b, c;
		a.id = "1"; a.name = "Saved"; a.online = false;
		b.id = "2"; b.name = "Live"; b.online = true;
		c.id = "3"; c.name = "Also saved"; c.online = false;
		s.players = { a, b, c };
		std::vector<size_t> all = PlayerOrder(s, "");
		CHECK(all.size() == 3 && all[0] == 1 && all[1] == 0 && all[2] == 2);
		std::vector<size_t> some = PlayerOrder(s, "saved");
		CHECK(some.size() == 2 && some[0] == 0 && some[1] == 2);
		CHECK(PlayerOrder(s, "zzz").empty());
		ModInfo m1, m2;
		m1.plugin = "Alpha"; m2.plugin = "Beta";
		s.mods = { m1, m2 };
		s.mins = { { "", "1.0" }, { "Beta", "2" }, { "Gone", "3" } };
		std::vector<std::string> p = MinPlugins(s);
		CHECK(p.size() == 3 && p[0] == "Alpha" && p[1] == "Beta" && p[2] == "Gone");
	}

	// Built-in defaults: a key the server denies unless a rule names it. The rule overrides, removing it brings the default back.
	{
		Snapshot s = Base();
		s.defaults = { "Alpha.fly", "Beta.fly", "Alpha.build" };
		CHECK(IsDefaultDenied(s, "Alpha.fly") && IsDefaultDenied(s, "Alpha.build"));
		CHECK(!IsDefaultDenied(s, "Beta.fly"));                              // a rule names it (allow)
		CHECK(!IsDefaultDenied(s, "Alpha.*") && !IsDefaultDenied(s, "Alpha.other"));
		CHECK(IsDefaultDenied(s, "alpha.fly") && !IsDefaultDenied(s, "Alpha.Fly"));   // mod name without case, id exact
		CHECK(RuleValue(s, "Alpha.fly").empty());                            // no rule line: the default is not a rule
		Snapshot allowed = ApplyStaged(s, { MakeOp("rule.set", { "Alpha.fly", "allow" }) });
		CHECK(RuleValue(allowed, "Alpha.fly") == "allow" && !IsDefaultDenied(allowed, "Alpha.fly"));
		Snapshot back = ApplyStaged(allowed, { MakeOp("rule.set", { "Alpha.fly", "unset" }) });
		CHECK(IsDefaultDenied(back, "Alpha.fly"));
		Snapshot denied = ApplyStaged(s, { MakeOp("rule.set", { "Alpha.fly", "deny" }) });
		CHECK(RuleValue(denied, "Alpha.fly") == "deny" && !IsDefaultDenied(denied, "Alpha.fly"));
		CHECK(!IsNoop(s, MakeOp("rule.set", { "Alpha.fly", "deny" })) && IsNoop(s, MakeOp("rule.set", { "Alpha.fly", "unset" })));
		// Defaults are not rules: none appear among the unknown rules, even for a mod the catalog lacks.
		s.defaults.push_back("Missing.feature");
		std::vector<std::string> unknown = UnknownRuleKeys(s);
		CHECK(unknown.size() == 1 && unknown[0] == "Gone.thing");
		CHECK(!IsDefaultDenied(Base(), "Alpha.fly"));                        // an older server: no defaults
	}

	// Count labels: singular only for one.
	CHECK(CountLabel(0, "mod") == "0 mods" && CountLabel(1, "mod") == "1 mod" && CountLabel(2, "mod") == "2 mods");

	std::printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
