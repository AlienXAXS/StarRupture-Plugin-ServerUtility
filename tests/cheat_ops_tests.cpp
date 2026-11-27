// Unit tests of the su.cheat ops apart from the game: cheat.give, cheat.wave and cheat.stack, with fake Envs.
// No project file: from a Visual Studio developer prompt, in the repo root:
//   cl /nologo /EHsc /std:c++17 /Isrc tests\cheat_ops_tests.cpp src\cheats\op_give.cpp src\cheats\op_wave.cpp src\cheats\op_stack.cpp src\cheats\cheat_dispatch.cpp src\cheats\op_state.cpp src\cheats\cheat_wire.cpp src\admin_wire\admin_wire.cpp /Fe:cheat_ops_tests.exe
//   cheat_ops_tests.exe
#include "cheats/op_give.h"
#include "cheats/op_wave.h"
#include "cheats/op_stack.h"
#include "cheats/op_corp.h"
#include "cheats/op_power.h"
#include "cheats/op_enemy.h"
#include "cheats/op_weapon.h"
#include "cheats/op_grid.h"
#include "cheats/op_mine.h"
#include "cheats/op_plant.h"
#include "cheats/op_build.h"

#include <cstdio>
#include <map>
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

struct CorpFake
{
	bool available = true;
	bool callsOk = true;
	std::optional<int> value = 100;
	int sets = 0;
	uint32_t lastReq = 0;

	OpCorp::Env MakeEnv()
	{
		OpCorp::Env e;
		e.available = [this] { return available; };
		e.current = [this] { return value; };
		e.set = [this](const Context&, uint32_t id, int points) { ++sets; lastReq = id; if (callsOk) value = points; return callsOk; };
		e.log = [](const std::string&) {};
		return e;
	}
};

static CheatWire::Request CorpReq(const char* points)
{
	CheatWire::Request r;
	r.id = 13;
	r.op = "cheat.corp";
	if (points) r.fields.emplace_back("points", points);
	return r;
}

static void CorpTests()
{
	const Context ctx;
	CorpFake f;
	OpCorp::SetEnv(f.MakeEnv());
	CHECK(OpCorp::Available());

	// The contract's example, and the edges 0 and 999999999.
	{
		const Result r = OpCorp::Handle(ctx, CorpReq("50000"));
		CHECK(r.status == CheatWire::Status::Ok && r.reason == "Data points set to 50000.");
		CHECK(r.fields.size() == 1 && r.fields[0].first == "points" && r.fields[0].second == "50000");
		CHECK(f.sets == 1 && f.lastReq == 13 && f.value == 50000);
		CHECK(OpCorp::Handle(ctx, CorpReq("0")).status == CheatWire::Status::Ok && f.value == 0);
		CHECK(OpCorp::Handle(ctx, CorpReq("999999999")).status == CheatWire::Status::Ok && f.value == 999999999);
		CHECK(OpCorp::Handle(ctx, CorpReq("0007")).status == CheatWire::Status::Ok && f.value == 7);
	}

	// Out of range or not plain digits: refused, the game is not asked.
	{
		CorpFake g;
		OpCorp::SetEnv(g.MakeEnv());
		for (const char* bad : { "1000000000", "-1", "+5", "12x", " 5", "", "abc", "99999999999999999999", "1.5" })
		{
			const Result r = OpCorp::Handle(ctx, CorpReq(bad));
			CHECK(r.status == CheatWire::Status::Refused && r.reason == OpCorp::kTextBounds && r.fields.empty());
		}
		CHECK(OpCorp::Handle(ctx, CorpReq(nullptr)).reason == kTextBadRequest);
		CHECK(g.sets == 0);
	}

	// No world: an error, nothing written.
	{
		CorpFake g;
		g.value = std::nullopt;
		OpCorp::SetEnv(g.MakeEnv());
		const Result r = OpCorp::Handle(ctx, CorpReq("5"));
		CHECK(r.status == CheatWire::Status::Error && r.reason == OpCorp::kTextNoWorld && g.sets == 0);
	}

	// The write fails: an error with no fields.
	{
		CorpFake g;
		g.callsOk = false;
		OpCorp::SetEnv(g.MakeEnv());
		const Result r = OpCorp::Handle(ctx, CorpReq("5"));
		CHECK(r.status == CheatWire::Status::Error && r.reason == OpCorp::kTextFailed && r.fields.empty());
	}

	// A key missing on this build: off, checked on each call; and no env at all.
	{
		CorpFake g;
		g.available = false;
		OpCorp::SetEnv(g.MakeEnv());
		CHECK(!OpCorp::Available());
		const Result r = OpCorp::Handle(ctx, CorpReq("5"));
		CHECK(r.status == CheatWire::Status::Refused && r.reason == "Not available on this server's game build.");
		CHECK(OpCorp::Handle(ctx, CorpReq(nullptr)).reason == OpCorp::kTextOff && g.sets == 0);
		g.available = true;
		CHECK(OpCorp::Available());
		OpCorp::SetEnv(OpCorp::Env());
		CHECK(!OpCorp::Available());
		CHECK(OpCorp::Handle(ctx, CorpReq("5")).reason == OpCorp::kTextOff);
	}

	// The state hook: points=<n>, nothing when there is no world or no env.
	{
		CorpFake g;
		OpCorp::SetEnv(g.MakeEnv());
		CHECK(OpCorp::StateFields("p1").size() == 1 && OpCorp::StateFields("p1")[0].first == "points" && OpCorp::StateFields("p1")[0].second == "100");
		g.value = std::nullopt;
		CHECK(OpCorp::StateFields("p1").empty());
		OpCorp::SetEnv(OpCorp::Env());
		CHECK(OpCorp::StateFields("p1").empty());
	}

	// ParsePoints on its own.
	{
		int n = 77;
		CHECK(OpCorp::ParsePoints("0", n) && n == 0);
		CHECK(OpCorp::ParsePoints("999999999", n) && n == 999999999);
		CHECK(!OpCorp::ParsePoints("1000000000", n) && n == 999999999);
	}
}

// A fake power game: buildings by asset name, the calls recorded.
struct PowerFake
{
	bool available = true;
	bool world = true;
	bool callsOk = true;
	std::map<std::string, OpPower::Building> buildings;
	int sets = 0, resets = 0, resetAlls = 0;
	uint32_t lastReq = 0;

	PowerFake()
	{
		buildings["DA_WindPowerGeneratorTier2"] = { "DA_WindPowerGeneratorTier2", "Wind Power Generator Tier 2", "producer", 100, 100 };
		buildings["DA_Smelter"] = { "DA_Smelter", "Smelter", "consumer", 20, 20 };
	}

	int Edited() const
	{
		int n = 0;
		for (const auto& b : buildings) if (b.second.value != b.second.defaultValue) ++n;
		return n;
	}

