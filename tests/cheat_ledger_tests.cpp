// Unit tests of the cheat revert ledger: capture once, restore only our own value, and when
// Toggles and Overrides revert. No project file: from a Visual Studio developer prompt, in the repo root:
//   cl /nologo /EHsc /std:c++17 /Isrc tests\cheat_ledger_tests.cpp src\cheats\cheat_ledger.cpp /Fe:cheat_ledger_tests.exe
//   cheat_ledger_tests.exe
#include "cheats/cheat_ledger.h"

#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

static int g_checks = 0, g_failed = 0;

#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failed; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

using namespace CheatLedger;

static bool Has(const std::vector<std::string>& logs, const std::string& part)
{
	for (const auto& l : logs)
		if (l.find(part) != std::string::npos)
			return true;
	return false;
}

struct Rig
{
	std::map<std::string, Value> live;      // "target index|slot" -> live value
	std::set<int32_t> dead;                 // target / conn indexes that no longer exist
	std::set<std::string> denied;           // player ids the feature is denied for (owner's stored proof is false)
	std::set<std::string> connected;        // player ids connected now
	std::map<std::string, int32_t> pawn;    // player id -> the index of the object their pawn has now (none: no pawn)
	bool gameThread = true;
	uint64_t now = 1000;
	int writes = 0;
	std::vector<std::string> logs;
	Ledger ledger;

	Rig() : ledger(MakeEnv()) {}

	static std::string Key(const Entry& e) { return std::to_string(e.target.index) + "|" + e.slot; }

	Env MakeEnv()
	{
		Env env;
		env.read = [this](const Entry& e) { auto it = live.find(Key(e)); return it == live.end() ? Value() : it->second; };
		env.write = [this](const Entry& e, const Value& v) { live[Key(e)] = v; ++writes; return true; };
		env.alive = [this](const Weak& w) { return !dead.count(w.index); };
		env.rebind = [this](const Entry& e)
		{
			const auto it = pawn.find(e.playerId);
			return it == pawn.end() ? Weak() : Weak{ it->second, 1, 2, 0x2000 };
		};
		env.allowed = [this](Entry& e)
		{
			// Connected owner: live state, refreshed into the entry. Gone owner: the stored state.
			if (connected.count(e.playerId))
				e.ownerProven = !denied.count(e.playerId);
			return e.ownerProven ? Allowance::Allowed : Allowance::Denied;
		};
		env.nowMs = [this] { return now; };
		env.isGameThread = [this] { return gameThread; };
		env.log = [this](const std::string& s) { logs.push_back(s); };
		return env;
	}

	Request Req(Kind kind, const std::string& player, const std::string& slot, Value v, int32_t target = 10, bool follow = false)
	{
		Request r;
		r.playerId = player; r.playerName = "Name" + player;
		r.conn = Weak{ 5, 1, 1, 0x1000 };
		r.feature = "feat"; r.slot = slot; r.kind = kind;
		r.target = Weak{ target, 1, 2, 0x2000 };
		r.follow = follow; r.value = v; r.reqId = 7; r.ownerProven = true;
		connected.insert(player);
		return r;
	}
	// A tick that first sees the new pawn, then one after the settle period: the entry has moved.
	void Respawned() { ledger.Tick(); now += kFollowSettleMs; ledger.Tick(); }
	void Seed(int32_t target, const std::string& slot, Value v) { live[std::to_string(target) + "|" + slot] = v; }
	Value Live(int32_t target, const std::string& slot) { return live[std::to_string(target) + "|" + slot]; }
};

