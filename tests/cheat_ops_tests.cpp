// Unit tests of the su.cheat ops apart from the game: cheat.give, cheat.wave and cheat.stack, with fake Envs.
// No project file: from a Visual Studio developer prompt, in the repo root:
//   cl /nologo /EHsc /std:c++17 /Isrc tests\cheat_ops_tests.cpp src\cheats\op_give.cpp src\cheats\op_wave.cpp src\cheats\op_stack.cpp src\cheats\cheat_dispatch.cpp src\cheats\cheat_wire.cpp src\admin_wire\admin_wire.cpp /Fe:cheat_ops_tests.exe
//   cheat_ops_tests.exe
#include "cheats/op_give.h"
#include "cheats/op_wave.h"
#include "cheats/op_stack.h"

#include <cstdio>
#include <optional>
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

struct WaveFake
{
	bool available = true;
	std::optional<OpWave::State> state = OpWave::State();
	bool callsOk = true;
	int starts = 0, pauses = 0, resumes = 0, cancels = 0;
	int lastType = 0;
	uint32_t lastReq = 0;

	OpWave::Env MakeEnv()
	{
		OpWave::Env e;
		e.available = [this] { return available; };
		e.state = [this] { return state; };
		e.start = [this](int type) { ++starts; lastType = type; return callsOk; };
		e.pause = [this](const Context&, uint32_t id) { ++pauses; lastReq = id; return callsOk; };
		e.resume = [this](const Context&, uint32_t id) { ++resumes; lastReq = id; return callsOk; };
		e.cancel = [this](const Context&, uint32_t id) { ++cancels; lastReq = id; return callsOk; };
		e.log = [](const std::string&) {};
		return e;
	}
	int Total() const { return starts + pauses + resumes + cancels; }
};

static CheatWire::Request WaveReq(const char* what, const char* type = nullptr)
{
	CheatWire::Request r;
	r.id = 9;
	r.op = "cheat.wave";
	if (what) r.fields.emplace_back("do", what);
	if (type) r.fields.emplace_back("type", type);
	return r;
}

