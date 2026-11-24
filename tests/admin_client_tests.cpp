// Unit tests of the in-game admin client state model (no game needed).
// From a Visual Studio developer prompt, in the repo root:
//   cl /nologo /EHsc /std:c++17 /Isrc tests\admin_client_tests.cpp src\client\admin_client.cpp src\admin_wire\admin_wire.cpp src\admin_wire\admin_crypto.cpp /Fe:admin_client_tests.exe
//   admin_client_tests.exe
#include "client/admin_client.h"
#include "admin_wire/admin_crypto.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

static int g_checks = 0, g_failed = 0;

#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failed; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

using namespace AdminWire;
using AdminClient::Status;

namespace
{
	const std::string kSecret(64, 'a');
	const std::string kServerId = "0123456789abcdef";
	const std::string kAddr = "10.0.0.1:7777";
	const std::string kOtherAddr = "10.0.0.2:7777";
	const char* const kMismatchText = "This server's address doesn't match the one you paired with. Ask the host for a new code.";
	const char* const kLoaderText = "The in-game admin needs a newer mod loader.";

	struct Rig
	{
		std::vector<Message> sent;
		double clock = 100.0;
		std::string store;
		std::unique_ptr<AdminClient::Client> client;

		explicit Rig(const std::string& storeContents = std::string(), const char* name = "store")
		{
			std::filesystem::path dir = std::filesystem::temp_directory_path() / "su_admin_client_tests" / name;
			std::filesystem::remove_all(dir);
			std::filesystem::create_directories(dir);
			store = (dir / "admin-pairing.txt").string();
			if (!storeContents.empty()) { std::ofstream(store) << storeContents; }
			Start();
		}

		void Start()
		{
			AdminClient::Env env;
			env.send = [this](const std::string& s) { Message m; if (Decode(s, m) == Decoded::Ok) sent.push_back(m); };
			env.now = [this]() { return clock; };
			env.storePath = store;
			client.reset(new AdminClient::Client(env));
			client->SetServerAddress(kAddr);
		}

		void Feed(const Message& m)
		{
			std::string s;
			Encode(m, s);
			client->OnPacket(reinterpret_cast<const uint8_t*>(s.data()), s.size());
		}
		void FeedRaw(const std::string& s) { client->OnPacket(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }

		Status St() { AdminClient::View v; client->GetView(v); return v.status; }
		AdminClient::View View() { AdminClient::View v; client->GetView(v); return v; }
		// What went out: the game-thread tick (Flush) is what sends, so every read ticks first.
		const std::vector<Message>& Sent() { client->Flush(); return sent; }
		void Clear() { client->Flush(); sent.clear(); }
		size_t Count(Tag t) { size_t n = 0; for (const Message& m : Sent()) if (m.tag == t) ++n; return n; }
		const Message* Last(Tag t) { Sent(); for (size_t i = sent.size(); i-- > 0;) if (sent[i].tag == t) return &sent[i]; return nullptr; }
	};

	Message Welcome(const char* role) { Message m; m.tag = Tag::Welcome; m.role = role; return m; }
	Message Challenge() { Message m; m.tag = Tag::Challenge; m.serverId = kServerId; m.nonce = "00ff00ff00ff00ff"; return m; }
	Message Result(uint32_t id, const char* status, const char* text = "") { Message m; m.tag = Tag::Result; m.requestId = id; m.status = status; m.text = text; return m; }

	// Admin handshake with a stored secret, ends Ready with a first snapshot requested.
	void MakeReady(Rig& r)
	{
		r.client->OnServerReady();
		r.Feed(Challenge());
		r.Feed(Welcome(kRoleAdmin));
	}

