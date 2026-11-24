// Unit tests of the rules parser and the text it makes for the mod loader.
// No project file: from a Visual Studio developer prompt, in the repo root:
//   cl /nologo /EHsc /std:c++17 /Isrc tests\rules_model_tests.cpp src\host_rules\rules_model.cpp src\host_rules\claim_model.cpp src\host_rules\admin_ops.cpp src\host_rules\known_features.cpp src\host_rules\ingame_admin_model.cpp src\host_rules\player_list.cpp src\host_rules\ini_file.cpp src\admin_wire\admin_wire.cpp src\admin_wire\admin_crypto.cpp user32.lib /I<Plugin SDK>\include /Fe:rules_model_tests.exe
//   rules_model_tests.exe
#include "host_rules/rules_model.h"
#include "host_rules/claim_model.h"
#include "host_rules/admin_ops.h"
#include "host_rules/known_features.h"
#include "host_rules/player_list.h"
#include "admin/url_match.h"
#include "host_rules/ingame_admin_model.h"
#include "admin_wire/admin_wire.h"
#include "admin_wire/admin_crypto.h"
#include "rcon/commands/word_split.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <vector>

struct IPluginSelf;
IPluginSelf* GetSelf() { return nullptr; }   // player_list.cpp logs through the plugin; no plugin here

static int g_checks = 0, g_failed = 0;

#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failed; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

using namespace RulesModel;

static Model ParseText(const std::vector<std::string>& lines, std::vector<std::string>& warnings)
{
	Model m;
	Parse(lines, m, warnings);
	return m;
}

// ---- In-game admin handshake and pairing ----

namespace
{
	struct IgaEnv
	{
		std::set<std::string> admins;
		double now = 1000;
		bool on = true;
		std::vector<std::string> audit;
		std::string path;
		// SA3b: the state a snapshot carries and what a batch of ops comes to. Unset = none.
		std::function<AdminWire::Snapshot()> snapshot;
		std::function<InGameAdminModel::OpsOutcome(const std::string&, const std::string&,
		                                           const std::vector<AdminWire::Op>&, uint32_t)> runOps;
	};

	std::unique_ptr<InGameAdminModel::Server> MakeIga(IgaEnv& e)
	{
		InGameAdminModel::Env env;
		env.enabled = [&e] { return e.on; };
		env.isAdmin = [&e](const std::string& id) { return e.admins.count(id) > 0; };
		env.revision = [] { return 7u; };
		env.now = [&e] { return e.now; };
		env.audit = [&e](const std::string& line) { e.audit.push_back(line); };
		env.storePath = e.path;
		env.snapshot = e.snapshot;
		env.runOps = e.runOps;
		return std::make_unique<InGameAdminModel::Server>(env);
	}

	std::vector<AdminWire::Message> IgaSendRaw(InGameAdminModel::Server& s, uint64_t conn, const std::string& id, const std::string& wire)
	{
		std::vector<AdminWire::Message> replies;
		for (const std::string& r : s.OnPacket(conn, id, id, reinterpret_cast<const uint8_t*>(wire.data()), wire.size()))
		{
			AdminWire::Message d;
			if (AdminWire::Decode(r, d) == AdminWire::Decoded::Ok) replies.push_back(d);
		}
		return replies;
	}

	std::vector<AdminWire::Message> IgaSend(InGameAdminModel::Server& s, uint64_t conn, const std::string& id, const AdminWire::Message& m)
	{
		std::string wire;
		AdminWire::Encode(m, wire);
		return IgaSendRaw(s, conn, id, wire);
	}

	AdminWire::Message IgaHello()
	{
		AdminWire::Message m;
		m.tag = AdminWire::Tag::Hello;
		return m;
	}

	AdminWire::Message IgaPair(const std::string& code, const std::string& secret)
	{
		AdminWire::Message m;
		m.tag = AdminWire::Tag::Pair;
		m.code = code;
		m.secret = secret;
		return m;
	}

	// Hello, then the nonce of the challenge ("" when there is none).
	std::string IgaChallenge(InGameAdminModel::Server& s, uint64_t conn, const std::string& id)
	{
		const auto r = IgaSend(s, conn, id, IgaHello());
		return (r.size() == 1 && r[0].tag == AdminWire::Tag::Challenge) ? r[0].nonce : std::string();
	}

	std::string IgaRole(const std::vector<AdminWire::Message>& r)
	{
		return (r.size() == 1 && r[0].tag == AdminWire::Tag::Welcome) ? r[0].role : std::string("?");
	}

	const std::string kSecretA(64, 'a');
	const std::string kSecretB(64, 'b');
}

