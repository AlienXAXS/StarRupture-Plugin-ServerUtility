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
		size_t Count(Tag t) { size_t n = 0; for (const Message& m : sent) if (m.tag == t) ++n; return n; }
		const Message* Last(Tag t) { for (size_t i = sent.size(); i-- > 0;) if (sent[i].tag == t) return &sent[i]; return nullptr; }
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
		CHECK(r.sent.empty());
		CHECK(r.St() == Status::Waiting);
		r.client->OnServerReady();
		CHECK(r.sent.size() == 1 && r.sent[0].tag == Tag::Hello);
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
		Rig r(kServerId + " " + kSecret + "\n", "withsecret");
		r.client->OnServerReady();
		r.Feed(Challenge());
		const Message* p = r.Last(Tag::Prove);
		CHECK(p != nullptr);
		std::string expect;
		CHECK(AdminCrypto::MakeProof(kSecret, "00ff00ff00ff00ff", kServerId, expect));
		CHECK(p && p->proof == expect && expect.size() == 64);
		r.Feed(Welcome(kRoleAdmin));
		CHECK(r.St() == Status::Ready);
		CHECK(r.Count(Tag::Snap) == 1);
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
		r.sent.clear();
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
		CHECK(line == kServerId + " " + secret);
		// A new client reads it once at start and proves with it.
		r.sent.clear();
		r.Start();
		r.client->OnServerReady();
		r.Feed(Challenge());
		const Message* p = r.Last(Tag::Prove);
		std::string expect;
		AdminCrypto::MakeProof(secret, "00ff00ff00ff00ff", kServerId, expect);
		CHECK(p && p->proof == expect);
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
		r.sent.clear();
		CHECK(r.client->Apply());
		const Message* o = r.Last(Tag::Ops);
		CHECK(o && o->baseRevision == 5 && o->ops.size() == 2);
		CHECK(r.sent.size() == 1);
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
		r.sent.clear();
		CHECK(r.client->Apply());
		uint32_t id = r.Last(Tag::Ops)->requestId;
		r.sent.clear();
		r.Feed(Result(id, kResultChanged));
		AdminClient::View v = r.View();
		CHECK(v.staged.size() == 1 && v.lastResultIsError && v.lastResult == "Changed elsewhere" && !v.applyPending);
		CHECK(r.Count(Tag::Snap) == 1);
		FeedSnapshot(r, 6);
		CHECK(r.View().snapshot.revision == 6 && r.View().staged.size() == 1);
		r.sent.clear();
		CHECK(r.client->Apply());
		CHECK(r.Last(Tag::Ops)->baseRevision == 6);

		Message ch; ch.tag = Tag::Changed; ch.revision = 9; ch.sections = kSectionAll;
		r.sent.clear();
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
		r.sent.clear();
		CHECK(r.client->Kick("76561198000000001", "bye"));
		const Message* o = r.Last(Tag::Ops);
		CHECK(o && o->ops.size() == 1 && o->ops[0].name == "player.kick" && o->baseRevision == 2);
		CHECK(r.View().staged.size() == 1);
		r.Feed(Result(o->requestId, kResultOk, "Kicked"));
		CHECK(r.View().staged.size() == 1);                 // a kick result does not clear a batch
		r.sent.clear();
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
		r.sent.clear();
		r.client->OnServerReady();
		CHECK(r.Count(Tag::Hello) == 1);
	}

	std::printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