	void FeedSnapshot(Rig& r, uint32_t rev)
	{
		Snapshot s;
		s.revision = rev;
		s.defaultPolicy = "deny";
		s.message = "hi";
		s.rules.push_back({ "a", "allow" });
		s.mins.push_back({ "", "1.0" });
		ModInfo mod; mod.plugin = "P"; mod.version = "1"; mod.wire = "1"; mod.features.push_back({ "f1", "Feature 1" });
		s.mods.push_back(mod);
		PlayerInfo p; p.id = "76561198000000001"; p.name = "Bob"; p.flags = "admin"; p.online = true; p.mods.push_back({ "P", "1" });
		s.players.push_back(p);
		for (const std::string& e : EncodeSnapshot(s)) r.FeedRaw(e);
	}
}

int main()
{
	// Hello only after the server-ready callback.
	{
		Rig r;
		CHECK(r.Sent().empty());
		CHECK(r.St() == Status::Waiting);
		r.client->OnServerReady();
		CHECK(r.Sent().size() == 1 && r.sent[0].tag == Tag::Hello);
	}

	// 5 s without an answer: no-in-game-admin; a late welcome recovers.
	{
		Rig r;
		r.client->OnServerReady();
		r.clock += 4.9;
		CHECK(r.St() == Status::Waiting);
		r.clock += 0.2;
		CHECK(r.St() == Status::NoInGameAdmin);
		r.Feed(Welcome(kRoleNone));
		CHECK(r.St() == Status::NotAdmin);
	}

	// An answer within 5 s never times out.
	{
		Rig r;
		r.client->OnServerReady();
		r.Feed(Welcome(kRoleNone));
		r.clock += 60;
		CHECK(r.St() == Status::NotAdmin);
	}

	// Welcome update, wrong protocol, garbage.
	{
		Rig r;
		r.client->OnServerReady();
		r.Feed(Welcome(kRoleUpdate));
		CHECK(r.St() == Status::UpdateClient);
	}
	{
		Rig r;
		r.client->OnServerReady();
		std::string s;
		Message m = Welcome(kRoleAdmin);
		Encode(m, s);
		size_t us = s.find(kUS);
		s.replace(us + 1, 1, "9");   // protocol 9
		r.FeedRaw(s);
		CHECK(r.St() == Status::UpdateClient);
		Rig g;
		g.client->OnServerReady();
		g.FeedRaw("junk");
		CHECK(g.St() == Status::Waiting);
	}

	// Challenge answered with a proof from the stored secret.
	{
		Rig r(kServerId + " " + kAddr + " " + kSecret + "\n", "withsecret");
		r.client->OnServerReady();
		r.Feed(Challenge());
		const Message* p = r.Last(Tag::Prove);
		CHECK(p != nullptr);
		std::string expect;
		CHECK(AdminCrypto::MakeProof(kSecret, "00ff00ff00ff00ff", kServerId, expect));
		CHECK(p && p->proof == expect && expect.size() == 64);
		r.Feed(Welcome(kRoleAdmin));
		CHECK(r.St() == Status::Ready);
		CHECK(r.Count(Tag::Snap) == 0);                     // the server pushes the snapshot with the welcome
	}

	// Challenge without a stored secret: empty proof, then welcome pair.
	{
		Rig r;
		r.client->OnServerReady();
		r.Feed(Challenge());
		const Message* p = r.Last(Tag::Prove);
		CHECK(p && p->proof.empty());
		r.Feed(Welcome(kRolePair));
		CHECK(r.St() == Status::PairNeeded);
	}

	// Pair saves the secret only after welcome admin.
	{
		Rig r;
		r.client->OnServerReady();
		r.Feed(Challenge());
		r.Feed(Welcome(kRolePair));
		r.Clear();
		CHECK(r.client->Pair("ABCD2345"));
		const Message* pm = r.Last(Tag::Pair);
		CHECK(pm && pm->code == "ABCD2345" && pm->secret.size() == 64);
		CHECK(!std::filesystem::exists(r.store));          // not yet
		r.Feed(Welcome(kRolePair));                         // pairing refused
		CHECK(!std::filesystem::exists(r.store));
		CHECK(r.client->Pair("ABCD2345"));
		pm = r.Last(Tag::Pair);
		std::string secret = pm ? pm->secret : std::string();
		r.Feed(Welcome(kRoleAdmin));
		CHECK(r.St() == Status::Ready);
		CHECK(std::filesystem::exists(r.store));
		CHECK(!std::filesystem::exists(r.store + ".tmp"));
		std::ifstream in(r.store);
		std::string line;
		std::getline(in, line);
		CHECK(line == kServerId + " " + kAddr + " " + secret);
		// A new client reads it once at start and proves with it.
		r.Clear();
		r.Start();
		r.client->OnServerReady();
		r.Feed(Challenge());
		const Message* p = r.Last(Tag::Prove);
		std::string expect;
		AdminCrypto::MakeProof(secret, "00ff00ff00ff00ff", kServerId, expect);
		CHECK(p && p->proof == expect);
	}

	// The secret is pinned to the connection address: a challenge from another address gets no answer.
	{
		Rig r(kServerId + " " + kAddr + " " + kSecret + "\n", "pinned");
		r.client->SetServerAddress(kOtherAddr);
		r.client->OnServerReady();
		r.Clear();
		r.Feed(Challenge());
		CHECK(r.Count(Tag::Prove) == 0);
		AdminClient::View v = r.View();
		CHECK(v.status == Status::PairNeeded && v.lastResultIsError && v.lastResult == kMismatchText);
		r.client->SetServerAddress(kAddr);
		r.Feed(Challenge());
		CHECK(r.Count(Tag::Prove) == 1 && !r.Last(Tag::Prove)->proof.empty());
	}

	// An unknown address blocks both pairing and proving.
	{
		Rig r(kServerId + " " + kAddr + " " + kSecret + "\n", "unknownaddr");
		r.client->SetServerAddress(AdminClient::kUnknownAddress);
		r.client->OnServerReady();
		r.Clear();
		r.Feed(Challenge());
		CHECK(r.Count(Tag::Prove) == 0);
		AdminClient::View v = r.View();
		CHECK(v.status == Status::PairNeeded && v.lastResult == kLoaderText);

		Rig q;
		q.client->SetServerAddress("");
		q.client->OnServerReady();
		q.Feed(Challenge());
		q.Feed(Welcome(kRolePair));
		q.Clear();
		CHECK(!q.client->Pair("ABCD2345"));
		CHECK(q.Count(Tag::Pair) == 0 && !q.View().pairPending);
		CHECK(q.View().lastResultIsError && q.View().lastResult == kLoaderText);
	}

	// Two servers each keep their own pin, and the pin is the address at pairing time.
	{
		const std::string id2 = "fedcba9876543210", secret2(64, 'b');
		Rig r(kServerId + " " + kAddr + " " + kSecret + "\n" + id2 + " " + kOtherAddr + " " + secret2 + "\n", "twopins");
		r.client->OnServerReady();
		r.Clear();
		r.Feed(Challenge());                                // server 1 on its own address
		std::string expect;
		AdminCrypto::MakeProof(kSecret, "00ff00ff00ff00ff", kServerId, expect);
		CHECK(r.Count(Tag::Prove) == 1 && r.Last(Tag::Prove)->proof == expect);
		r.client->SetServerAddress(kOtherAddr);
		r.Clear();
		Message c2 = Challenge();
		c2.serverId = id2;
		r.Feed(c2);                                         // server 2 on its own address
		AdminCrypto::MakeProof(secret2, "00ff00ff00ff00ff", id2, expect);
		CHECK(r.Count(Tag::Prove) == 1 && r.Last(Tag::Prove)->proof == expect);
		r.Clear();
		r.Feed(Challenge());                                // server 1 id on server 2's address
		CHECK(r.Count(Tag::Prove) == 0);
	}

	// No answer to a pair message in 10 s: Pair is enabled again with a note; a late answer changes nothing saved.
	{
		Rig r;
		r.client->OnServerReady();
		r.Feed(Challenge());
		r.Feed(Welcome(kRolePair));
		CHECK(r.client->Pair("ABCD2345"));
		r.clock += AdminClient::kPairTimeoutSeconds - 0.1;
		CHECK(r.View().pairPending && r.View().lastResult.empty());
		r.clock += 0.2;
		AdminClient::View v = r.View();
		CHECK(!v.pairPending && v.lastResultIsError && v.lastResult == "No answer from the server. Try again.");
		CHECK(v.status == Status::PairNeeded);
		CHECK(r.client->Pair("ABCD2345"));
	}

	// Snapshot assembly: continuation records merge, swapped in only at snap-end.
	{
		Rig r;
		MakeReady(r);
		CHECK(!r.View().haveSnapshot);
		Snapshot s;
		s.revision = 7;
		s.rules.push_back({ "a", "allow" });
		ModInfo mod; mod.plugin = "Big"; mod.version = "1"; mod.wire = "1";
		for (int i = 0; i < 40; ++i) mod.features.push_back({ "feature" + std::to_string(i), "A fairly long feature label to force several pages of this mod" });
		s.mods.push_back(mod);
		PlayerInfo p; p.id = "id1"; p.name = "N";
		for (int i = 0; i < 30; ++i) p.mods.push_back({ "Mod" + std::to_string(i), "1.2.3" });
		s.players.push_back(p);
		std::vector<std::string> pages = EncodeSnapshot(s);
		CHECK(pages.size() > 4);
		for (size_t i = 0; i + 1 < pages.size(); ++i) r.FeedRaw(pages[i]);
		CHECK(!r.View().haveSnapshot);                      // nothing visible before snap-end
		r.FeedRaw(pages.back());
		AdminClient::View v = r.View();
		CHECK(v.haveSnapshot && v.snapshot.revision == 7);
		CHECK(v.snapshot.mods.size() == 1 && v.snapshot.mods[0].features.size() == 40);
		CHECK(v.snapshot.players.size() == 1 && v.snapshot.players[0].mods.size() == 30);
		CHECK(v.snapshot.rules.size() == 1);

		// A truncated snapshot (missing a page) is discarded, the old one stays.
		Snapshot s2 = s;
		s2.revision = 8;
		std::vector<std::string> p2 = EncodeSnapshot(s2);
		for (size_t i = 0; i < p2.size(); ++i) if (i != p2.size() - 2) r.FeedRaw(p2[i]);
		CHECK(r.View().snapshot.revision == 7);
	}

	// Staged ops, Apply with the snapshot revision, ok clears.
	{
		Rig r;
		MakeReady(r);
		FeedSnapshot(r, 5);
		Op bad; bad.name = "nope";
		CHECK(!r.client->Stage(bad));
		Op wrongArgs; wrongArgs.name = "rule.set"; wrongArgs.args = { "x" };
		CHECK(!r.client->Stage(wrongArgs));
		Op a; a.name = "rule.set"; a.args = { "k", "deny" };
		Op b; b.name = "policy.default"; b.args = { "deny" };
		CHECK(r.client->Stage(a) && r.client->Stage(b));
		CHECK(r.View().staged.size() == 2);
		r.client->Unstage(1);
		CHECK(r.View().staged.size() == 1);
		CHECK(r.client->Stage(b));
		r.Clear();
		CHECK(r.client->Apply());
		const Message* o = r.Last(Tag::Ops);
		CHECK(o && o->baseRevision == 5 && o->ops.size() == 2);
		CHECK(r.Sent().size() == 1);
		CHECK(!r.client->Apply());                          // one batch at a time
		CHECK(r.View().applyPending);
		r.Feed(Result(o->requestId, kResultOk, "Applied"));
		AdminClient::View v = r.View();
		CHECK(v.staged.empty() && !v.applyPending && v.lastResult == "Applied" && !v.lastResultIsError);
		CHECK(!r.client->Apply());                          // nothing staged
	}

	// A batch that would not fit one message is refused at stage time.
	{
		Rig r;
		MakeReady(r);
		FeedSnapshot(r, 1);
		Op big; big.name = "policy.message"; big.args = { std::string(300, 'x') };
		size_t accepted = 0;
		for (int i = 0; i < 10; ++i) if (r.client->Stage(big)) ++accepted;
		CHECK(accepted >= 1 && accepted < 10);
		CHECK(r.View().staged.size() == accepted);
	}

	// Stale base: "Changed elsewhere" keeps staged ops and re-snaps; a changed message re-snaps.
	{
		Rig r;
		MakeReady(r);
		FeedSnapshot(r, 5);
		Op a; a.name = "rule.set"; a.args = { "k", "deny" };
		CHECK(r.client->Stage(a));
		r.Clear();
		CHECK(r.client->Apply());
		uint32_t id = r.Last(Tag::Ops)->requestId;
		r.Clear();
		r.Feed(Result(id, kResultChanged));
		AdminClient::View v = r.View();
		CHECK(v.staged.size() == 1 && v.lastResultIsError && v.lastResult == "Changed elsewhere" && !v.applyPending);
		CHECK(r.Count(Tag::Snap) == 1);
		FeedSnapshot(r, 6);
		CHECK(r.View().snapshot.revision == 6 && r.View().staged.size() == 1);
		r.Clear();
		CHECK(r.client->Apply());
		CHECK(r.Last(Tag::Ops)->baseRevision == 6);

		Message ch; ch.tag = Tag::Changed; ch.revision = 9; ch.sections = kSectionAll;
		r.Clear();
		r.Feed(ch);
		CHECK(r.Count(Tag::Snap) == 1);
	}

	// Revert drops staged ops and re-snaps. Kick sends at once and leaves staged ops.
	{
		Rig r;
		MakeReady(r);
		FeedSnapshot(r, 2);
		Op a; a.name = "rule.set"; a.args = { "k", "deny" };
		r.client->Stage(a);
		r.Clear();
		CHECK(r.client->Kick("76561198000000001", "bye"));
		const Message* o = r.Last(Tag::Ops);
		CHECK(o && o->ops.size() == 1 && o->ops[0].name == "player.kick" && o->baseRevision == 2);
		CHECK(r.View().staged.size() == 1);
		r.Feed(Result(o->requestId, kResultOk, "Kicked"));
		CHECK(r.View().staged.size() == 1);                 // a kick result does not clear a batch
		r.Clear();
		r.client->Revert();
		CHECK(r.View().staged.empty() && r.Count(Tag::Snap) == 1);
	}

	// World end resets to waiting; the next server-ready starts over.
	{
		Rig r;
		MakeReady(r);
		FeedSnapshot(r, 3);
		Op a; a.name = "rule.set"; a.args = { "k", "deny" };
		r.client->Stage(a);
		r.client->OnWorldEnd();
		AdminClient::View v = r.View();
		CHECK(v.status == Status::Waiting && !v.haveSnapshot && v.staged.empty() && v.snapshot.rules.empty());
		r.clock += 30;
		CHECK(r.St() == Status::Waiting);                    // no hello pending, no timeout
		r.Clear();
		r.client->OnServerReady();
		CHECK(r.Count(Tag::Hello) == 1);
	}

	// Nothing leaves until the game-thread tick: Apply, Revert, Kick and Pair only queue.
	{
		Rig r;
		MakeReady(r);
		FeedSnapshot(r, 5);
		r.Clear();
		Op a; a.name = "rule.set"; a.args = { "k", "deny" };
		CHECK(r.client->Stage(a));
		CHECK(r.client->Apply());
		CHECK(r.client->Kick("76561198000000001", "bye"));
		r.client->Revert();
		CHECK(r.sent.empty());
		r.client->Flush();
		CHECK(r.Count(Tag::Ops) == 2 && r.Count(Tag::Snap) == 1);
		const size_t after = r.sent.size();
		r.client->Flush();
		CHECK(r.sent.size() == after);                      // drained once
		Rig p;
		p.client->OnServerReady();
		p.Feed(Challenge());
		p.Feed(Welcome(kRolePair));
		p.Clear();
		CHECK(p.client->Pair("ABCD2345"));
		CHECK(p.sent.empty());
		p.client->Flush();
		CHECK(p.Count(Tag::Pair) == 1);
	}

	// Edits made while an Apply is pending survive its ok result.
	{
		Rig r;
		MakeReady(r);
		FeedSnapshot(r, 5);
		Op a; a.name = "rule.set"; a.args = { "k", "deny" };
		Op b; b.name = "policy.default"; b.args = { "deny" };
		Op c; c.name = "rule.set"; c.args = { "j", "allow" };
		CHECK(r.client->Stage(a) && r.client->Stage(b));
		CHECK(r.client->Apply());
		uint32_t id = r.Last(Tag::Ops)->requestId;
		CHECK(r.client->Stage(c));
		r.Feed(Result(id, kResultOk, "Applied"));
		AdminClient::View v = r.View();
		CHECK(v.staged.size() == 1 && v.staged[0].args[0] == "j");
		// Unstaging an op that is in flight leaves the later edit alone too.
		Rig u;
		MakeReady(u);
		FeedSnapshot(u, 5);
		CHECK(u.client->Stage(a) && u.client->Stage(b));
		CHECK(u.client->Apply());
		uint32_t uid = u.Last(Tag::Ops)->requestId;
		u.client->Unstage(0);
		CHECK(u.client->Stage(c));
		u.Feed(Result(uid, kResultOk, "Applied"));
		CHECK(u.View().staged.size() == 1 && u.View().staged[0].args[0] == "j");
	}

	// A refused pairing code says so; five refusals say the lockout; a second Pair press waits.
	{
		Rig r;
		r.client->OnServerReady();
		r.Feed(Challenge());
		r.Feed(Welcome(kRolePair));
		CHECK(r.View().lastResult.empty());
		CHECK(r.client->Pair("AAAA2222"));
		CHECK(!r.client->Pair("BBBB3333") && r.View().pairPending);   // answer awaited
		r.Feed(Welcome(kRolePair));
		AdminClient::View v = r.View();
		CHECK(!v.pairPending && v.lastResultIsError && v.lastResult == "Wrong code, try again");
		for (int i = 1; i < AdminClient::kMaxWrongCodes; ++i)
		{
			CHECK(r.client->Pair("AAAA2222"));
			r.Feed(Welcome(kRolePair));
		}
		CHECK(r.View().lastResult == "Too many wrong codes. Reconnect, or ask the host for a new code");
		r.Clear();
		CHECK(!r.client->Pair("AAAA2222") && r.Count(Tag::Pair) == 0);
		r.client->OnServerReady();                          // a new connection starts clean
		r.Feed(Challenge());
		r.Feed(Welcome(kRolePair));
		CHECK(r.client->Pair("AAAA2222"));

		// The saved secret is the one in the single pair message that went out.
		Rig d;
		d.client->OnServerReady();
		d.Feed(Challenge());
		d.Feed(Welcome(kRolePair));
		d.Clear();
		CHECK(d.client->Pair("AAAA2222"));
		CHECK(!d.client->Pair("AAAA2222"));
		CHECK(d.Count(Tag::Pair) == 1);
		std::string sentSecret = d.Last(Tag::Pair)->secret;
		d.Feed(Welcome(kRoleAdmin));
		std::ifstream in(d.store);
		std::string line;
		std::getline(in, line);
		CHECK(line == kServerId + " " + kAddr + " " + sentSecret);
	}

	// Staged edits keep the revision they were made on, so the server can refuse them.
	{
		Rig r;
		MakeReady(r);
		FeedSnapshot(r, 5);
		Op a; a.name = "rule.set"; a.args = { "k", "deny" };
		CHECK(r.client->Stage(a));
		Message ch; ch.tag = Tag::Changed; ch.revision = 6; ch.sections = kSectionAll;
		r.Feed(ch);
		FeedSnapshot(r, 6);
		AdminClient::View v = r.View();
		CHECK(v.snapshot.revision == 6 && v.staged.size() == 1);
		CHECK(v.lastResultIsError && v.lastResult == "The server changed since you started editing. Apply will check your edits.");
		r.Clear();
		CHECK(r.client->Apply());
		const Message* o = r.Last(Tag::Ops);
		CHECK(o && o->baseRevision == 5);                   // the old base: the server answers changed
		r.Feed(Result(o->requestId, kResultChanged));
		CHECK(r.View().lastResult == "Changed elsewhere");
		FeedSnapshot(r, 7);
		r.Clear();
		CHECK(r.client->Apply());
		CHECK(r.Last(Tag::Ops)->baseRevision == 7);         // told once, then re-based
	}

	// Our own Apply moves the revision: leftover edits follow it without a warning.
	{
		Rig r;
		MakeReady(r);
		FeedSnapshot(r, 5);
		Op a; a.name = "rule.set"; a.args = { "k", "deny" };
		Op c; c.name = "rule.set"; c.args = { "j", "allow" };
		CHECK(r.client->Stage(a) && r.client->Apply());
		uint32_t id = r.Last(Tag::Ops)->requestId;
		CHECK(r.client->Stage(c));
		r.Feed(Result(id, kResultOk, "Applied"));
		FeedSnapshot(r, 6);
		CHECK(r.View().lastResult == "Applied");
		r.Clear();
		CHECK(r.client->Apply());
		CHECK(r.Last(Tag::Ops)->baseRevision == 6);
	}

	// Snapshot reassembly is capped, and a bad or stalled one is dropped and asked for again once.
	{
		Rig r;
		MakeReady(r);
		FeedSnapshot(r, 5);
		r.Clear();
		Message begin; begin.tag = Tag::SnapBegin; begin.revision = 6; begin.sections = kSectionAll;
		Message rule; rule.tag = Tag::SnapRule; rule.rules.push_back({ "a", "allow" });
		r.Feed(begin);
		for (size_t i = 0; i <= AdminClient::kMaxSnapshotParts; ++i) r.Feed(rule);
		AdminClient::View v = r.View();
		CHECK(v.lastResult == AdminClient::kRefreshing && v.snapshot.revision == 5);
		CHECK(r.Count(Tag::Snap) == 1);
		r.Feed(begin);                                      // the retry fails the same way
		for (size_t i = 0; i <= AdminClient::kMaxSnapshotParts; ++i) r.Feed(rule);
		v = r.View();
		CHECK(v.lastResultIsError && v.lastResult != AdminClient::kRefreshing);
		CHECK(r.Count(Tag::Snap) == 1);                     // no second automatic request

		// A missing page: snap-end does not match what arrived.
		Rig m;
		MakeReady(m);
		Snapshot s;
		s.revision = 4;
		s.rules.push_back({ "a", "allow" });
		s.rules.push_back({ "b", "deny" });
		std::vector<std::string> pages = EncodeSnapshot(s);
		for (size_t i = 0; i < pages.size(); ++i) if (i != pages.size() - 2) m.FeedRaw(pages[i]);
		CHECK(!m.View().haveSnapshot && m.View().lastResult == AdminClient::kRefreshing);
		CHECK(m.Count(Tag::Snap) == 1);
		FeedSnapshot(m, 4);                                 // the refresh arrives
		CHECK(m.View().haveSnapshot && m.View().lastResult.empty());

		// A stall: no snap-end within the timeout.
		Rig t;
		MakeReady(t);
		t.Feed(begin);
		t.Clear();
		t.clock += AdminClient::kSnapshotTimeoutSeconds + 0.1;
		CHECK(t.View().lastResult == AdminClient::kRefreshing);
		CHECK(t.Count(Tag::Snap) == 1);
	}

	std::printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
