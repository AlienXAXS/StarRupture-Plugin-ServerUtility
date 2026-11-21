// Unit tests of the rules parser and the text it makes for the mod loader.
// No project file: from a Visual Studio developer prompt, in the repo root:
//   cl /nologo /EHsc /std:c++17 /Isrc tests\rules_model_tests.cpp src\host_rules\rules_model.cpp /Fe:rules_model_tests.exe
//   rules_model_tests.exe
#include "host_rules/rules_model.h"

#include <cstdio>

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
	CHECK(VersionMinimumsText(m) == "$loader=v1.27.0\nRuptureTimer=0.11.0\nproductionviewer=0.19.0\n");

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

	// Changes.
	std::string e;
	CHECK(SetRule(m, "BetterCheats.movement", true, e));
	CHECK(SetRule(m, "bettercheats.movement", false, e) && m.rules.size() == 1 && !m.rules[0].deny);
	CHECK(!SetRule(m, "nodot", true, e) && !e.empty());
	CHECK(!SetRule(m, "A.b c", true, e));
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

	// Rule limit.
	m = Model{};
	for (int i = 0; i < 2047; ++i) CHECK(SetRule(m, "M.f" + std::to_string(i), true, e));
	CHECK(!SetRule(m, "M.over", true, e));

	// Versions.
	CHECK(ValidVersion("0.11.0") && ValidVersion("v1.27") && ValidVersion("1.2.3.4") && ValidVersion("1.27.0-dev.1"));
	CHECK(!ValidVersion("") && !ValidVersion("1") && !ValidVersion("v") && !ValidVersion("1.x") && !ValidVersion("1.2.3.4.5") && !ValidVersion("dev"));

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

	std::printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