static void InGameAdminTests()
{
	using AdminWire::Tag;
	const std::string path = (std::filesystem::temp_directory_path() / "su-iga-pairing-test.txt").string();
	std::filesystem::remove(path);

	// Codes: alphabet, length, expiry, wrong-try limits.
	{
		IgaEnv e; e.path = path; e.admins = { "p1" };
		auto s = MakeIga(e);
		CHECK(s->Ready());
		std::string code, err;
		CHECK(!s->IssueCode("nobody", code, err));
		CHECK(s->IssueCode("p1", code, err));
		CHECK(code.size() == 8);
		CHECK(code.find_first_not_of("23456789ABCDEFGHJKMNPQRSTUVWXYZ") == std::string::npos);

		// Expires after 10 minutes.
		CHECK(IgaChallenge(*s, 1, "p1") != "");
		e.now += 601;
		CHECK(IgaRole(IgaSend(*s, 1, "p1", IgaPair(code, kSecretA))) == "pair");
		CHECK(!s->IsProven(1));
		CHECK(!s->HasSecret("p1"));

		// 5 wrong per connection: the next try is refused even with the right code.
		CHECK(s->IssueCode("p1", code, err));
		for (int i = 0; i < 5; ++i)
			CHECK(IgaRole(IgaSend(*s, 1, "p1", IgaPair("22222222", kSecretA))) == "pair");
		CHECK(IgaRole(IgaSend(*s, 1, "p1", IgaPair(code, kSecretA))) == "pair");
		CHECK(!s->HasSecret("p1"));
		// Another connection still can.
		CHECK(IgaChallenge(*s, 2, "p1") != "");
		CHECK(IgaRole(IgaSend(*s, 2, "p1", IgaPair(code, kSecretA))) == "admin");
		CHECK(s->HasSecret("p1"));
		std::filesystem::remove(path);
	}
	{
		// 5 wrong in total, across connections, voids the code.
		IgaEnv e; e.path = path; e.admins = { "p1" };
		auto s = MakeIga(e);
		std::string code, err;
		CHECK(s->IssueCode("p1", code, err));
		for (uint64_t c = 1; c <= 5; ++c)
		{
			CHECK(IgaChallenge(*s, c, "p1") != "");
			IgaSend(*s, c, "p1", IgaPair("22222222", kSecretA));
		}
		CHECK(IgaChallenge(*s, 9, "p1") != "");
		CHECK(IgaRole(IgaSend(*s, 9, "p1", IgaPair(code, kSecretA))) == "pair");
		CHECK(!s->HasSecret("p1"));
		std::filesystem::remove(path);
	}

	// Pairing with a code stores the secret and writes the file; prove: valid, wrong, empty.
	{
		IgaEnv e; e.path = path; e.admins = { "p1" };
		auto s = MakeIga(e);
		std::string code, err;
		CHECK(s->IssueCode("p1", code, err));
		CHECK(IgaChallenge(*s, 1, "p1") != "");
		CHECK(IgaRole(IgaSend(*s, 1, "p1", IgaPair(code, kSecretA))) == "admin");
		CHECK(s->IsProven(1));
		CHECK(s->HasSecret("p1"));
		CHECK(std::filesystem::exists(path));
		// One use.
		CHECK(IgaChallenge(*s, 2, "p1") != "");
		CHECK(IgaRole(IgaSend(*s, 2, "p1", IgaPair(code, kSecretB))) == "pair");
		for (const std::string& line : e.audit)
			CHECK(line.find(code) == std::string::npos && line.find(kSecretA) == std::string::npos);

		const std::string sid = s->ServerId();
		AdminWire::Message prove; prove.tag = Tag::Prove;
		std::string nonce = IgaChallenge(*s, 3, "p1");
		CHECK(!nonce.empty());
		AdminCrypto::MakeProof(kSecretA, nonce, sid, prove.proof);
		CHECK(IgaRole(IgaSend(*s, 3, "p1", prove)) == "admin");
		CHECK(s->IsProven(3));

		nonce = IgaChallenge(*s, 4, "p1");
		AdminCrypto::MakeProof(kSecretB, nonce, sid, prove.proof);
		CHECK(IgaRole(IgaSend(*s, 4, "p1", prove)) == "pair");
		CHECK(!s->IsProven(4));

		nonce = IgaChallenge(*s, 5, "p1");
		prove.proof.clear();
		CHECK(IgaRole(IgaSend(*s, 5, "p1", prove)) == "pair");
		CHECK(!s->IsProven(5));

		// A restart keeps the secret and the server id.
		auto again = MakeIga(e);
		CHECK(again->ServerId() == sid);
		CHECK(again->HasSecret("p1"));
		again.reset();

		// Losing the Admin flag drops the secret and un-proves the connection.
		e.admins.clear();
		s->OnPlayersChanged();
		CHECK(!s->HasSecret("p1"));
		CHECK(!s->IsProven(1) && !s->IsProven(3));
		CHECK(!MakeIga(e)->HasSecret("p1"));
		std::filesystem::remove(path);
	}

	// Unpair.
	{
		IgaEnv e; e.path = path; e.admins = { "p1" };
		auto s = MakeIga(e);
		std::string code, err;
		s->IssueCode("p1", code, err);
		IgaChallenge(*s, 1, "p1");
		IgaSend(*s, 1, "p1", IgaPair(code, kSecretA));
		CHECK(s->Unpair("p1"));
		CHECK(!s->HasSecret("p1") && !s->IsProven(1));
		CHECK(!s->Unpair("p1"));
		std::filesystem::remove(path);
	}

	// The claim shortcut belongs to the connection that claimed, not to the ID.
	{
		IgaEnv e; e.path = path;
		auto s = MakeIga(e);
		CHECK(IgaRole(IgaSend(*s, 1, "p1", IgaHello())) == "none");
		CHECK(IgaRole(IgaSend(*s, 2, "p1", IgaHello())) == "none");
		e.admins = { "p1" };
		const auto out = s->OnClaimed(1, "p1");
		CHECK(out.size() == 2);   // both silent connections with that ID get a challenge
		// A second connection with the same ID cannot use the shortcut.
		CHECK(IgaRole(IgaSend(*s, 2, "p1", IgaPair("", kSecretB))) == "pair");
		CHECK(!s->IsProven(2) && !s->HasSecret("p1"));
		// The claiming connection can, once.
		CHECK(IgaRole(IgaSend(*s, 1, "p1", IgaPair("", kSecretA))) == "admin");
		CHECK(s->IsProven(1) && s->HasSecret("p1"));
		CHECK(IgaRole(IgaSend(*s, 1, "p1", IgaPair("", kSecretB))) == "pair");
		std::filesystem::remove(path);
	}
	{
		// Cleared on disconnect, on world end, and after 10 minutes.
		IgaEnv e; e.path = path; e.admins = { "p1" };
		auto s = MakeIga(e);
		s->OnClaimed(1, "p1");
		s->OnDisconnect(1);
		CHECK(IgaChallenge(*s, 1, "p1") != "");   // a new connection that reuses the number
		CHECK(IgaRole(IgaSend(*s, 1, "p1", IgaPair("", kSecretA))) == "pair");

		s->OnClaimed(2, "p1");
		s->OnWorldEnd();
		CHECK(IgaChallenge(*s, 2, "p1") != "");
		CHECK(IgaRole(IgaSend(*s, 2, "p1", IgaPair("", kSecretA))) == "pair");

		s->OnClaimed(3, "p1");
		e.now += 601;
		CHECK(IgaChallenge(*s, 3, "p1") != "");
		CHECK(IgaRole(IgaSend(*s, 3, "p1", IgaPair("", kSecretA))) == "pair");
		CHECK(!s->HasSecret("p1"));
		std::filesystem::remove(path);
	}

	// A non-admin gets one welcome none, then silence.
	{
		IgaEnv e; e.path = path;
		auto s = MakeIga(e);
		CHECK(IgaRole(IgaSend(*s, 1, "p9", IgaHello())) == "none");
		CHECK(IgaSend(*s, 1, "p9", IgaHello()).empty());
		CHECK(IgaSend(*s, 1, "p9", IgaPair("ABCDEFGH", kSecretA)).empty());
		std::filesystem::remove(path);
	}

	// InGameAdmin=0: silent.
	{
		IgaEnv e; e.path = path; e.admins = { "p1" }; e.on = false;
		auto s = MakeIga(e);
		CHECK(IgaSend(*s, 1, "p1", IgaHello()).empty());
		CHECK(IgaSend(*s, 2, "p9", IgaHello()).empty());
		std::filesystem::remove(path);
	}

	// A hello with another protocol gets welcome role update.
	{
		IgaEnv e; e.path = path; e.admins = { "p1" };
		auto s = MakeIga(e);
		std::string wire;
		AdminWire::Encode(IgaHello(), wire);
		const size_t us = wire.find(AdminWire::kUS);
		CHECK(us != std::string::npos && us + 1 < wire.size());
		if (us != std::string::npos && us + 1 < wire.size()) wire[us + 1] = '9';
		CHECK(IgaRole(IgaSendRaw(*s, 1, "p1", wire)) == "update");
		std::filesystem::remove(path);
	}
}