	OpPower::Env MakeEnv()
	{
		OpPower::Env e;
		e.available = [this] { return available; };
		e.find = [this](const std::string& asset, OpPower::Building& out)
		{
			if (!world) return OpPower::Lookup::NoWorld;
			const auto it = buildings.find(asset);
			if (it == buildings.end()) return OpPower::Lookup::NotFound;
			out = it->second;
			return OpPower::Lookup::Found;
		};
		e.list = [this]() -> std::optional<std::vector<OpPower::Building>>
		{
			if (!world) return std::nullopt;
			std::vector<OpPower::Building> all;
			for (const auto& b : buildings) all.push_back(b.second);
			return all;
		};
		e.edited = [this] { return Edited(); };
		e.set = [this](const Context&, uint32_t id, const std::string& asset, float v) { ++sets; lastReq = id; if (callsOk) buildings[asset].value = v; return callsOk; };
		e.reset = [this](const Context&, uint32_t id, const std::string& asset) { ++resets; lastReq = id; if (callsOk) buildings[asset].value = buildings[asset].defaultValue; return callsOk; };
		e.resetAll = [this](const Context&, uint32_t id) { ++resetAlls; lastReq = id; if (callsOk) for (auto& b : buildings) b.second.value = b.second.defaultValue; return callsOk; };
		e.log = [](const std::string&) {};
		return e;
	}
};

static CheatWire::Request PowerReq(const char* action, const char* building = nullptr, const char* value = nullptr)
{
	CheatWire::Request r;
	r.id = 14;
	r.op = "cheat.power";
	if (action) r.fields.emplace_back("do", action);
	if (building) r.fields.emplace_back("building", building);
	if (value) r.fields.emplace_back("value", value);
	return r;
}

static const std::string* Field(const Result& r, const char* key)
{
	for (const auto& f : r.fields) if (f.first == key) return &f.second;
	return nullptr;
}

static void PowerTests()
{
	const Context ctx;
	PowerFake f;
	OpPower::SetEnv(f.MakeEnv());
	CHECK(OpPower::Available());
	const char* wind = "DA_WindPowerGeneratorTier2";

	// set: the reply names the building, the value in effect and the default.
	{
		const Result r = OpPower::Handle(ctx, PowerReq("set", wind, "500"));
		CHECK(r.status == CheatWire::Status::Ok && r.reason == "Wind Power Generator Tier 2 power set to 500.");
		CHECK(r.fields.size() == 3 && *Field(r, "building") == "Wind Power Generator Tier 2" && *Field(r, "value") == "500" && *Field(r, "default") == "100");
		CHECK(f.sets == 1 && f.lastReq == 14 && f.buildings[wind].value == 500);
		CHECK(OpPower::Handle(ctx, PowerReq("set", wind, "0.01")).status == CheatWire::Status::Ok && f.buildings[wind].value == 0.01f);
		CHECK(OpPower::Handle(ctx, PowerReq("set", wind, "1000000")).status == CheatWire::Status::Ok);
		CHECK(OpPower::Handle(ctx, PowerReq("set", wind, "12.5")).reason == "Wind Power Generator Tier 2 power set to 12.5.");
	}

	// reset one: back to the default; a building with no edit is a quiet ok.
	{
		const Result r = OpPower::Handle(ctx, PowerReq("reset", wind));
		CHECK(r.status == CheatWire::Status::Ok && r.reason == "Wind Power Generator Tier 2 power back to 100.");
		CHECK(*Field(r, "value") == "100" && f.buildings[wind].value == 100 && f.resets == 1);
		CHECK(OpPower::Handle(ctx, PowerReq("reset", "DA_Smelter")).status == CheatWire::Status::Ok);
	}

	// reset-all.
	{
		OpPower::Handle(ctx, PowerReq("set", wind, "7"));
		OpPower::Handle(ctx, PowerReq("set", "DA_Smelter", "8"));
		CHECK(f.Edited() == 2);
		const Result r = OpPower::Handle(ctx, PowerReq("reset-all"));
		CHECK(r.status == CheatWire::Status::Ok && r.reason == "All building power values reset." && r.fields.empty());
		CHECK(f.resetAlls == 1 && f.Edited() == 0);
	}

	// Out of bounds: the bounds text, nothing touched.
	{
		const int sets = f.sets;
		for (const char* bad : { "0", "0.005", "-5", "1000000.01", "99999999" })
		{
			const Result r = OpPower::Handle(ctx, PowerReq("set", wind, bad));
			CHECK(r.status == CheatWire::Status::Refused && r.reason == "Power must be 0.01 to 1,000,000." && r.fields.empty());
		}
		CHECK(f.sets == sets);
	}

	// Unreadable requests: no do, a wrong do, no or bad building, no or non-numeric value.
	{
		const int sets = f.sets, resets = f.resets;
		const std::string longName(OpPower::kMaxName + 1, 'a');
		CHECK(OpPower::Handle(ctx, PowerReq(nullptr)).reason == kTextBadRequest);
		CHECK(OpPower::Handle(ctx, PowerReq("explode", wind, "5")).reason == kTextBadRequest);
		CHECK(OpPower::Handle(ctx, PowerReq("set", nullptr, "5")).reason == kTextBadRequest);
		CHECK(OpPower::Handle(ctx, PowerReq("set", "", "5")).reason == kTextBadRequest);
		CHECK(OpPower::Handle(ctx, PowerReq("reset", longName.c_str())).reason == kTextBadRequest);
		CHECK(OpPower::Handle(ctx, PowerReq("reset")).reason == kTextBadRequest);
		CHECK(OpPower::Handle(ctx, PowerReq("set", wind)).reason == kTextBadRequest);
		for (const char* bad : { "", "abc", "1e3", "+5", "5.", ".5", "1.2.3", "5 0", "nan", "inf", "1,000" })
			CHECK(OpPower::Handle(ctx, PowerReq("set", wind, bad)).reason == kTextBadRequest);
		CHECK(f.sets == sets && f.resets == resets);
		const std::string maxName(OpPower::kMaxName, 'a');
		CHECK(OpPower::Handle(ctx, PowerReq("reset", maxName.c_str())).reason == OpPower::kTextNoSuch);
	}

	// An unknown building is refused; no world is an error with its own text.
	{
		const Result r = OpPower::Handle(ctx, PowerReq("set", "DA_Nope", "5"));
		CHECK(r.status == CheatWire::Status::Refused && r.reason == "No such building on this server.");
		CHECK(OpPower::Handle(ctx, PowerReq("reset", "DA_Nope")).reason == OpPower::kTextNoSuch);
		f.world = false;
		const int sets = f.sets;
		CHECK(OpPower::Handle(ctx, PowerReq("set", wind, "5")).status == CheatWire::Status::Error);
		CHECK(OpPower::Handle(ctx, PowerReq("set", wind, "5")).reason == "Not in a world right now.");
		CHECK(OpPower::Handle(ctx, PowerReq("list")).reason == OpPower::kTextNoWorld);
		CHECK(f.sets == sets);
		f.world = true;
	}

	// The game call failed: the one failure text.
	{
		f.callsOk = false;
		CHECK(OpPower::Handle(ctx, PowerReq("set", wind, "5")).reason == "Could not change power on this server.");
		CHECK(OpPower::Handle(ctx, PowerReq("reset", wind)).status == CheatWire::Status::Error);
		CHECK(OpPower::Handle(ctx, PowerReq("reset-all")).reason == OpPower::kTextFailed);
		f.callsOk = true;
	}

	// list: count, then one entry per building.
	{
		const Result r = OpPower::Handle(ctx, PowerReq("list"));
		CHECK(r.status == CheatWire::Status::Ok && r.reason.empty());
		CHECK(*Field(r, "count") == "2" && !Field(r, "more"));
		CHECK(*Field(r, "b0") == "DA_Smelter~consumer~20~20");
		CHECK(*Field(r, "b1") == "DA_WindPowerGeneratorTier2~producer~100~100");
	}

	// list caps: 20 entries and more=1, and the whole reply stays inside 1024 bytes even with long names.
	{
		PowerFake g;
		g.buildings.clear();
		for (int i = 0; i < 45; ++i)
		{
			const std::string asset = "DA_B" + std::to_string(100 + i);
			g.buildings[asset] = { asset, "B", "producer", 1.5f, 1.5f };
		}
		OpPower::SetEnv(g.MakeEnv());
		const Result r = OpPower::Handle(ctx, PowerReq("list"));
		CHECK(*Field(r, "count") == "45" && Field(r, "more") && *Field(r, "more") == "1");
		CHECK(Field(r, "b19") && !Field(r, "b20"));
		CHECK(CheatWire::EncodeReply(14, r.status, r.reason, r.fields).size() <= CheatWire::kMaxMessage);

		g.buildings.clear();
		for (int i = 0; i < 20; ++i)
		{
			const std::string asset = std::string(OpPower::kMaxName - 3, 'a') + std::to_string(100 + i);
			g.buildings[asset] = { asset, "B", "producer", 999999.f, 999999.f };
		}
		const Result big = OpPower::Handle(ctx, PowerReq("list"));
		const std::string wire = CheatWire::EncodeReply(14, big.status, big.reason, big.fields);
		CHECK(wire.size() <= CheatWire::kMaxMessage);
		CHECK(*Field(big, "count") == "20" && Field(big, "more") && *Field(big, "more") == "1" && !Field(big, "b19"));
		CHECK(Field(big, "b0") != nullptr);
		OpPower::SetEnv(f.MakeEnv());
	}

	// The state hook: how many buildings are edited, off for none.
	{
		CHECK(OpPower::StateFields("p1").size() == 1 && OpPower::StateFields("p1")[0].second == "off");
		OpPower::Handle(ctx, PowerReq("set", wind, "9"));
		OpPower::Handle(ctx, PowerReq("set", "DA_Smelter", "9"));
		CHECK(OpPower::StateFields("p1")[0].first == "power" && OpPower::StateFields("p1")[0].second == "2");
		OpPower::Handle(ctx, PowerReq("reset-all"));
		CHECK(OpPower::StateFields("p1")[0].second == "off");
	}

	// A key missing on this build: off, checked on each call. No env: off.
	{
		f.available = false;
		CHECK(!OpPower::Available());
		const int sets = f.sets;
		const Result r = OpPower::Handle(ctx, PowerReq("set", wind, "5"));
		CHECK(r.status == CheatWire::Status::Refused && r.reason == "Not available on this server's game build." && f.sets == sets);
		CHECK(OpPower::Handle(ctx, PowerReq("list")).reason == OpPower::kTextOff);
		f.available = true;
		CHECK(OpPower::Available());
		OpPower::SetEnv(OpPower::Env());
		CHECK(!OpPower::Available());
		CHECK(OpPower::Handle(ctx, PowerReq("reset-all")).reason == OpPower::kTextOff);
		CHECK(OpPower::StateFields("p1").empty());
	}

	// Helpers: the slot name, the value text, the parser.
	{
		CHECK(OpPower::Slot("DA_Smelter") == "power:DA_Smelter");
		CHECK(OpPower::FormatValue(500) == "500" && OpPower::FormatValue(0.5f) == "0.5" && OpPower::FormatValue(33.333f) == "33.33" && OpPower::FormatValue(0.01f) == "0.01");
		double d = 0;
		CHECK(OpPower::ParseValue("12.5", d) && d == 12.5);
		CHECK(OpPower::ParseValue("-3", d) && d == -3);
		CHECK(!OpPower::ParseValue("1e3", d));
	}
}

