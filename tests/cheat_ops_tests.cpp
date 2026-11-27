// Unit tests of the su.cheat ops apart from the game: cheat.give, with a fake Env.
// No project file: from a Visual Studio developer prompt, in the repo root:
//   cl /nologo /EHsc /std:c++17 /Isrc tests\cheat_ops_tests.cpp src\cheats\op_give.cpp src\cheats\cheat_dispatch.cpp src\cheats\cheat_wire.cpp src\admin_wire\admin_wire.cpp /Fe:cheat_ops_tests.exe
//   cheat_ops_tests.exe
#include "cheats/op_give.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_checks = 0, g_failed = 0;

#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failed; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

using namespace CheatDispatch;

struct Fake
{
	bool available = true;
	OpGive::GiveResult result;
	int calls = 0;
	std::string lastItem;
	int lastCount = 0;
	uint32_t lastReq = 0;

	OpGive::Env MakeEnv()
	{
		OpGive::Env e;
		e.available = [this] { return available; };
		e.give = [this](const Context&, const std::string& item, int count, uint32_t reqId)
		{
			++calls; lastItem = item; lastCount = count; lastReq = reqId;
			OpGive::GiveResult r = result;
			if (r.outcome == OpGive::GiveResult::Outcome::Gave) r.given = count;
			return r;
		};
		e.log = [](const std::string&) {};
		return e;
	}
};

static CheatWire::Request Req(const char* item, const char* count)
{
	CheatWire::Request r;
	r.id = 7;
	r.op = "cheat.give";
	if (item)  r.fields.emplace_back("item", item);
	if (count) r.fields.emplace_back("count", count);
	return r;
}

int main()
{
	const Context ctx;
	Fake f;
	f.result.outcome = OpGive::GiveResult::Outcome::Gave;
	f.result.name = "Iron Ore";
	OpGive::SetEnv(f.MakeEnv());
	CHECK(OpGive::Available());

	// A good request: the reply carries the display name and the count.
	{
		const Result r = OpGive::Handle(ctx, Req("/Game/Items/DA_IronOre.DA_IronOre", "500"));
		CHECK(r.status == CheatWire::Status::Ok);
		CHECK(r.reason == "Gave 500 Iron Ore.");
		CHECK(r.fields.size() == 2);
		CHECK(r.fields.size() == 2 && r.fields[0].first == "item" && r.fields[0].second == "Iron Ore");
		CHECK(r.fields.size() == 2 && r.fields[1].first == "count" && r.fields[1].second == "500");
		CHECK(f.calls == 1 && f.lastItem == "/Game/Items/DA_IronOre.DA_IronOre" && f.lastCount == 500 && f.lastReq == 7);
	}

	// The count edges: 1 and 9999 pass.
	f.calls = 0;
	CHECK(OpGive::Handle(ctx, Req("DA_IronOre", "1")).status == CheatWire::Status::Ok && f.lastCount == 1);
	CHECK(OpGive::Handle(ctx, Req("DA_IronOre", "9999")).status == CheatWire::Status::Ok && f.lastCount == 9999);
	CHECK(f.calls == 2);

	// Counts 0, 10000, non-numeric, signed, empty, missing and absurdly long are refused; the game is not asked.
	f.calls = 0;
	for (const char* bad : { "0", "10000", "abc", "12x", "-5", "+5", " 5", "", "123456", "99999999999999999999" })
	{
		const Result r = OpGive::Handle(ctx, Req("DA_IronOre", bad));
		CHECK(r.status == CheatWire::Status::Refused && r.reason == "Count must be 1 to 9999.");
	}
	{
		const Result r = OpGive::Handle(ctx, Req("DA_IronOre", nullptr));
		CHECK(r.status == CheatWire::Status::Refused && r.reason == "Count must be 1 to 9999.");
	}
	CHECK(f.calls == 0);

	// An item path over 256 bytes, an empty one and a missing one are refused without asking the game.
	{
		OpGive::Handle(ctx, Req(std::string(256, 'a').c_str(), "5"));
		CHECK(f.calls == 1);   // exactly 256 bytes still goes through
		f.calls = 0;
		const Result over = OpGive::Handle(ctx, Req(std::string(257, 'a').c_str(), "5"));
		CHECK(over.status == CheatWire::Status::Refused && over.reason == "No such item on this server.");
		CHECK(OpGive::Handle(ctx, Req("", "5")).reason == "No such item on this server.");
		CHECK(OpGive::Handle(ctx, Req(nullptr, "5")).reason == "No such item on this server.");
		CHECK(f.calls == 0);
	}

	// An unknown item: the game says so.
	{
		Fake g;
		g.result.outcome = OpGive::GiveResult::Outcome::NoItem;
		OpGive::SetEnv(g.MakeEnv());
		const Result r = OpGive::Handle(ctx, Req("DA_Nothing", "5"));
		CHECK(r.status == CheatWire::Status::Refused && r.reason == "No such item on this server." && r.fields.empty());
		CHECK(g.calls == 1);
	}

	// The other outcomes.
	{
		Fake g;
		OpGive::SetEnv(g.MakeEnv());
		g.result.outcome = OpGive::GiveResult::Outcome::NoCharacter;
		CHECK(OpGive::Handle(ctx, Req("DA_IronOre", "5")).reason == OpGive::kTextNoCharacter);
		g.result.outcome = OpGive::GiveResult::Outcome::NothingAdded;
		CHECK(OpGive::Handle(ctx, Req("DA_IronOre", "5")).reason == OpGive::kTextNothingAdded);
		g.result.outcome = OpGive::GiveResult::Outcome::Failed;
		const Result r = OpGive::Handle(ctx, Req("DA_IronOre", "5"));
		CHECK(r.status == CheatWire::Status::Error && r.reason == OpGive::kTextFailed);
	}

	// AddNewItem unavailable: the op is off, and a request is refused with the version map text
	// before anything is parsed or asked of the game.
	{
		Fake g;
		g.result.outcome = OpGive::GiveResult::Outcome::Gave;
		g.available = false;
		OpGive::SetEnv(g.MakeEnv());
		CHECK(!OpGive::Available());
		const Result r = OpGive::Handle(ctx, Req("DA_IronOre", "5"));
		CHECK(r.status == CheatWire::Status::Refused && r.reason == "Item giving needs a newer version map on this server.");
		CHECK(OpGive::Handle(ctx, Req("DA_IronOre", "0")).reason == OpGive::kTextOff);
		CHECK(g.calls == 0);
		g.available = true;
		CHECK(OpGive::Available());   // checked on each call: a later version map turns it on
	}

	// No env at all (a build without the game glue): off.
	{
		OpGive::SetEnv(OpGive::Env());
		CHECK(!OpGive::Available());
		CHECK(OpGive::Handle(ctx, Req("DA_IronOre", "5")).reason == OpGive::kTextOff);
	}

	std::printf("cheat_ops_tests: %d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