static void Toggles()
{
	{   // leave
		Rig r; r.Seed(10, "fly", false);
		CHECK(r.ledger.Apply(r.Req(Kind::Toggle, "A", "fly", true)));
		CHECK(r.Live(10, "fly") == Value(true));
		r.ledger.RestorePlayer("B");
		CHECK(r.ledger.Size() == 1);
		r.ledger.RestorePlayer("A");
		CHECK(r.ledger.Size() == 0);
		CHECK(r.Live(10, "fly") == Value(false));
	}
	{   // deny
		Rig r; r.Seed(10, "fly", false);
		r.ledger.Apply(r.Req(Kind::Toggle, "A", "fly", true));
		r.ledger.Reconcile();
		CHECK(r.ledger.Size() == 1);
		r.denied.insert("A");
		r.ledger.ReconcilePlayer("B");
		CHECK(r.ledger.Size() == 1);
		r.ledger.ReconcilePlayer("A");
		CHECK(r.ledger.Size() == 0);
		CHECK(r.Live(10, "fly") == Value(false));
	}
	{   // deny through MarkReconcileDue + Tick
		Rig r; r.Seed(10, "fly", false);
		r.ledger.Apply(r.Req(Kind::Toggle, "A", "fly", true));
		r.denied.insert("A");
		r.ledger.MarkReconcileDue();
		r.ledger.Tick();
		CHECK(r.ledger.Size() == 0);
		CHECK(r.Live(10, "fly") == Value(false));
	}
	{   // stop
		Rig r; r.Seed(10, "fly", false);
		r.ledger.Apply(r.Req(Kind::Toggle, "A", "fly", true));
		r.ledger.RestoreAll();
		CHECK(r.ledger.Size() == 0);
		CHECK(r.Live(10, "fly") == Value(false));
	}
	{   // world end drops without a write
		Rig r; r.Seed(10, "fly", false);
		r.ledger.Apply(r.Req(Kind::Toggle, "A", "fly", true));
		const int before = r.writes;
		r.ledger.OnWorldEnd();
		CHECK(r.ledger.Size() == 0);
		CHECK(r.writes == before);
	}
	{   // a Toggle of a player whose controller is gone is restored on rejoin
		Rig r; r.Seed(10, "fly", false);
		r.ledger.Apply(r.Req(Kind::Toggle, "A", "fly", true));
		r.ledger.SweepRejoined("A");
		CHECK(r.ledger.Size() == 1);
		r.dead.insert(5);
		r.ledger.SweepRejoined("A");
		CHECK(r.ledger.Size() == 0);
		CHECK(r.Live(10, "fly") == Value(false));
	}
}