// A fake enemy game: the two settings, the calls recorded.
struct EnemyFake
{
	bool available = true;
	bool world = true;
	bool callsOk = true;
	bool ai = false, hit = false;
	int tracked = 12;
	int aiCalls = 0, hitCalls = 0;
	uint32_t lastReq = 0;

	OpEnemy::Env MakeEnv()
	{
		OpEnemy::Env e;
		e.available  = [this] { return available; };
		e.inWorld    = [this] { return world; };
		e.aiDisabled = [this] { return ai; };
		e.oneHit     = [this] { return hit; };
		e.tracked    = [this] { return tracked; };
		e.setAi      = [this](const Context&, uint32_t id, bool on) { ++aiCalls; lastReq = id; if (callsOk) ai = on; return callsOk; };
		e.setOneHit  = [this](const Context&, uint32_t id, bool on) { ++hitCalls; lastReq = id; if (callsOk) hit = on; return callsOk; };
		e.log        = [](const std::string&) {};
		return e;
	}
};

static CheatWire::Request EnemyReq(const char* ai, const char* onehit = nullptr)
{
	CheatWire::Request r;
	r.id = 15;
	r.op = "cheat.enemy";
	if (ai) r.fields.emplace_back("ai", ai);
	if (onehit) r.fields.emplace_back("onehit", onehit);
	return r;
}

static void EnemyTests()
{
	const Context ctx;
	EnemyFake f;
	OpEnemy::SetEnv(f.MakeEnv());
	CHECK(OpEnemy::Available());

	// ai=on turns Disable AI on: the reply carries both settings and the tracked count.
	{
		const Result r = OpEnemy::Handle(ctx, EnemyReq("on"));
		CHECK(r.status == CheatWire::Status::Ok && r.reason == "Enemy AI turned off.");
		CHECK(r.fields.size() == 3 && *Field(r, "ai") == "on" && *Field(r, "onehit") == "off" && *Field(r, "tracked") == "12");
		CHECK(f.aiCalls == 1 && f.hitCalls == 0 && f.lastReq == 15);
	}
	CHECK(OpEnemy::Handle(ctx, EnemyReq("off")).reason == "Enemy AI turned on.");
	CHECK(!f.ai);
	CHECK(OpEnemy::Handle(ctx, EnemyReq(nullptr, "on")).reason == "One-hit kill on.");
	CHECK(f.hit && f.aiCalls == 2 && f.hitCalls == 1);
	CHECK(OpEnemy::Handle(ctx, EnemyReq(nullptr, "off")).reason == "One-hit kill off.");
	CHECK(!f.hit);

	// Both fields: both calls, both texts.
	{
		const Result r = OpEnemy::Handle(ctx, EnemyReq("on", "on"));
		CHECK(r.status == CheatWire::Status::Ok && r.reason == "Enemy AI turned off. One-hit kill on.");
		CHECK(*Field(r, "ai") == "on" && *Field(r, "onehit") == "on");
	}

	// Neither field: refused, nothing asked of the game. Bad values: unreadable, nothing changed.
	{
		const int a = f.aiCalls, h = f.hitCalls;
		const Result r = OpEnemy::Handle(ctx, EnemyReq(nullptr));
		CHECK(r.status == CheatWire::Status::Refused && r.reason == "Pick at least one setting.");
		CHECK(OpEnemy::Handle(ctx, EnemyReq("yes")).reason == CheatDispatch::kTextBadRequest);
		CHECK(OpEnemy::Handle(ctx, EnemyReq("on", "1")).reason == CheatDispatch::kTextBadRequest);
		CHECK(OpEnemy::Handle(ctx, EnemyReq("ON")).status == CheatWire::Status::Refused);
		CHECK(f.aiCalls == a && f.hitCalls == h);
	}

	// No world: error, nothing asked.
	{
		f.world = false;
		const int a = f.aiCalls;
		const Result r = OpEnemy::Handle(ctx, EnemyReq("on"));
		CHECK(r.status == CheatWire::Status::Error && r.reason == "Not in a world right now.");
		CHECK(f.aiCalls == a);
		f.world = true;
	}

	// The game side fails: the error text; a failed first call stops before the second.
	{
		f.callsOk = false;
		const int h = f.hitCalls;
		const Result r = OpEnemy::Handle(ctx, EnemyReq("off", "off"));
		CHECK(r.status == CheatWire::Status::Error && r.reason == "Could not change enemies on this server.");
		CHECK(f.hitCalls == h);
		f.callsOk = true;
	}

	// State hook: ai and onehit, on = in force.
	{
		f.ai = true; f.hit = false;
		const Fields s = OpEnemy::StateFields("p1");
		CHECK(s.size() == 2 && s[0].first == "ai" && s[0].second == "on" && s[1].first == "onehit" && s[1].second == "off");
	}

	// Feature unavailable: off, refused with the build text before anything is parsed.
	{
		f.available = false;
		CHECK(!OpEnemy::Available());
		const Result r = OpEnemy::Handle(ctx, EnemyReq("on"));
		CHECK(r.status == CheatWire::Status::Refused && r.reason == "Not available on this server's game build.");
		CHECK(OpEnemy::Handle(ctx, EnemyReq(nullptr)).reason == OpEnemy::kTextOff);
		f.available = true;
		CHECK(OpEnemy::Available());
	}

	// No env at all: off, no state fields.
	OpEnemy::SetEnv(OpEnemy::Env());
	CHECK(!OpEnemy::Available());
	CHECK(OpEnemy::StateFields("p1").empty());

	CHECK(std::string(OpEnemy::kFeature) == "enemies" && std::string(OpEnemy::kSlotAi) == "ai" && std::string(OpEnemy::kSlotOneHit) == "onehit");
}

