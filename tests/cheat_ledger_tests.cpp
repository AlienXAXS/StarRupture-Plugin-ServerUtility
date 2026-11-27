// Unit tests of the cheat revert ledger: capture once, restore only our own value, and when
// Toggles and Overrides revert. No project file: from a Visual Studio developer prompt, in the repo root:
//   cl /nologo /EHsc /std:c++17 /Isrc tests\cheat_ledger_tests.cpp src\cheats\cheat_ledger.cpp /Fe:cheat_ledger_tests.exe
//   cheat_ledger_tests.exe
#include "cheats/cheat_ledger.h"
#include "pawn_kind.h"

#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <variant>
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

static const Value kHalf = std::vector<float>{ 9.0f, 9.0f, 1.0f, 1.0f };

struct Rig
{
	std::map<std::string, Value> live;      // "target index|slot" -> live value
	std::set<int32_t> dead;                 // target / conn indexes that no longer exist
	std::set<std::string> denied;           // player ids the feature is denied for (owner's stored proof is false)
	std::set<std::string> connected;        // player ids connected now
	std::map<std::string, int32_t> pawn;    // player id -> the index of the object their pawn has now (none: no pawn)
	std::set<std::string> vehicle;          // player ids who possess something that is not their character
	std::set<std::string> spectator;        // player ids who hold a spectator pawn (their character died)
	int stanceCalls = 0;                    // pawn reads, one per controller per tick expected
	bool failWrites = false;                // every write fails
	std::set<int32_t> failTargets;          // writes to these targets fail, nothing written
	int halfWrites = 0;                     // the next N writes leave a half value (kHalf) and fail
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
		env.write = [this](const Entry& e, const Value& v)
		{
			if (halfWrites > 0) { --halfWrites; live[Key(e)] = kHalf; return false; }
			if (failWrites || failTargets.count(e.target.index)) return false;
			live[Key(e)] = v; ++writes; return true;
		};
		env.alive = [this](const Weak& w) { return w.Valid() && !dead.count(w.index); };
		env.stance = [this](const Entry& e)
		{
			++stanceCalls;
			const bool inVehicle = vehicle.count(e.playerId) != 0;
			const bool inSpectator = spectator.count(e.playerId) != 0;
			const auto it = pawn.find(e.playerId);
			const bool isCharacter = !inVehicle && !inSpectator && it != pawn.end();
			Stance st;
			switch (ClassifyPawn(inVehicle || inSpectator || it != pawn.end(), isCharacter, inSpectator))
			{
			case PawnKind::Character:
				st.where = Stance::Where::Character;
				st.character = Weak{ it->second, 1, 2, 0x2000 };
				break;
			case PawnKind::Other:
				st.where = Stance::Where::Elsewhere;
				break;
			case PawnKind::None:
				break;
			}
			return st;
		};
		// The rig's character is the object the value lives on.
		env.rebind = [](const Entry&, const Weak& character) { return character; };
		// The three composed shapes of the game side: a weapon multiplier (float), a speed
		// ({top, braking}) and a boost ({cur base, cur, max base, max}).
		env.compose = [](const Entry& e, const Value& prior) -> Value
		{
			const float* m = std::get_if<float>(&e.asked);
			if (!m)
				return Value();
			if (e.slot == "weapon")
			{
				const float* p = std::get_if<float>(&prior);
				return p ? Value(*p * *m) : Value();
			}
			const auto* v = std::get_if<std::vector<float>>(&prior);
			if (e.slot == "speed" && v && v->size() == 2)
				return Value(std::vector<float>{ (*v)[0] * *m, (*v)[0] * *m * 4.0f });
			if (e.slot == "boost" && v && v->size() == 4)
			{
				std::vector<float> a = { *m, *m, (*v)[2], (*v)[3] };
				if ((*v)[3] < *m)
					a[2] = a[3] = *m;
				return Value(a);
			}
			return Value();
		};
		// The boost's current pair is derived by the game from what SU writes; only its max BaseValue counts.
		env.rebases = [](const Entry& e, size_t member) { return e.slot != "boost" || member == 2; };
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
	{   // the old pawn still alive but no longer the player's: the entry moves, the old one is put back
		Rig r; Setup(r);
		r.Seed(20, "fly", Value(false));
		r.pawn["A"] = 20;
		const int before = r.writes;
		r.Respawned();
		CHECK(r.writes == before + 2);              // the new pawn's value, the old pawn's prior
		CHECK(r.Live(20, "fly") == Value(true));
		CHECK(r.Live(10, "fly") == Value(false));
		CHECK(Has(r.logs, "(the player moved to another character)"));
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

// A follow Toggle while the player possesses a vehicle: the character is put back and nothing is
// written to the vehicle; back in the character, it settles, captures the originals and writes.
// A player who moves between two live characters leaves neither holding a value with no prior.
static void FollowBack()
{
	const auto Setup = [](Rig& r)
	{
		r.Seed(10, "fly", false);
		r.Seed(20, "fly", false);
		r.pawn["A"] = 10;
		Request q = r.Req(Kind::Toggle, "A", "fly", true, 10, true);
		q.pin = true;
		CHECK(r.ledger.Apply(q));
	};
	{   // A -> B -> A: A is clean while the player is on B, and its prior is its true value on return
		Rig r; Setup(r);
		r.pawn["A"] = 20;
		r.Respawned();
		CHECK(r.Live(10, "fly") == Value(false));
		CHECK(r.Live(20, "fly") == Value(true));
		r.pawn["A"] = 10;
		r.Respawned();
		CHECK(r.Live(10, "fly") == Value(true));
		CHECK(r.Live(20, "fly") == Value(false));
		const Entry* e = r.ledger.Find(Kind::Toggle, "A", "feat", "fly");
		CHECK(e && e->target.index == 10 && e->prior == Value(false) && e->applied == Value(true));
		CHECK(r.ledger.Release(Kind::Toggle, "A", "feat", "fly"));
		CHECK(r.Live(10, "fly") == Value(false));   // a release leaves both clean
		CHECK(r.Live(20, "fly") == Value(false));
		CHECK(r.ledger.Size() == 0);
	}
	{   // a new pawn that cannot take the value yet: the old one is left holding it until the move happens
		Rig r; Setup(r);
		r.live.erase("20|fly");
		r.pawn["A"] = 20;
		r.Respawned();
		CHECK(r.Live(10, "fly") == Value(true));
		r.Seed(20, "fly", false);
		r.Respawned();
		CHECK(r.Live(10, "fly") == Value(false));
		CHECK(r.Live(20, "fly") == Value(true));
	}
	{   // Apply for another character: the old one is put back before the entry moves
		Rig r; Setup(r);
		Request q = r.Req(Kind::Toggle, "A", "fly", true, 20, true);
		q.pin = true;
		CHECK(r.ledger.Apply(q));
		CHECK(r.Live(10, "fly") == Value(false));
		CHECK(r.Live(20, "fly") == Value(true));
		const Entry* e = r.ledger.Find(Kind::Toggle, "A", "feat", "fly");
		CHECK(e && e->target.index == 20 && e->prior == Value(false));
		CHECK(Has(r.logs, "(the target changed)"));
		CHECK(r.ledger.Release(Kind::Toggle, "A", "feat", "fly"));
		CHECK(r.Live(20, "fly") == Value(false));
	}
	{   // Apply for another character when the old one is gone: nothing is written to it
		Rig r; Setup(r);
		r.dead.insert(10);
		const int before = r.writes;
		Request q = r.Req(Kind::Toggle, "A", "fly", true, 20, true);
		q.pin = true;
		CHECK(r.ledger.Apply(q));
		CHECK(r.writes == before + 1);
		CHECK(r.Live(10, "fly") == Value(true));
	}
}

// A composed entry keeps what the player asked for, follows them, and takes the game's own change as its new prior.
static void Composed()
{
	const auto Weapon = [](Rig& r, float start)
	{
		r.Seed(10, "weapon", start);
		r.pawn["A"] = 10;
		Request q = r.Req(Kind::Toggle, "A", "weapon", Value(start * 2.0f), 10, true);
		q.pin = true;
		q.asked = Value(2.0f);
		CHECK(r.ledger.Apply(q));
	};
	{   // the toggle survives a respawn and a vehicle, and reads on all the while (cheat.state)
		Rig r; Weapon(r, 1.0f);
		CHECK(r.Live(10, "weapon") == Value(2.0f));
		r.dead.insert(10);
		r.pawn.erase("A");
		for (int i = 0; i < 5; ++i)
			r.ledger.Tick();
		CHECK(r.ledger.Size() == 1);
		CHECK(r.ledger.Has(Kind::Toggle, "A", "feat", "weapon"));
		CHECK(!Has(r.logs, "its target is gone"));
		r.Seed(20, "weapon", 1.0f);
		r.pawn["A"] = 20;
		r.Respawned();
		CHECK(r.Live(20, "weapon") == Value(2.0f));
		CHECK(r.ledger.Has(Kind::Toggle, "A", "feat", "weapon"));
		r.vehicle.insert("A");
		r.ledger.Tick();
		CHECK(r.Live(20, "weapon") == Value(1.0f));   // parked: the character is put back
		CHECK(r.ledger.Has(Kind::Toggle, "A", "feat", "weapon"));
	}
	{   // a new pawn: a fresh prior, composed with the multiplier asked for
		Rig r; Weapon(r, 1.0f);
		r.Seed(20, "weapon", 3.0f);
		r.dead.insert(10);
		r.pawn["A"] = 20;
		r.Respawned();
		CHECK(r.Live(20, "weapon") == Value(6.0f));
		const Entry* e = r.ledger.Find(Kind::Toggle, "A", "feat", "weapon");
		CHECK(e && e->prior == Value(3.0f) && e->applied == Value(6.0f) && e->asked == Value(2.0f));
		CHECK(r.ledger.Release(Kind::Toggle, "A", "feat", "weapon"));
		CHECK(r.Live(20, "weapon") == Value(3.0f));
	}
	{   // the game recomputes it (an attachment): its value is the new prior, composed again; our own write is not mistaken for that
		Rig r; Weapon(r, 1.0f);
		const int before = r.writes;
		for (int i = 0; i < 5; ++i)
			r.ledger.Tick();
		CHECK(r.writes == before);                    // held at the applied value: nothing to do
		CHECK(!Has(r.logs, "rebase"));
		r.live["10|weapon"] = Value(1.5f);            // the game recomputed
		r.ledger.Tick();
		CHECK(r.Live(10, "weapon") == Value(3.0f));
		const Entry* e = r.ledger.Find(Kind::Toggle, "A", "feat", "weapon");
		CHECK(e && e->prior == Value(1.5f) && e->applied == Value(3.0f));
		CHECK(Has(r.logs, "rebase 'NameA' (A) feat/weapon"));
		const int after = r.writes;
		for (int i = 0; i < 5; ++i)
			r.ledger.Tick();
		CHECK(r.writes == after);                     // our write is held, not taken for a game change
		int lines = 0;
		for (const auto& l : r.logs)
			lines += l.find("rebase") != std::string::npos ? 1 : 0;
		CHECK(lines == 1);
		CHECK(r.ledger.Release(Kind::Toggle, "A", "feat", "weapon"));
		CHECK(r.Live(10, "weapon") == Value(1.5f));   // off writes the current prior
	}
	{   // the game sets the value back to the prior it had: no change of prior, written again, no new line
		Rig r; Weapon(r, 1.0f);
		r.live["10|weapon"] = Value(1.0f);
		r.ledger.Tick();
		CHECK(r.Live(10, "weapon") == Value(2.0f));
		CHECK(!Has(r.logs, "rebase"));
	}
	{   // a changed multiplier on a held entry is composed from the same prior (the op passes it)
		Rig r; Weapon(r, 1.0f);
		Request q = r.Req(Kind::Toggle, "A", "weapon", Value(3.0f), 10, true);
		q.pin = true;
		q.asked = Value(3.0f);
		CHECK(r.ledger.Apply(q));
		const Entry* e = r.ledger.Find(Kind::Toggle, "A", "feat", "weapon");
		CHECK(e && e->prior == Value(1.0f) && e->applied == Value(3.0f) && e->asked == Value(3.0f));
	}
	{   // speed: {top, braking}
		Rig r; r.Seed(10, "speed", Value(std::vector<float>{ 600.0f, 2000.0f }));
		r.pawn["A"] = 10;
		Request q = r.Req(Kind::Toggle, "A", "speed", Value(std::vector<float>{ 1200.0f, 4800.0f }), 10, true);
		q.pin = true;
		q.asked = Value(2.0f);
		CHECK(r.ledger.Apply(q));
		r.Seed(20, "speed", Value(std::vector<float>{ 500.0f, 1500.0f }));
		r.dead.insert(10);
		r.pawn["A"] = 20;
		r.Respawned();
		CHECK(r.Live(20, "speed") == Value(std::vector<float>{ 1000.0f, 4000.0f }));
		r.live["20|speed"] = Value(std::vector<float>{ 700.0f, 1500.0f });   // the game changed the top speed
		r.ledger.Tick();
		CHECK(r.Live(20, "speed") == Value(std::vector<float>{ 1400.0f, 5600.0f }));
		CHECK(r.ledger.Release(Kind::Toggle, "A", "feat", "speed"));
		CHECK(r.Live(20, "speed") == Value(std::vector<float>{ 700.0f, 1500.0f }));
	}
	{   // boost: the four values, with the max raised to the value when it is below it
		Rig r; r.Seed(10, "boost", Value(std::vector<float>{ 1.0f, 1.0f, 1.0f, 1.0f }));
		r.pawn["A"] = 10;
		Request q = r.Req(Kind::Toggle, "A", "boost", Value(std::vector<float>{ 3.0f, 3.0f, 3.0f, 3.0f }), 10, true);
		q.pin = true;
		q.asked = Value(3.0f);
		CHECK(r.ledger.Apply(q));
		r.Seed(20, "boost", Value(std::vector<float>{ 1.0f, 1.0f, 5.0f, 5.0f }));
		r.dead.insert(10);
		r.pawn["A"] = 20;
		r.Respawned();
		CHECK(r.Live(20, "boost") == Value(std::vector<float>{ 3.0f, 3.0f, 5.0f, 5.0f }));   // a max already above: kept
		r.live["20|boost"] = Value(std::vector<float>{ 1.0f, 1.0f, 2.0f, 2.0f });             // the game lowered the max BaseValue (its CurrentValues are derived, never rebased)
		r.ledger.Tick();
		CHECK(r.Live(20, "boost") == Value(std::vector<float>{ 3.0f, 3.0f, 2.0f, 5.0f }));   // composed again from the new max base
		CHECK(r.ledger.Release(Kind::Toggle, "A", "feat", "boost"));
		CHECK(r.Live(20, "boost") == Value(std::vector<float>{ 1.0f, 1.0f, 2.0f, 5.0f }));
	}
	{   // a failed write of the composed value is tried again next tick, from the game's value
		Rig r; Weapon(r, 1.0f);
		r.live["10|weapon"] = Value(1.5f);
		r.failWrites = true;
		r.ledger.Tick();
		CHECK(r.Live(10, "weapon") == Value(1.5f));
		r.failWrites = false;
		r.ledger.Tick();
		CHECK(r.Live(10, "weapon") == Value(3.0f));
	}
}

static void FollowVehicle()
{
	const auto Setup = [](Rig& r)
	{
		r.Seed(10, "fly", false);
		r.pawn["A"] = 10;
		Request q = r.Req(Kind::Toggle, "A", "fly", true, 10, true);
		q.pin = true;
		CHECK(r.ledger.Apply(q));
	};
	{   // character -> vehicle: the character is reverted at once, the vehicle gets no write
		Rig r; Setup(r);
		r.vehicle.insert("A");
		r.ledger.Tick();
		CHECK(r.Live(10, "fly") == Value(false));
		CHECK(r.writes == 2);                                    // the apply and the revert
		CHECK(r.ledger.Has(Kind::Toggle, "A", "feat", "fly"));   // read-back stays on
		CHECK(Has(r.logs, "(possessed a vehicle)"));
		// no further write, not even from the pin, however long it lasts
		for (int i = 0; i < 5; ++i)
		{
			r.now += kFollowSettleMs;
			r.ledger.Tick();
		}
		CHECK(r.writes == 2);
		CHECK(r.Live(10, "fly") == Value(false));
		CHECK(r.live.size() == 1);                               // nothing was written to any other object
		CHECK(!Has(r.logs, "retrying"));                         // an expected vehicle is not a stuck follow
		const Entry* e = r.ledger.Find(Kind::Toggle, "A", "feat", "fly");
		CHECK(e && e->parked && e->applied == Value(true));
	}
	{   // vehicle -> the same character: it settles, then captures the originals and writes
		Rig r; Setup(r);
		r.vehicle.insert("A");
		r.ledger.Tick();
		r.vehicle.erase("A");
		const int before = r.writes;
		r.ledger.Tick();                                         // sees the character: the settle starts
		CHECK(r.writes == before);
		r.now += kFollowSettleMs - 1;
		r.ledger.Tick();
		CHECK(r.writes == before);
		CHECK(r.Live(10, "fly") == Value(false));
		r.now += 1;
		r.ledger.Tick();
		CHECK(r.writes == before + 1);
		CHECK(r.Live(10, "fly") == Value(true));
		const Entry* e = r.ledger.Find(Kind::Toggle, "A", "feat", "fly");
		CHECK(e && !e->parked && e->target.index == 10 && e->prior == Value(false) && e->applied == Value(true));
		CHECK(Has(r.logs, "character again"));
		// the capture was the true original: a revert puts it back
		r.ledger.Release(Kind::Toggle, "A", "feat", "fly");
		CHECK(r.Live(10, "fly") == Value(false));
	}
	{   // the prior is captured afresh, never the one saved before the vehicle
		Rig r; r.Seed(10, "fly", Value(1));
		r.pawn["A"] = 10;
		CHECK(r.ledger.Apply(r.Req(Kind::Toggle, "A", "fly", Value(2), 10, true)));
		r.vehicle.insert("A");
		r.ledger.Tick();
		CHECK(r.Live(10, "fly") == Value(1));
		r.vehicle.erase("A");
		r.Seed(10, "fly", Value(5));                             // the game changed it while the player was away
		r.ledger.Tick();
		r.now += kFollowSettleMs;
		r.ledger.Tick();
		const Entry* e = r.ledger.Find(Kind::Toggle, "A", "feat", "fly");
		CHECK(e && e->prior == Value(5) && e->applied == Value(2));
	}
	{   // the settle restarts when the player goes back into a vehicle
		Rig r; Setup(r);
		r.vehicle.insert("A");
		r.ledger.Tick();
		r.vehicle.erase("A");
		r.ledger.Tick();
		r.now += kFollowSettleMs - 1;
		r.vehicle.insert("A");
		r.ledger.Tick();
		r.vehicle.erase("A");
		r.ledger.Tick();
		r.now += 1;
		r.ledger.Tick();
		CHECK(r.Live(10, "fly") == Value(false));                // not settled yet
		r.now += kFollowSettleMs;
		r.ledger.Tick();
		CHECK(r.Live(10, "fly") == Value(true));
	}
	{   // leave while in a vehicle: nothing is written, the entry is gone
		Rig r; Setup(r);
		r.vehicle.insert("A");
		r.ledger.Tick();
		const int before = r.writes;
		r.ledger.RestorePlayer("A");
		CHECK(r.ledger.Size() == 0);
		CHECK(!r.ledger.Has(Kind::Toggle, "A", "feat", "fly"));
		CHECK(r.writes == before);
	}
	{   // deny while in a vehicle
		Rig r; Setup(r);
		r.vehicle.insert("A");
		r.ledger.Tick();
		const int before = r.writes;
		r.denied.insert("A");
		r.ledger.ReconcilePlayer("A");
		CHECK(r.ledger.Size() == 0);
		CHECK(r.writes == before);
	}
	{   // server stop while in a vehicle
		Rig r; Setup(r);
		r.vehicle.insert("A");
		r.ledger.Tick();
		const int before = r.writes;
		r.ledger.RestoreAll();
		CHECK(r.ledger.Size() == 0);
		CHECK(r.writes == before);
		CHECK(r.Live(10, "fly") == Value(false));
	}
	{   // switched off while in a vehicle
		Rig r; Setup(r);
		r.vehicle.insert("A");
		r.ledger.Tick();
		const int before = r.writes;
		CHECK(r.ledger.Release(Kind::Toggle, "A", "feat", "fly"));
		CHECK(r.ledger.Size() == 0);
		CHECK(r.writes == before);
	}
	{   // the character died while its player was in a vehicle: nothing is written, a later character is followed
		Rig r; Setup(r);
		r.dead.insert(10);
		r.vehicle.insert("A");
		const int before = r.writes;
		r.ledger.Tick();
		CHECK(r.writes == before);
		CHECK(r.ledger.Has(Kind::Toggle, "A", "feat", "fly"));
		r.vehicle.erase("A");
		r.pawn["A"] = 20;
		r.Seed(20, "fly", Value(false));
		r.Respawned();
		CHECK(r.Live(20, "fly") == Value(true));
	}
	{   // a revert that cannot be written is retried, logged once, and the entry is not parked half-reverted
		Rig r; Setup(r);
		r.vehicle.insert("A");
		r.failWrites = true;
		r.ledger.Tick();
		r.ledger.Tick();
		int stuck = 0;
		for (const auto& l : r.logs)
			stuck += l.find("put back yet, retrying") != std::string::npos ? 1 : 0;
		CHECK(stuck == 1);
		const Entry* e = r.ledger.Find(Kind::Toggle, "A", "feat", "fly");
		CHECK(e && !e->parked);
		r.failWrites = false;
		r.ledger.Tick();
		e = r.ledger.Find(Kind::Toggle, "A", "feat", "fly");
		CHECK(e && e->parked);
		CHECK(r.Live(10, "fly") == Value(false));
	}
	{   // asking for the toggle again once back in the character captures afresh
		Rig r; Setup(r);
		r.vehicle.insert("A");
		r.ledger.Tick();
		r.vehicle.erase("A");
		CHECK(r.ledger.Apply(r.Req(Kind::Toggle, "A", "fly", true, 10, true)));
		const Entry* e = r.ledger.Find(Kind::Toggle, "A", "feat", "fly");
		CHECK(e && !e->parked && e->prior == Value(false));
		CHECK(r.Live(10, "fly") == Value(true));
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

// The pawn is read once per controller per tick, however many entries the player has; a spectator
// pawn (after death) is no pawn: no park, no write, no vehicle line.
static void FollowStance()
{
	{   // three entries of one controller: one pawn read; a second controller: one more
		Rig r;
		r.pawn["A"] = 10;
		for (const char* slot : { "fly", "speed", "boost" })
		{
			r.Seed(10, slot, false);
			CHECK(r.ledger.Apply(r.Req(Kind::Toggle, "A", slot, true, 10, true)));
		}
		r.stanceCalls = 0;
		r.ledger.Tick();
		CHECK(r.stanceCalls == 1);
		r.Seed(11, "fly", false);
		r.pawn["B"] = 11;
		Request other = r.Req(Kind::Toggle, "B", "fly", true, 11, true);
		other.conn = Weak{ 6, 1, 1, 0x1001 };
		CHECK(r.ledger.Apply(other));
		r.stanceCalls = 0;
		r.ledger.Tick();
		CHECK(r.stanceCalls == 2);
	}
	{   // an entry that does not follow reads no pawn
		Rig r;
		r.Seed(10, "fly", false);
		CHECK(r.ledger.Apply(r.Req(Kind::Toggle, "A", "fly", true)));
		r.stanceCalls = 0;
		r.ledger.Tick();
		CHECK(r.stanceCalls == 0);
	}
	{   // a spectator pawn after death: nothing parked, written or logged; the next character moves
		Rig r;
		r.Seed(10, "fly", false);
		r.Seed(20, "fly", false);
		r.pawn["A"] = 10;
		Request q = r.Req(Kind::Toggle, "A", "fly", true, 10, true);
		q.pin = true;
		CHECK(r.ledger.Apply(q));
		const int writesBefore = r.writes;
		const size_t logsBefore = r.logs.size();
		r.dead.insert(10);
		r.spectator.insert("A");
		for (int i = 0; i < 3; ++i)
		{
			r.now += kFollowSettleMs;
			r.ledger.Tick();
		}
		CHECK(r.writes == writesBefore);
		CHECK(r.logs.size() == logsBefore);
		CHECK(!Has(r.logs, "vehicle"));
		const Entry* e = r.ledger.Find(Kind::Toggle, "A", "feat", "fly");
		CHECK(e && !e->parked && e->applied == Value(true));
		r.spectator.erase("A");
		r.pawn["A"] = 20;
		r.Respawned();
		CHECK(r.Live(20, "fly") == Value(true));
		e = r.ledger.Find(Kind::Toggle, "A", "feat", "fly");
		CHECK(e && !e->parked && e->target.index == 20 && e->prior == Value(false));
	}
}

// The pawn classes the cheats and the admin endpoints tell apart.
static void PawnClasses()
{
	CHECK(ClassifyPawn(false, false, false) == PawnKind::None);    // no pawn
	CHECK(ClassifyPawn(false, true, false) == PawnKind::None);     // no pawn wins over stray flags
	CHECK(ClassifyPawn(false, false, true) == PawnKind::None);
	CHECK(ClassifyPawn(true, true, false) == PawnKind::Character); // their character
	CHECK(ClassifyPawn(true, true, true) == PawnKind::Character);  // a character is never a spectator
	CHECK(ClassifyPawn(true, false, false) == PawnKind::Other);    // a vehicle
	CHECK(ClassifyPawn(true, false, true) == PawnKind::None);      // a spectator pawn is no pawn
}

// The game changes only some members of a composed slot, or a write fails part-way, or a put-back
// fails: SU's own value is never taken for the game's, and nothing is left behind.
static void RebaseMembers()
{
	const Value speedOrig = std::vector<float>{ 600.0f, 2000.0f };
	const auto Speed = [&](Rig& r)
	{
		r.Seed(10, "speed", speedOrig);
		r.pawn["A"] = 10;
		Request q = r.Req(Kind::Toggle, "A", "speed", Value(std::vector<float>{ 1200.0f, 4800.0f }), 10, true);
		q.pin = true;
		q.asked = Value(2.0f);
		CHECK(r.ledger.Apply(q));
	};
	{   // speed: the game changes only the braking; the top speed prior stays, the multiplier does not compound
		Rig r; Speed(r);
		r.live["10|speed"] = Value(std::vector<float>{ 1200.0f, 3000.0f });
		r.ledger.Tick();
		CHECK(r.Live(10, "speed") == Value(std::vector<float>{ 1200.0f, 4800.0f }));
		const Entry* e = r.ledger.Find(Kind::Toggle, "A", "feat", "speed");
		CHECK(e && e->prior == Value(std::vector<float>{ 600.0f, 3000.0f }) && e->applied == Value(std::vector<float>{ 1200.0f, 4800.0f }));
		for (int i = 0; i < 3; ++i)
			r.ledger.Tick();
		CHECK(r.Live(10, "speed") == Value(std::vector<float>{ 1200.0f, 4800.0f }));
		CHECK(r.ledger.Release(Kind::Toggle, "A", "feat", "speed"));
		CHECK(r.Live(10, "speed") == Value(std::vector<float>{ 600.0f, 3000.0f }));   // the game's braking, the original top speed
	}
	{   // the same change seen by a release before the next tick: the same mix
		Rig r; Speed(r);
		r.live["10|speed"] = Value(std::vector<float>{ 1200.0f, 3000.0f });
		CHECK(r.ledger.Release(Kind::Toggle, "A", "feat", "speed"));
		CHECK(r.Live(10, "speed") == Value(std::vector<float>{ 600.0f, 3000.0f }));
	}
	const Value boostOrig = std::vector<float>{ 1.0f, 1.0f, 1.0f, 1.0f };
	const auto Boost = [&](Rig& r)
	{
		r.Seed(10, "boost", boostOrig);
		r.pawn["A"] = 10;
		Request q = r.Req(Kind::Toggle, "A", "boost", Value(std::vector<float>{ 3.0f, 3.0f, 3.0f, 3.0f }), 10, true);
		q.pin = true;
		q.asked = Value(3.0f);
		CHECK(r.ledger.Apply(q));
	};
	{   // boost: the game drains only the current pair; it is not rebased, and off puts the original back
		Rig r; Boost(r);
		r.live["10|boost"] = Value(std::vector<float>{ 2.0f, 2.0f, 3.0f, 3.0f });
		r.ledger.Tick();
		CHECK(r.Live(10, "boost") == Value(std::vector<float>{ 3.0f, 3.0f, 3.0f, 3.0f }));
		CHECK(!Has(r.logs, "rebase"));
		const Entry* e = r.ledger.Find(Kind::Toggle, "A", "feat", "boost");
		CHECK(e && e->prior == boostOrig);
		r.live["10|boost"] = Value(std::vector<float>{ 2.0f, 2.0f, 3.0f, 3.0f });
		CHECK(r.ledger.Release(Kind::Toggle, "A", "feat", "boost"));
		CHECK(r.Live(10, "boost") == boostOrig);
	}
	{   // a value the game changes every tick: one rebase line until the toggle is set again
		Rig r; r.Seed(10, "weapon", 1.0f);
		r.pawn["A"] = 10;
		Request q = r.Req(Kind::Toggle, "A", "weapon", Value(2.0f), 10, true);
		q.pin = true; q.asked = Value(2.0f);
		CHECK(r.ledger.Apply(q));
		for (int i = 0; i < 6; ++i)
		{
			r.live["10|weapon"] = Value(1.5f + 0.1f * static_cast<float>(i));
			r.ledger.Tick();
		}
		const auto Lines = [&] { int n = 0; for (const auto& l : r.logs) n += l.find("rebase") != std::string::npos ? 1 : 0; return n; };
		CHECK(Lines() == 1);
		CHECK(r.ledger.Apply(q));
		r.live["10|weapon"] = Value(4.0f);
		r.ledger.Tick();
		CHECK(Lines() == 2);
	}
	{   // a write that fails part-way in the pin: the half value is undone or waited out, never taken as the game's
		Rig r; Boost(r);
		r.live["10|boost"] = Value(std::vector<float>{ 3.0f, 3.0f, 2.0f, 2.0f });
		r.halfWrites = 2;
		r.ledger.Tick();
		const Entry* e = r.ledger.Find(Kind::Toggle, "A", "feat", "boost");
		CHECK(e && e->prior == boostOrig);
		r.ledger.Tick();
		CHECK(r.Live(10, "boost") == Value(std::vector<float>{ 3.0f, 3.0f, 3.0f, 3.0f }));
		e = r.ledger.Find(Kind::Toggle, "A", "feat", "boost");
		CHECK(e && e->prior == Value(std::vector<float>{ 1.0f, 1.0f, 2.0f, 1.0f }));
	}
	{   // an Apply whose put-back fails, then a rebase: the half value is no game change
		Rig r; Boost(r);
		r.halfWrites = 2;
		CHECK(!r.ledger.Apply(r.Req(Kind::Toggle, "A", "boost", std::vector<float>{ 4.0f, 4.0f, 4.0f, 4.0f }, 10, true)));
		r.failWrites = true;
		r.ledger.Tick();
		const Entry* e = r.ledger.Find(Kind::Toggle, "A", "feat", "boost");
		CHECK(e && e->prior == boostOrig && !Has(r.logs, "rebase"));
		r.failWrites = false;
		r.ledger.Tick();
		CHECK(r.Live(10, "boost") == Value(std::vector<float>{ 3.0f, 3.0f, 3.0f, 3.0f }));
		e = r.ledger.Find(Kind::Toggle, "A", "feat", "boost");
		CHECK(e && e->prior == boostOrig && !Has(r.logs, "rebase"));
	}
}

static void DeadCharacter()
{
	const auto Setup = [](Rig& r)
	{
		r.Seed(10, "fly", false);
		r.Seed(20, "fly", false);
		r.pawn["A"] = 10;
		Request q = r.Req(Kind::Toggle, "A", "fly", true, 10, true);
		q.pin = true;
		CHECK(r.ledger.Apply(q));
	};
	{   // the character died but its object still resolves: no pin, no put-back, a fresh prior on the next one
		Rig r; Setup(r);
		r.spectator.insert("A");
		r.live["10|fly"] = Value(false);          // the death logic changed it
		const int before = r.writes;
		for (int i = 0; i < 3; ++i)
		{
			r.now += kFollowSettleMs;
			r.ledger.Tick();
		}
		CHECK(r.writes == before && r.Live(10, "fly") == Value(false));
		r.spectator.erase("A");
		r.pawn["A"] = 20;
		r.live["10|fly"] = Value(true);           // whatever the corpse holds is left alone
		r.Respawned();
		CHECK(r.Live(10, "fly") == Value(true));
		CHECK(r.Live(20, "fly") == Value(true));
		const Entry* e = r.ledger.Find(Kind::Toggle, "A", "feat", "fly");
		CHECK(e && e->target.index == 20 && e->prior == Value(false) && !e->noPawn);
	}
	{   // a put-back that fails is tried again until it works
		Rig r; Setup(r);
		r.failTargets.insert(10);
		r.pawn["A"] = 20;
		r.Respawned();
		CHECK(r.Live(10, "fly") == Value(true));
		r.failTargets.clear();
		r.ledger.Tick();
		CHECK(r.Live(10, "fly") == Value(false));
		const int after = r.writes;
		r.ledger.Tick();
		CHECK(r.writes == after);
	}
	{   // a failed release put-back outlives the entry; it stops when the target stops resolving
		Rig r; Setup(r);
		r.failTargets.insert(10);
		CHECK(r.ledger.Release(Kind::Toggle, "A", "feat", "fly"));
		CHECK(r.ledger.Size() == 0);
		r.failTargets.clear();
		r.ledger.Tick();
		CHECK(r.Live(10, "fly") == Value(false));
		Rig q; Setup(q);
		q.failTargets.insert(10);
		CHECK(q.ledger.Release(Kind::Toggle, "A", "feat", "fly"));
		q.failTargets.clear();
		q.dead.insert(10);
		const int before = q.writes;
		q.ledger.Tick();
		CHECK(q.writes == before);
	}
	{   // a write to the new pawn that fails part-way: undone or waited out, never taken as its prior
		Rig r; Setup(r);
		r.pawn["A"] = 20;
		r.halfWrites = 2;
		r.Respawned();
		CHECK(r.ledger.Find(Kind::Toggle, "A", "feat", "fly")->target.index == 10);
		r.Respawned();
		const Entry* e = r.ledger.Find(Kind::Toggle, "A", "feat", "fly");
		CHECK(e && e->target.index == 20 && e->prior == Value(false));
		CHECK(r.Live(20, "fly") == Value(true) && r.Live(10, "fly") == Value(false));
	}
}

static void HalfWrite()
{
	const Value prior = std::vector<float>{ 1.0f, 1.0f, 5.0f, 5.0f };
	const Value want  = std::vector<float>{ 3.0f, 3.0f, 5.0f, 5.0f };
	{   // a new entry whose write fails part-way: the value read first is put back, no entry
		Rig r; r.Seed(10, "boost", prior);
		r.halfWrites = 1;
		CHECK(!r.ledger.Apply(r.Req(Kind::Toggle, "A", "boost", want)));
		CHECK(r.Live(10, "boost") == prior);
		CHECK(r.ledger.Size() == 0);
		CHECK(r.writes == 1);
		CHECK(!r.logs.empty() && r.logs.back().find("could not be written") != std::string::npos);
		CHECK(r.logs.back().find("put back") == std::string::npos);
	}
	{   // the put back fails too: it is said in the log
		Rig r; r.Seed(10, "boost", prior);
		r.halfWrites = 2;
		CHECK(!r.ledger.Apply(r.Req(Kind::Toggle, "A", "boost", want)));
		CHECK(r.ledger.Size() == 0);
		CHECK(r.logs.back().find("could not be put back") != std::string::npos);
	}
	{   // an entry that already exists: the half write is undone, so the release still finds its own value
		Rig r; r.Seed(10, "boost", prior);
		CHECK(r.ledger.Apply(r.Req(Kind::Toggle, "A", "boost", want)));
		const int before = r.writes;
		r.halfWrites = 1;
		CHECK(!r.ledger.Apply(r.Req(Kind::Toggle, "A", "boost", std::vector<float>{ 4.0f, 4.0f, 5.0f, 5.0f })));
		CHECK(r.writes == before + 1);
		CHECK(r.Live(10, "boost") == want);
		CHECK(r.ledger.Size() == 1);
		CHECK(r.ledger.Release(Kind::Toggle, "A", "feat", "boost"));
		CHECK(r.Live(10, "boost") == prior);
	}
	{   // a write that fails with nothing written puts nothing back and says nothing about it
		Rig r; r.Seed(10, "boost", prior);
		r.failWrites = true;
		CHECK(!r.ledger.Apply(r.Req(Kind::Toggle, "A", "boost", want)));
		CHECK(r.Live(10, "boost") == prior);
		CHECK(r.logs.back().find("put back") == std::string::npos);
	}
}

static void PendingWrites()
{
	const auto Setup = [](Rig& r)
	{
		r.Seed(10, "fly", false);
		r.Seed(20, "fly", false);
		r.pawn["A"] = 10;
		Request q = r.Req(Kind::Toggle, "A", "fly", true, 10, true);
		q.pin = true;
		CHECK(r.ledger.Apply(q));
	};
	const auto Spectating = [&](Rig& r)
	{
		Setup(r);
		r.spectator.insert("A");
		r.ledger.Tick();
		CHECK(r.ledger.Find(Kind::Toggle, "A", "feat", "fly")->noPawn);
	};
	{   // a release, a leave, a deny and a stop while the character is a corpse write nothing to it
		Rig r; Spectating(r);
		const int before = r.writes;
		CHECK(r.ledger.Release(Kind::Toggle, "A", "feat", "fly"));
		CHECK(r.writes == before && r.Live(10, "fly") == Value(true) && r.ledger.Size() == 0);
		Rig l; Spectating(l);
		l.ledger.RestorePlayer("A");
		CHECK(l.writes == before && l.Live(10, "fly") == Value(true) && l.ledger.Size() == 0);
		Rig d; Spectating(d);
		d.denied.insert("A");
		d.ledger.Reconcile();
		CHECK(d.writes == before && d.Live(10, "fly") == Value(true) && d.ledger.Size() == 0);
		Rig p; Spectating(p);
		p.denied.insert("A");
		p.ledger.ReconcilePlayer("A");
		CHECK(p.writes == before && p.ledger.Size() == 0);
		Rig s; Spectating(s);
		s.ledger.RestoreAll();
		CHECK(s.writes == before && s.Live(10, "fly") == Value(true) && s.ledger.Size() == 0);
	}
	{   // an Apply in the settle window after the respawn leaves the corpse alone
		Rig r; Spectating(r);
		r.spectator.erase("A");
		r.pawn["A"] = 20;
		r.ledger.Tick();
		const int before = r.writes;
		Request q = r.Req(Kind::Toggle, "A", "fly", true, 20, true);
		q.pin = true;
		CHECK(r.ledger.Apply(q));
		CHECK(r.writes == before + 1 && r.Live(10, "fly") == Value(true) && r.Live(20, "fly") == Value(true));
	}
	{   // a failed put-back of an Override with no target is kept, not dropped as dead
		Rig r; r.Seed(-1, "stack", 100);
		Request q = r.Req(Kind::Override, "A", "stack", 500);
		q.target = Weak();
		CHECK(r.ledger.Apply(q));
		r.failTargets.insert(-1);
		CHECK(r.ledger.Release(Kind::Override, "A", "feat", "stack"));
		for (int i = 0; i < 3; ++i)
			r.ledger.Tick();
		CHECK(r.Live(-1, "stack") == Value(500));
		r.failTargets.clear();
		r.ledger.Tick();
		CHECK(r.Live(-1, "stack") == Value(100));
	}
	{   // the put-back and its undo both fail: the true prior is still written once the slot works
		Rig r; Setup(r);
		r.halfWrites = 2;
		CHECK(r.ledger.Release(Kind::Toggle, "A", "feat", "fly"));
		r.ledger.Tick();
		CHECK(r.Live(10, "fly") == Value(false));
		const int after = r.writes;
		r.ledger.Tick();
		CHECK(r.writes == after);
	}
	{   // a release while an earlier write to the slot is pending: the prior takes its place
		Rig r; r.Seed(10, "stack", 100);
		CHECK(r.ledger.Apply(r.Req(Kind::Override, "A", "stack", 500)));
		r.halfWrites = 2;
		CHECK(!r.ledger.Apply(r.Req(Kind::Override, "A", "stack", 700)));
		r.failTargets.insert(10);
		CHECK(r.ledger.Release(Kind::Override, "A", "feat", "stack"));
		r.failTargets.clear();
		r.ledger.Tick();
		CHECK(r.Live(10, "stack") == Value(100));
		r.ledger.Tick();
		CHECK(r.Live(10, "stack") == Value(100));
	}
	{   // a park while an undo is pending waits for it, adds nothing, and the character ends up put back
		Rig r; Setup(r);
		r.vehicle.insert("A");
		r.halfWrites = 2;
		r.ledger.Tick();
		CHECK(!r.ledger.Find(Kind::Toggle, "A", "feat", "fly")->parked);
		r.failTargets.insert(10);
		const int before = r.writes;
		for (int i = 0; i < 3; ++i)
			r.ledger.Tick();
		CHECK(r.writes == before);
		r.failTargets.clear();
		for (int i = 0; i < 3; ++i)
			r.ledger.Tick();
		CHECK(r.Live(10, "fly") == Value(false) && r.ledger.Find(Kind::Toggle, "A", "feat", "fly")->parked);
		const int after = r.writes;
		r.ledger.Tick();
		CHECK(r.writes == after);
	}
	{   // a write that never works is given up after a bounded number of tries, said once, and no longer blocks the slot
		Rig r; Setup(r);
		r.failTargets.insert(10);
		CHECK(r.ledger.Release(Kind::Toggle, "A", "feat", "fly"));
		for (int i = 0; i < kPendingMaxTries + 5; ++i)
			r.ledger.Tick();
		int said = 0;
		for (const auto& l : r.logs)
			said += l.find("tries, left as is") != std::string::npos ? 1 : 0;
		CHECK(said == 1);
		r.failTargets.clear();
		const int before = r.writes;
		r.ledger.Tick();
		CHECK(r.writes == before);
		CHECK(r.ledger.Apply(r.Req(Kind::Toggle, "A", "fly", true, 10, false)));
	}
	{   // a world end drops the writes waiting on its objects without writing
		Rig r; Setup(r);
		r.failTargets.insert(10);
		CHECK(r.ledger.Release(Kind::Toggle, "A", "feat", "fly"));
		r.ledger.OnWorldEnd();
		r.failTargets.clear();
		const int before = r.writes;
		r.ledger.Tick();
		CHECK(r.writes == before && r.Live(10, "fly") == Value(true));
	}
	{   // a stop tries each waiting write once more, and says what it could not write
		Rig r; Setup(r);
		r.failTargets.insert(10);
		CHECK(r.ledger.Release(Kind::Toggle, "A", "feat", "fly"));
		r.failTargets.clear();
		r.ledger.RestoreAll();
		CHECK(r.Live(10, "fly") == Value(false));
		Rig q; Setup(q);
		q.failTargets.insert(10);
		CHECK(q.ledger.Release(Kind::Toggle, "A", "feat", "fly"));
		q.ledger.RestoreAll();
		CHECK(Has(q.logs, "stop: could not write"));
	}
}

int main()
{
	Toggles();
	Follow();
	FollowBack();
	Composed();
	FollowVehicle();
	FollowStance();
	PawnClasses();
	Overrides();
	Grants();
	OthersChange();
	CaptureOnce();
	LogOnCommit();
	ThreadGuard();
	SlotPrefixCut();
	HalfWrite();
	RebaseMembers();
	DeadCharacter();
	PendingWrites();
	std::printf("=== cheat_ledger_tests  %d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