// A follow Toggle belongs to the player: a respawn moves it to the new pawn's object.
static void Follow()
{
	const auto Setup = [](Rig& r)
	{
		r.Seed(10, "fly", false);
		r.pawn["A"] = 10;
		Request q = r.Req(Kind::Toggle, "A", "fly", true, 10, true);
		q.pin = true;
		CHECK(r.ledger.Apply(q));
	};
	{   // the pawn changes: the entry moves, with the new pawn's own prior
		Rig r; Setup(r);
		r.Seed(20, "fly", Value(false));
		r.dead.insert(10);
		r.pawn["A"] = 20;
		r.Respawned();
		CHECK(r.ledger.Size() == 1);
		CHECK(r.Live(20, "fly") == Value(true));
		const Entry* e = r.ledger.Find(Kind::Toggle, "A", "feat", "fly");
		CHECK(e && e->target.index == 20 && e->prior == Value(false) && e->applied == Value(true));
		CHECK(Has(r.logs, "follow 'NameA' (A) feat/fly"));
	}
	{   // the new pawn's prior is its own, not the old pawn's
		Rig r; Setup(r);
		r.Seed(20, "fly", Value(false));
		r.live["10|fly"] = Value(true);
		r.dead.insert(10);
		r.pawn["A"] = 20;
		r.Respawned();
		r.Seed(20, "fly", Value(true));      // the game or the new pawn's state moved on; applied is the same, so it is ours
		r.ledger.Release(Kind::Toggle, "A", "feat", "fly");
		CHECK(r.Live(20, "fly") == Value(false));   // restored to the new pawn's prior
		CHECK(r.Live(10, "fly") == Value(true));    // the old object was never written to
	}
	{   // the old pawn still alive but no longer the player's: the entry moves, the old one is left alone
		Rig r; Setup(r);
		r.Seed(20, "fly", Value(false));
		r.pawn["A"] = 20;
		const int before = r.writes;
		r.Respawned();
		CHECK(r.writes == before + 1);
		CHECK(r.Live(20, "fly") == Value(true));
		CHECK(r.Live(10, "fly") == Value(true));
	}
	{   // no pawn yet: the entry is held, nothing is written, the pin does not drop it
		Rig r; Setup(r);
		r.dead.insert(10);
		r.pawn.erase("A");
		const int before = r.writes;
		for (int i = 0; i < 5; ++i)
			r.ledger.Tick();
		CHECK(r.ledger.Size() == 1);
		CHECK(r.ledger.Has(Kind::Toggle, "A", "feat", "fly"));
		CHECK(r.writes == before);
		// the pawn arrives later
		r.Seed(20, "fly", Value(false));
		r.pawn["A"] = 20;
		r.Respawned();
		CHECK(r.Live(20, "fly") == Value(true));
	}
	{   // a new pawn whose value cannot be read yet is retried, and logged once
		Rig r; Setup(r);
		r.dead.insert(10);
		r.pawn["A"] = 20;
		r.live.erase("20|fly");
		r.Respawned();
		r.ledger.Tick();
		int stuck = 0;
		for (const auto& l : r.logs)
			stuck += l.find("retrying") != std::string::npos ? 1 : 0;
		CHECK(stuck == 1);
		CHECK(r.ledger.Size() == 1);
		r.Seed(20, "fly", Value(false));
		r.Respawned();
		CHECK(r.Live(20, "fly") == Value(true));
	}
	{   // a new pawn settles first: nothing is read or written until it has stood for the settle period
		Rig r; Setup(r);
		r.Seed(20, "fly", Value(0));         // a pawn the game has not set up yet; zero is a real value, not a signal
		r.dead.insert(10);
		r.pawn["A"] = 20;
		const int before = r.writes;
		r.ledger.Tick();
		r.now += kFollowSettleMs - 1;
		r.ledger.Tick();
		CHECK(r.writes == before);
		CHECK(r.Live(20, "fly") == Value(0));
		const Entry* e = r.ledger.Find(Kind::Toggle, "A", "feat", "fly");
		CHECK(e && e->target.index == 10);
		r.now += 1;
		r.ledger.Tick();
		CHECK(r.writes == before + 1);
		CHECK(r.Live(20, "fly") == Value(true));
		e = r.ledger.Find(Kind::Toggle, "A", "feat", "fly");
		CHECK(e && e->target.index == 20 && e->prior == Value(0));
	}
	{   // a revert during the settle period writes nothing to the new pawn
		Rig r; Setup(r);
		r.Seed(20, "fly", Value(false));
		r.dead.insert(10);
		r.pawn["A"] = 20;
		r.ledger.Tick();
		const int before = r.writes;
		CHECK(r.ledger.Release(Kind::Toggle, "A", "feat", "fly"));
		CHECK(r.ledger.Size() == 0);
		CHECK(r.writes == before);
		CHECK(r.Live(20, "fly") == Value(false));
	}
	{   // a pawn that changes again restarts the settle period
		Rig r; Setup(r);
		r.Seed(20, "fly", Value(false));
		r.Seed(30, "fly", Value(false));
		r.dead.insert(10);
		r.pawn["A"] = 20;
		r.ledger.Tick();
		r.now += kFollowSettleMs - 1;
		r.pawn["A"] = 30;
		r.ledger.Tick();
		r.now += 1;
		r.ledger.Tick();
		CHECK(r.Live(30, "fly") == Value(false));
		r.now += kFollowSettleMs;
		r.ledger.Tick();
		CHECK(r.Live(30, "fly") == Value(true));
		CHECK(r.Live(20, "fly") == Value(false));
	}
	{   // a revert against a dead object is a clean no-op, the old prior is never written to the new pawn
		Rig r; Setup(r);
		r.Seed(20, "fly", Value(false));
		r.dead.insert(10);
		const int before = r.writes;
		CHECK(r.ledger.Release(Kind::Toggle, "A", "feat", "fly"));
		CHECK(r.ledger.Size() == 0);
		CHECK(r.writes == before);
		CHECK(r.Live(20, "fly") == Value(false));
	}
	{   // leave, deny and stop still revert a followed toggle after a respawn
		Rig r; Setup(r);
		r.Seed(20, "fly", Value(false));
		r.dead.insert(10);
		r.pawn["A"] = 20;
		r.Respawned();
		r.denied.insert("A");
		r.ledger.ReconcilePlayer("A");
		CHECK(r.ledger.Size() == 0);
		CHECK(r.Live(20, "fly") == Value(false));
	}
	{
		Rig r; Setup(r);
		r.Seed(20, "fly", Value(false));
		r.dead.insert(10);
		r.pawn["A"] = 20;
		r.Respawned();
		r.ledger.RestorePlayer("A");
		CHECK(r.ledger.Size() == 0);
		CHECK(r.Live(20, "fly") == Value(false));
	}
	{
		Rig r; Setup(r);
		r.Seed(20, "fly", Value(false));
		r.dead.insert(10);
		r.pawn["A"] = 20;
		r.Respawned();
		r.ledger.RestoreAll();
		CHECK(r.ledger.Size() == 0);
		CHECK(r.Live(20, "fly") == Value(false));
	}
	{   // leave, deny and stop while the pawn is gone: a clean drop, nothing written
		Rig r; Setup(r);
		r.dead.insert(10);
		r.pawn.erase("A");
		const int before = r.writes;
		r.ledger.RestorePlayer("A");
		CHECK(r.ledger.Size() == 0);
		CHECK(r.writes == before);
	}
	{   // the pin keeps writing a followed value on the live pawn
		Rig r; Setup(r);
		r.Seed(10, "fly", Value(false));     // the game changed it
		r.ledger.Tick();
		CHECK(r.Live(10, "fly") == Value(true));
	}
	{   // a Toggle without follow is still dropped with its target
		Rig r; r.Seed(10, "fly", false);
		Request q = r.Req(Kind::Toggle, "A", "fly", true);
		q.pin = true;
		r.ledger.Apply(q);
		r.dead.insert(10);
		r.ledger.Tick();
		CHECK(r.ledger.Size() == 0);
	}
}

