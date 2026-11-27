// Unit tests of the su.cheat dispatch: the rate bucket, the op table, `cap` and the replies.
// No project file: from a Visual Studio developer prompt, in the repo root:
//   cl /nologo /EHsc /std:c++17 /Isrc tests\cheat_dispatch_tests.cpp src\cheats\cheat_dispatch.cpp src\cheats\op_state.cpp src\cheats\op_give.cpp src\cheats\op_wave.cpp src\cheats\op_stack.cpp src\cheats\cheat_wire.cpp src\admin_wire\admin_wire.cpp /Fe:cheat_dispatch_tests.exe
//   cheat_dispatch_tests.exe
#include "cheats/cheat_dispatch.h"
#include "cheats/op_give.h"
#include "cheats/op_wave.h"
#include "cheats/op_stack.h"
#include "cheats/op_state.h"

#include <cstdio>
#include <optional>
#include <cstring>
#include <string>
#include <vector>

static int g_checks = 0, g_failed = 0;

#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failed; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

using namespace CheatDispatch;

static const char US = '\x1F';

static std::string Req(uint32_t id, const std::string& op)
{
	return std::string("req") + US + "1" + US + std::to_string(id) + US + op;
}

static std::string Res(uint32_t id, const char* status, const std::string& reason, const std::string& tail = "")
{
	return std::string("res") + US + "1" + US + std::to_string(id) + US + status + US + reason + tail;
}

struct Rig
{
	double now = 100;
	std::vector<std::string> logs;
	RulesModel::Decision decision;   // what the fake check answers
	std::string denyFeature;         // this feature is refused for the caller
	int checks = 0;
	Server server;

	Rig() : server(MakeEnv()) { decision.allowed = true; }

	Env MakeEnv()
	{
		Env e;
		e.check = [this](void*, const std::string&, const char* f)
		{
			++checks;
			RulesModel::Decision d = decision;
			if (!denyFeature.empty() && denyFeature == f) { d.allowed = false; d.why = "denied"; }
			return d;
		};
		e.now = [this] { return now; };
		e.log = [this](const std::string& l) { logs.push_back(l); };
		e.suVersion = "0.19.0";
		return e;
	}