// A fake weapon game: a multiplier per slot for the one player, the calls recorded.
struct WeaponFake
{
	bool available = true;
	bool world = true;
	bool weapon = true;
	bool callsOk = true;
	float damage = 1.0f, rate = 1.0f;
	int calls = 0;
	uint32_t lastReq = 0;

	OpWeapon::Env MakeEnv()
	{
		OpWeapon::Env e;
		e.available = [this] { return available; };
		e.inWorld   = [this] { return world; };
		e.hasWeapon = [this](const Context&) { return weapon; };
		e.current   = [this](const std::string&, OpWeapon::Slot s) { return s == OpWeapon::Slot::Damage ? damage : rate; };
		e.set       = [this](const Context&, uint32_t id, OpWeapon::Slot s, bool on, float mult)
		{
			++calls; lastReq = id;
			if (!callsOk) return false;
			(s == OpWeapon::Slot::Damage ? damage : rate) = on ? mult : 1.0f;
			return true;
		};
		e.log       = [](const std::string&) {};
		return e;
	}
};

static CheatWire::Request WeaponReq(const char* damage, const char* rate = nullptr)
{
	CheatWire::Request r;
	r.id = 16;
	r.op = "cheat.weapon";
	if (damage) r.fields.emplace_back("damage", damage);
	if (rate) r.fields.emplace_back("rate", rate);
	return r;
}

static void WeaponTests()
{
	const std::string id = "p1";
	Context ctx;
	ctx.playerId = &id;
	WeaponFake f;
	OpWeapon::SetEnv(f.MakeEnv());
	CHECK(OpWeapon::Available());

	// Both multipliers: the text and the reply fields carry both.
	{
		const Result r = OpWeapon::Handle(ctx, WeaponReq("4", "2"));
		CHECK(r.status == CheatWire::Status::Ok && r.reason == "Weapon damage 4x, fire rate 2x.");
		CHECK(r.fields.size() == 2 && *Field(r, "damage") == "4" && *Field(r, "rate") == "2");
		CHECK(f.calls == 2 && f.lastReq == 16 && f.damage == 4.0f && f.rate == 2.0f);
		CHECK(OpWeapon::StateFields("p1") == Fields({ { "weapon", "4,2" } }));
	}
	// One slot off leaves the other.
	{
		const Result r = OpWeapon::Handle(ctx, WeaponReq("off"));
		CHECK(r.status == CheatWire::Status::Ok && r.reason == "Weapon damage 1x, fire rate 2x.");
		CHECK(f.damage == 1.0f && f.rate == 2.0f);
	}
	// 1 is off too; with both off the boost is off.
	{
		const Result r = OpWeapon::Handle(ctx, WeaponReq(nullptr, "1"));
		CHECK(r.status == CheatWire::Status::Ok && r.reason == "Weapon boost turned off.");
		CHECK(*Field(r, "damage") == "1" && *Field(r, "rate") == "1");
		CHECK(OpWeapon::StateFields("p1") == Fields({ { "weapon", "off" } }));
	}
	// Fractions, and the range ends.
	{
		CHECK(OpWeapon::Handle(ctx, WeaponReq("0.5", "5")).reason == "Weapon damage 0.5x, fire rate 5x.");
		CHECK(OpWeapon::Handle(ctx, WeaponReq("25", "0.1")).reason == "Weapon damage 25x, fire rate 0.1x.");
		CHECK(OpWeapon::Handle(ctx, WeaponReq("off", "off")).reason == "Weapon boost turned off.");
	}
	// Out of range: refused with text 16 and nothing is called, even when the other slot is fine.
	{
		f.calls = 0;
		for (const char* bad : { "0.05", "25.5", "0", "-2" })
		{
			const Result r = OpWeapon::Handle(ctx, WeaponReq(bad));
			CHECK(r.status == CheatWire::Status::Refused && r.reason == "Damage must be 0.1 to 25, and fire rate 0.1 to 5.");
		}
		CHECK(OpWeapon::Handle(ctx, WeaponReq("4", "5.5")).reason == OpWeapon::kTextRange);
		CHECK(OpWeapon::Handle(ctx, WeaponReq("2", "0.01")).status == CheatWire::Status::Refused);
		CHECK(f.calls == 0);
	}
	// Unreadable value; neither field.
	{
		CHECK(OpWeapon::Handle(ctx, WeaponReq("big")).reason == CheatDispatch::kTextBadRequest);
		const Result r = OpWeapon::Handle(ctx, WeaponReq(nullptr));
		CHECK(r.status == CheatWire::Status::Refused && r.reason == "Pick at least one setting.");
	}
	// No world: error, nothing called.
	{
		f.world = false;
		const Result r = OpWeapon::Handle(ctx, WeaponReq("2"));
		CHECK(r.status == CheatWire::Status::Error && r.reason == "Not in a world right now." && f.calls == 0);
		f.world = true;
	}
	// No weapon: text 17 when turning a boost on; taking one off still works.
	{
		f.weapon = false;
		const Result r = OpWeapon::Handle(ctx, WeaponReq("2"));
		CHECK(r.status == CheatWire::Status::Refused && r.reason == "No weapon found on you." && f.calls == 0);
		CHECK(OpWeapon::Handle(ctx, WeaponReq("off")).status == CheatWire::Status::Ok);
		f.weapon = true;
	}
	// A failed call: error, text from the op.
	{
		f.callsOk = false;
		const Result r = OpWeapon::Handle(ctx, WeaponReq("2"));
		CHECK(r.status == CheatWire::Status::Error && r.reason == "Couldn't change your weapon on this server.");
		f.callsOk = true;
	}
	// Off in cap: refused with the off text, even for a request with no field.
	{
		f.available = false;
		CHECK(!OpWeapon::Available());
		CHECK(OpWeapon::Handle(ctx, WeaponReq("2")).reason == OpWeapon::kTextOff);
		CHECK(OpWeapon::Handle(ctx, WeaponReq(nullptr)).reason == OpWeapon::kTextOff);
		f.available = true;
	}

	// No env at all: off, no state fields.
	OpWeapon::SetEnv(OpWeapon::Env());
	CHECK(!OpWeapon::Available());
	CHECK(OpWeapon::StateFields("p1").empty());

	CHECK(std::string(OpWeapon::kFeature) == "weapons" && std::string(OpWeapon::kSlotDamage) == "damage" && std::string(OpWeapon::kSlotRate) == "rate");
}