// ---- In-game admin: snapshot, ops, changed push, rate limit ----

namespace
{
	// The raw replies for one message from a connection.
	std::vector<std::string> IgaRaw(InGameAdminModel::Server& s, uint64_t conn, const std::string& id, const AdminWire::Message& m)
	{
		std::string wire;
		AdminWire::Encode(m, wire);
		return s.OnPacket(conn, id, id, reinterpret_cast<const uint8_t*>(wire.data()), wire.size());
	}

	std::vector<AdminWire::Message> IgaDecode(const std::vector<std::string>& raw)
	{
		std::vector<AdminWire::Message> out;
		for (const std::string& r : raw)
		{
			AdminWire::Message d;
			if (AdminWire::Decode(r, d) == AdminWire::Decoded::Ok) out.push_back(d);
		}
		return out;
	}

	// Pairs the connection with a fresh code; returns the raw replies to the pair message.
	std::vector<std::string> IgaMakeProven(InGameAdminModel::Server& s, uint64_t conn, const std::string& id)
	{
		std::string code, err;
		s.IssueCode(id, code, err);
		IgaChallenge(s, conn, id);
		return IgaRaw(s, conn, id, IgaPair(code, kSecretA));
	}

	AdminWire::Snapshot IgaState(int mods, int players)
	{
		AdminWire::Snapshot snap;
		snap.defaultPolicy = "deny";
		snap.message = "Welcome";
		snap.rules.push_back({ "Best Builder.fast", "deny" });
		snap.mins.push_back({ "", "1.27.0" });
		snap.mins.push_back({ "Best Builder", "0.2.0" });
		for (int i = 0; i < mods; ++i)
		{
			AdminWire::ModInfo m;
			m.plugin = (i == 0) ? std::string("Best Builder") : "Mod" + std::to_string(i);
			m.version = "1.0." + std::to_string(i);
			for (int f = 0; f < 6; ++f)
				m.features.push_back({ "feature" + std::to_string(f), "A feature title that is fairly long number " + std::to_string(f) });
			snap.mods.push_back(m);
		}
		for (int i = 0; i < players; ++i)
		{
			AdminWire::PlayerInfo p;
			p.id = "player" + std::to_string(i);
			p.name = "Player " + std::to_string(i);
			p.flags = "admin";
			p.online = i % 2 == 0;
			for (int k = 0; k < 8; ++k)
				p.mods.push_back({ "Mod" + std::to_string(k), "1.0." + std::to_string(k) });
			snap.players.push_back(p);
		}
		return snap;
	}

	AdminWire::Message IgaOps(uint32_t request, uint32_t base, std::vector<AdminWire::Op> ops)
	{
		AdminWire::Message m;
		m.tag = AdminWire::Tag::Ops;
		m.requestId = request;
		m.baseRevision = base;
		m.ops = std::move(ops);
		return m;
	}
}