static void WaveTests()
{
	const Context ctx;
	WaveFake w;
	OpWave::SetEnv(w.MakeEnv());
	CHECK(OpWave::Available());

	// A start with no type is Heat; Cold is accepted in any case; a bad type is refused.
	{
		const Result r = OpWave::Handle(ctx, WaveReq("start"));
		CHECK(r.status == CheatWire::Status::Ok && r.reason == OpWave::kTextStarted);
		CHECK(r.fields.size() == 1 && r.fields[0].first == "state" && r.fields[0].second == "running");
		CHECK(w.starts == 1 && w.lastType == OpWave::kTypeHeat);
		CHECK(OpWave::Handle(ctx, WaveReq("start", "Cold")).status == CheatWire::Status::Ok && w.lastType == OpWave::kTypeCold);
		CHECK(OpWave::Handle(ctx, WaveReq("start", "heat")).status == CheatWire::Status::Ok && w.lastType == OpWave::kTypeHeat);
		const int before = w.starts;
		const Result bad = OpWave::Handle(ctx, WaveReq("start", "acid"));
		CHECK(bad.status == CheatWire::Status::Refused && bad.reason == kTextBadRequest && w.starts == before);
	}

	// A start while a wave runs is refused.
	{
		WaveFake g;
		g.state = OpWave::State{ true, false };
		OpWave::SetEnv(g.MakeEnv());
		const Result r = OpWave::Handle(ctx, WaveReq("start"));
		CHECK(r.status == CheatWire::Status::Refused && r.reason == OpWave::kTextAlreadyWave && g.Total() == 0);
	}

	// Pause, resume and cancel with no wave running.
	{
		WaveFake g;
		OpWave::SetEnv(g.MakeEnv());
		for (const char* what : { "pause", "resume", "cancel" })
		{
			const Result r = OpWave::Handle(ctx, WaveReq(what));
			CHECK(r.status == CheatWire::Status::Refused && r.reason == OpWave::kTextNoWave);
		}
		CHECK(g.Total() == 0);
	}

	// Pause, resume and cancel on a running wave.
	{
		WaveFake g;
		g.state = OpWave::State{ true, false };
		OpWave::SetEnv(g.MakeEnv());
		const Result p = OpWave::Handle(ctx, WaveReq("pause"));
		CHECK(p.status == CheatWire::Status::Ok && p.reason == OpWave::kTextPaused && p.fields.size() == 1 && p.fields[0].second == "paused");
		CHECK(g.pauses == 1 && g.lastReq == 9);
		const Result nr = OpWave::Handle(ctx, WaveReq("resume"));
		CHECK(nr.status == CheatWire::Status::Refused && nr.reason == OpWave::kTextNotPaused && g.resumes == 0);
		const Result c = OpWave::Handle(ctx, WaveReq("cancel"));
		CHECK(c.status == CheatWire::Status::Ok && c.reason == OpWave::kTextCancelled && c.fields[0].second == "idle");
		CHECK(g.cancels == 1 && g.lastReq == 9);
	}

	// Pause on a paused wave is refused; resume goes through.
	{
		WaveFake g;
		g.state = OpWave::State{ true, true };
		OpWave::SetEnv(g.MakeEnv());
		const Result a = OpWave::Handle(ctx, WaveReq("pause"));
		CHECK(a.status == CheatWire::Status::Refused && a.reason == OpWave::kTextAlreadyPause && g.pauses == 0);
		const Result r = OpWave::Handle(ctx, WaveReq("resume"));
		CHECK(r.status == CheatWire::Status::Ok && r.reason == OpWave::kTextResumed && r.fields[0].second == "running");
		CHECK(g.resumes == 1 && g.lastReq == 9);
	}

	// A missing or unknown verb is a bad request; skip is not available yet. The game is not asked.
	{
		WaveFake g;
		OpWave::SetEnv(g.MakeEnv());
		CHECK(OpWave::Handle(ctx, WaveReq(nullptr)).reason == kTextBadRequest);
		CHECK(OpWave::Handle(ctx, WaveReq("explode")).reason == kTextBadRequest);
		CHECK(OpWave::Handle(ctx, WaveReq("")).reason == kTextBadRequest);
		const Result s = OpWave::Handle(ctx, WaveReq("skip"));
		CHECK(s.status == CheatWire::Status::Refused && s.reason == kTextNotYet);
		CHECK(g.Total() == 0);
	}

	// No world: the reply is an error with the no-world text.
	{
		WaveFake g;
		g.state.reset();
		OpWave::SetEnv(g.MakeEnv());
		const Result r = OpWave::Handle(ctx, WaveReq("start"));
		CHECK(r.status == CheatWire::Status::Error && r.reason == OpWave::kTextNoWorld && g.Total() == 0);
	}

	// A failed game call is an error with the failure text.
	{
		WaveFake g;
		g.callsOk = false;
		OpWave::SetEnv(g.MakeEnv());
		const Result r = OpWave::Handle(ctx, WaveReq("start"));
		CHECK(r.status == CheatWire::Status::Error && r.reason == OpWave::kTextFailed && r.fields.empty());
	}

	// A key missing on this build: the op is off, checked on each call.
	{
		WaveFake g;
		g.available = false;
		OpWave::SetEnv(g.MakeEnv());
		CHECK(!OpWave::Available());
		const Result r = OpWave::Handle(ctx, WaveReq("start"));
		CHECK(r.status == CheatWire::Status::Refused && r.reason == "Not available on this server's game build.");
		CHECK(OpWave::Handle(ctx, WaveReq(nullptr)).reason == OpWave::kTextOff && g.Total() == 0);
		g.available = true;
		CHECK(OpWave::Available());
	}

	// No env at all: off.
	{
		OpWave::SetEnv(OpWave::Env());
		CHECK(!OpWave::Available());
		CHECK(OpWave::Handle(ctx, WaveReq("start")).reason == OpWave::kTextOff);
	}
}

struct StackFake
{
	bool available = true;
	bool callsOk = true;
	int target = 0;
	int sets = 0, clears = 0;
	uint32_t lastReq = 0;

	OpStack::Env MakeEnv()
	{
		OpStack::Env e;
		e.available = [this] { return available; };
		e.current = [this] { return target; };
		e.set = [this](const Context&, uint32_t id, int size) { ++sets; lastReq = id; if (callsOk) target = size; return callsOk; };
		e.clear = [this](const Context&, uint32_t id) { ++clears; lastReq = id; if (callsOk) target = 0; return callsOk; };
		e.log = [](const std::string&) {};
		return e;
	}
};

static CheatWire::Request StackReq(const char* size)
{
	CheatWire::Request r;
	r.id = 11;
	r.op = "cheat.stack";
	if (size) r.fields.emplace_back("size", size);
	return r;
}

static OpStack::TickState Tick(int target, int lastApplied = 0, int tracked = 10)
{
	OpStack::TickState s;
	s.featureAvailable = true;
	s.target = target;
	s.lastApplied = lastApplied;
	s.itemsTracked = tracked;
	return s;
}