struct GridFake
{
	bool available = true;
	bool world = true;
	bool readable = true;
	bool callsOk = true;
	int used = 0;
	OpGrid::Size size{ 12, 8 };
	int calls = 0;
	uint32_t lastReq = 0;

	OpGrid::Env MakeEnv()
	{
		OpGrid::Env e;
		e.available = [this] { return available; };
		e.inWorld   = [this] { return world; };
		e.current   = [this](const std::string&) { return std::optional<OpGrid::Size>(size); };
		e.occupied  = [this](const Context&) { return readable ? std::optional<int>(used) : std::nullopt; };
		e.resize    = [this](const Context&, uint32_t id, int cols, int rows)
		{
			++calls; lastReq = id;
			if (!callsOk) return false;
			size = OpGrid::Size{ cols, rows };
			return true;
		};
		e.log       = [](const std::string&) {};
		return e;
	}
};

static CheatWire::Request GridReq(const char* cols, const char* rows)
{
	CheatWire::Request r;
	r.id = 55;
	r.op = "cheat.grid";
	if (cols) r.fields.emplace_back("cols", cols);
	if (rows) r.fields.emplace_back("rows", rows);
	return r;
}

static void GridTests()
{
	const std::string id = "p1";
	Context ctx;
	ctx.playerId = &id;
	GridFake f;
	OpGrid::SetEnv(f.MakeEnv());
	CHECK(OpGrid::Available());

	// A good request: text 18, the reply carries both fields, the call got the request id.
	{
		const Result r = OpGrid::Handle(ctx, GridReq("20", "12"));
		CHECK(r.status == CheatWire::Status::Ok && r.reason == "Inventory is now 20 by 12.");
		CHECK(r.fields.size() == 2 && r.fields[0].first == "cols" && r.fields[0].second == "20" && r.fields[1].first == "rows" && r.fields[1].second == "12");
		CHECK(f.calls == 1 && f.lastReq == 55 && f.size == (OpGrid::Size{ 20, 12 }));
		CHECK(OpGrid::StateFields("p1") == Fields({ { "grid", "20x12" } }));
	}
	// The same size as now still goes through (the game side handles that case).
	{
		f.calls = 0;
		CHECK(OpGrid::Handle(ctx, GridReq("20", "12")).status == CheatWire::Status::Ok && f.calls == 1);
	}
	// Bounds: 8 and 40 are fine, 7 and 41 are text 19 on either side; nothing called for those.
	{
		f.calls = 0;
		CHECK(OpGrid::Handle(ctx, GridReq("8", "40")).status == CheatWire::Status::Ok && f.calls == 1);
		for (const char* bad : { "7", "41", "0", "-8" })
		{
			CHECK(OpGrid::Handle(ctx, GridReq(bad, "12")).reason == "Grid must be 8 to 40 on each side.");
			CHECK(OpGrid::Handle(ctx, GridReq("12", bad)).reason == "Grid must be 8 to 40 on each side.");
		}
		CHECK(f.calls == 1);
	}
	// Unreadable or missing values: bad request, nothing called.
	{
		f.calls = 0;
		for (const char* bad : { "big", "12.5", "", "1e1" })
			CHECK(OpGrid::Handle(ctx, GridReq(bad, "12")).reason == CheatDispatch::kTextBadRequest);
		CHECK(OpGrid::Handle(ctx, GridReq("12", nullptr)).reason == CheatDispatch::kTextBadRequest);
		CHECK(OpGrid::Handle(ctx, GridReq(nullptr, nullptr)).reason == CheatDispatch::kTextBadRequest);
		CHECK(f.calls == 0);
	}
	// Items fill more slots than the new grid has: text 20 with the count, nothing called. An exact fit goes.
	{
		f.calls = 0;
		f.used = 100;
		const Result r = OpGrid::Handle(ctx, GridReq("8", "12"));
		CHECK(r.status == CheatWire::Status::Refused && r.reason == "Your items need 100 slots. Pick a bigger grid." && f.calls == 0);
		CHECK(OpGrid::Handle(ctx, GridReq("10", "10")).status == CheatWire::Status::Ok && f.calls == 1);
		f.used = 0;
	}
	// Slots can't be read: text 21, nothing called.
	{
		f.calls = 0;
		f.readable = false;
		const Result r = OpGrid::Handle(ctx, GridReq("20", "12"));
		CHECK(r.status == CheatWire::Status::Refused && r.reason == "Cannot check your slots right now." && f.calls == 0);
		f.readable = true;
	}
	// No world: error, nothing called.
	{
		f.calls = 0;
		f.world = false;
		const Result r = OpGrid::Handle(ctx, GridReq("20", "12"));
		CHECK(r.status == CheatWire::Status::Error && r.reason == "Not in a world right now." && f.calls == 0);
		f.world = true;
	}
	// A failed call: error, text from the op.
	{
		f.callsOk = false;
		const Result r = OpGrid::Handle(ctx, GridReq("20", "12"));
		CHECK(r.status == CheatWire::Status::Error && r.reason == "Couldn't change your inventory on this server.");
		f.callsOk = true;
	}
	// Off in cap: refused with the off text.
	{
		f.available = false;
		CHECK(!OpGrid::Available());
		CHECK(OpGrid::Handle(ctx, GridReq("20", "12")).reason == OpGrid::kTextOff);
		f.available = true;
	}
	// A grid that can't be read gives no state key.
	{
		OpGrid::Env e = f.MakeEnv();
		e.current = [](const std::string&) { return std::optional<OpGrid::Size>(); };
		OpGrid::SetEnv(e);
		CHECK(OpGrid::StateFields("p1").empty());
	}
	// No env at all: off, no state fields.
	OpGrid::SetEnv(OpGrid::Env());
	CHECK(!OpGrid::Available());
	CHECK(OpGrid::StateFields("p1").empty());

	CHECK(std::string(OpGrid::kFeature) == "inventory");
}

// A fake mining game: one player's heat and boost, the calls recorded.
struct MineFake
{
	bool available = true;
	bool world = true;
	bool callsOk = true;
	OpMine::State state;
	int calls = 0;
	uint32_t lastReq = 0;

	OpMine::Env MakeEnv()
	{
		OpMine::Env e;
		e.available = [this] { return available; };
		e.inWorld   = [this] { return world; };
		e.current   = [this](const std::string&) { return state; };
		e.apply     = [this](const Context&, uint32_t id, std::optional<bool> heat, std::optional<float> boost)
		{
			++calls; lastReq = id;
			if (!callsOk) return false;
			if (heat) state.heat = *heat;
			if (boost) state.boost = *boost;
			return true;
		};
		e.log       = [](const std::string&) {};
		return e;
	}
};