static void InGameAdminOpsTests()
{
	using AdminWire::Tag;
	const std::string path = (std::filesystem::temp_directory_path() / "su-iga-ops-test.txt").string();
	std::filesystem::remove(path);

	// A snapshot follows welcome admin, paged under 1 KB, carrying the revision.
	{
		IgaEnv e; e.path = path; e.admins = { "p1" };
		e.snapshot = [] { return IgaState(30, 40); };
		auto s = MakeIga(e);
		const std::vector<std::string> raw = IgaMakeProven(*s, 1, "p1");
		const auto msgs = IgaDecode(raw);
		CHECK(raw.size() > 10 && msgs.size() == raw.size());
		CHECK(!msgs.empty() && msgs[0].tag == Tag::Welcome && msgs[0].role == "admin");
		CHECK(msgs.size() > 2 && msgs[1].tag == Tag::SnapBegin && msgs[1].revision == 7);
		CHECK(!msgs.empty() && msgs.back().tag == Tag::SnapEnd && msgs.back().revision == 7);
		CHECK(msgs.size() > 3 && msgs[2].tag == Tag::SnapPolicy);
		size_t mods = 0, players = 0, rules = 0, mins = 0;
		bool small = true, spaced = false;
		for (size_t i = 0; i < raw.size(); ++i)
		{
			small = small && raw[i].size() < AdminWire::kMaxMessage;
			if (msgs[i].tag == Tag::SnapMod && !msgs[i].cont) { ++mods; spaced = spaced || msgs[i].mod.plugin == "Best Builder"; }
			if (msgs[i].tag == Tag::SnapPlayer && !msgs[i].cont) ++players;
			if (msgs[i].tag == Tag::SnapRule) rules += msgs[i].rules.size();
			if (msgs[i].tag == Tag::SnapMin) mins += msgs[i].mins.size();
		}
		CHECK(small);
		CHECK(mods == 30 && players == 40 && rules == 1 && mins == 2);
		CHECK(spaced);

		// A snap request gets it again; a connection that is not proven gets nothing.
		AdminWire::Message snap; snap.tag = Tag::Snap; snap.sections = "all";
		const auto again = IgaDecode(IgaRaw(*s, 1, "p1", snap));
		CHECK(again.size() == raw.size() - 1 && again[0].tag == Tag::SnapBegin);
		IgaChallenge(*s, 2, "p1");
		CHECK(IgaRaw(*s, 2, "p1", snap).empty());
		CHECK(IgaRaw(*s, 3, "p1", snap).empty());

		// The Admin flag is re-read on every message: lose it and the connection is dropped.
		e.admins.clear();
		CHECK(IgaRole(IgaDecode(IgaRaw(*s, 1, "p1", snap))) == "none");
		CHECK(!s->IsProven(1));
		CHECK(IgaRaw(*s, 1, "p1", snap).empty());
		std::filesystem::remove(path);
	}

	// Ops: handed to the runner as the server-read caller, the result echoes the request id.
	{
		IgaEnv e; e.path = path; e.admins = { "p1" };
		e.snapshot = [] { return IgaState(2, 2); };
		std::string seenId, seenName; std::vector<AdminWire::Op> seenOps; uint32_t seenBase = 99;
		int mode = 0;   // 0 ok, 1 stale, 2 error
		e.runOps = [&](const std::string& id, const std::string& name, const std::vector<AdminWire::Op>& ops, uint32_t base)
		{
			seenId = id; seenName = name; seenOps = ops; seenBase = base;
			InGameAdminModel::OpsOutcome o;
			o.revision = 8;
			if (mode == 0) { o.ok = true; o.text = "Saved"; }
			else if (mode == 1) { o.stale = true; o.text = AdminOps::kStaleText; o.revision = 9; }
			else { o.text = "that would remove the last Admin; use RCON for that"; }
			return o;
		};
		auto s = MakeIga(e);
		IgaMakeProven(*s, 1, "p1");

		const auto ok = IgaDecode(IgaRaw(*s, 1, "p1", IgaOps(41, 7, { { "rule.set", { "Best Builder.fast", "allow" } },
		                                                               { "player.kick", { "p7", "Be nice" } } })));
		CHECK(ok.size() == 1 && ok[0].tag == Tag::Result && ok[0].status == "ok" && ok[0].requestId == 41 && ok[0].revision == 8);
		CHECK(ok.size() == 1 && ok[0].text == "Saved");
		CHECK(seenId == "p1" && seenBase == 7 && seenOps.size() == 2);
		CHECK(seenOps.size() == 2 && seenOps[0].name == "rule.set" && seenOps[0].args[0] == "Best Builder.fast" && seenOps[1].args[1] == "Be nice");

		mode = 1;
		const auto stale = IgaDecode(IgaRaw(*s, 1, "p1", IgaOps(42, 3, { { "policy.default", { "deny" } } })));
		CHECK(stale.size() > 2 && stale[0].tag == Tag::Result && stale[0].status == "changed" && stale[0].requestId == 42);
		CHECK(stale.size() > 2 && stale[0].text == AdminOps::kStaleText && stale[1].tag == Tag::SnapBegin && stale.back().tag == Tag::SnapEnd);

		mode = 2;
		const auto err = IgaDecode(IgaRaw(*s, 1, "p1", IgaOps(43, 0, { { "player.remove", { "p1" } } })));
		CHECK(err.size() == 1 && err[0].status == "error" && err[0].text.find("last Admin") != std::string::npos);

		// Not proven, or no longer an Admin: nothing runs.
		seenOps.clear();
		IgaChallenge(*s, 2, "p1");
		CHECK(IgaRaw(*s, 2, "p1", IgaOps(1, 0, { { "policy.default", { "allow" } } })).empty());
		e.admins.clear();
		CHECK(IgaRole(IgaDecode(IgaRaw(*s, 1, "p1", IgaOps(2, 0, { { "policy.default", { "allow" } } })))) == "none");
		CHECK(seenOps.empty());
		std::filesystem::remove(path);
	}

	// Rate limit: 20 ops messages a second per connection, then an error result and the op is dropped.
	{
		IgaEnv e; e.path = path; e.admins = { "p1", "p2" };
		int ran = 0;
		e.runOps = [&](const std::string&, const std::string&, const std::vector<AdminWire::Op>&, uint32_t)
		{
			++ran;
			InGameAdminModel::OpsOutcome o; o.ok = true; o.revision = 7;
			return o;
		};
		auto s = MakeIga(e);
		IgaMakeProven(*s, 1, "p1");
		IgaMakeProven(*s, 2, "p2");
		int refused = 0;
		for (uint32_t i = 1; i <= 25; ++i)
		{
			const auto r = IgaDecode(IgaRaw(*s, 1, "p1", IgaOps(i, 0, { { "policy.default", { "allow" } } })));
			if (r.size() == 1 && r[0].status == "error" && r[0].requestId == i) ++refused;
		}
		CHECK(ran == 20 && refused == 5);
		// Another connection has its own allowance; time refills this one.
		CHECK(IgaDecode(IgaRaw(*s, 2, "p2", IgaOps(1, 0, { { "policy.default", { "allow" } } })))[0].status == "ok");
		e.now += 1.0;
		CHECK(IgaDecode(IgaRaw(*s, 1, "p1", IgaOps(99, 0, { { "policy.default", { "allow" } } })))[0].status == "ok");
		std::filesystem::remove(path);
	}

	// Changed push: every proven admin connection, nobody else.
	{
		IgaEnv e; e.path = path; e.admins = { "p1", "p2", "p3" };
		auto s = MakeIga(e);
		IgaMakeProven(*s, 1, "p1");
		IgaMakeProven(*s, 2, "p2");
		IgaChallenge(*s, 3, "p3");   // not proven
		const auto out = s->OnChanged("rules,players", 12);
		CHECK(out.size() == 2);
		for (const auto& o : out)
		{
			AdminWire::Message m;
			CHECK(AdminWire::Decode(o.data, m) == AdminWire::Decoded::Ok);
			CHECK(m.tag == Tag::Changed && m.revision == 12 && m.sections == "rules,players");
			CHECK(o.conn == 1 || o.conn == 2);
		}
		e.admins.erase("p2");   // lost the flag since: no push
		CHECK(s->OnChanged("all", 13).size() == 1);
		s->OnDisconnect(1);
		CHECK(s->OnChanged("all", 14).empty());
		std::filesystem::remove(path);
	}

	// Plugin names with single inner spaces travel intact through the codec and the catalog.
	{
		AdminWire::Message m = IgaOps(5, 2, { { "min.mod", { "Best Builder", "wire" } }, { "rule.set", { "Best Builder.*", "deny" } } });
		std::string wire; AdminWire::Message d;
		CHECK(AdminWire::Encode(m, wire) && AdminWire::Decode(wire, d) == AdminWire::Decoded::Ok);
		CHECK(d.ops.size() == 2 && d.ops[0].args[0] == "Best Builder" && d.ops[1].args[0] == "Best Builder.*");

		KnownFeatures::Catalog cat;
		for (const KnownFeatures::Report& r : KnownFeatures::ParseReportText("loader=1.2.3 interface=4\nBest Builder.fast=on\n", "b1"))
			cat.Merge(r, "2026-11-23");
		const auto entries = cat.Entries();
		CHECK(entries.size() == 1 && entries[0].plugin == "Best Builder" && entries[0].id == "fast");
		KnownFeatures::Catalog back;
		back.Parse(cat.Serialize());
		CHECK(back.Entries().size() == 1 && back.Entries()[0].plugin == "Best Builder");
	}
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
	CHECK(!ValidSectionKey("a|b") && !ValidSectionKey("a=b") && !ValidSectionKey("a;b") && !ValidSectionKey("a\"b") && ValidSectionKey("Best Builder.x"));
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

	// AdminOps: one dispatcher for RCON, the panel and the in-game tool.
	{
		using namespace AdminOps;

		struct Fake
		{
			RulesModel::Model model;
			std::vector<PlayerList::Entry> list;
			std::set<std::string> online;
			int commits = 0, refreshes = 0, kicks = 0;
			std::vector<std::string> audit;
			Backend be;

			Fake()
			{
				PlayerList::Entry a; a.id = "adm1"; a.admin = true; list.push_back(a);
				PlayerList::Entry p; p.id = "ply1"; list.push_back(p);
				online = { "adm1", "ply1", "ply2" };
				be.getModel = [this] { return model; };
				be.commitModel = [this](const RulesModel::Model& m, std::string&) { model = m; ++commits; return true; };
				be.players = [this] { return list; };
				be.validId = [](const std::string& id) { return !id.empty() && id.find(' ') == std::string::npos; };
				be.applyFlags = [](PlayerList::Entry& e, const std::string& flags, std::string& err)
				{
					if (flags == "admin") e.admin = true;
					else if (flags == "-admin") e.admin = false;
					else if (flags.rfind("label=", 0) == 0) e.label = flags.substr(6);
					else { err = "unknown flag"; return false; }
					return true;
				};
				be.setPlayer = [this](const std::string& id, const std::string& flags, std::string& err)
				{
					PlayerList::Entry e; e.id = id;
					for (auto& x : list) if (x.id == id) e = x;
					if (!be.applyFlags(e, flags, err)) return false;
					bool found = false;
					for (auto& x : list) if (x.id == id) { x = e; found = true; }
					if (!found) list.push_back(e);
					return true;
				};
				be.removePlayer = [this](const std::string& id, std::string&)
				{
					for (size_t i = 0; i < list.size(); ++i) if (list[i].id == id) { list.erase(list.begin() + i); return true; }
					return false;
				};
				be.clearAdmins = [this](int& n, std::string&) { n = 0; for (auto& e : list) if (e.admin) { e.admin = false; ++n; } return true; };
				be.refreshPlayers = [this] { ++refreshes; };
				be.reloadRules = [](std::string&) { return true; };
				be.reloadPlayers = [](std::string&) { return true; };
				be.connected = [this](const std::string& id) { return online.count(id) != 0; };
				be.kick = [this](const std::string&, const std::string&, std::string&) { ++kicks; return true; };
				be.audit = [this](const std::string& line) { audit.push_back(line); };
			}
			bool IsAdmin(const std::string& id) const { for (auto& e : list) if (e.id == id) return e.admin; return false; }
			bool Has(const std::string& id) const { for (auto& e : list) if (e.id == id) return true; return false; }
		};
		auto op = [](const char* n, std::vector<std::string> a = {}) { return Op{ n, std::move(a) }; };
		const Caller rcon = Caller::FromRcon();
		const Caller adm = Caller::FromPlayer("adm1", "Boss");

		// Each rules op, valid and invalid.
		{
			Fake f;
			CHECK(Apply(f.be, rcon, { op("rule.set", { "Codex.map", "deny" }) }).ok);
			CHECK(f.model.rules.size() == 1 && f.model.rules[0].deny);
			CHECK(Apply(f.be, rcon, { op("rule.set", { "Codex.map", "unset" }) }).ok && f.model.rules.empty());
			CHECK(!Apply(f.be, rcon, { op("rule.set", { "Codex.map", "maybe" }) }).ok);
			CHECK(!Apply(f.be, rcon, { op("rule.set", { "nodot", "deny" }) }).ok);
			// A mod name with a single inner space is accepted and round-trips.
			CHECK(Apply(f.be, rcon, { op("rule.set", { "Best Builder.fast", "deny" }) }).ok);
			CHECK(f.model.rules.size() == 1 && f.model.rules[0].plugin == "Best Builder" && f.model.rules[0].id == "fast" && f.model.rules[0].deny);
			CHECK(Apply(f.be, rcon, { op("rule.set", { "Best Builder.fast", "unset" }) }).ok && f.model.rules.empty());
			CHECK(!Apply(f.be, rcon, { op("rule.set", { "Codex.map" }) }).ok);
			CHECK(Apply(f.be, rcon, { op("policy.default", { "deny" }) }).ok && f.model.defaultDeny);
			CHECK(!Apply(f.be, rcon, { op("policy.default", { "sometimes" }) }).ok);
			CHECK(Apply(f.be, rcon, { op("policy.message", { "Hello there" }) }).ok && f.model.message == "Hello there");
			CHECK(Apply(f.be, rcon, { op("policy.message", { "off" }) }).ok && f.model.message.empty());
			CHECK(Apply(f.be, rcon, { op("min.loader", { "1.2.3" }) }).ok && f.model.minLoaderVersion == "1.2.3");
			CHECK(Apply(f.be, rcon, { op("min.loader", { "off" }) }).ok && f.model.minLoaderVersion.empty());
			CHECK(!Apply(f.be, rcon, { op("min.loader", { "not a version" }) }).ok);
			CHECK(Apply(f.be, rcon, { op("min.mod", { "Codex", "1.0.0" }) }).ok && f.model.modMins.size() == 1);
			CHECK(Apply(f.be, rcon, { op("min.mod", { "Codex", "off" }) }).ok && f.model.modMins.empty());
			CHECK(!Apply(f.be, rcon, { op("min.mod", { "Codex", "x.y" }) }).ok);
			CHECK(!Apply(f.be, rcon, { op("no.such.op") }).ok);
			CHECK(!Apply(f.be, rcon, {}).ok);
		}

		// Each player op, valid and invalid.
		{
			Fake f;
			CHECK(Apply(f.be, rcon, { op("player.set", { "ply2", "label=Joe" }) }).ok && f.Has("ply2"));
			CHECK(!Apply(f.be, rcon, { op("player.set", { "ply2", "bogus" }) }).ok);
			CHECK(!Apply(f.be, rcon, { op("player.set", { "bad id", "admin" }) }).ok);

			// A spaced section key goes through the real flag parser and the file formatter untouched.
			f.be.applyFlags = [](PlayerList::Entry& e, const std::string& flags, std::string& err) { return PlayerList::ApplyFlags(e, flags, err); };
			CHECK(Apply(f.be, rcon, { op("player.set", { "ply1", "sections=\"Best Builder.x;Other.y\"" }) }).ok);
			PlayerList::Entry sp;
			for (auto& x : f.list) if (x.id == "ply1") sp = x;
			CHECK(sp.sections.size() == 2 && sp.sections[0] == "Best Builder.x" && sp.sections[1] == "Other.y");
			PlayerList::Entry spBack;
			CHECK(PlayerList::ParseLine(PlayerList::FormatLine(sp), spBack) && spBack.sections == sp.sections);
			CHECK(Apply(f.be, rcon, { op("player.remove", { "ply2" }) }).ok && !f.Has("ply2"));
			CHECK(!Apply(f.be, rcon, { op("player.remove", { "ply2" }) }).ok);
			CHECK(Apply(f.be, rcon, { op("player.kick", { "ply1", "bye" }) }).ok && f.kicks == 1);
			CHECK(!Apply(f.be, rcon, { op("player.kick", { "gone", "" }) }).ok && f.kicks == 1);
			const Result c = Apply(f.be, rcon, { op("admin.clear") });
			CHECK(c.ok && c.text == "Took Admin off 1 entries." && !f.IsAdmin("adm1"));
			CHECK(Apply(f.be, rcon, { op("rules.reload") }).ok && Apply(f.be, rcon, { op("players.reload") }).ok);
			CHECK(!Apply(f.be, rcon, { op("rules.reload"), op("policy.default", { "deny" }) }).ok);
		}

		// A batch is one commit, one refresh, one listener call; a refused op applies nothing.
		{
			Fake f;
			int calls = 0; unsigned secs = 0; uint64_t rev = 0;
			const int h = AddListener([&](uint64_t r, unsigned s) { ++calls; secs = s; rev = r; });
			const uint64_t before = Revision();
			const Result r = Apply(f.be, rcon, { op("rule.set", { "Codex.map", "deny" }), op("policy.default", { "deny" }),
			                                     op("min.loader", { "1.0.0" }), op("player.set", { "ply2", "label=A" }),
			                                     op("player.set", { "ply1", "label=B" }) });
			CHECK(r.ok && f.commits == 1 && f.refreshes == 1);
			CHECK(calls == 1 && secs == (kSectionRules | kSectionMins | kSectionPlayers));
			CHECK(Revision() == before + 1 && r.revision == before + 1 && rev == before + 1);
			CHECK(f.audit.size() == 5);

			const Result bad = Apply(f.be, rcon, { op("rule.set", { "Codex.x", "deny" }), op("player.set", { "ply2", "bogus" }) });
			CHECK(!bad.ok && f.commits == 1 && calls == 1 && f.model.rules.size() == 1 && Revision() == before + 1);
			RemoveListener(h);
			Apply(f.be, rcon, { op("policy.default", { "allow" }) });
			CHECK(calls == 1);
		}

		// A stale base revision is refused; the current one and 0 are accepted.
		{
			Fake f;
			const uint64_t cur = Revision();
			const Result stale = Apply(f.be, rcon, { op("policy.default", { "deny" }) }, cur - 1);
			CHECK(!stale.ok && stale.stale && stale.text == AdminOps::kStaleText && f.commits == 0);
			CHECK(Apply(f.be, rcon, { op("policy.default", { "deny" }) }, cur).ok);
			CHECK(Apply(f.be, rcon, { op("policy.default", { "allow" }) }, 0).ok);
			CHECK(!Apply(f.be, rcon, { op("policy.default", { "deny" }) }, cur).ok);   // cur is now old
		}

		// The in-game caller is checked on every call.
		{
			Fake f;
			CHECK(Apply(f.be, adm, { op("policy.default", { "deny" }) }).ok);
			CHECK(!Apply(f.be, Caller::FromPlayer("ply1", "Joe"), { op("policy.default", { "allow" }) }).ok);   // not an admin
			CHECK(!Apply(f.be, Caller::FromPlayer("nobody", "X"), { op("policy.default", { "allow" }) }).ok);   // not listed
			const Result unread = Apply(f.be, Caller::FromPlayer("", "X"), { op("policy.default", { "allow" }) });
			CHECK(!unread.ok && unread.text == "your player ID cannot be read yet" && f.model.defaultDeny);
			f.list[0].admin = false;   // demoted between two calls
			CHECK(!Apply(f.be, adm, { op("policy.default", { "allow" }) }).ok);
		}

		// In-game guards: last Admin, kicking an admin, RCON-only ops. RCON is not limited.
		{
			Fake f;
			CHECK(!Apply(f.be, adm, { op("player.set", { "adm1", "-admin" }) }).ok && f.IsAdmin("adm1"));
			CHECK(!Apply(f.be, adm, { op("player.remove", { "adm1" }) }).ok && f.Has("adm1"));
			CHECK(!Apply(f.be, adm, { op("player.kick", { "adm1", "" }) }).ok && f.kicks == 0);
			CHECK(!Apply(f.be, adm, { op("admin.clear") }).ok);
			CHECK(!Apply(f.be, adm, { op("rules.reload") }).ok && !Apply(f.be, adm, { op("players.reload") }).ok);
			CHECK(Apply(f.be, adm, { op("player.kick", { "ply1", "x" }) }).ok && f.kicks == 1);
			// With a second Admin the first may step down.
			CHECK(Apply(f.be, adm, { op("player.set", { "ply1", "admin" }) }).ok);
			CHECK(Apply(f.be, adm, { op("player.set", { "adm1", "-admin" }) }).ok && !f.IsAdmin("adm1"));

			Fake g;
			CHECK(Apply(g.be, rcon, { op("player.set", { "adm1", "-admin" }) }).ok && !g.IsAdmin("adm1"));
			Fake h;
			CHECK(Apply(h.be, rcon, { op("player.remove", { "adm1" }) }).ok);
			Fake k;
			CHECK(Apply(k.be, rcon, { op("player.kick", { "adm1", "" }) }).ok);
		}

		// An edit made outside Apply counts as a change.
		{
			int calls = 0; unsigned secs = 0;
			const int h = AddListener([&](uint64_t, unsigned s) { ++calls; secs = s; });
			const uint64_t before = Revision();
			NoteChanged(kSectionPlayers);
			CHECK(Revision() == before + 1 && calls == 1 && secs == kSectionPlayers);
			RemoveListener(h);
		}
	}

	// Remembered feature catalog.
	{
		using namespace KnownFeatures;
		auto rep = [](const char* plugin, const char* id, const char* version, const char* build, bool off, const char* title = "")
		{
			Report r; r.plugin = plugin; r.id = id; r.version = version; r.build = build; r.off = off; r.title = title;
			return r;
		};
		const std::string d1 = "2026-11-20", d2 = "2026-11-23";

		// Merge: a new feature is added, the same one again is not a change, a new day is.
		Catalog c;
		CHECK(c.Merge(rep("BetterDrone", "drone-limits", "1.2.0", "", false, "Drone limits"), d1));
		CHECK(c.Size() == 1);
		CHECK(!c.Merge(rep("betterdrone", "Drone-Limits", "1.2.0", "", false), d1));
		CHECK(c.Size() == 1);
		CHECK(c.Merge(rep("BetterDrone", "drone-limits", "1.2.0", "", false), d2));
		CHECK(c.Entries()[0].lastSeen == d2 && c.Entries()[0].title == "Drone limits" && c.Entries()[0].source == "reported");
		CHECK(c.Merge(rep("BetterCheats", "movement", "", "", false), d1) && c.Size() == 2);
		CHECK(c.Entries()[0].plugin == "BetterCheats");   // sorted

		// Newest version wins (numbers, not text).
		CHECK(CompareVersions("1.10.0", "1.9.0") > 0 && CompareVersions("v1.2", "1.2.0") == 0);
		CHECK(CompareVersions("1.2.0-dev.3", "1.2.0") == 0 && CompareVersions("", "0.0.1") < 0);
		Catalog v;
		v.Merge(rep("P", "f", "1.9.0", "", false, "Old title"), d1);
		CHECK(v.Merge(rep("P", "f", "1.10.0", "", false, "New title"), d1));
		CHECK(v.Entries()[0].version == "1.10.0" && v.Entries()[0].title == "New title");
		CHECK(!v.Merge(rep("P", "f", "1.9.5", "", false, "Older title"), d1));
		CHECK(v.Entries()[0].version == "1.10.0" && v.Entries()[0].title == "New title");

		// Off by build accumulates; an on report does not clear it.
		Catalog b;
		b.Merge(rep("P", "f", "1.0", "1.0.100", true), d1);
		CHECK(b.Merge(rep("P", "f", "1.0", "1.0.200", true), d1));
		CHECK(!b.Merge(rep("P", "f", "1.0", "1.0.200", true), d1));
		CHECK(!b.Merge(rep("P", "f", "1.0", "1.0.300", false), d1));
		CHECK(b.Entries()[0].offBuilds == std::set<std::string>({ "1.0.100", "1.0.200" }));

		// A loader's text: first line skipped, bad lines skipped.
		const std::vector<Report> parsed = ParseReportText("loader=1.2 interface=79\nA.x=on\nB.y=off\nnoequals\nC=on\nD.z=maybe\n", "b1");
		CHECK(parsed.size() == 2 && parsed[0].plugin == "A" && !parsed[0].off && parsed[1].id == "y" && parsed[1].off && parsed[1].build == "b1");

		// Save then load round trip, through the file.
		const std::filesystem::path dir = std::filesystem::temp_directory_path() / "su_known_features_test";
		std::filesystem::remove_all(dir);
		const std::wstring path = (dir / "known-features.txt").wstring();
		Catalog full;
		full.Merge(rep("Plug", "id1", "2.0.1", "b1", true, "A title with spaces"), d1);
		full.Merge(rep("Plug", "id1", "2.0.1", "b2", true), d2);
		full.Merge(rep("Plug", "id2", "", "", false), d2);
		CHECK(SaveFile(path, full.Serialize()));
		CHECK(!std::filesystem::exists(dir / "known-features.txt.tmp"));
		std::string back;
		CHECK(LoadFile(path, back));
		Catalog loaded;
		CHECK(loaded.Parse(back) == 0);
		CHECK(loaded.Serialize() == full.Serialize() && loaded.Size() == 2);
		CHECK(loaded.Entries()[0].offBuilds.size() == 2 && loaded.Entries()[0].lastSeen == d2 && loaded.Entries()[0].title == "A title with spaces");
		CHECK(SaveFile(path, loaded.Serialize()));   // overwrites an existing file
		std::filesystem::remove_all(dir);
		std::string none;
		CHECK(!LoadFile(path, none));

		// A corrupt line is skipped, the rest kept.
		Catalog corrupt;
		const int skipped = corrupt.Parse("known-features 1\n"
			"feature\tP\t1.0\ta\tTitle\t\t2026-11-20\treported\n"
			"garbage line\n"
			"feature\tP\t1.0\tb\tTitle\t\tnot-a-date\treported\n"
			"feature\tP\t1.0\n"
			"feature\tQ\t\tc\t\tb1,b2\t2026-11-21\tpublished\n");
		CHECK(skipped == 3 && corrupt.Size() == 2);
		CHECK(corrupt.Entries()[1].source == "published" && corrupt.Entries()[1].offBuilds.size() == 2);

		// Debounce: the first change writes, none again inside 10 s, one after.
		Debounce db;
		CHECK(!db.Due(false, 0));
		CHECK(db.Due(true, 1000));
		db.Wrote(1000);
		CHECK(!db.Due(true, 1001) && !db.Due(true, 10999));
		CHECK(!db.Due(false, 20000));
		CHECK(db.Due(true, 11000));
		db.Wrote(11000);
		CHECK(!db.Due(true, 12000));

		// Rule keys that match nothing remembered.
		const std::vector<std::string> unmatched = UnmatchedKeys({ "BetterDrone.drone-limits", "betterdrone.*", "Other.x", "Other.*", "BetterCheats.nope" }, c.Entries());
		CHECK(unmatched.size() == 3 && unmatched[0] == "Other.x" && unmatched[1] == "Other.*" && unmatched[2] == "BetterCheats.nope");
	}

	InGameAdminTests();
	InGameAdminOpsTests();

	// Mod names with single inner spaces, as the mod loader allows them.
	{
		std::string plugin, id, err;
		CHECK(ParseKey("Best Builder.some_feature", plugin, id, err) && plugin == "Best Builder" && id == "some_feature");
		CHECK(ParseKey("A_b-1 C.*", plugin, id, err) && plugin == "A_b-1 C" && id == "*");
		CHECK(!ParseKey(" Best.x", plugin, id, err));
		CHECK(!ParseKey("Best .x", plugin, id, err));
		CHECK(!ParseKey("Best  Builder.x", plugin, id, err));
		CHECK(!ParseKey("Best Builder.some feature", plugin, id, err));
		CHECK(!ParseKey("Best.Builder.x", plugin, id, err));
		CHECK(ParseKey((std::string(64, 'a') + ".x").c_str(), plugin, id, err));
		CHECK(!ParseKey((std::string(65, 'a') + ".x").c_str(), plugin, id, err));

		Model sp;
		std::string e;
		CHECK(SetRule(sp, "Best Builder.some_feature", true, e) && SetRule(sp, "best builder.some_feature", false, e) && sp.rules.size() == 1);
		CHECK(SetRule(sp, "Best Builder.other", true, e));
		CHECK(SetModMin(sp, "Best Builder", "1.2.0", e) && SetModMin(sp, "best builder", "1.3.0", e) && sp.modMins.size() == 1);
		CHECK(!SetModMin(sp, " Best", "1.0.0", e) && !SetModMin(sp, "Best ", "1.0.0", e) && !SetModMin(sp, "Best  Builder", "1.0.0", e));
		CHECK(!SetModMin(sp, std::string(65, 'a'), "1.0.0", e) && SetModMin(sp, std::string(64, 'a'), "1.0.0", e));
		CHECK(!SetModMin(sp, "Best.Builder", "1.0.0", e));
		CHECK(SetModMin(sp, std::string(64, 'a'), "", e));
		CHECK(RulesText(sp) == "default=allow\nBest Builder.some_feature=allow\nBest Builder.other=deny\n");
		CHECK(VersionMinimumsText(sp) == "Best Builder=1.3.0\n");

		// The file round trip: written rows read back the same, outer spaces trimmed.
		std::vector<std::string> file = { "[Rules]" };
		for (const std::string& l : RuleLines(sp)) file.push_back("  " + l + "  ");
		file.push_back("[MinVersions]");
		for (const std::string& l : ModMinLines(sp)) file.push_back("  " + l + "  ");
		std::vector<std::string> warn;
		Model back = ParseText(file, warn);
		CHECK(warn.empty() && back.rules.size() == 2 && back.rules[0].plugin == "Best Builder" && back.rules[1].id == "other" && back.rules[1].deny);
		CHECK(back.modMins.size() == 1 && back.modMins[0].plugin == "Best Builder" && back.modMins[0].version == "1.3.0");
		CHECK(RuleLines(back) == RuleLines(sp) && ModMinLines(back) == ModMinLines(sp));

		// A row with a bad name stays in the file and warns.
		std::vector<std::string> bad = { "[Rules]", "Best  Builder.x=deny", "[MinVersions]", "Best  Builder=1.0.0" };
		std::vector<std::string> bw;
		Model bm = ParseText(bad, bw);
		CHECK(bw.size() == 2 && bm.rules.empty() && bm.modMins.empty() && bm.rulesKept.size() == 1 && bm.modMinKept.size() == 1);
	}

	// Console words: a double-quoted part may hold spaces.
	{
		std::string rest = "deny \"Best Builder.some_feature\"  extra";
		CHECK(WordSplit::TakeWord(rest) == "deny");
		CHECK(WordSplit::TakeWord(rest) == "Best Builder.some_feature" && rest == "extra");
		CHECK(WordSplit::TakeWord(rest) == "extra" && rest.empty());
		CHECK(WordSplit::TakeWord(rest).empty());

		rest = "  modmin \"Best Builder\" 1.2.0";
		CHECK(WordSplit::TakeWord(rest) == "modmin");
		CHECK(WordSplit::TakeWord(rest) == "Best Builder" && rest == "1.2.0");

		rest = "\"Best Builder\".feature tail";
		CHECK(WordSplit::TakeWord(rest) == "Best Builder.feature" && rest == "tail");

		rest = "RuptureTimer 0.11.0";
		CHECK(WordSplit::TakeWord(rest) == "RuptureTimer" && rest == "0.11.0");

		rest = "\"unterminated name";
		CHECK(WordSplit::TakeWord(rest) == "unterminated name" && rest.empty());
		rest = "   ";
		CHECK(WordSplit::TakeWord(rest).empty() && rest.empty());

		rest = "\"";
		CHECK(WordSplit::TakeWord(rest).empty() && rest.empty());
		rest = "ab\"c d\"";
		CHECK(WordSplit::TakeWord(rest) == "abc d" && rest.empty());
		rest = "\"\" x";
		CHECK(WordSplit::TakeWord(rest).empty() && rest == "x");
		rest = "a	b c";
		CHECK(WordSplit::TakeWord(rest) == "a	b" && rest == "c");
	}

	// Per-player section keys: a mod name with a space is quoted in the flags and in the file.
	{
		PlayerList::Entry e;
		std::string err;
		CHECK(PlayerList::ApplyFlags(e, "sections=\"Best Builder.x;Other.y\" admin", err));
		CHECK(e.admin && e.sections.size() == 2 && e.sections[0] == "Best Builder.x" && e.sections[1] == "Other.y");

		const std::string line = PlayerList::FormatLine(e);
		CHECK(line.find("Sections=\"Best Builder.x;Other.y\"") != std::string::npos);
		PlayerList::Entry back;
		CHECK(PlayerList::ParseLine("76561198000000001=Al|ExemptRules Sections=\"Best Builder.x;Other.y\" ExemptVersion", back));
		CHECK(back.exemptRules && back.exemptVersion && back.sections == e.sections && back.label == "Al");
		e.id = "76561198000000001";
		CHECK(PlayerList::ParseLine(PlayerList::FormatLine(e), back) && back.sections == e.sections && back.admin);

		// Unspaced keys: unchanged on the way in and out, written without quotes.
		PlayerList::Entry plain;
		CHECK(PlayerList::ApplyFlags(plain, "sections=wave;save label=Some Name", err));
		CHECK(plain.sections.size() == 2 && plain.sections[1] == "save" && plain.label == "Some Name");
		plain.id = "1";
		CHECK(PlayerList::FormatLine(plain) == "1=Some Name|Sections=wave;save");
		CHECK(PlayerList::ParseLine("2=Bob|ExemptVersion Sections=wave;save", back) && back.sections.size() == 2 && back.exemptVersion);

		// An unquoted space still ends the value, as before.
		CHECK(PlayerList::ParseLine("3=Bob|Sections=Best Builder.x", back) && back.sections.size() == 1 && back.sections[0] == "Best");
	}

	std::printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