static void StackTests()
{
	const Context ctx;
	StackFake f;
	OpStack::SetEnv(f.MakeEnv());
	CHECK(OpStack::Available());

	// A size from 1 to 9999 is set through the env; the reply carries the size in effect.
	{
		const Result r = OpStack::Handle(ctx, StackReq("999"));
		CHECK(r.status == CheatWire::Status::Ok && r.reason == "Stack size set to 999.");
		CHECK(r.fields.size() == 1 && r.fields[0].first == "size" && r.fields[0].second == "999");
		CHECK(f.sets == 1 && f.target == 999 && f.lastReq == 11);
		CHECK(OpStack::Handle(ctx, StackReq("9999")).status == CheatWire::Status::Ok && f.target == 9999);
		CHECK(OpStack::Handle(ctx, StackReq("0042")).status == CheatWire::Status::Ok && f.target == 42);
	}

	// size=0 puts the game's sizes back.
	{
		const Result r = OpStack::Handle(ctx, StackReq("0"));
		CHECK(r.status == CheatWire::Status::Ok && r.reason == OpStack::kTextCleared);
		CHECK(r.fields.size() == 1 && r.fields[0].second == "0");
		CHECK(f.clears == 1 && f.target == 0);
	}

	// A missing, empty, signed, fractional, text or too large size is refused and touches nothing.
	{
		const int sets = f.sets, clears = f.clears;
		for (const char* bad : { "", "-1", "+5", "1.5", "abc", "10000", "5 0", "99999999999999999999" })
		{
			const Result r = OpStack::Handle(ctx, StackReq(bad));
			CHECK(r.status == CheatWire::Status::Refused && r.reason == kTextBadRequest);
		}
		CHECK(OpStack::Handle(ctx, StackReq(nullptr)).reason == kTextBadRequest);
		CHECK(f.sets == sets && f.clears == clears);
	}

	// The call failed in the game: an error with the one failure text, and the size is unchanged.
	{
		f.callsOk = false;
		const Result r = OpStack::Handle(ctx, StackReq("500"));
		CHECK(r.status == CheatWire::Status::Error && r.reason == OpStack::kTextFailed && r.fields.empty());
		CHECK(OpStack::Handle(ctx, StackReq("0")).status == CheatWire::Status::Error);
		f.callsOk = true;
	}

	// A key missing on this build: the op is off, checked on each call.
	{
		StackFake g;
		g.available = false;
		OpStack::SetEnv(g.MakeEnv());
		CHECK(!OpStack::Available());
		const Result r = OpStack::Handle(ctx, StackReq("500"));
		CHECK(r.status == CheatWire::Status::Refused && r.reason == "Not available on this server's game build.");
		CHECK(OpStack::Handle(ctx, StackReq(nullptr)).reason == OpStack::kTextOff && g.sets == 0 && g.clears == 0);
		g.available = true;
		CHECK(OpStack::Available());
	}

	// No env at all: off.
	{
		OpStack::SetEnv(OpStack::Env());
		CHECK(!OpStack::Available());
		CHECK(OpStack::Handle(ctx, StackReq("500")).reason == OpStack::kTextOff);
	}

	// The tick: nothing to do with no size, a rescan before the first write, then a write, then a
	// guard write, and a restore when the size goes to 0 or the feature goes off.
	{
		using OpStack::Decide;
		using OpStack::Action;
		CHECK(Decide(Tick(0)) == Action::None);
		CHECK(Decide(Tick(0, 500)) == Action::Restore);
		OpStack::TickState raised = Tick(0);
		raised.itemsRaised = 3;
		CHECK(Decide(raised) == Action::Restore);
		OpStack::TickState off = Tick(500, 500);
		off.featureAvailable = false;
		CHECK(Decide(off) == Action::Restore);
		off.lastApplied = 0;
		CHECK(Decide(off) == Action::None);
		CHECK(Decide(Tick(500, 0, 0)) == Action::Rescan);
		CHECK(Decide(Tick(500, 0)) == Action::Apply);
		CHECK(Decide(Tick(700, 500)) == Action::Apply);
		CHECK(Decide(Tick(500, 500)) == Action::None);
		OpStack::TickState moved = Tick(500, 500);
		moved.worldChanged = true;
		CHECK(Decide(moved) == Action::Rescan);
		OpStack::TickState grown = Tick(500, 500);
		grown.catalogueChanged = true;
		CHECK(Decide(grown) == Action::Rescan);
	}

	// ParseSize on its own.
	{
		int n = 77;
		CHECK(OpStack::ParseSize("0", n) && n == 0);
		CHECK(OpStack::ParseSize("9999", n) && n == 9999);
		CHECK(!OpStack::ParseSize("10000", n) && n == 9999);
	}
}

int main()
{
	const Context ctx;
	WaveTests();
	StackTests();
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