static CheatWire::Request MineReq(const char* heat, const char* boost = nullptr)
{
	CheatWire::Request r;
	r.id = 17;
	r.op = "cheat.mine";
	if (heat) r.fields.emplace_back("heat", heat);
	if (boost) r.fields.emplace_back("boost", boost);
	return r;
}

static void MineTests()
{
	const std::string id = "p1";
	Context ctx;
	ctx.playerId = &id;
	MineFake f;
	OpMine::SetEnv(f.MakeEnv());
	CHECK(OpMine::Available());

	// Both: text 25, both fields, the call carries the request id.
	{
		const Result r = OpMine::Handle(ctx, MineReq("on", "3"));
		CHECK(r.status == CheatWire::Status::Ok && r.reason == "Mining heat off, boost 3x.");
		CHECK(r.fields == Fields({ { "heat", "on" }, { "boost", "3" } }));
		CHECK(f.calls == 1 && f.lastReq == 17);
		CHECK(OpMine::StateFields("p1") == Fields({ { "mine", "3" }, { "heat", "on" } }));
	}
	// One at a time: the other stays as it is, and the text follows what is in force.
	{
		CHECK(OpMine::Handle(ctx, MineReq("off")).reason == "Mining boost 3x.");
		CHECK(f.state.boost == 3.0f && !f.state.heat);
		CHECK(OpMine::Handle(ctx, MineReq("on")).reason == "Mining heat off, boost 3x.");
		CHECK(OpMine::Handle(ctx, MineReq(nullptr, "off")).reason == OpMine::kTextHeatOnly);
		CHECK(f.state.heat && f.state.boost == 1.0f);
	}
	// Both back: text 26; boost 1 is off too.
	{
		OpMine::Handle(ctx, MineReq("on", "2.5"));
		const Result r = OpMine::Handle(ctx, MineReq("off", "1"));
		CHECK(r.status == CheatWire::Status::Ok && r.reason == OpMine::kTextBothOff);
		CHECK(r.fields == Fields({ { "heat", "off" }, { "boost", "1" } }));
		CHECK(OpMine::StateFields("p1") == Fields({ { "mine", "off" }, { "heat", "off" } }));
	}
	// Range: 1 to 10 inclusive, text 27 outside.
	{
		CHECK(OpMine::Handle(ctx, MineReq(nullptr, "10")).reason == "Mining boost 10x.");
		for (const char* bad : { "0.5", "10.5", "0", "-2", "100" })
		{
			const int before = f.calls;
			const Result r = OpMine::Handle(ctx, MineReq(nullptr, bad));
			CHECK(r.status == CheatWire::Status::Refused && r.reason == "Boost must be 1 to 10." && f.calls == before);
		}
	}
	// Bad text, nothing to do.
	{
		const int before = f.calls;
		CHECK(OpMine::Handle(ctx, MineReq(nullptr, "fast")).reason == CheatDispatch::kTextBadRequest);
		CHECK(OpMine::Handle(ctx, MineReq("maybe")).reason == CheatDispatch::kTextBadRequest);
		const Result r = OpMine::Handle(ctx, MineReq(nullptr));
		CHECK(r.status == CheatWire::Status::Refused && r.reason == "Pick at least one setting.");
		CHECK(f.calls == before);
	}
	// No world: error, no call.
	{
		f.world = false;
		const int before = f.calls;
		const Result r = OpMine::Handle(ctx, MineReq("on"));
		CHECK(r.status == CheatWire::Status::Error && r.reason == "Not in a world right now." && f.calls == before);
		f.world = true;
	}
	// A failed call: error, text from the op.
	{
		f.callsOk = false;
		const Result r = OpMine::Handle(ctx, MineReq("on", "2"));
		CHECK(r.status == CheatWire::Status::Error && r.reason == "Couldn't change your mining on this server.");
		f.callsOk = true;
	}
	// Off in cap: refused with the off text.
	{
		f.available = false;
		CHECK(!OpMine::Available());
		CHECK(OpMine::Handle(ctx, MineReq("on")).reason == OpMine::kTextOff);
		f.available = true;
	}

	// No env at all: off, no state fields.
	OpMine::SetEnv(OpMine::Env());
	CHECK(!OpMine::Available());
	CHECK(OpMine::StateFields("p1").empty());

	CHECK(std::string(OpMine::kFeature) == "mining-tools" && std::string(OpMine::kSlotHeat) == "heat" && std::string(OpMine::kSlotBoost) == "boost");
}

// A fake plant game: the calls recorded, the hook installs counted.
struct PlantFake
{
	bool available = true;
	bool world = true;
	bool callsOk = true;
	int calls = 0;
	int installs = 0, removes = 0;
	uint32_t lastReq = 0;
	int lastMult = 0;

	OpPlant::Env MakeEnv()
	{
		OpPlant::Env e;
		e.available   = [this] { return available; };
		e.inWorld     = [this] { return world; };
		e.apply       = [this](const Context&, uint32_t id, int mult) { ++calls; lastReq = id; lastMult = mult; return callsOk; };
		e.installHook = [this] { ++installs; return true; };
		e.removeHook  = [this] { ++removes; };
		e.log         = [](const std::string&) {};
		return e;
	}
};

static CheatWire::Request PlantReq(const char* mult)
{
	CheatWire::Request r;
	r.id = 19;
	r.op = "cheat.plant";
	if (mult) r.fields.emplace_back("mult", mult);
	return r;
}