static void Overrides()
{
	{   // survives the owner leaving and the world ending
		Rig r; r.Seed(10, "stack", 100);
		CHECK(r.ledger.Apply(r.Req(Kind::Override, "A", "stack", 500)));
		r.connected.erase("A");
		r.ledger.RestorePlayer("A");
		CHECK(r.ledger.Size() == 1);
		CHECK(r.Live(10, "stack") == Value(500));
		const int before = r.writes;
		r.ledger.OnWorldEnd();
		CHECK(r.ledger.Size() == 1);
		CHECK(r.writes == before);
	}
	{   // an Override whose target is gone is dropped at world end
		Rig r; r.Seed(10, "stack", 100);
		r.ledger.Apply(r.Req(Kind::Override, "A", "stack", 500));
		r.dead.insert(10);
		r.ledger.OnWorldEnd();
		CHECK(r.ledger.Size() == 0);
	}
	{   // deny, owner connected
		Rig r; r.Seed(10, "stack", 100);
		r.ledger.Apply(r.Req(Kind::Override, "A", "stack", 500));
		r.ledger.Reconcile();
		CHECK(r.ledger.Size() == 1);
		r.denied.insert("A");
		r.ledger.Reconcile();
		CHECK(r.ledger.Size() == 0);
		CHECK(r.Live(10, "stack") == Value(100));
	}
	{   // deny, owner gone, still proven when last seen: stays; no longer proven when last seen: reverts
		Rig r; r.Seed(10, "stack", 100);
		r.ledger.Apply(r.Req(Kind::Override, "A", "stack", 500));
		r.connected.erase("A");
		r.ledger.Reconcile();
		CHECK(r.ledger.Size() == 1);
		const Entry* e = r.ledger.Find(Kind::Override, "", "feat", "stack");
		CHECK(e && e->ownerProven);
	}
	{   // the proof state last seen while connected is what decides after they leave
		Rig r; r.Seed(10, "stack", 100);
		r.ledger.Apply(r.Req(Kind::Override, "A", "stack", 500));
		r.denied.insert("A");
		r.ledger.ReconcilePlayer("B");          // another player: nothing happens
		CHECK(r.ledger.Size() == 1);
		r.ledger.MarkReconcileDue();            // owner still connected: the deny is seen and the entry reverts
		r.ledger.Tick();
		CHECK(r.ledger.Size() == 0);
		CHECK(r.Live(10, "stack") == Value(100));
	}
	{   // owner lost the flag (seen while connected) and then left; a later reload still reverts
		Rig r; r.Seed(10, "stack", 100);
		r.ledger.Apply(r.Req(Kind::Override, "A", "stack", 500));
		// Owner left with a stored "not proven" state.
		Request q = r.Req(Kind::Override, "A", "stack", 500);
		q.ownerProven = false;
		r.ledger.Apply(q);
		r.connected.erase("A");
		r.ledger.Reconcile();
		CHECK(r.ledger.Size() == 0);
		CHECK(r.Live(10, "stack") == Value(100));
	}
	{   // stop
		Rig r; r.Seed(10, "stack", 100);
		r.ledger.Apply(r.Req(Kind::Override, "A", "stack", 500));
		r.connected.erase("A");
		r.ledger.RestoreAll();
		CHECK(r.ledger.Size() == 0);
		CHECK(r.Live(10, "stack") == Value(100));
	}
	{   // one entry per feature and slot, whoever sets it; the first prior stays
		Rig r; r.Seed(10, "stack", 100);
		r.ledger.Apply(r.Req(Kind::Override, "A", "stack", 500));
		r.ledger.Apply(r.Req(Kind::Override, "B", "stack", 900));
		CHECK(r.ledger.Size() == 1);
		r.ledger.Release(Kind::Override, "", "feat", "stack");
		CHECK(r.Live(10, "stack") == Value(100));
	}
}

