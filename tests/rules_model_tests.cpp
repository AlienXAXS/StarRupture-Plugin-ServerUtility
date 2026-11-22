// Unit tests of the rules parser and the text it makes for the mod loader.
// No project file: from a Visual Studio developer prompt, in the repo root:
//   cl /nologo /EHsc /std:c++17 /Isrc tests\rules_model_tests.cpp src\host_rules\rules_model.cpp src\host_rules\claim_model.cpp /Fe:rules_model_tests.exe
//   rules_model_tests.exe
#include "host_rules/rules_model.h"
#include "host_rules/claim_model.h"
#include "admin/url_match.h"

#include <cstdio>
#include <cstring>
#include <set>

static int g_checks = 0, g_failed = 0;

#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failed; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

using namespace RulesModel;

static Model ParseText(const std::vector<std::string>& lines, std::vector<std::string>& warnings)
{
	Model m;
	Parse(lines, m, warnings);
	return m;
}

int main()
{
	std::vector<std::string> w;

	// A sample file: comments, policy, rules, minimums, a players section that is skipped.
	Model m = ParseText({
		"; host rules", "[Policy]", "Default = deny", "Message=Some mods are off here #rules = ok", "MinLoaderVersion=v1.27.0",
		"AdminPrompt=1", "[MinVersions]", "RuptureTimer=0.11.0", "productionviewer=0.19.0",
		"[Rules]", "BetterCheats.*=allow", "BetterCheats.movement=DENY", "BetterDrone.drone-limits=deny",
		"[Players]", "76561198000000001=Mike|Admin" }, w);
	CHECK(w.empty());
	CHECK(m.defaultDeny);
	CHECK(m.message == "Some mods are off here #rules = ok");
	CHECK(m.minLoaderVersion == "v1.27.0");
	CHECK(m.rules.size() == 3 && m.rules[1].plugin == "BetterCheats" && m.rules[1].id == "movement" && m.rules[1].deny);
	CHECK(m.modMins.size() == 2);
	CHECK(m.policyKept.size() == 1 && m.policyKept[0] == "AdminPrompt=1");
	const std::string text = RulesText(m);
	CHECK(text == "default=deny\nmessage=Some mods are off here #rules = ok\nBetterCheats.*=allow\nBetterCheats.movement=deny\nBetterDrone.drone-limits=deny\n");
	CHECK(VersionMinimumsText(m) == "loader=v1.27.0\nRuptureTimer=0.11.0\nproductionviewer=0.19.0\n");

	// Unmatched keys and bad values are warned about and kept.
	w.clear();
	m = ParseText({ "[Policy]", "Defualt=deny", "Default=maybe", "MinLoaderVersion=soon", "[Rules]", "Mod.x=allow", "nodot=deny", "Mod.y=perhaps",
	                "$x.y=deny", "[MinVersions]", "Mod=latest", "justtext" }, w);
	CHECK(w.size() == 8);
	CHECK(!m.defaultDeny && m.minLoaderVersion.empty());
	CHECK(m.rules.size() == 1 && m.policyKept.size() == 3 && m.rulesKept.size() == 3 && m.modMinKept.size() == 2);
	CHECK(RuleLines(m).size() == 4);   // the one good rule and the three kept lines

	// The same key twice: deny wins, plugin name ignores case.
	w.clear();
	m = ParseText({ "[Rules]", "Mod.x=allow", "mod.x=deny", "MOD.x=allow" }, w);
	CHECK(m.rules.size() == 1 && m.rules[0].deny && w.size() == 2);

	// Empty and comment-only files: allow everything.
	w.clear();
	m = ParseText({ "# nothing", "" }, w);
	CHECK(RulesText(m) == "default=allow\n" && VersionMinimumsText(m).empty() && w.empty());

	// "loader" is the loader's own row, so a mod cannot take that name.
	w.clear();
	const Model withLoaderRow = ParseText({ "[MinVersions]", "Loader=1.0.0", "Mod=1.0.0" }, w);
	CHECK(w.size() == 1 && withLoaderRow.modMins.size() == 1 && withLoaderRow.modMinKept.size() == 1 && VersionMinimumsText(withLoaderRow) == "Mod=1.0.0\n");
	w.clear();
	const Model withWire = ParseText({ "[MinVersions]", "ProductionViewer=Wire", "Other=wired" }, w);
	CHECK(w.size() == 1 && withWire.modMins.size() == 1 && withWire.modMins[0].version == "wire" && withWire.modMinKept.size() == 1);
	w.clear();

	// Changes.
	std::string e;
	CHECK(SetRule(m, "BetterCheats.movement", true, e));
	CHECK(SetRule(m, "bettercheats.movement", false, e) && m.rules.size() == 1 && !m.rules[0].deny);
	CHECK(!SetRule(m, "nodot", true, e) && !e.empty());
	CHECK(!SetRule(m, "A.b c", true, e));
	CHECK(!SetRule(m, "My.Mod.feature", true, e) && e.find("more than one dot") != std::string::npos);
	CHECK(!SetRule(m, "My.Mod.*", false, e) && !UnsetRule(m, "My.Mod.feature", e) && m.rules.size() == 1);
	CHECK(!SetRule(m, "#A.b", true, e) && !SetRule(m, ";A.b", true, e) && !SetRule(m, "A.#b", true, e) && m.rules.size() == 1);
	CHECK(UnsetRule(m, "BETTERCHEATS.movement", e) && m.rules.empty());
	CHECK(!UnsetRule(m, "Nope.x", e));
	CHECK(SetDefault(m, true) && m.defaultDeny);
	CHECK(SetMessage(m, "  hello  ", e) && m.message == "hello");
	CHECK(!SetMessage(m, std::string(201, 'x'), e) && m.message == "hello");
	CHECK(!SetMessage(m, "a\tb", e));
	CHECK(SetMinLoader(m, "1.27.0", e) && m.minLoaderVersion == "1.27.0");
	CHECK(!SetMinLoader(m, "soon", e) && m.minLoaderVersion == "1.27.0");
	CHECK(SetMinLoader(m, "", e) && m.minLoaderVersion.empty());
	CHECK(SetModMin(m, "RuptureTimer", "0.11.0", e) && SetModMin(m, "rupturetimer", "v0.12.0", e) && m.modMins.size() == 1 && m.modMins[0].version == "v0.12.0");
	CHECK(!SetModMin(m, "RuptureTimer", "x", e) && m.modMins[0].version == "v0.12.0");
	CHECK(SetModMin(m, "RuptureTimer", "", e) && m.modMins.empty());
	CHECK(!SetModMin(m, "RuptureTimer", "", e));
	CHECK(!SetModMin(m, "Loader", "1.0.0", e) && m.modMins.empty());
	CHECK(SetModMin(m, "ProductionViewer", "WIRE", e) && m.modMins.size() == 1 && m.modMins[0].version == "wire" && VersionMinimumsText(m) == "ProductionViewer=wire\n");
	CHECK(!SetModMin(m, "ProductionViewer", "wired", e) && !SetMinLoader(m, "wire", e) && SetModMin(m, "ProductionViewer", "", e) && m.modMins.empty());
	CHECK(SetMinLoader(m, "v1.27.0", e) && SetModMin(m, "A", "1.2", e) && VersionMinimumsText(m) == "loader=v1.27.0\nA=1.2\n");
	CHECK(SetMinLoader(m, "", e) && VersionMinimumsText(m) == "A=1.2\n");
	CHECK(SetModMin(m, "A", "", e) && VersionMinimumsText(m).empty());

	// Rule limit.
	m = Model{};
	for (int i = 0; i < 2047; ++i) CHECK(SetRule(m, "M.f" + std::to_string(i), true, e));
	CHECK(!SetRule(m, "M.over", true, e));

	// Versions.
	CHECK(ValidVersion("0.11.0") && ValidVersion("v1.27") && ValidVersion("1.2.3.4") && ValidVersion("1.27.0-dev.1"));
	CHECK(ValidSectionKey("Codex") && ValidSectionKey("Plugin.feature") && ValidSectionKey("a-b_c"));
	CHECK(!ValidSectionKey("") && !ValidSectionKey("a\nB=|Admin") && !ValidSectionKey("a\rb") && !ValidSectionKey("a\tb"));
	CHECK(!ValidSectionKey("a|b") && !ValidSectionKey("a=b") && !ValidSectionKey("a;b") && !ValidSectionKey("a b"));
	CHECK(!ValidSectionKey("[a]") && !ValidSectionKey("a]") && !ValidSectionKey("#a") && !ValidSectionKey("a\x7f"));
	CHECK(!ValidVersion("") && !ValidVersion("1") && !ValidVersion("v") && !ValidVersion("1.x") && !ValidVersion("1.2.3.4.5") && !ValidVersion("dev"));
	CHECK(!ValidVersion("1.2.") && !ValidVersion("1.2.3.4.") && !ValidVersion("1..2") && !ValidVersion(".1.2") && !ValidVersion("1.2.-dev"));
	CHECK(ValidVersion("999999999.0") && !ValidVersion("1234567890.0") && !ValidVersion("1.99999999999"));

	// A controller held until its ID reads.
	CHECK(NextHeldStep(false, false, 0) == HeldStep::Drop && NextHeldStep(false, true, 0) == HeldStep::Drop);
	CHECK(NextHeldStep(true, false, 0) == HeldStep::Wait && NextHeldStep(true, false, kLateIdGraceMs) == HeldStep::Wait);
	CHECK(NextHeldStep(true, false, kLateIdGraceMs + 1) == HeldStep::ActNoId);
	CHECK(NextHeldStep(true, true, 0) == HeldStep::Act && NextHeldStep(true, true, kLateIdGraceMs + 1) == HeldStep::Act);

	// What is written back reads back the same.
	m = Model{};
	SetDefault(m, true); SetMessage(m, "No cheats", e); SetMinLoader(m, "1.27.0", e);
	SetRule(m, "A.b", true, e); SetRule(m, "A.*", false, e); SetModMin(m, "C", "2.0", e);
	std::vector<std::string> file = { "[Policy]" };
	for (auto& l : PolicyLines(m)) file.push_back(l);
	file.push_back("[Rules]");
	for (auto& l : RuleLines(m)) file.push_back(l);
	file.push_back("[MinVersions]");
	for (auto& l : ModMinLines(m)) file.push_back(l);
	w.clear();
	Model back = ParseText(file, w);
	CHECK(w.empty() && RulesText(back) == RulesText(m) && VersionMinimumsText(back) == VersionMinimumsText(m));

	// The rules one player is sent.
	w.clear();
	m = ParseText({ "[Policy]", "Default=deny", "Message=Off here", "[Rules]", "BetterCheats.*=allow", "BetterCheats.movement=deny",
	                "BetterDrone.drone-limits=deny", "Other.x=deny" }, w);
	PlayerFlags none;
	CHECK(!NeedsOwnRules(none) && PlayerRulesText(m, none) == RulesText(m));
	PlayerFlags exempt; exempt.exemptRules = true;
	CHECK(NeedsOwnRules(exempt) && PlayerRulesText(m, exempt) == "default=allow\nmessage=Off here\n");
	exempt.sections = { "BetterDrone.drone-limits" };   // exempt wins over sections
	CHECK(PlayerRulesText(m, exempt) == "default=allow\nmessage=Off here\n");
	PlayerFlags sec; sec.sections = { "betterdrone.drone-limits" };
	CHECK(NeedsOwnRules(sec));
	CHECK(PlayerRulesText(m, sec) == "default=deny\nmessage=Off here\nBetterCheats.*=allow\nBetterCheats.movement=deny\nBetterDrone.drone-limits=allow\nOther.x=deny\n");
	sec.sections = { "BetterCheats.*", "Other.*" };   // a whole-mod key opens its specific rows
	CHECK(PlayerRulesText(m, sec) == "default=deny\nmessage=Off here\nBetterDrone.drone-limits=deny\nBetterCheats.*=allow\nOther.*=allow\n");
	sec.sections = { "New.thing", "bad key", "nodot" };   // a key with no rule is added; unusable keys are skipped
	CHECK(PlayerRulesText(m, sec).find("New.thing=allow\n") != std::string::npos && PlayerRulesText(m, sec).find("bad") == std::string::npos);
	Model open = m; open.defaultDeny = false;
	sec.sections = { "Fresh.thing" };
	CHECK(PlayerRulesText(open, sec).find("Fresh.thing=allow\n") != std::string::npos);   // still stands alone with default allow

	// Claim code: making one.
	{
		unsigned state = 12345;   // a plain generator is enough here: the test checks the mapping, not the randomness
		ClaimModel::RandomFill ramp = [&](unsigned char* b, size_t n) { for (size_t i = 0; i < n; ++i) { state = state * 1103515245u + 12345u; b[i] = static_cast<unsigned char>(state >> 16); } return true; };
		std::string code;
		CHECK(ClaimModel::GenerateCode(ramp, code) && code.size() == ClaimModel::kCodeLength);
		bool allInAlphabet = true;
		std::set<char> seen;
		for (int n = 0; n < 200; ++n)
		{
			CHECK(ClaimModel::GenerateCode(ramp, code) && code.size() == 8);
			for (char c : code)
			{
				seen.insert(c);
				if (!std::strchr(ClaimModel::kAlphabet, c)) allInAlphabet = false;
				if (std::strchr("01OIL", c)) allInAlphabet = false;
			}
		}
		CHECK(allInAlphabet);
		CHECK(seen.size() == ClaimModel::kAlphabetSize);   // every symbol turns up
		CHECK(std::strlen(ClaimModel::kAlphabet) == ClaimModel::kAlphabetSize);

		// Bytes at 248 and up are skipped (no bias): 0 -> '2', 30 -> 'Z', 31 -> '2', 247 -> 'Z' (247 % 31 = 30).
		const unsigned char fixed[16] = { 0, 30, 31, 247, 248, 255, 250, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
		ClaimModel::RandomFill fx = [&](unsigned char* b, size_t n) { std::memcpy(b, fixed, n < 16 ? n : 16); return true; };
		CHECK(ClaimModel::GenerateCode(fx, code) && code == "2Z2Z3456");
		ClaimModel::RandomFill bad = [](unsigned char*, size_t) { return false; };
		code = "KEEP";
		CHECK(!ClaimModel::GenerateCode(bad, code) && code.empty());
		ClaimModel::RandomFill high = [](unsigned char* b, size_t n) { std::memset(b, 255, n); return true; };
		CHECK(!ClaimModel::GenerateCode(high, code) && code.empty());   // a source that never gives a usable byte ends
		CHECK(!ClaimModel::GenerateCode(ClaimModel::RandomFill(), code));
	}

	// Claim code: what a player typed.
	{
		std::string out;
		CHECK(ClaimModel::NormalizeCode("  k7m2qxzr \r\n", out) && out == "K7M2QXZR");
		CHECK(ClaimModel::NormalizeCode("23456789", out) && out == "23456789");
		CHECK(ClaimModel::NormalizeCode("ABCDEFGHJKMNPQRSTUVWXYZ", out) && out == "ABCDEFGHJKMNPQRSTUVWXYZ");
		for (const char* bad : { "", "   ", "K7M2 QXZR", "K7M20XZR", "K7M2OXZR", "K7M21XZR", "K7M2IXZR", "K7M2LXZR", "K7M2-XZR", "K7M2\tXZR" })
		{
			out = "stale";
			CHECK(!ClaimModel::NormalizeCode(bad, out) && out.empty());
		}
		CHECK(!ClaimModel::NormalizeCode(std::string(65, 'A'), out));
		CHECK(ClaimModel::NormalizeCode(std::string(64, 'a'), out) && out.size() == 64);
		CHECK(!ClaimModel::NormalizeCode(std::string("AB\0CD", 5), out));
	}

	// Claim code: compare, wipe, prompt switch.
	{
		CHECK(ClaimModel::ConstantTimeEquals("K7M2QXZR", "K7M2QXZR"));
		CHECK(!ClaimModel::ConstantTimeEquals("K7M2QXZS", "K7M2QXZR"));
		CHECK(!ClaimModel::ConstantTimeEquals("A7M2QXZR", "K7M2QXZR"));
		CHECK(!ClaimModel::ConstantTimeEquals("K7M2QXZ", "K7M2QXZR"));
		CHECK(!ClaimModel::ConstantTimeEquals("K7M2QXZRR", "K7M2QXZR"));
		CHECK(!ClaimModel::ConstantTimeEquals("", "K7M2QXZR"));
		CHECK(!ClaimModel::ConstantTimeEquals(std::string("\0\0\0\0\0\0\0\0", 8), "K7M2QXZR"));

		std::string s = "K7M2QXZR";
		ClaimModel::Wipe(s);
		CHECK(s.empty());
		std::string e;
		ClaimModel::Wipe(e);
		CHECK(e.empty());

		CHECK(ClaimModel::AdminPromptOn({}));
		CHECK(ClaimModel::AdminPromptOn({ "AdminPrompt=1" }));
		CHECK(!ClaimModel::AdminPromptOn({ "Other=1", "adminprompt = 0 " }));
		CHECK(!ClaimModel::AdminPromptOn({ "AdminPrompt=off" }));
		CHECK(!ClaimModel::AdminPromptOn({ "AdminPrompt=Off" }) && !ClaimModel::AdminPromptOn({ "adminprompt=FALSE" }) && !ClaimModel::AdminPromptOn({ "AdminPrompt = NO" }));
		CHECK(ClaimModel::AdminPromptOn({ "Other=0", "AdminPromptX=0", "no equals" }));
	}

	// Claim code: counting wrong tries.
	{
		ClaimModel::Attempts a;
		CHECK(!a.Locked("p1"));
		for (int i = 1; i <= 4; ++i)
		{
			const auto o = a.RecordWrong("p1");
			CHECK(o.left == 5 - i && !o.playerLocked && !o.codeReplaced);
			CHECK(!a.Locked("p1"));
		}
		const auto last = a.RecordWrong("p1");   // 5th for p1, 5th overall
		CHECK(last.left == 0 && last.playerLocked && last.codeReplaced);
		CHECK(a.Locked("p1") && !a.Locked("p2"));

		// The overall count restarted with the new code: four more from others do not replace it.
		for (int i = 0; i < 4; ++i)
		{
			const auto o = a.RecordWrong("p2");
			CHECK(!o.codeReplaced && !o.playerLocked);
		}
		const auto o2 = a.RecordWrong("p3");   // 5th since the replace
		CHECK(o2.codeReplaced && o2.left == 4 && !o2.playerLocked);
		CHECK(!a.Locked("p2") && !a.Locked("p3"));
		CHECK(a.Locked("p1"));   // stays locked

		// Two players sharing the overall limit.
		ClaimModel::Attempts b;
		int replaced = 0;
		for (int i = 0; i < 10; ++i)
			if (b.RecordWrong(i % 2 ? "x" : "y").codeReplaced) ++replaced;
		CHECK(replaced == 2 && b.Locked("x") && b.Locked("y"));
	}

	// Admin url routing: the loader appends one '/'; exactly one is ignored.
	{
		CHECK(UrlEndsWith("/ServerUtility/api/players/list/", "/list"));
		CHECK(UrlEndsWith("/ServerUtility/api/players/list", "/list"));
		CHECK(UrlEndsWith("/ServerUtility/API/Players/LIST/", "/list"));
		CHECK(!UrlEndsWith("/ServerUtility/api/players/list//", "/list"));
		CHECK(!UrlEndsWith("", "/list"));
		CHECK(!UrlEndsWith("/", "/list"));
		CHECK(UrlEndsWith("", ""));
		CHECK(!UrlEndsWith("/ServerUtility/api/players/blist/", "/list"));
		CHECK(!UrlEndsWith("/ServerUtility/api/players/listx/", "/list"));
		CHECK(!UrlEndsWith("/ServerUtility/api/players/lis/", "/list"));
	}

	std::printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