static void PlantTests()
{
	const std::string id = "p1";
	Context ctx;
	ctx.playerId = &id;
	PlantFake f;
	OpPlant::SetEnv(f.MakeEnv());
	CHECK(OpPlant::Available());
	CHECK(OpPlant::MultiplierFor("p1") == 1);

	// On: text 22, the field, the call carries the request id, the hook goes in once.
	{
		const Result r = OpPlant::Handle(ctx, PlantReq("3"));
		CHECK(r.status == CheatWire::Status::Ok && r.reason == "Plant pickup 3x.");
		CHECK(r.fields == Fields({ { "mult", "3" } }));
		CHECK(f.calls == 1 && f.lastReq == 19 && f.lastMult == 3 && f.installs == 1);
		CHECK(OpPlant::MultiplierFor("p1") == 3 && OpPlant::MultiplierFor("p2") == 1);
		CHECK(OpPlant::StateFields("p1") == Fields({ { "plant", "3" } }));
		CHECK(OpPlant::StateFields("p2") == Fields({ { "plant", "off" } }));
	}
	// A second player or a changed value does not install the hook again.
	{
		CHECK(OpPlant::Handle(ctx, PlantReq("5")).reason == "Plant pickup 5x.");
		OpPlant::SetMultiplier("p2", 2);
		CHECK(f.installs == 1 && f.removes == 0);
		OpPlant::SetMultiplier("p2", 1);
		CHECK(f.removes == 0 && OpPlant::MultiplierFor("p1") == 5);
	}
	// Back: text 23 for 1 and off; the last one going back removes the hook.
	{
		const Result r = OpPlant::Handle(ctx, PlantReq("1"));
		CHECK(r.status == CheatWire::Status::Ok && r.reason == OpPlant::kTextNormal);
		CHECK(r.fields == Fields({ { "mult", "1" } }));
		CHECK(f.removes == 1 && OpPlant::MultiplierFor("p1") == 1);
		CHECK(OpPlant::StateFields("p1") == Fields({ { "plant", "off" } }));
		OpPlant::Handle(ctx, PlantReq("2"));
		CHECK(OpPlant::Handle(ctx, PlantReq("off")).reason == OpPlant::kTextNormal);
		CHECK(f.installs == 2 && f.removes == 2);
	}
	// A revert from the ledger (leave, deny, stop) goes through SetMultiplier.
	{
		OpPlant::Handle(ctx, PlantReq("4"));
		OpPlant::SetMultiplier("p1", 1);
		CHECK(OpPlant::MultiplierFor("p1") == 1 && f.removes == 3);
	}
	// Range: 1 to 5 whole numbers, text 24 outside.
	{
		for (const char* bad : { "0", "6", "-1", "2.5", "100" })
		{
			const int before = f.calls;
			const Result r = OpPlant::Handle(ctx, PlantReq(bad));
			CHECK(r.status == CheatWire::Status::Refused && r.reason == "Plant pickup must be 1 to 5." && f.calls == before);
		}
	}
	// Bad text, nothing to do.
	{
		const int before = f.calls;
		CHECK(OpPlant::Handle(ctx, PlantReq("lots")).reason == CheatDispatch::kTextBadRequest);
		CHECK(OpPlant::Handle(ctx, PlantReq(nullptr)).reason == CheatDispatch::kTextBadRequest);
		CHECK(f.calls == before);
	}
	// No world: error, no call.
	{
		f.world = false;
		const int before = f.calls;
		const Result r = OpPlant::Handle(ctx, PlantReq("2"));
		CHECK(r.status == CheatWire::Status::Error && r.reason == "Not in a world right now." && f.calls == before);
		f.world = true;
	}
	// A failed call: error, the multiplier is not recorded.
	{
		f.callsOk = false;
		const Result r = OpPlant::Handle(ctx, PlantReq("2"));
		CHECK(r.status == CheatWire::Status::Error && r.reason == "Couldn't change your plant pickup on this server.");
		CHECK(OpPlant::MultiplierFor("p1") == 1);
		f.callsOk = true;
	}
	// Off in cap: refused with the off text.
	{
		f.available = false;
		CHECK(!OpPlant::Available());
		CHECK(OpPlant::Handle(ctx, PlantReq("2")).reason == OpPlant::kTextOff);
		f.available = true;
	}

	// No env at all: off, no state fields.
	OpPlant::SetEnv(OpPlant::Env());
	CHECK(!OpPlant::Available());
	CHECK(OpPlant::StateFields("p1").empty());

	CHECK(std::string(OpPlant::kFeature) == "inventory" && std::string(OpPlant::kSlot) == "plant");
}

// A fake build game: the calls recorded, the hook installs counted per part.
struct BuildFake
{
	bool parts[OpBuild::kPartCount] = { true, true, true, true };
	bool unlockAvail = true;
	bool world = true;
	bool callsOk = true;
	bool unlockedNow = false;
	int applies = 0, unlocks = 0;
	uint32_t lastReq = 0;
	bool lastOn = false;
	std::vector<OpBuild::Part> lastParts;
	int installs[OpBuild::kPartCount] = {}, removes[OpBuild::kPartCount] = {};

	OpBuild::Env MakeEnv()
	{
		OpBuild::Env e;
		e.available       = [this](OpBuild::Part p) { return parts[static_cast<int>(p)]; };
		e.unlockAvailable = [this] { return unlockAvail; };
		e.inWorld         = [this] { return world; };
		e.apply           = [this](const Context&, uint32_t id, const std::vector<OpBuild::Part>& ps, bool on)
		                    { ++applies; lastReq = id; lastOn = on; lastParts = ps; return callsOk; };
		e.installHook     = [this](OpBuild::Part p) { ++installs[static_cast<int>(p)]; return true; };
		e.removeHook      = [this](OpBuild::Part p) { ++removes[static_cast<int>(p)]; };
		e.unlocked        = [this] { return unlockedNow; };
		e.unlock          = [this](const Context&, uint32_t id) { ++unlocks; lastReq = id; if (callsOk) unlockedNow = true; return callsOk; };
		e.log             = [](const std::string&) {};
		return e;
	}
};

static CheatWire::Request BuildReq(const char* rule, const char* on, const char* part = nullptr)
{
	CheatWire::Request r;
	r.id = 23;
	r.op = "cheat.build";
	if (rule) r.fields.emplace_back("rule", rule);
	if (on) r.fields.emplace_back("on", on);
	if (part) r.fields.emplace_back("part", part);
	return r;
}