static void Grants()
{
	Rig r;
	r.ledger.Grant("A", "NameA", "items", "give", "10 x Iron", 3);
	CHECK(r.ledger.Size() == 0);
	CHECK(Has(r.logs, "grant"));
	CHECK(r.writes == 0);
}

static void OthersChange()
{
	Rig r; r.Seed(10, "fly", false);
	r.ledger.Apply(r.Req(Kind::Toggle, "A", "fly", true));
	r.Seed(10, "fly", false);                       // someone else changed it
	r.Seed(10, "fly", Value(int32_t(3)));           // to something that is not ours
	const int before = r.writes;
	r.ledger.RestorePlayer("A");
	CHECK(r.writes == before);
	CHECK(r.Live(10, "fly") == Value(int32_t(3)));
	CHECK(Has(r.logs, "changed by the game or someone else"));
	CHECK(r.ledger.Size() == 0);
}

static void CaptureOnce()
{
	Rig r; r.Seed(10, "stack", 100);
	r.ledger.Apply(r.Req(Kind::Toggle, "A", "stack", 200));
	r.ledger.Apply(r.Req(Kind::Toggle, "A", "stack", 300));
	CHECK(r.ledger.Size() == 1);
	const Entry* e = r.ledger.Find(Kind::Toggle, "A", "feat", "stack");
	CHECK(e && e->prior == Value(100) && e->applied == Value(300));
	r.ledger.RestorePlayer("A");
	CHECK(r.Live(10, "stack") == Value(100));

	// an unreadable value is refused and recorded nowhere
	Rig u;
	CHECK(!u.ledger.Apply(u.Req(Kind::Toggle, "A", "none", 1)));
	CHECK(u.ledger.Size() == 0);
}

static void LogOnCommit()
{
	Rig r; r.Seed(10, "fly", false);
	Request q = r.Req(Kind::Toggle, "A", "fly", true);
	q.pin = true;
	r.ledger.Apply(q);
	const size_t logged = r.logs.size();
	r.Seed(10, "fly", false);                       // the game flips it back; the pin writes it again
	for (int i = 0; i < 1000; ++i)
	{
		r.ledger.Tick();
		r.Seed(10, "fly", false);
	}
	CHECK(r.logs.size() == logged);
	CHECK(r.writes >= 1000);
	// Same request again changes nothing: no log line.
	r.ledger.Apply(q);
	CHECK(r.logs.size() == logged);
	// A dead target is dropped once, with one line.
	r.dead.insert(10);
	r.ledger.Tick();
	r.ledger.Tick();
	CHECK(r.ledger.Size() == 0);
	CHECK(r.logs.size() == logged + 1);
}

static void ThreadGuard()
{
	Rig r; r.Seed(10, "fly", false);
	r.ledger.Apply(r.Req(Kind::Toggle, "A", "fly", true));
	r.gameThread = false;
	const int before = r.writes;
	r.ledger.RestoreAll();
	CHECK(r.ledger.Size() == 1);
	CHECK(r.writes == before);
	CHECK(Has(r.logs, "not on the game thread"));
}

static void SlotPrefixCut()
{
	CHECK(SlotPrefix("power:Furnace_01") == "power:");
	CHECK(SlotPrefix("power:a:b") == "power:");
	CHECK(SlotPrefix("stack") == "stack");
	CHECK(SlotPrefix("power:") == "power:");
	CHECK(SlotPrefix("").empty());
}

int main()
{
	Toggles();
	Follow();
	Overrides();
	Grants();
	OthersChange();
	CaptureOnce();
	LogOnCommit();
	ThreadGuard();
	SlotPrefixCut();
	std::printf("=== cheat_ledger_tests  %d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