	std::string Send(const std::string& player, const std::string& msg)
	{
		return server.Handle(nullptr, player, reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
	}
};

int main()
{
	// The bucket: a burst of 20, then refused, then 5 a second.
	{
		Bucket b;
		for (int i = 0; i < 20; ++i) CHECK(b.Take(10.0));
		CHECK(!b.Take(10.0));
		CHECK(!b.Take(10.1));            // 0.5 token
		CHECK(b.Take(10.3));             // 1.5 tokens
		CHECK(!b.Take(10.3));
		CHECK(b.Take(11.3));             // a second later: 5 more
		for (int i = 0; i < 4; ++i) CHECK(b.Take(11.3));
		CHECK(!b.Take(11.3));
		CHECK(b.Take(1000.0));           // the refill is capped at the burst
		for (int i = 0; i < 19; ++i) CHECK(b.Take(1000.0));
		CHECK(!b.Take(1000.0));
		CHECK(!b.Idle(1000.0));
		CHECK(b.Idle(1004.0));
		Bucket fresh;
		CHECK(fresh.Idle(0));
	}

	// The op table: ten ops. Give, wave and stack have a handler (with an `available` hook); the rest are stubs.
	{
		CHECK(Ops().size() == 10);
		for (const OpEntry& op : Ops())
		{
			CHECK(op.version == 1 && op.feature && *op.feature);
			if (std::strcmp(op.name, "cheat.give") == 0)
				CHECK(op.run == &OpGive::Handle && op.available == &OpGive::Available);
			else if (std::strcmp(op.name, "cheat.wave") == 0)
				CHECK(op.run == &OpWave::Handle && op.available == &OpWave::Available);
			else if (std::strcmp(op.name, "cheat.stack") == 0)
				CHECK(op.run == &OpStack::Handle && op.available == &OpStack::Available);
			else
				CHECK(op.run == &NotYet && !op.available);
		}
	}

	// `cap` lists all ten ops as off: give, wave and stack have no game glue here, so they are off too.
	{
		Rig r;
		std::string expect = Res(1, "ok", "", std::string(1, US) + "su=0.19.0");
		for (const char* op : { "cheat.give", "cheat.wave", "cheat.stack", "cheat.stat", "cheat.move", "cheat.skill", "cheat.corp", "cheat.power", "cheat.enemy", "cheat.build" })
			expect += std::string(1, US) + op + "=1,off";
		expect += std::string(1, US) + "cheat.state=1,on";
		CHECK(r.Send("p1", Req(1, "cap")) == expect);
		CHECK(r.checks == 0);   // cap needs no authority
	}

	// Every other op is stubbed: allowed by the rules, answered "not available yet". Give and wave,
	// with no glue, are refused as off.
	{
		Rig r;
		uint32_t id = 10;
		for (const OpEntry& op : Ops())
		{
			r.now += 10;
			const char* text = kTextNotYet;
			if (std::strcmp(op.name, "cheat.give") == 0) text = OpGive::kTextOff;
			if (std::strcmp(op.name, "cheat.wave") == 0) text = OpWave::kTextOff;
			if (std::strcmp(op.name, "cheat.stack") == 0) text = OpStack::kTextOff;
			CHECK(r.Send("p1", Req(id, op.name)) == Res(id, "refused", text));
			++id;
		}
		CHECK(r.checks == 10);
	}

	// The two refusal texts, exact.
	CHECK(std::strcmp(kTextRuleDeny, "Turned off by this server's rules. Ask the server's admin.") == 0);
	CHECK(std::strcmp(kTextAdminsOnly, "Only this server's admins can use this.") == 0);
	{
		Rig r;
		r.decision.allowed = false;
		r.decision.why = "Some rule detail";
		CHECK(r.Send("p1", Req(2, "cheat.give")) == Res(2, "refused", kTextRuleDeny));
		bool detailInLog = false;
		for (const auto& l : r.logs) if (l.find("Some rule detail") != std::string::npos) detailInLog = true;
		CHECK(detailInLog);
		// The detail never reaches the player.
		CHECK(r.Send("p1", Req(3, "cheat.give")).find("Some rule detail") == std::string::npos);

		r.decision.why = kTextAdminsOnly;
		CHECK(r.Send("p1", Req(4, "cheat.wave")) == Res(4, "refused", kTextAdminsOnly));
	}

	// An op this server does not know.
	{
		Rig r;
		CHECK(r.Send("p1", Req(5, "cheat.unknown")) == Res(5, "refused", kTextUnknownOp));
		CHECK(r.checks == 0);
	}

	// Malformed: an error reply when an id was read, silence (and one log line) when not.
	{
		Rig r;
		CHECK(r.Send("p1", Req(6, "")) == Res(6, "error", kTextBadRequest));
		const std::string noId = std::string("req") + US + "1" + US + "0" + US + "cap";
		CHECK(r.Send("p1", noId).empty());
		CHECK(r.Send("p1", noId).empty());
		int lines = 0;
		for (const auto& l : r.logs) if (l.find("unreadable") != std::string::npos) ++lines;
		CHECK(lines == 1);   // logged once per player
	}

	// Dropped messages: no reply, one log line each kind per player.
	{
		Rig r;
		CHECK(r.Send("p1", "nonsense").empty());
		CHECK(r.Send("p1", "nonsense").empty());
		CHECK(r.Send("p1", std::string("req") + US + "2" + US + "1" + US + "cap").empty());
		CHECK(r.Send("p1", std::string(1025, 'x')).empty());
		CHECK(r.logs.size() == 3);
		CHECK(r.Send("p2", "nonsense").empty());
		CHECK(r.logs.size() == 4);
	}

	// The rate limit: 20 pass, the 21st is refused, and 5 a second come back. An injected clock, no sleep.
	{
		Rig r;
		for (uint32_t i = 1; i <= 20; ++i) CHECK(r.Send("p1", Req(i, "cap")).find("ok") != std::string::npos);
		CHECK(r.Send("p1", Req(21, "cap")) == Res(21, "refused", kTextTooMany));
		CHECK(r.Send("p1", Req(22, "cap")) == Res(22, "refused", kTextTooMany));
		int lines = 0;
		for (const auto& l : r.logs) if (l.find("Rate limit") != std::string::npos) ++lines;
		CHECK(lines == 1);
		r.now += 0.3;   // one token and a half
		CHECK(r.Send("p1", Req(23, "cap")).compare(0, 6 + 2, std::string("res") + US + "1" + US + "23") == 0);
		CHECK(r.Send("p1", Req(24, "cap")) == Res(24, "refused", kTextTooMany));
		r.now += 1.0;   // five more
		for (uint32_t i = 30; i < 35; ++i) CHECK(r.Send("p1", Req(i, "cap")).find(std::string(1, US) + "ok") != std::string::npos);
		CHECK(r.Send("p1", Req(40, "cap")) == Res(40, "refused", kTextTooMany));
		// Another player has a bucket of their own.
		CHECK(r.Send("p2", Req(1, "cap")).find(std::string(1, US) + "ok") != std::string::npos);
		// Forget gives the player a fresh bucket.
		r.server.Forget("p1");
		CHECK(r.Send("p1", Req(41, "cap")).find(std::string(1, US) + "ok") != std::string::npos);
	}

	// A malformed message with an id still costs a token.
	{
		Rig r;
		for (uint32_t i = 0; i < 20; ++i) r.Send("p1", Req(1, ""));
		CHECK(r.Send("p1", Req(2, "")).empty());   // over the rate: silence
	}

	// cheat.state: with no op on, an ok reply with nothing in it, no authority check and no log.
	{
		Rig r;
		CHECK(r.Send("p1", Req(50, "cheat.state")) == Res(50, "ok", ""));
		CHECK(r.checks == 0 && r.logs.empty());
	}

	// cheat.state with wave and stack on in cap.
	{
		OpWave::Env we;
		we.available = [] { return true; };
		we.state  = [] { return std::optional<OpWave::State>(OpWave::State{ true, true }); };
		we.start  = [](int) { return true; };
		we.pause  = [](const Context&, uint32_t) { return true; };
		we.resume = [](const Context&, uint32_t) { return true; };
		we.cancel = [](const Context&, uint32_t) { return true; };
		OpWave::SetEnv(we);
		OpStack::Env se;
		bool overridden = true;
		se.available  = [] { return true; };
		se.current    = [] { return 500; };
		se.set        = [](const Context&, uint32_t, int) { return true; };
		se.clear      = [](const Context&, uint32_t) { return true; };
		se.overridden = [&overridden] { return overridden; };
		OpStack::SetEnv(se);
		const std::string U = std::string(1, US);

		// An allowed caller gets both keys; no Override reads off.
		{
			Rig r;
			CHECK(r.Send("p1", Req(60, "cheat.state")) == Res(60, "ok", "", U + "wave=paused" + U + "stack=500"));
			CHECK(r.logs.empty());
			overridden = false;
			r.now += 10;
			CHECK(r.Send("p1", Req(61, "cheat.state")) == Res(61, "ok", "", U + "wave=paused" + U + "stack=off"));
			overridden = true;
		}
		// An op the rules do not allow for this caller is left out.
		{
			Rig r;
			r.denyFeature = "stack-size";
			CHECK(r.Send("p1", Req(62, "cheat.state")) == Res(62, "ok", "", U + "wave=paused"));
			r.denyFeature = "rupture-wave";
			r.now += 10;
			CHECK(r.Send("p1", Req(63, "cheat.state")) == Res(63, "ok", "", U + "stack=500"));
		}
		// An op that is off in cap is left out.
		{
			Rig r;
			OpStack::Env off = se;
			off.available = [] { return false; };
			OpStack::SetEnv(off);
			CHECK(r.Send("p1", Req(64, "cheat.state")) == Res(64, "ok", "", U + "wave=paused"));
			OpStack::SetEnv(se);
		}
		// The wave's three states.
		{
			Rig r;
			bool running = false, paused = false;
			OpWave::Env w = we;
			w.state = [&] { return std::optional<OpWave::State>(OpWave::State{ running, paused }); };
			OpWave::SetEnv(w);
			r.denyFeature = "stack-size";
			CHECK(r.Send("p1", Req(65, "cheat.state")) == Res(65, "ok", "", U + "wave=none"));
			running = true; r.now += 10;
			CHECK(r.Send("p1", Req(66, "cheat.state")) == Res(66, "ok", "", U + "wave=running"));
			paused = true; r.now += 10;
			CHECK(r.Send("p1", Req(67, "cheat.state")) == Res(67, "ok", "", U + "wave=paused"));
			OpWave::SetEnv(we);
		}
		// A field on the request is refused as unreadable, with no rules check.
		{
			Rig r;
			CHECK(r.Send("p1", Req(68, "cheat.state") + US + "x=1") == Res(68, "refused", kTextBadRequest));
			CHECK(r.checks == 0);
		}
		// cap lists cheat.state as on.
		{
			Rig r;
			CHECK(r.Send("p1", Req(69, "cap")).find("cheat.state=1,on") != std::string::npos);
		}
		OpWave::SetEnv(OpWave::Env());
		OpStack::SetEnv(OpStack::Env());
	}

	// The reply with every planned key at its longest fits the 1024-byte limit.
	{
		const std::string reply = CheatWire::EncodeReply(2147483647, CheatWire::Status::Ok, "", OpState::WorstCaseFields());
		CHECK(!reply.empty() && reply.size() <= CheatWire::kMaxMessage);
		CHECK(OpState::WorstCaseFields().size() == 12);
	}

	std::printf("cheat_dispatch_tests: %d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