static void BuildTests()
{
	using OpBuild::Part;
	const std::string id = "p1";
	Context ctx;
	ctx.playerId = &id;
	BuildFake f;
	OpBuild::SetEnv(f.MakeEnv());
	CHECK(OpBuild::Available());
	const auto all = [](const char*) { return true; };

	// The feature follows rule=; missing or unknown is build-free.
	CHECK(std::string(OpBuild::FeatureFor(BuildReq("free", "on"))) == "build-free");
	CHECK(std::string(OpBuild::FeatureFor(BuildReq("anywhere", "on"))) == "build-anywhere");
	CHECK(std::string(OpBuild::FeatureFor(BuildReq("unlock", "on"))) == "build-unlock");
	CHECK(std::string(OpBuild::FeatureFor(BuildReq("bogus", "on"))) == "build-free");
	CHECK(std::string(OpBuild::FeatureFor(BuildReq(nullptr, "on"))) == "build-free");

	// Free on, every part: text 28, parts listed, each hook goes in once.
	{
		const Result r = OpBuild::Handle(ctx, BuildReq("free", "on"));
		CHECK(r.status == CheatWire::Status::Ok && r.reason == "Free building is on for your builds.");
		CHECK(r.fields == Fields({ { "rule", "free" }, { "on", "on" }, { "parts", "cost,stability" } }));
		CHECK(f.applies == 1 && f.lastReq == 23 && f.lastOn && f.lastParts.size() == 2);
		CHECK(OpBuild::PartOn("p1", Part::Cost) && OpBuild::PartOn("p1", Part::Stability));
		CHECK(!OpBuild::PartOn("p1", Part::Windows) && !OpBuild::PartOn("p2", Part::Cost));
		CHECK(f.installs[0] == 1 && f.installs[1] == 1 && f.installs[2] == 0);
		CHECK(OpBuild::StateFieldsFor("p1", all) == Fields({ { "build", "cost,stability" }, { "unlock", "off" } }));
	}
	// Anywhere with one part: text 30, only that part; the free part stays.
	{
		const Result r = OpBuild::Handle(ctx, BuildReq("anywhere", "on", "windows"));
		CHECK(r.reason == "Building anywhere is on for your builds.");
		CHECK(r.fields == Fields({ { "rule", "anywhere" }, { "on", "on" }, { "parts", "windows" } }));
		CHECK(OpBuild::PartOn("p1", Part::Windows) && !OpBuild::PartOn("p1", Part::Benches) && OpBuild::PartOn("p1", Part::Cost));
		CHECK(f.installs[2] == 1 && f.installs[3] == 0);
	}
	// A second player does not install again; the first one leaving does not remove while another is on.
	{
		OpBuild::SetPart("p2", Part::Cost, true);
		CHECK(f.installs[0] == 1);
		OpBuild::SetPart("p1", Part::Cost, false);
		CHECK(f.removes[0] == 0 && OpBuild::PartOn("p2", Part::Cost) && !OpBuild::PartOn("p1", Part::Cost));
		OpBuild::SetPart("p2", Part::Cost, false);
		CHECK(f.removes[0] == 1);
	}
	// Off: texts 29 and 31; the last one off removes the hooks. A revert from the ledger uses SetPart.
	{
		const Result r = OpBuild::Handle(ctx, BuildReq("free", "off"));
		CHECK(r.status == CheatWire::Status::Ok && r.reason == "Free building is off." && !f.lastOn);
		CHECK(r.fields == Fields({ { "rule", "free" }, { "on", "off" }, { "parts", "cost,stability" } }));
		CHECK(!OpBuild::PartOn("p1", Part::Stability) && f.removes[1] == 1);
		CHECK(OpBuild::Handle(ctx, BuildReq("anywhere", "off")).reason == "Building anywhere is off.");
		CHECK(f.removes[2] == 1 && f.removes[3] == 0);
		CHECK(OpBuild::StateFieldsFor("p1", all) == Fields({ { "build", "none" }, { "unlock", "off" } }));
		OpBuild::SetPart("p1", Part::Benches, true);
		OpBuild::SetPart("p1", Part::Benches, false);
	}
	// State: a part counts only while its own feature is allowed; no key with both features denied;
	// unlock goes out whatever the rules say.
	{
		OpBuild::SetPart("p1", Part::Cost, true);
		OpBuild::SetPart("p1", Part::Windows, true);
		const auto onlyAnywhere = [](const char* feat) { return std::string(feat) == "build-anywhere"; };
		CHECK(OpBuild::StateFieldsFor("p1", onlyAnywhere) == Fields({ { "build", "windows" }, { "unlock", "off" } }));
		const auto none = [](const char*) { return false; };
		CHECK(OpBuild::StateFieldsFor("p1", none) == Fields({ { "unlock", "off" } }));
		f.unlockedNow = true;
		CHECK(OpBuild::StateFieldsFor("p1", none) == Fields({ { "unlock", "on" } }));
		f.unlockedNow = false;
		OpBuild::SetPart("p1", Part::Cost, false);
		OpBuild::SetPart("p1", Part::Windows, false);
	}
	// A part that does not work here is left out of a whole-rule request and refused when named.
	{
		f.parts[static_cast<int>(Part::Stability)] = false;
		const Result r = OpBuild::Handle(ctx, BuildReq("free", "on"));
		CHECK(r.fields == Fields({ { "rule", "free" }, { "on", "on" }, { "parts", "cost" } }));
		OpBuild::Handle(ctx, BuildReq("free", "off"));
		const int before = f.applies;
		CHECK(OpBuild::Handle(ctx, BuildReq("free", "on", "stability")).reason == OpBuild::kTextOff && f.applies == before);
		f.parts[static_cast<int>(Part::Cost)] = false;
		CHECK(OpBuild::Handle(ctx, BuildReq("free", "on")).reason == OpBuild::kTextOff);
		CHECK(OpBuild::Available());   // anywhere and unlock still work
		f.parts[static_cast<int>(Part::Cost)] = true;
		f.parts[static_cast<int>(Part::Stability)] = true;
	}
	// Unlock: grant once, text 32; asking again is ok with the same text and no second call.
	{
		const Result r = OpBuild::Handle(ctx, BuildReq("unlock", "on"));
		CHECK(r.status == CheatWire::Status::Ok && r.reason == "All buildings are unlocked on this server.");
		CHECK(r.fields == Fields({ { "rule", "unlock" }, { "on", "on" } }));
		CHECK(f.unlocks == 1 && f.lastReq == 23 && f.unlockedNow);
		CHECK(OpBuild::Handle(ctx, BuildReq("unlock", "on")).reason == OpBuild::kTextUnlocked && f.unlocks == 1);
		CHECK(OpBuild::StateFieldsFor("p1", [](const char*) { return false; }) == Fields({ { "unlock", "on" } }));
		// Locking again: refused, text 33, no call.
		const Result no = OpBuild::Handle(ctx, BuildReq("unlock", "off"));
		CHECK(no.status == CheatWire::Status::Refused && no.reason == "Unlocked buildings can't be locked again.");
		CHECK(f.unlocks == 1);
		f.unlockedNow = false;
	}
	// Bad requests: nothing is called.
	{
		const int before = f.applies + f.unlocks;
		const Result bad[] = {
			OpBuild::Handle(ctx, BuildReq(nullptr, "on")),
			OpBuild::Handle(ctx, BuildReq("free", nullptr)),
			OpBuild::Handle(ctx, BuildReq("free", "maybe")),
			OpBuild::Handle(ctx, BuildReq("bogus", "on")),
			OpBuild::Handle(ctx, BuildReq("free", "on", "windows")),
			OpBuild::Handle(ctx, BuildReq("anywhere", "on", "cost")),
			OpBuild::Handle(ctx, BuildReq("free", "on", "nope")),
			OpBuild::Handle(ctx, BuildReq("unlock", "on", "cost")),
		};
		for (const Result& r : bad)
			CHECK(r.status == CheatWire::Status::Refused && r.reason == CheatDispatch::kTextBadRequest);
		CHECK(f.applies + f.unlocks == before);
	}
	// No world: error, no call.
	{
		f.world = false;
		const int before = f.applies + f.unlocks;
		for (const char* rule : { "free", "anywhere", "unlock" })
		{
			const Result r = OpBuild::Handle(ctx, BuildReq(rule, "on"));
			CHECK(r.status == CheatWire::Status::Error && r.reason == "Not in a world right now.");
		}
		CHECK(f.applies + f.unlocks == before);
		f.world = true;
	}
	// A failed call: error, nothing recorded, the flag not set.
	{
		f.callsOk = false;
		const Result a = OpBuild::Handle(ctx, BuildReq("free", "on"));
		CHECK(a.status == CheatWire::Status::Error && a.reason == "Couldn't change building on this server.");
		CHECK(!OpBuild::PartOn("p1", Part::Cost));
		const Result u = OpBuild::Handle(ctx, BuildReq("unlock", "on"));
		CHECK(u.status == CheatWire::Status::Error && !f.unlockedNow);
		f.callsOk = true;
	}
	// Unlock off here while the parts work: unlock is refused as off, the rest runs.
	{
		f.unlockAvail = false;
		CHECK(OpBuild::Handle(ctx, BuildReq("unlock", "on")).reason == OpBuild::kTextOff);
		CHECK(OpBuild::Available());
		f.unlockAvail = true;
	}
	// Everything off: not available, refused with the off text.
	{
		for (bool& p : f.parts) p = false;
		f.unlockAvail = false;
		CHECK(!OpBuild::Available());
		CHECK(OpBuild::Handle(ctx, BuildReq("free", "on")).reason == OpBuild::kTextOff);
	}

	// No env at all: off, no state fields.
	OpBuild::SetEnv(OpBuild::Env());
	CHECK(!OpBuild::Available());
	CHECK(OpBuild::StateFieldsFor("p1", all).empty());

	CHECK(std::string(OpBuild::kFeatureFree) == "build-free" && std::string(OpBuild::kFeatureAnywhere) == "build-anywhere"
		&& std::string(OpBuild::kFeatureUnlock) == "build-unlock");
}

int main()
{
	const Context ctx;
	PowerTests();
	EnemyTests();
	WeaponTests();
	GridTests();
	MineTests();
	PlantTests();
	BuildTests();
	WaveTests();
	StackTests();
	CorpTests();
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
