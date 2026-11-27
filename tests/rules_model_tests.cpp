// Unit tests of the rules parser and the text it makes for the mod loader.
// No project file: from a Visual Studio developer prompt, in the repo root:
//   cl /nologo /EHsc /std:c++17 /Isrc tests\rules_model_tests.cpp src\host_rules\rules_model.cpp src\host_rules\claim_model.cpp src\host_rules\admin_ops.cpp src\host_rules\known_features.cpp src\host_rules\ingame_admin_model.cpp src\host_rules\player_list.cpp src\host_rules\ini_file.cpp src\admin_wire\admin_wire.cpp src\admin_wire\admin_crypto.cpp user32.lib /I<Plugin SDK>\include /Fe:rules_model_tests.exe
//   rules_model_tests.exe
#include "host_rules/rules_model.h"
#include "host_rules/claim_model.h"
#include "host_rules/admin_ops.h"
#include "host_rules/known_features.h"
#include "host_rules/line_text.h"
#include "host_rules/ini_file.h"
#include "cheats/cheat_authority.h"
#include "host_rules/player_list.h"
#include "admin/url_match.h"
#include "admin/admin_json.h"
#include "host_rules/ingame_admin_model.h"
#include "admin_wire/admin_wire.h"
#include "admin_wire/admin_crypto.h"
#include "client/admin_client.h"   // only its catalog limits are used
#include "rcon/commands/word_split.h"

#include <cctype>
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
		std::set<std::string> rulesFlagged;   // IDs with ExemptRules or Sections= but no Admin
		int proofChanges = 0;
		std::vector<uint64_t> rejected;   // connections whose proof was refused, in order
		double now = 1000;
		bool on = true;
		std::vector<std::string> audit;
		std::string path;
		// The state an in-game admin snapshot carries and what a batch of ops comes to. Unset = none.
		std::function<AdminWire::Snapshot()> snapshot;
		std::function<InGameAdminModel::OpsOutcome(const std::string&, const std::string&,
		                                           const std::vector<AdminWire::Op>&, uint32_t)> runOps;
		std::function<void(const std::vector<std::string>&)> catalogPush;
	};

	std::unique_ptr<InGameAdminModel::Server> MakeIga(IgaEnv& e)
	{
		InGameAdminModel::Env env;
		env.enabled = [&e] { return e.on; };
		env.isAdmin = [&e](const std::string& id) { return e.admins.count(id) > 0; };
		env.canProve = [&e](const std::string& id) { return e.rulesFlagged.count(id) > 0; };
		env.proofChanged = [&e] { ++e.proofChanges; };
		env.proofRejected = [&e](uint64_t conn) { e.rejected.push_back(conn); };
		env.revision = [] { return 7u; };
		env.now = [&e] { return e.now; };
		env.audit = [&e](const std::string& line) { e.audit.push_back(line); };
		env.storePath = e.path;
		env.snapshot = e.snapshot;
		env.runOps = e.runOps;
		env.catalogPush = e.catalogPush;
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
		// A prove on a proven connection gets no reply and leaves it proven.
		CHECK(IgaSend(*s, 3, "p1", prove).empty());
		CHECK(s->IsProven(3));

		nonce = IgaChallenge(*s, 4, "p1");
		AdminCrypto::MakeProof(kSecretB, nonce, sid, prove.proof);
		CHECK(IgaRole(IgaSend(*s, 4, "p1", prove)) == "pair");
		CHECK(!s->IsProven(4));

		nonce = IgaChallenge(*s, 5, "p1");
		prove.proof.clear();
		CHECK(IgaRole(IgaSend(*s, 5, "p1", prove)) == "pair");
		CHECK(!s->IsProven(5));

		// A challenge is good for one proof, on its own connection, for 30 seconds.
		{
			const auto proveFor = [&](const std::string& n)
			{
				AdminWire::Message p; p.tag = Tag::Prove;
				AdminCrypto::MakeProof(kSecretA, n, sid, p.proof);
				return p;
			};
			// A proof replayed on another connection fails; on its own it still works.
			const std::string n6 = IgaChallenge(*s, 6, "p1");
			IgaChallenge(*s, 7, "p1");
			const AdminWire::Message p6 = proveFor(n6);
			CHECK(IgaRole(IgaSend(*s, 7, "p1", p6)) == "pair" && !s->IsProven(7));
			CHECK(IgaRole(IgaSend(*s, 6, "p1", p6)) == "admin" && s->IsProven(6));
			// A second prove on a proven connection gets no reply.
			CHECK(IgaSend(*s, 6, "p1", p6).empty() && s->IsProven(6));
			const std::string n8 = IgaChallenge(*s, 8, "p1");
			prove.proof = std::string(64, '0');
			IgaSend(*s, 8, "p1", prove);   // a wrong proof uses the nonce up
			CHECK(IgaRole(IgaSend(*s, 8, "p1", proveFor(n8))) == "pair" && !s->IsProven(8));
			// A fresh challenge replaces the older nonce.
			const std::string n9 = IgaChallenge(*s, 9, "p1");
			const std::string n9b = IgaChallenge(*s, 9, "p1");
			CHECK(n9 != n9b);
			CHECK(IgaRole(IgaSend(*s, 9, "p1", proveFor(n9))) == "pair" && !s->IsProven(9));
			// 29 seconds is in time, 31 is not.
			const std::string n10 = IgaChallenge(*s, 10, "p1");
			const std::string n11 = IgaChallenge(*s, 11, "p1");
			e.now += 29;
			CHECK(IgaRole(IgaSend(*s, 10, "p1", proveFor(n10))) == "admin" && s->IsProven(10));
			e.now += 2;
			CHECK(IgaRole(IgaSend(*s, 11, "p1", proveFor(n11))) == "pair" && !s->IsProven(11));
			// A connection whose player ID changes after the proof loses proven status.
			AdminWire::Message snap; snap.tag = Tag::Snap; snap.sections = "all";
			CHECK(s->IsProven(6));
			IgaSend(*s, 6, "p2", snap);
			CHECK(!s->IsProven(6));
		}

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
		snap.defaults.push_back("Best Builder.build-anywhere");
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
		size_t mods = 0, players = 0, rules = 0, mins = 0, defaults = 0;
		bool allSmall = true, spaced = false;
		for (size_t i = 0; i < raw.size(); ++i)
		{
			allSmall = allSmall && raw[i].size() < AdminWire::kMaxMessage;
			if (msgs[i].tag == Tag::SnapMod && !msgs[i].cont) { ++mods; spaced = spaced || msgs[i].mod.plugin == "Best Builder"; }
			if (msgs[i].tag == Tag::SnapPlayer && !msgs[i].cont) ++players;
			if (msgs[i].tag == Tag::SnapRule) rules += msgs[i].rules.size();
			if (msgs[i].tag == Tag::SnapMin) mins += msgs[i].mins.size();
			if (msgs[i].tag == Tag::SnapDefault) defaults += msgs[i].keys.size();
		}
		CHECK(allSmall);
		CHECK(mods == 30 && players == 40 && rules == 1 && mins == 2);
		CHECK(defaults == 1);   // a full snapshot carries the default-denied keys
		CHECK(spaced);

		// A mods-only snapshot carries the catalog and none of the defaults.
		AdminWire::Message modsOnly; modsOnly.tag = Tag::Snap; modsOnly.sections = "mods";
		size_t modsOnlyDefaults = 0, modsOnlyMods = 0;
		for (const auto& d : IgaDecode(IgaRaw(*s, 1, "p1", modsOnly)))
		{
			if (d.tag == Tag::SnapDefault) modsOnlyDefaults += d.keys.size();
			if (d.tag == Tag::SnapMod && !d.cont) ++modsOnlyMods;
		}
		CHECK(modsOnlyDefaults == 0 && modsOnlyMods == 30);

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

	// Rate limit: a burst of 100 tokens per connection; every decoded message takes tokens,
	// an ops batch one per op. Over the limit: dropped with no reply, one audit line per burst.
	{
		IgaEnv e; e.path = path; e.admins = { "p1", "p2" };
		int ran = 0;
		e.runOps = [&](const std::string&, const std::string&, const std::vector<AdminWire::Op>& ops, uint32_t)
		{
			ran += static_cast<int>(ops.size());
			InGameAdminModel::OpsOutcome o; o.ok = true; o.revision = 7;
			return o;
		};
		auto s = MakeIga(e);
		IgaMakeProven(*s, 1, "p1");
		IgaMakeProven(*s, 2, "p2");
		auto tooMany = [&e] { int n = 0; for (const auto& l : e.audit) if (l.find("too many requests") != std::string::npos) ++n; return n; };

		// Single-op messages: the burst, then silence.
		int answered = 0;
		for (uint32_t i = 1; i <= 120; ++i)
			if (!IgaRaw(*s, 1, "p1", IgaOps(i, 0, { { "policy.default", { "allow" } } })).empty()) ++answered;
		CHECK(answered <= 98 && answered >= 90);   // hello and pair above took a few tokens
		CHECK(ran == answered && tooMany() == 1);
		// Another connection has its own allowance; time refills this one.
		CHECK(IgaDecode(IgaRaw(*s, 2, "p2", IgaOps(1, 0, { { "policy.default", { "allow" } } })))[0].status == "ok");
		e.now += 1.0;
		CHECK(IgaDecode(IgaRaw(*s, 1, "p1", IgaOps(999, 0, { { "policy.default", { "allow" } } }))).size() == 1);
		CHECK(tooMany() == 1);

		// A batch is charged per op: 60 ops, then a second 60 is dropped.
		std::vector<AdminWire::Op> sixty(60, AdminWire::Op{ "min.loader", { "1" } });
		ran = 0;
		e.now += 10.0;
		CHECK(IgaDecode(IgaRaw(*s, 2, "p2", IgaOps(5, 0, sixty))).size() == 1 && ran == 60);
		CHECK(IgaRaw(*s, 2, "p2", IgaOps(6, 0, sixty)).empty() && ran == 60);
		CHECK(tooMany() == 2);
		std::filesystem::remove(path);
	}

	// Messages that are not proven take tokens too, and audit lines about failures stop at 5.
	{
		IgaEnv e; e.path = path; e.admins = { "p1", "p2" };
		auto s = MakeIga(e);
		IgaMakeProven(*s, 1, "p1");   // p1 has a secret
		auto count = [&e](const char* what) { int n = 0; for (const auto& l : e.audit) if (l.find(what) != std::string::npos) ++n; return n; };

		// Hellos from an unproven connection: 100 are answered, the rest dropped with one line.
		int replies = 0;
		for (int i = 0; i < 300; ++i)
			replies += static_cast<int>(IgaRaw(*s, 7, "p2", IgaHello()).size());
		CHECK(replies == 100 && count("too many requests") == 1);

		// A flood that runs on across several refill periods still logs once; a full refill re-arms it.
		for (int round = 0; round < 5; ++round)
		{
			e.now += 0.5;   // 10 tokens back
			for (int i = 0; i < 30; ++i) IgaRaw(*s, 7, "p2", IgaHello());
		}
		CHECK(count("too many requests") == 1);
		e.now += 30.0;
		CHECK(!IgaRaw(*s, 7, "p2", IgaHello()).empty());
		for (int i = 0; i < 300; ++i) IgaRaw(*s, 7, "p2", IgaHello());
		CHECK(count("too many requests") == 2);

		// Wrong proofs on one connection: the answer stays, the audit line stops at 5.
		const std::string sid = s->ServerId();
		e.audit.clear();
		for (int i = 0; i < 12; ++i)
		{
			const std::string nonce = IgaChallenge(*s, 8, "p1");
			CHECK(!nonce.empty());
			AdminWire::Message prove; prove.tag = Tag::Prove;
			AdminCrypto::MakeProof(kSecretB, nonce, sid, prove.proof);
			CHECK(IgaRole(IgaSend(*s, 8, "p1", prove)) == "pair");
		}
		CHECK(count("proof failed") == 5);

		// Wrong pairing codes: same cap.
		e.audit.clear();
		for (int i = 0; i < 12; ++i)
		{
			IgaChallenge(*s, 9, "p2");
			IgaSend(*s, 9, "p2", IgaPair("22222222", kSecretA));
		}
		CHECK(count("pairing refused") >= 1 && count("pairing refused") + count("voided") <= 5);
		std::filesystem::remove(path);
	}

	// A connection told "none" that later gains Admin is woken with a challenge, once.
	{
		IgaEnv e; e.path = path; e.admins = { "p1" };
		auto s = MakeIga(e);
		CHECK(IgaRole(IgaSend(*s, 5, "p9", IgaHello())) == "none");
		CHECK(IgaRaw(*s, 5, "p9", IgaHello()).empty());   // silent
		CHECK(s->OnPlayersChanged().empty());
		e.admins.insert("p9");
		const auto woke = s->OnPlayersChanged();
		CHECK(woke.size() == 1 && woke[0].conn == 5);
		AdminWire::Message ch;
		CHECK(woke.size() == 1 && AdminWire::Decode(woke[0].data, ch) == AdminWire::Decoded::Ok && ch.tag == Tag::Challenge && !ch.nonce.empty());
		CHECK(s->OnPlayersChanged().empty());
		std::filesystem::remove(path);
	}

	// Section keys with spaces are quoted in the flag text and survive the real parser.
	{
		const std::string flag = PlayerList::SectionsFlag("sections", { "Best Builder.x", "Other.y" });
		CHECK(flag == "sections=\"Best Builder.x;Other.y\"");
		CHECK(PlayerList::SectionsFlag("sections", { "A.b", "C.d" }) == "sections=A.b;C.d");
		PlayerList::Entry en; std::string err;
		CHECK(PlayerList::ApplyFlags(en, flag, err) && en.sections.size() == 2 && en.sections[0] == "Best Builder.x");
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

	// catalog.push: ignored from an unproven connection; from a proven one stored with titles
	// and versions, capped, bad names dropped; the originator gets result before changed.
	{
		IgaEnv e; e.path = path; e.admins = { "p1", "p2" };
		InGameAdminModel::Server* sp = nullptr;
		int pushes = 0;
		e.catalogPush = [&](const std::vector<std::string>& rows)
		{
			++pushes;
			if (KnownFeatures::NoteDeclaredRows(rows))
				sp->OnChanged(AdminWire::kSectionMods, 9);   // as the plugin does when the catalog changed
		};
		auto s = MakeIga(e);
		sp = s.get();
		auto push = [&](uint64_t conn, const std::string& id, const std::vector<std::string>& rows)
		{
			std::vector<AdminWire::Op> ops;
			for (const std::string& r : rows) ops.push_back({ "catalog.push", { r } });
			return IgaRaw(*s, conn, id, IgaOps(31, 0, ops));
		};
		auto find = [](const char* plugin, const char* id) -> const KnownFeatures::Entry*
		{
			static std::vector<KnownFeatures::Entry> snap;
			snap = KnownFeatures::Snapshot();
			for (const auto& en : snap) if (en.plugin == plugin && en.id == id) return &en;
			return nullptr;
		};

		IgaChallenge(*s, 5, "p2");   // challenged, not proven
		CHECK(push(5, "p2", { "Unproven	1	f	on	T" }).empty());
		CHECK(pushes == 0 && !find("Unproven", "f"));

		IgaMakeProven(*s, 1, "p1");
		IgaMakeProven(*s, 2, "p2");
		const auto out = push(1, "p1", { "Best Builder	2.1	fast	on	Fast build mode", "Bad Plugin!	1	f	on	T", "Ok	1	bad id	on	T" });
		// The originator: result first, then changed. The other admin's changed goes out on its own.
		CHECK(out.size() == 2);
		if (out.size() == 2)
		{
			AdminWire::Message a, b;
			CHECK(AdminWire::Decode(out[0], a) == AdminWire::Decoded::Ok && a.tag == Tag::Result && a.requestId == 31);
			CHECK(AdminWire::Decode(out[1], b) == AdminWire::Decoded::Ok && b.tag == Tag::Changed && b.sections == "mods");
		}
		const KnownFeatures::Entry* en = find("Best Builder", "fast");
		CHECK(en && en->title == "Fast build mode" && en->version == "2.1");
		CHECK(!find("Bad Plugin!", "f") && !find("Ok", "bad id"));

		// Past the per-report cap only the first rows are kept.
		std::vector<std::string> many;
		for (int i = 0; i < 70; ++i) many.push_back("Capped	1.0	f" + std::to_string(i) + "	on	T");
		KnownFeatures::NoteDeclaredRows(many);   // the cap applies to one report, so call the catalog directly
		int kept = 0;
		for (const auto& x : KnownFeatures::Snapshot()) if (x.plugin == "Capped") ++kept;
		CHECK(kept == static_cast<int>(KnownFeatures::kMaxRowsPerReport));

		// Mixed with another op: refused with an error result.
		std::vector<AdminWire::Op> mixed = { { "catalog.push", { "Mixed	1	f	on	T" } }, { "policy.default", { "allow" } } };
		const auto r = IgaDecode(IgaRaw(*s, 1, "p1", IgaOps(32, 0, mixed)));
		CHECK(r.size() == 1 && r[0].status == AdminWire::kResultError && !find("Mixed", "f"));
		std::filesystem::remove(path);
	}

	// A big catalog push, paced as the admin client sends it (a burst of kCatalogBurstRows, then
	// kCatalogRowsPerSecond, in messages of about ten rows, what fits kMaxMessage): every row arrives and the request
	// limit never trips. Sent back to back it does trip, which is why the client paces.
	{
		auto run = [&](size_t rowCount, bool paced, std::vector<std::string>& got)
		{
			IgaEnv e; e.path = path; e.admins = { "p1" };
			e.catalogPush = [&](const std::vector<std::string>& rows) { got.insert(got.end(), rows.begin(), rows.end()); };
			auto s = MakeIga(e);
			IgaMakeProven(*s, 1, "p1");
			uint32_t req = 50;
			size_t at = 0;
			double budget = AdminClient::kCatalogBurstRows;
			while (at < rowCount)
			{
				const size_t n = std::min<size_t>(std::min<size_t>(10, rowCount - at), paced ? static_cast<size_t>(budget) : 10);
				if (n == 0) { e.now += 0.25; budget += 0.25 * AdminClient::kCatalogRowsPerSecond; continue; }
				std::vector<AdminWire::Op> ops;
				for (size_t i = at; i < at + n; ++i)
					ops.push_back({ "catalog.push", { "Paced	1.0	f" + std::to_string(i) + "	on	T" } });
				IgaRaw(*s, 1, "p1", IgaOps(req++, 0, ops));
				at += n;
				budget -= static_cast<double>(n);
			}
			for (const std::string& line : e.audit)
				if (line.find("too many requests") != std::string::npos) return true;
			return false;
		};
		std::vector<std::string> got;
		CHECK(!run(82, true, got) && got.size() == 82);
		got.clear();
		CHECK(!run(256, true, got) && got.size() == 256);
		got.clear();
		CHECK(run(256, false, got) && got.size() < 256);     // back to back: dropped past the burst
	}

	// Unmatched-rule warnings stop at the cap with a count of the rest.
	{
		std::vector<std::string> keys;
		for (int i = 0; i < 5; ++i) keys.push_back("P.f" + std::to_string(i));
		const std::string few = KnownFeatures::UnmatchedWarnings(keys, 10);
		CHECK(few.find("P.f4") != std::string::npos && few.find("more") == std::string::npos);
		const std::string capped = KnownFeatures::UnmatchedWarnings(keys, 3);
		CHECK(capped.find("P.f2") != std::string::npos && capped.find("P.f3") == std::string::npos);
		CHECK(capped.find("and 2 more") != std::string::npos);
		CHECK(KnownFeatures::UnmatchedWarnings({}, 3).empty());
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

// A [Players] flag that gives something or loosens a rule counts only on a connection that proved
// the pairing secret as that ID. ExemptVersion stays on the ID.
static void ProofGateTests()
{
	using AdminWire::Tag;
	const std::string path = (std::filesystem::temp_directory_path() / "su-iga-gate-test.txt").string();
	std::filesystem::remove(path);

	RulesModel::Model model;
	model.defaultDeny = false;
	PlayerList::Entry flagged;
	flagged.id = "p1"; flagged.exemptRules = true; flagged.sections = { "Codex.map" };

	// What ApplyRules does for one connection: the flags its entry gets, for that proof state.
	const auto flagsFor = [&](InGameAdminModel::Server& s, uint64_t conn, const std::string& id)
	{
		return RulesModel::FlagsFor(flagged.admin, flagged.exemptRules, flagged.sections, s.IsProvenAs(conn, id));
	};

	// A spoofed ID with no proof gets the default rules; the same ID with proof gets the flags.
	{
		IgaEnv e; e.path = path; e.rulesFlagged = { "p1" };
		auto s = MakeIga(e);
		CHECK(!RulesModel::NeedsOwnRules(flagsFor(*s, 1, "p1")));
		CHECK(RulesModel::PlayerRulesText(model, flagsFor(*s, 1, "p1"), true) == RulesModel::RulesText(model, true));

		// Hello alone proves nothing, and a wrong proof does not either.
		CHECK(IgaChallenge(*s, 1, "p1") != "");
		CHECK(!RulesModel::NeedsOwnRules(flagsFor(*s, 1, "p1")));
		AdminWire::Message bad; bad.tag = Tag::Prove; bad.proof = std::string(64, '0');
		IgaSend(*s, 1, "p1", bad);
		CHECK(!RulesModel::NeedsOwnRules(flagsFor(*s, 1, "p1")));
		CHECK(e.proofChanges == 0);

		std::string code, err;
		CHECK(s->IssueCode("p1", code, err));
		CHECK(IgaChallenge(*s, 2, "p1") != "");
		CHECK(IgaRole(IgaSend(*s, 2, "p1", IgaPair(code, kSecretA))) == "paired");
		CHECK(s->IsProvenAs(2, "p1"));
		CHECK(RulesModel::NeedsOwnRules(flagsFor(*s, 2, "p1")));
		CHECK(RulesModel::PlayerRulesText(model, flagsFor(*s, 2, "p1"), true) != RulesModel::RulesText(model, true));
		CHECK(e.proofChanges == 1);   // the rules are sent again when the proof lands

		// The other connection with the same ID stays on the default rules.
		CHECK(!RulesModel::NeedsOwnRules(flagsFor(*s, 1, "p1")));

		// The server reads another ID on the next packet: the proof and the flags go, rules are re-sent.
		IgaSend(*s, 2, "p2", IgaHello());
		CHECK(!s->IsProvenAs(2, "p1") && !s->IsProvenAs(2, "p2") && !s->IsProven(2));
		CHECK(!RulesModel::NeedsOwnRules(flagsFor(*s, 2, "p1")));
		CHECK(e.proofChanges == 2);

		// The ID read right now must be the proven one, with no packet in between.
		const std::string sid = s->ServerId();
		AdminWire::Message prove; prove.tag = Tag::Prove;
		AdminCrypto::MakeProof(kSecretA, IgaChallenge(*s, 3, "p1"), sid, prove.proof);
		CHECK(IgaRole(IgaSend(*s, 3, "p1", prove)) == "paired");
		CHECK(s->IsProvenAs(3, "p1") && !s->IsProvenAs(3, "p9"));
		std::filesystem::remove(path);
	}

	// A flagged non-admin ID is challenged and pairs, but gets no admin tool.
	{
		IgaEnv e; e.path = path; e.rulesFlagged = { "r1" };
		int opsRun = 0;
		e.snapshot = [] { return IgaState(1, 1); };
		e.runOps = [&](const std::string&, const std::string&, const std::vector<AdminWire::Op>&, uint32_t)
		{ ++opsRun; InGameAdminModel::OpsOutcome o; o.ok = true; return o; };
		auto s = MakeIga(e);

		// An ID with no flag at all gets no challenge and cannot get a code.
		std::string code, err;
		CHECK(!s->IssueCode("nobody", code, err));
		CHECK(IgaRole(IgaSend(*s, 9, "nobody", IgaHello())) == "none");

		CHECK(s->IssueCode("r1", code, err));
		CHECK(IgaChallenge(*s, 1, "r1") != "");
		const auto pairReply = IgaDecode(IgaRaw(*s, 1, "r1", IgaPair(code, kSecretA)));
		CHECK(pairReply.size() == 1 && pairReply[0].tag == Tag::Welcome && pairReply[0].role == "paired");   // no snapshot
		CHECK(s->IsProven(1));
		CHECK(IgaRaw(*s, 1, "r1", IgaOps(1, 7, { { "policy.default", { "allow" } } })).empty());
		CHECK(opsRun == 0);

		// It proves with the secret later, still without an admin tool.
		const std::string sid = s->ServerId();
		AdminWire::Message prove; prove.tag = Tag::Prove;
		AdminCrypto::MakeProof(kSecretA, IgaChallenge(*s, 2, "r1"), sid, prove.proof);
		CHECK(IgaRole(IgaSend(*s, 2, "r1", prove)) == "paired");
		CHECK(s->IsProvenAs(2, "r1"));

		// The flag removed: the proof goes with it on the next change.
		e.rulesFlagged.clear();
		s->OnPlayersChanged();
		CHECK(!s->IsProven(1) && !s->IsProven(2));
		CHECK(!s->HasSecret("r1"));
		std::filesystem::remove(path);
	}

	// A kick by ID needs exactly one connected player with that ID.
	{
		std::string err;
		size_t pos = 99;
		CHECK(AdminOps::UniqueConnected({ "a", "b", "c" }, "b", pos, err) && pos == 1);
		CHECK(!AdminOps::UniqueConnected({ "a", "b", "b" }, "b", pos, err) && err == AdminOps::kAmbiguousIdText);
		CHECK(!AdminOps::UniqueConnected({ "a", "b" }, "z", pos, err) && err == "player not found");
		CHECK(!AdminOps::UniqueConnected({}, "z", pos, err));
	}

	// ExemptVersion works on the ID alone; Admin exempts from neither the version check nor the rules.
	// The listen host and solo have no list entry or no connection to prove, so the gate changes nothing for them.
	{
		PlayerList::Entry v; v.id = "v1"; v.exemptVersion = true;
		CHECK(v.exemptVersion && !v.NeedsProof() && !v.exemptRules);
		PlayerList::Entry a; a.id = "a1"; a.admin = true;
		CHECK(!a.exemptVersion && !a.exemptRules && a.NeedsProof());   // an admin is not exempt by default
		CHECK(!RulesModel::NeedsOwnRules(RulesModel::FlagsFor(false, false, {}, true)));
		CHECK(!RulesModel::NeedsOwnRules(RulesModel::FlagsFor(false, true, { "x.y" }, false)));
		CHECK(!RulesModel::NeedsOwnRules(RulesModel::FlagsFor(true, true, { "x.y" }, false)));   // an admin claim without proof is nobody
		CHECK(RulesModel::NeedsOwnRules(RulesModel::FlagsFor(false, false, { "x.y" }, true)));
		CHECK(RulesModel::NeedsOwnRules(RulesModel::FlagsFor(true, false, {}, true)));
		CHECK(RulesModel::NeedsOwnRules(RulesModel::FlagsFor(false, true, {}, true)));
	}
}

// The join hold, through RulesModel::JoinHolds (the unit the host calls): a listed admin with a secret
// on file gets the admin's set from the join until the admin check passes (same text, so nothing is
// sent again), is refused, or runs out kProofHoldMs after the client's feature list (kProofHoldCapMs
// after the join when no list arrives).
static void ProofHoldTests()
{
	using namespace RulesModel;
	std::vector<std::string> dw;
	const std::string path = (std::filesystem::temp_directory_path() / "su-iga-hold-test.txt").string();
	std::filesystem::remove(path);

	// One connection as the host sees it when it applies rules.
	struct Conn { bool listed = false, admin = false, secret = false, proven = false, tool = false; };
	// What ApplyRules sends one connection: the hold decides first, then the server rules or its own set.
	const auto sent = [](const Model& m, JoinHolds<int>& holds, int key, unsigned long long now, const Conn& c)
	{
		PlayerFlags flags;
		if (c.listed) flags = FlagsFor(c.admin, false, {}, c.proven);
		HoldInputs in;
		in.listedAdmin = c.listed && c.admin; in.secretOnFile = c.secret; in.proven = c.proven; in.clientHasTool = c.tool;
		if (holds.Step(key, now, in) == HoldStep::Held) flags = HoldFlags();
		return NeedsOwnRules(flags) ? PlayerRulesText(m, flags, true) : RulesText(m, true);
	};
	const auto denied = [](const std::string& t) { return t.find("BetterCheats.stack-size=deny") != std::string::npos; };
	const auto admin = [](bool secret, bool proven, bool tool)
	{
		Conn c; c.listed = c.admin = true; c.secret = secret; c.proven = proven; c.tool = tool;
		return c;
	};

	const Model none = ParseText({ "[Policy]", "Default=allow" }, dw);
	const std::string denies = RulesText(none, true);
	const std::string adminText = PlayerRulesText(none, FlagsFor(true, false, {}, true), true);
	CHECK(denied(denies) && !denied(adminText) && adminText == "default=allow\n");
	CHECK(NeedsOwnRules(HoldFlags()) && PlayerRulesText(none, HoldFlags(), true) == adminText);
	CHECK(kProofHoldMs == 8000 && kProofHoldCapMs == 30000);

	const unsigned long long t0 = 1000000;

	// Join: the hold starts and the admin's set is sent at once, before any feature list.
	{
		JoinHolds<int> h;
		CHECK(!h.Has(1) && h.Size() == 0);
		CHECK(sent(none, h, 1, t0, admin(true, false, false)) == denies);   // never began: the server rules
		h.Begin(1, t0);
		CHECK(h.Has(1) && h.Size() == 1);
		CHECK(sent(none, h, 1, t0, admin(true, false, false)) == adminText);
		CHECK(h.Has(1));
		// A second Begin for the same connection starts over.
		h.Begin(1, t0 + 5000);
		CHECK(h.Size() == 1);
		CHECK(sent(none, h, 1, t0 + 5000 + kProofHoldCapMs - 1, admin(true, false, false)) == adminText);
	}

	// Which set applies while held: the admin's set, at every step until the hold ends. The time is
	// counted from the feature list, so a slow load (list at 4.3 s) is still held 4.3 s + 8 s after join.
	{
		JoinHolds<int> h;
		h.Begin(1, t0);
		CHECK(sent(none, h, 1, t0 + 100, admin(true, false, false)) == adminText);
		CHECK(sent(none, h, 1, t0 + 4300, admin(true, false, false)) == adminText);
		h.FeaturesArrived(1, t0 + 4300);
		CHECK(sent(none, h, 1, t0 + 4300, admin(true, false, true)) == adminText);
		CHECK(sent(none, h, 1, t0 + 4300 + kProofHoldMs - 1, admin(true, false, true)) == adminText);   // past 8 s from the join
		CHECK(h.Has(1));
		// Ended by the timeout counted from the list.
		CHECK(denied(sent(none, h, 1, t0 + 4300 + kProofHoldMs, admin(true, false, true))));
		CHECK(!h.Has(1));
		CHECK(denied(sent(none, h, 1, t0 + 4300 + kProofHoldMs + 1, admin(true, false, true))));   // not held again
	}
	// Only the first feature list counts: a later one does not extend the hold.
	{
		JoinHolds<int> h;
		h.Begin(1, t0);
		h.FeaturesArrived(1, t0 + 1000);
		h.FeaturesArrived(1, t0 + 6000);
		CHECK(denied(sent(none, h, 1, t0 + 1000 + kProofHoldMs, admin(true, false, true))));
	}

	// The cap: no feature list ever arrives, the hold ends 30 s after the join.
	{
		JoinHolds<int> h;
		h.Begin(1, t0);
		CHECK(sent(none, h, 1, t0 + kProofHoldMs + 1, admin(true, false, false)) == adminText);   // 8 s with no list: still held
		CHECK(sent(none, h, 1, t0 + kProofHoldCapMs - 1, admin(true, false, false)) == adminText);
		CHECK(denied(sent(none, h, 1, t0 + kProofHoldCapMs, admin(true, false, false))));
		CHECK(!h.Has(1));
	}
	// The cap also bounds a list that arrives late: held to 30 s from the join, not 8 s after a late list.
	{
		JoinHolds<int> h;
		h.Begin(1, t0);
		h.FeaturesArrived(1, t0 + 29000);
		CHECK(sent(none, h, 1, t0 + 29999, admin(true, false, true)) == adminText);
		CHECK(denied(sent(none, h, 1, t0 + kProofHoldCapMs, admin(true, false, true))));
	}

	// The check passed: the hold ends and the proven text is the held text, so the loader is sent nothing new.
	{
		JoinHolds<int> h;
		h.Begin(1, t0);
		h.FeaturesArrived(1, t0 + 2000);
		CHECK(sent(none, h, 1, t0 + 2200, admin(true, false, true)) == adminText);
		CHECK(sent(none, h, 1, t0 + 2260, admin(true, true, true)) == adminText);   // the pass
		CHECK(!h.Has(1));
		CHECK(sent(none, h, 1, t0 + 60000, admin(true, true, true)) == adminText);  // proven long after: the admin's rules
	}

	// The same property under the other rule shapes: the held text is the proven text.
	{
		const Model hostDeny = ParseText({ "[Rules]", "BetterCheats.*=deny" }, dw);
		const Model adminsRow = ParseText({ "[Rules]", "BetterCheats.stack-size=admins", "P.f=deny" }, dw);
		const Model closed = ParseText({ "[Policy]", "Default=deny" }, dw);
		for (const Model* m : { &hostDeny, &adminsRow, &closed })
		{
			JoinHolds<int> h, h2;
			h.Begin(1, t0); h2.Begin(1, t0);
			CHECK(sent(*m, h, 1, t0 + 100, admin(true, false, true)) == sent(*m, h2, 1, t0 + 100, admin(true, true, true)));
		}
		CHECK(PlayerRulesText(closed, FlagsFor(true, false, {}, true), true) == "default=deny\n");   // T3
		CHECK(!DefaultRowAdminsOnly(closed, "BetterCheats") && DefaultRowAdminsOnly(none, "BetterCheats"));
		CHECK(!DefaultRowAdminsOnly(hostDeny, "bettercheats") && DefaultRowAdminsOnly(hostDeny, "Best Builder"));
		CHECK(DefaultRowAdminsOnly(adminsRow, "BetterCheats"));
	}

	// The first join during `admin pair`: no secret yet, so no hold and the denies; once paired and proven, the allows.
	{
		IgaEnv e; e.path = path; e.admins = { "p1" };
		auto s = MakeIga(e);
		JoinHolds<int> h;
		h.Begin(1, t0);
		CHECK(!s->HasSecret("p1") && denied(sent(none, h, 1, t0, admin(s->HasSecret("p1"), false, true))));
		CHECK(!h.Has(1));                                      // nothing is pending, so no hold
		std::string code, err;
		CHECK(s->IssueCode("p1", code, err));
		CHECK(IgaChallenge(*s, 1, "p1") != "");
		CHECK(IgaRole(IgaSend(*s, 1, "p1", IgaPair(code, kSecretA))) == "admin");
		CHECK(s->HasSecret("p1") && s->IsProvenAs(1, "p1"));
		CHECK(sent(none, h, 1, t0 + 5000, admin(true, s->IsProvenAs(1, "p1"), true)) == adminText);

		// A rejoin now: a secret is on file, so the hold applies until the check passes.
		h.Begin(2, t0 + 9000);
		CHECK(!s->IsProvenAs(2, "p1") && sent(none, h, 2, t0 + 9000, admin(s->HasSecret("p1"), false, false)) == adminText);

		// A failed check: the wrong secret never proves. The server refuses the proof, which tells the host,
		// and the host ends the hold at once (EndAdminHold calls End): the denies, long before the limit.
		CHECK(IgaChallenge(*s, 2, "p1") != "");
		CHECK(e.rejected.empty());
		AdminWire::Message bad; bad.tag = AdminWire::Tag::Prove; bad.proof = std::string(64, '0');
		IgaSend(*s, 2, "p1", bad);
		CHECK(!s->IsProvenAs(2, "p1"));
		CHECK(e.rejected.size() == 1 && e.rejected[0] == 2);
		CHECK(h.End(2) && !h.Has(2));
		CHECK(!h.End(2));                                      // already over
		CHECK(denied(sent(none, h, 2, t0 + 9040, admin(true, false, true))));
		// The same refusal for a proof with no challenge behind it, and one that is empty.
		IgaSend(*s, 2, "p1", bad);
		CHECK(e.rejected.size() == 2);
		AdminWire::Message empty; empty.tag = AdminWire::Tag::Prove;
		CHECK(IgaChallenge(*s, 2, "p1") != "");
		IgaSend(*s, 2, "p1", empty);
		CHECK(e.rejected.size() == 3 && e.rejected[2] == 2);
		// A right proof is not refused.
		AdminWire::Message good; good.tag = AdminWire::Tag::Prove;
		AdminCrypto::MakeProof(kSecretA, IgaChallenge(*s, 3, "p1"), s->ServerId(), good.proof);
		CHECK(IgaRole(IgaSend(*s, 3, "p1", good)) == "admin");
		CHECK(e.rejected.size() == 3);
		// Without the early end the hold still runs out and then ends on the denies.
		h.Begin(4, t0 + 20000);
		h.FeaturesArrived(4, t0 + 20500);
		CHECK(sent(none, h, 4, t0 + 20500 + kProofHoldMs - 1, admin(true, false, true)) == adminText);
		CHECK(denied(sent(none, h, 4, t0 + 20500 + kProofHoldMs, admin(true, false, true))));
		std::filesystem::remove(path);
	}

	// A refused check ends the hold even when the hold would otherwise still have time.
	{
		JoinHolds<int> h;
		h.Begin(1, t0);
		h.FeaturesArrived(1, t0 + 500);
		CHECK(h.End(1));
		CHECK(denied(sent(none, h, 1, t0 + 600, admin(true, false, true))));
		CHECK(!h.End(1));
		CHECK(!h.End(99));                                     // a connection that never held
	}

	// A client without the ServerUtility client part can never prove: once its list is in, the hold ends at once.
	// Before the list the part is not known yet, so the hold runs.
	{
		JoinHolds<int> h;
		h.Begin(1, t0);
		CHECK(sent(none, h, 1, t0 + 100, admin(true, false, false)) == adminText);
		h.FeaturesArrived(1, t0 + 1000);
		CHECK(denied(sent(none, h, 1, t0 + 1000, admin(true, false, false))));
		CHECK(!h.Has(1));
	}

	// A hold with no secret on file ends the first time it is looked at: nothing for the client to answer.
	{
		JoinHolds<int> h;
		h.Begin(1, t0);
		CHECK(denied(sent(none, h, 1, t0, admin(false, false, true))));
		CHECK(!h.Has(1));
	}

	// `admin remove` before the feature list arrives: the very next apply (the loader's first send) is the denies,
	// and so is the one after. After the list the same.
	{
		JoinHolds<int> h;
		h.Begin(1, t0);
		CHECK(sent(none, h, 1, t0 + 500, admin(true, false, false)) == adminText);
		Conn removed; removed.secret = true; removed.tool = false;       // unlisted: no Admin flag any more
		CHECK(denied(sent(none, h, 1, t0 + 1500, removed)));
		CHECK(!h.Has(1));
		CHECK(denied(sent(none, h, 1, t0 + 1600, removed)));
		JoinHolds<int> g;
		g.Begin(1, t0);
		g.FeaturesArrived(1, t0 + 100);
		Conn listedNotAdmin; listedNotAdmin.listed = true; listedNotAdmin.secret = true; listedNotAdmin.tool = true;   // Admin flag taken off
		CHECK(denied(sent(none, g, 1, t0 + 2000, listedNotAdmin)));
		CHECK(!g.Has(1));
	}

	// A rules reload during the hold: the held text follows the new model; after it, the proven text does.
	{
		const Model reloaded = ParseText({ "[Rules]", "BetterCheats.*=deny" }, dw);
		JoinHolds<int> h;
		h.Begin(1, t0);
		h.FeaturesArrived(1, t0 + 100);
		CHECK(sent(none, h, 1, t0 + 3000, admin(true, false, true)) == "default=allow\n");
		CHECK(sent(reloaded, h, 1, t0 + 3100, admin(true, false, true)) == "default=allow\nBetterCheats.*=deny\n");
		CHECK(sent(reloaded, h, 1, t0 + 20000, admin(true, true, true)) == "default=allow\nBetterCheats.*=deny\n");
		// A reload does not end the hold, and the timeout still does: no pass 8 s after the list, so the player is on the server rules.
		JoinHolds<int> g;
		g.Begin(1, t0);
		g.FeaturesArrived(1, t0 + 100);
		CHECK(sent(reloaded, g, 1, t0 + 3100, admin(true, false, true)) == "default=allow\nBetterCheats.*=deny\n");
		CHECK(sent(reloaded, g, 1, t0 + 100 + kProofHoldMs + 1, admin(true, false, true)) == RulesText(reloaded, true));
	}

	// Expired: the connections whose time is up, for the host to look at. Not before, not twice a hold.
	{
		JoinHolds<int> h;
		h.Begin(1, t0);                                         // no list ever
		h.Begin(2, t0);
		h.FeaturesArrived(2, t0 + 3000);
		h.Begin(3, t0 + 10000);
		CHECK(h.Expired(t0 + 3000 + kProofHoldMs - 1).empty());
		const std::vector<int> a = h.Expired(t0 + 3000 + kProofHoldMs);
		CHECK(a.size() == 1 && a[0] == 2);
		const std::vector<int> b = h.Expired(t0 + kProofHoldCapMs);
		CHECK(b.size() == 2 && b[0] == 1 && b[1] == 2);
		const std::vector<int> c = h.Expired(t0 + 10000 + kProofHoldCapMs);
		CHECK(c.size() == 3);
		// What the host does with an expired connection: apply the rules, which ends it.
		CHECK(denied(sent(none, h, 2, t0 + 3000 + kProofHoldMs, admin(true, false, true))));
		CHECK(h.Has(1) && !h.Has(2) && h.Has(3) && h.Size() == 2);
	}

	// Connections are independent: the same player on two connections, one leaves, the other keeps its hold.
	{
		JoinHolds<int> h;
		h.Begin(1, t0); h.Begin(2, t0 + 1000);
		CHECK(h.End(1));
		CHECK(h.Has(2) && sent(none, h, 2, t0 + 2000, admin(true, false, false)) == adminText);
		CHECK(denied(sent(none, h, 1, t0 + 2000, admin(true, false, false))));
	}

	// A non-admin: never held, the denies at once, at every moment and whatever else is true.
	{
		for (unsigned long long age : { 0ull, 1000ull, kProofHoldMs - 1, kProofHoldMs, 60000ull })
			for (int secret = 0; secret < 2; ++secret)
				for (int tool = 0; tool < 2; ++tool)
				{
					Conn unlisted; unlisted.secret = secret != 0; unlisted.tool = tool != 0;
					Conn plain = unlisted; plain.listed = true;
					JoinHolds<int> h;
					h.Begin(1, t0); h.Begin(2, t0);
					CHECK(denied(sent(none, h, 1, t0 + age, unlisted)));
					CHECK(denied(sent(none, h, 2, t0 + age, plain)));
				}
	}
}

// Announcing catalog changes to the admins: the first goes out at once, changes within the interval
// go out as one, and the tick sends it.
static void PushGateTests()
{
	using namespace KnownFeatures;
	const uint64_t t0 = 500000;
	CHECK(PushGate::kIntervalMs == 1000);

	// The first push goes at once; nothing is held.
	{
		PushGate g;
		CHECK(!g.Held() && !g.Due(t0));
		CHECK(g.Changed(t0) && !g.Held());
	}
	// A burst inside 1 s: one trailing push, sent by Tick (Due) once the interval is over, and only once.
	{
		PushGate g;
		CHECK(g.Changed(t0));
		for (int i = 1; i <= 23; ++i)
			CHECK(!g.Changed(t0 + 500u * i / 23));              // 24 messages in 12 s would be one per 500 ms; here all within 500 ms
		CHECK(g.Held());
		CHECK(!g.Due(t0 + 500) && !g.Due(t0 + 999));
		CHECK(g.Due(t0 + 1000));                               // the trailing push
		CHECK(!g.Held() && !g.Due(t0 + 1000) && !g.Due(t0 + 5000));
	}
	// A change after the trailing push starts a new interval from it.
	{
		PushGate g;
		CHECK(g.Changed(t0));
		CHECK(!g.Changed(t0 + 100));
		CHECK(g.Due(t0 + 1000));
		CHECK(!g.Changed(t0 + 1500));                          // inside 1 s of the trailing push
		CHECK(!g.Due(t0 + 1999) && g.Due(t0 + 2000));
	}
	// A change with the interval over goes out at once and clears anything held.
	{
		PushGate g;
		CHECK(g.Changed(t0));
		CHECK(!g.Changed(t0 + 200) && g.Held());
		CHECK(g.Changed(t0 + 1200) && !g.Held());
		CHECK(!g.Due(t0 + 5000));
	}
	// A paced batch of 24 messages over 12 s (one per 500 ms): at most one push per second.
	{
		PushGate g;
		int pushes = 0;
		uint64_t last = 0;
		for (int i = 0; i < 24; ++i)
		{
			const uint64_t now = t0 + 500u * i;
			if (g.Due(now)) { ++pushes; CHECK(last == 0 || now - last >= 1000); last = now; }
			if (g.Changed(now)) { ++pushes; CHECK(last == 0 || now - last >= 1000); last = now; }
		}
		for (uint64_t now = t0 + 12000; now < t0 + 14000; now += 50)
			if (g.Due(now)) { ++pushes; CHECK(now - last >= 1000); last = now; }
		CHECK(pushes >= 12 && pushes <= 13 && !g.Held());
	}
}

int main()
{
	std::vector<std::string> w;

	// A key counts only when a ':' follows it, not when its text sits inside a value.
	{
		const std::string b1 = "{\"message\":\"default\"}";
		CHECK(!AdminJson::Has(b1.data(), b1.size(), "default"));
		CHECK(AdminJson::Has(b1.data(), b1.size(), "message"));
		CHECK(AdminJson::ExtractString(b1.data(), b1.size(), "message") == "default");
		const std::string b2 = "{\"plugin\":\"version\",\"version\":\"2.0\"}";
		CHECK(AdminJson::ExtractString(b2.data(), b2.size(), "version") == "2.0");
		CHECK(AdminJson::ExtractString(b2.data(), b2.size(), "plugin") == "version");
		const std::string b3 = "{\"plugin\":\"version\"}";
		CHECK(!AdminJson::Has(b3.data(), b3.size(), "version"));
		CHECK(AdminJson::ExtractString(b3.data(), b3.size(), "version").empty());
		const std::string b4 = "{ \"n\" : 5 }";
		CHECK(AdminJson::ExtractInt(b4.data(), b4.size(), "n") == 5);
	}

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
	const std::string text = RulesText(m, false);
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
	CHECK(RulesText(m, false) == "default=allow\n" && VersionMinimumsText(m).empty() && w.empty());

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
	// The loader takes 2048 records: the header and every default row count against them.
	const size_t ruleLimit = 2048 - 1 - DefaultRows().size();
	for (size_t i = 0; i < ruleLimit; ++i) CHECK(SetRule(m, "M.f" + std::to_string(i), true, e));
	CHECK(!SetRule(m, "M.over", true, e));
	CHECK(RulesText(m, true).find("M.over") == std::string::npos);
	{
		size_t lines = 0;   // the header line counts as one
		for (char c : RulesText(m, true)) if (c == '\n') ++lines;
		CHECK(lines <= 2048);
	}

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
	CHECK(w.empty() && RulesText(back, false) == RulesText(m, false) && VersionMinimumsText(back) == VersionMinimumsText(m));

	// The rules one player is sent.
	w.clear();
	m = ParseText({ "[Policy]", "Default=deny", "Message=Off here", "[Rules]", "BetterCheats.*=allow", "BetterCheats.movement=deny",
	                "BetterDrone.drone-limits=deny", "Other.x=deny" }, w);
	PlayerFlags none;
	CHECK(!NeedsOwnRules(none) && PlayerRulesText(m, none, false) == RulesText(m, false));
	PlayerFlags exempt; exempt.exemptRules = true;
	CHECK(NeedsOwnRules(exempt) && PlayerRulesText(m, exempt, false) == "default=allow\nmessage=Off here\n");
	exempt.sections = { "BetterDrone.drone-limits" };   // exempt wins over sections
	CHECK(PlayerRulesText(m, exempt, false) == "default=allow\nmessage=Off here\n");
	PlayerFlags sec; sec.sections = { "betterdrone.drone-limits" };
	CHECK(NeedsOwnRules(sec));
	CHECK(PlayerRulesText(m, sec, false) == "default=deny\nmessage=Off here\nBetterCheats.*=allow\nBetterCheats.movement=deny\nBetterDrone.drone-limits=allow\nOther.x=deny\n");
	sec.sections = { "BetterCheats.*", "Other.*" };   // a whole-mod key opens its specific rows
	CHECK(PlayerRulesText(m, sec, false) == "default=deny\nmessage=Off here\nBetterDrone.drone-limits=deny\nBetterCheats.*=allow\nOther.*=allow\n");
	sec.sections = { "New.thing", "bad key", "nodot" };   // a key with no rule is added; unusable keys are skipped
	CHECK(PlayerRulesText(m, sec, false).find("New.thing=allow\n") != std::string::npos && PlayerRulesText(m, sec, false).find("bad") == std::string::npos);
	Model open = m; open.defaultDeny = false;
	sec.sections = { "Fresh.thing" };
	CHECK(PlayerRulesText(open, sec, false).find("Fresh.thing=allow\n") != std::string::npos);   // still stands alone with default allow

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
			// admins only: set, replaced by allow, and unset
			CHECK(Apply(f.be, rcon, { op("rule.set", { "Codex.map", "admins" }) }).ok && f.model.rules.size() == 1 && f.model.rules[0].adminsOnly && f.model.rules[0].deny);
			CHECK(Apply(f.be, rcon, { op("rule.set", { "Codex.map", "allow" }) }).ok && !f.model.rules[0].adminsOnly && !f.model.rules[0].deny);
			CHECK(Apply(f.be, rcon, { op("rule.set", { "Codex.map", "admins" }) }).ok && Apply(f.be, rcon, { op("rule.set", { "Codex.map", "unset" }) }).ok && f.model.rules.empty());
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

		// No silent downgrade: an rc.2 client sends only the ops it staged (rule.set for the edited key),
		// so editing one unrelated rule leaves every admins-only row as it was. Only an op naming the same key
		// (the rc.2 window shows "admins only" as Deny, so choosing Deny there) changes that row.
		{
			Fake f;
			std::string err;
			CHECK(SetRuleAdminsOnly(f.model, "BetterCheats.god", err) && SetRuleAdminsOnly(f.model, "BetterCheats.*", err));
			CHECK(Apply(f.be, rcon, { op("rule.set", { "Other.thing", "deny" }) }).ok);
			CHECK(Apply(f.be, rcon, { op("policy.message", { "Hello" }) }).ok && Apply(f.be, rcon, { op("policy.default", { "deny" }) }).ok);
			CHECK(f.model.rules.size() == 3 && f.model.rules[0].adminsOnly && f.model.rules[1].adminsOnly && !f.model.rules[2].adminsOnly);
			CHECK(RuleLines(f.model)[0] == "BetterCheats.god=admins" && RuleLines(f.model)[1] == "BetterCheats.*=admins");
			CHECK(Apply(f.be, rcon, { op("rule.set", { "BetterCheats.god", "deny" }) }).ok && !f.model.rules[0].adminsOnly && f.model.rules[1].adminsOnly);
		}

		// server.stacksize: who may change it, which values, one batch, revision, audit.
		{
			Fake f;
			int stack = 0, sets = 0;
			std::string lastSource;
			f.be.getStackSize = [&] { return stack; };
			f.be.setStackSize = [&](int n, const std::string& src, std::string&) { stack = n; ++sets; lastSource = src; return true; };
			const Caller panel = Caller::FromPanel();

			// Admin only: a listed player who is not an admin, an unreadable ID and an unlisted ID are refused.
			CHECK(!Apply(f.be, Caller::FromPlayer("ply1", "Pat"), { op("server.stacksize", { "500" }) }).ok);
			CHECK(!Apply(f.be, Caller::FromPlayer("", "Pat"), { op("server.stacksize", { "500" }) }).ok);
			CHECK(!Apply(f.be, Caller::FromPlayer("nobody", "Pat"), { op("server.stacksize", { "500" }) }).ok);
			CHECK(sets == 0 && stack == 0);
			CHECK(Apply(f.be, adm, { op("server.stacksize", { "500" }) }).ok && stack == 500 && lastSource == "admin window");
			CHECK(Apply(f.be, rcon, { op("server.stacksize", { "600" }) }).ok && stack == 600 && lastSource == "rcon");
			CHECK(Apply(f.be, panel, { op("server.stacksize", { "700" }) }).ok && stack == 700 && lastSource == "panel");
			CHECK(sets == 3);

			// Values.
			CHECK(Apply(f.be, rcon, { op("server.stacksize", { "0" }) }).ok && stack == 0);
			CHECK(Apply(f.be, rcon, { op("server.stacksize", { "1" }) }).ok && stack == 1);
			CHECK(Apply(f.be, rcon, { op("server.stacksize", { "9999" }) }).ok && stack == 9999);
			CHECK(Apply(f.be, rcon, { op("server.stacksize", { "off" }) }).ok && stack == 0);
			CHECK(Apply(f.be, rcon, { op("server.stacksize", { "OFF" }) }).ok && stack == 0);
			stack = 42;
			sets = 0;
			CHECK(!Apply(f.be, rcon, { op("server.stacksize", { "-1" }) }).ok);
			CHECK(!Apply(f.be, rcon, { op("server.stacksize", { "10000" }) }).ok);
			CHECK(!Apply(f.be, rcon, { op("server.stacksize", { "abc" }) }).ok);
			CHECK(!Apply(f.be, rcon, { op("server.stacksize", { "" }) }).ok);
			CHECK(!Apply(f.be, rcon, { op("server.stacksize", { "5 0" }) }).ok);
			CHECK(!Apply(f.be, rcon, { op("server.stacksize") }).ok);
			CHECK(sets == 0 && stack == 42);

			// A batch with one bad op applies nothing.
			const size_t commits = static_cast<size_t>(f.commits);
			CHECK(!Apply(f.be, rcon, { op("server.stacksize", { "300" }), op("rule.set", { "Codex.map", "maybe" }) }).ok);
			CHECK(!Apply(f.be, rcon, { op("rule.set", { "Codex.map", "deny" }), op("server.stacksize", { "x" }) }).ok);
			CHECK(sets == 0 && stack == 42 && static_cast<size_t>(f.commits) == commits && f.model.rules.empty());
			// A good batch applies both.
			CHECK(Apply(f.be, rcon, { op("server.stacksize", { "300" }), op("rule.set", { "Codex.map", "deny" }) }).ok);
			CHECK(stack == 300 && f.model.rules.size() == 1);

			// The revision goes up once, with the server section bit.
			int calls = 0; unsigned secs = 0;
			const int h = AddListener([&](uint64_t, unsigned s) { ++calls; secs = s; });
			const uint64_t before = Revision();
			f.audit.clear();
			CHECK(Apply(f.be, rcon, { op("server.stacksize", { "250" }) }).ok);
			CHECK(calls == 1 && secs == kSectionServer && Revision() == before + 1);
			CHECK(f.audit.size() == 1 && f.audit[0] == "[Admin] (RCON) server.stacksize=250");
			CHECK(Apply(f.be, adm, { op("server.stacksize", { "off" }) }).ok);
			CHECK(f.audit.size() == 2 && f.audit[1] == "[Admin] 'Boss' (in game) server.stacksize=off");
			CHECK(!Apply(f.be, adm, { op("server.stacksize", { "99999" }) }).ok && calls == 2);
			RemoveListener(h);

			// A setter that fails reports its text and bumps nothing.
			f.be.setStackSize = [](int, const std::string&, std::string& e) { e = "no write"; return false; };
			const uint64_t rev2 = Revision();
			const Result bad = Apply(f.be, rcon, { op("server.stacksize", { "10" }) });
			CHECK(!bad.ok && bad.text == "no write" && Revision() == rev2);

			// No setter: not available on this server.
			f.be.setStackSize = nullptr;
			const Result none = Apply(f.be, rcon, { op("server.stacksize", { "10" }) });
			CHECK(!none.ok && none.text.find("not available") != std::string::npos && Revision() == rev2);

			// The section text and the wire op count.
			CHECK(kSectionServer == 16 && AdminWire::OpArgCount("server.stacksize") == 1);
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

		// A catalog change reaches listeners as the mods section and leaves the revision alone,
		// so an apply staged before it is not stale.
		{
			Fake f;
			const uint64_t cur = Revision();
			int calls = 0; unsigned secs = 0; uint64_t seen = 0;
			const int h = AddListener([&](uint64_t r, unsigned s) { ++calls; secs = s; seen = r; });
			NoteCatalogChanged();
			CHECK(calls == 1 && secs == kSectionMods && seen == cur && Revision() == cur);
			RemoveListener(h);
			const Result after = Apply(f.be, rcon, { op("policy.default", { "deny" }) }, cur);
			CHECK(after.ok && !after.stale);
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

		// Pushed rows: only a change a window shows counts; a newer lastSeen alone does not.
		{
			Catalog g;
			bool shown = false;
			Report r = rep("Best Builder", "fast", "2.1", "", false, "Fast build mode");
			CHECK(g.Merge(r, d1, &shown) && shown);                     // new row
			CHECK(!g.Merge(r, d1, &shown) && !shown);                   // same again
			CHECK(g.Merge(r, d2, &shown) && !shown);                    // lastSeen only: saved, not shown
			r.title = "Faster build mode";
			CHECK(!g.Merge(r, d2, &shown) && !shown);                   // same version: title kept
			r.version = "2.2";
			CHECK(g.Merge(r, d2, &shown) && shown && g.Entries()[0].title == "Faster build mode");   // version and title change
			// An off row pushed without a build is not recorded as off on "unknown".
			Report o = rep("Best Builder", "other", "1.0", "", true, "Other");
			o.recordOff = false;
			CHECK(g.Merge(o, d2, &shown) && shown);
			for (const auto& en : g.Entries()) if (en.id == "other") CHECK(en.offBuilds.empty());
		}

		// A long title is cut to 80 bytes on a UTF-8 boundary; a junk version is kept out, the row stays.
		{
			const std::string kb(4000, 'x');
			std::string wide;
			for (int i = 0; i < 100; ++i) wide += "\xE2\x82\xAC";   // 3-byte characters (euro sign): byte 80 falls mid-character
			const auto rows = ParseDeclaredRows({ "P	1.0	long	on	" + kb, "P	1.0	wide	on	" + wide, "P	99999	junk	on	T", "P	abc	junk2	on	T", "P		blank	on	T" });
			CHECK(rows.size() == 5);
			CHECK(rows.size() == 5 && rows[0].title.size() == 80 && rows[0].id == "long");
			CHECK(rows.size() == 5 && rows[1].title.size() <= 80 && rows[1].title.size() < 80 && rows[1].title.size() % 3 == 0
			      && rows[1].title.compare(rows[1].title.size() - 3, 3, "\xE2\x82\xAC") == 0);
			CHECK(rows.size() == 5 && rows[2].id == "junk" && rows[3].id == "junk2" && rows[4].id == "blank");
			Catalog g;
			Report big = rep("P", "big", "1.0", "", false, kb.c_str());
			CHECK(g.Merge(big, d1) && g.Entries()[0].title.size() == 80);
			// A version that is not one: the row is kept with no version, and the plugin is named once.
			CHECK(g.Merge(rep("P", "junk", "99999", "", false), d1) && g.Size() == 2);
			CHECK(g.Merge(rep("P", "junk2", "dev", "", false), d1) && g.Size() == 3);
			for (const auto& en : g.Entries()) if (en.id == "junk" || en.id == "junk2") CHECK(en.version.empty());
			const auto warned = g.TakeVersionWarnings();
			CHECK(warned.size() == 1 && warned[0] == "P");
			g.Merge(rep("p", "junk3", "dev", "", false), d1);
			CHECK(g.TakeVersionWarnings().empty());
			// Our real version strings are versions, except a plugin built with no tag, which reports "dev".
			CHECK(ValidVersion("0.19.0-dev.13"));
			CHECK(ValidVersion("1.26.1-dev.13"));
			CHECK(ValidVersion("1.0"));
			CHECK(!ValidVersion("dev"));
		}

		// Names are checked and rows are capped.
		{
			Catalog g;
			CHECK(!g.Merge(rep("Bad  Name", "x", "", "", false), d1));
			CHECK(!g.Merge(rep("Bad\"Name", "x", "", "", false), d1));
			CHECK(!g.Merge(rep("Plug", "has space", "", "", false), d1));
			CHECK(!g.Merge(rep("Plug", "$id", "", "", false), d1));
			CHECK(g.Merge(rep("Best Builder", "fast", "", "", false), d1));
			for (size_t i = 1; g.Size() < kMaxRows; ++i)
				g.Merge(rep("Plug", ("f" + std::to_string(i)).c_str(), "", "", false), d1);
			CHECK(g.Size() == kMaxRows);
			CHECK(!g.Merge(rep("Plug", "f1", "", "", false), d1));   // an old row still merges, unchanged
			CHECK(g.Merge(rep("Plug", "f1", "", "", false), d2));    // a new day still counts
			// Full: a new feature replaces the reported row seen longest ago.
			CHECK(g.Merge(rep("Plug", "one-too-many", "", "", false), d2));
			CHECK(g.Size() == kMaxRows);
			bool hasNew = false, hasF1 = false, hasBest = false, hasF2 = false;
			for (const auto& en : g.Entries())
			{
				hasNew |= en.id == "one-too-many"; hasF1 |= en.id == "f1";
				hasBest |= en.id == "fast"; hasF2 |= en.id == "f2";
			}
			CHECK(hasNew && hasF1);                                  // f1 was seen d2, so it stays
			CHECK(!(hasBest && hasF2));                              // an older d1 row went (the first in key order)
		}

		// A full catalog of declared rows refuses a new reported one; a declared row is never evicted.
		// Rows go through the production parse and merge, as the admin client's push does.
		{
			auto fillDeclared = [&](Catalog& cat, const std::string& day)
			{
				for (size_t i = 1; cat.Size() < kMaxRows;)
				{
					std::vector<std::string> rows;
					for (size_t n = 0; n < kMaxRowsPerReport && cat.Size() + rows.size() < kMaxRows; ++n, ++i)
						rows.push_back("Plug	1.0	d" + std::to_string(i) + "	on	T");
					for (const Report& r : ParseDeclaredRows(rows)) cat.Merge(r, day);
				}
			};
			Catalog g;
			fillDeclared(g, d1);
			for (const auto& en : g.Entries()) CHECK(en.source == kSourceDeclared);
			CHECK(g.Size() == kMaxRows && !g.Merge(rep("Plug", "late", "", "", false), d2) && g.Size() == kMaxRows);
			// One reported row among them is the one that goes.
			Catalog h;
			h.Merge(rep("Plug", "rep", "", "", false), d1);
			fillDeclared(h, d2);
			CHECK(h.Merge(rep("Plug", "late", "", "", false), d2) && h.Size() == kMaxRows);
			bool hasRep = false, hasLate = false;
			for (const auto& en : h.Entries()) { hasRep |= en.id == "rep"; hasLate |= en.id == "late"; }
			CHECK(!hasRep && hasLate);
			// A reported row a loader then declares is kept, not evicted.
			Catalog u;
			u.Merge(rep("Plug", "up", "", "", false), d1);
			CHECK(u.Entries()[0].source == kSourceReported);
			CHECK(u.Merge(ParseDeclaredRows({ "Plug	1.0	up	on	T" })[0], d1));
			CHECK(u.Entries()[0].source == kSourceDeclared);
			fillDeclared(u, d2);
			CHECK(u.Size() == kMaxRows && !u.Merge(rep("Plug", "late", "", "", false), d2));
			bool hasUp = false;
			for (const auto& en : u.Entries()) hasUp |= en.id == "up";
			CHECK(hasUp);
		}

		// Reply lines: control characters go, and a long line is cut on a character boundary.
		{
			CHECK(LineText::OneLine("a\nb\rc\td") == "a b c d");
			CHECK(LineText::OneLine("a\x7F" "b") == "a b");
			CHECK(LineText::OneLine("a\xC2\x85" "b") == "a b");          // U+0085
			CHECK(LineText::OneLine("a\xE2\x80\xA8" "b") == "a b");      // U+2028
			CHECK(LineText::OneLine("a\xE2\x80\xA9" "b") == "a b");      // U+2029
			CHECK(LineText::OneLine("\xC3\xA9\xE2\x82\xAC") == "\xC3\xA9\xE2\x82\xAC");   // other text is untouched
			CHECK(LineText::Clip("short", 120) == "short");
			const std::string longLine(500, 'x');
			CHECK(LineText::Clip(longLine, 120).size() == 120 && LineText::Clip(longLine, 120).substr(117) == "...");
			std::string wide;
			for (int i = 0; i < 100; ++i) wide += "\xE2\x82\xAC";   // 3-byte characters
			const std::string cut = LineText::Clip(wide, 120);
			CHECK(cut.size() <= 120 && (cut.size() - 3) % 3 == 0 && cut.compare(cut.size() - 3, 3, "...") == 0);
		}

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
		CHECK(CompareVersions("1.2.0-dev.3", "1.2.0") < 0 && CompareVersions("", "0.0.1") < 0);

		// Pre-releases: SemVer order on equal numbers (dev < rc < release), numbers by value.
		CHECK(CompareVersions("0.26.0-rc.6", "0.26.0-dev.1") > 0 && CompareVersions("0.26.0-dev.1", "0.26.0-rc.6") < 0);
		CHECK(CompareVersions("0.26.0", "0.26.0-rc.6") > 0 && CompareVersions("0.26.0-rc.6", "0.26.0") < 0);
		CHECK(CompareVersions("1.0.0-rc.10", "1.0.0-rc.2") > 0 && CompareVersions("1.0.0-rc.9", "1.0.0-rc.10") < 0);
		CHECK(CompareVersions("1.0.0-dev.9", "1.0.0-dev.10") < 0 && CompareVersions("1.0.0-rc.007", "1.0.0-rc.7") == 0);
		CHECK(CompareVersions("1.0.0-rc.1", "1.0.0-rc") > 0);                      // the longer list is higher
		CHECK(CompareVersions("1.0.0-1", "1.0.0-a") < 0);                           // a number is below text
		CHECK(CompareVersions("v0.11.0-dev.9", "0.11.0-rc.2") < 0);
		CHECK(CompareVersions("1.2.0+b1", "1.2.0") == 0 && CompareVersions("1.2.0-rc.1+b2", "1.2.0-rc.1") == 0);
		CHECK(CompareVersions("2.4.0-beta.4", "2.4.0") < 0 && CompareVersions("2.4.0-beta.4", "2.3.9") > 0);
		CHECK(CompareVersions("0.19.0-rc.4", "0.19.0-rc.4") == 0 && CompareVersions("1.0.0-", "1.0.0") == 0);
		Catalog v;
		v.Merge(rep("P", "f", "1.9.0", "", false, "Old title"), d1);
		CHECK(v.Merge(rep("P", "f", "1.10.0", "", false, "New title"), d1));
		CHECK(v.Entries()[0].version == "1.10.0" && v.Entries()[0].title == "New title");
		CHECK(!v.Merge(rep("P", "f", "1.9.5", "", false, "Older title"), d1));
		CHECK(v.Entries()[0].version == "1.10.0" && v.Entries()[0].title == "New title");

		// A declared row sets the running version, up or down; a reported row only raises it.
		{
			auto decl = [&](const char* plugin, const char* id, const char* version, const char* title = "")
			{
				Report r = rep(plugin, id, version, "", false, title);
				r.source = kSourceDeclared;
				return r;
			};
			Catalog dv;
			bool shown = false;
			CHECK(dv.Merge(decl("Cheats", "move", "0.26.0-dev.1", "Move"), d1, &shown) && shown);
			CHECK(dv.Merge(decl("Cheats", "move", "0.26.0-rc.6", "Move now"), d1, &shown) && shown);
			CHECK(dv.Entries()[0].version == "0.26.0-rc.6" && dv.Entries()[0].title == "Move now");
			CHECK(!dv.Merge(decl("Cheats", "move", "0.26.0-rc.6", "Move now"), d1, &shown) && !shown);   // same again, same day
			CHECK(dv.Merge(decl("Cheats", "move", "0.26.0", "Move now"), d1, &shown) && shown);         // a release replaces the rc
			CHECK(dv.Entries()[0].version == "0.26.0");
			CHECK(dv.Merge(decl("Cheats", "move", "0.26.0-rc.6", "Move now"), d1, &shown) && shown);    // a downgrade shows
			CHECK(dv.Entries()[0].version == "0.26.0-rc.6");
			dv.Merge(decl("Cheats", "move", "", "Move now"), d2, &shown);                              // no version: kept
			CHECK(!shown && dv.Entries()[0].version == "0.26.0-rc.6");

			// A stored plain version is replaced by the pre-release an untagged build later runs.
			Catalog dp;
			dp.Merge(decl("Cam", "zoom", "v0.2.0"), d1);
			CHECK(dp.Merge(decl("Cam", "zoom", "0.2.0-rc.2"), d1, &shown) && shown && dp.Entries()[0].version == "0.2.0-rc.2");

			// A report (an older pre-release of equal numbers) never lowers it; a newer one raises it.
			CHECK(!dv.Merge(rep("Cheats", "move", "0.26.0-dev.1", "", false), d1, &shown) && !shown);
			CHECK(dv.Entries()[0].version == "0.26.0-rc.6");
			CHECK(dv.Merge(rep("Cheats", "move", "0.26.0", "", false), d1, &shown) && shown && dv.Entries()[0].version == "0.26.0");
		}

		// Two pushes through the shared catalog, dev then rc: the snapshot shows the rc.
		{
			NoteDeclaredRows({ "Stale Probe	0.26.0-dev.1	a	on	A", "Stale Probe	0.26.0-dev.1	b	off	B" });
			NoteDeclaredRows({ "Stale Probe	0.26.0-rc.6	a	on	A", "Stale Probe	0.26.0-rc.6	b	off	B" });
			std::vector<Entry> rows;
			for (const Entry& x : Snapshot()) if (x.plugin == "Stale Probe") rows.push_back(x);
			CHECK(rows.size() == 2 && rows[0].version == "0.26.0-rc.6" && rows[1].version == "0.26.0-rc.6");
			CHECK(ModVersion(rows) == "0.26.0-rc.6");
		}

		// The version a mod shows: the newest declared row wins over a higher leftover; no declared row, the highest.
		{
			auto ent = [](const char* version, const std::string& day, const char* source)
			{
				Entry x; x.plugin = "P"; x.id = version; x.version = version; x.lastSeen = day; x.source = source;
				return x;
			};
			CHECK(ModVersion({}).empty() && ModVersion({ ent("", d2, "declared") }).empty());
			// After a downgrade a removed feature's row still holds the higher version, one push older.
			CHECK(ModVersion({ ent("2.0.0", d1, "declared"), ent("1.9.0", d2, "declared") }) == "1.9.0");
			CHECK(ModVersion({ ent("1.9.0", d2, "declared"), ent("2.0.0", d1, "declared") }) == "1.9.0");
			// Same day: the highest of the declared rows. A declared row beats a reported one.
			CHECK(ModVersion({ ent("1.8.0", d2, "declared"), ent("1.9.0-rc.1", d2, "declared") }) == "1.9.0-rc.1");
			// Same day and the same source, a downgrade: the row merged last (arrival order in this
			// process, not saved) wins over the higher number a dropped feature's row still holds.
			auto seq = [&](const char* version, uint64_t n) { Entry x = ent(version, d2, "declared"); x.declaredSeq = n; return x; };
			CHECK(ModVersion({ seq("2.0.0", 1), seq("1.0.0", 3) }) == "1.0.0");
			CHECK(ModVersion({ seq("1.0.0", 3), seq("2.0.0", 1) }) == "1.0.0");
			CHECK(ModVersion({ seq("2.0.0", 0), seq("1.0.0", 0) }) == "2.0.0");   // no arrival order known (loaded from the file): the highest
			Catalog tie;
			const auto declare = [&](const char* id, const char* version)
			{
				Report r = rep("Tie", id, version, "", false);
				r.source = kSourceDeclared;
				tie.Merge(r, d2);
			};
			declare("a", "2.0.0"); declare("b", "2.0.0");        // the first run
			declare("b", "1.0.0");                               // a downgrade that no longer declares "a"
			CHECK(tie.Size() == 2 && ModVersion(tie.Entries()) == "1.0.0");
			CHECK(ModVersion({ ent("3.0.0", d2, "reported"), ent("1.0.0", d1, "declared") }) == "1.0.0");
			CHECK(ModVersion({ ent("1.0.0", d1, "reported"), ent("1.1.0", d1, "reported") }) == "1.1.0");
		}

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
		CHECK(IniFile::WriteTextFile(path, full.Serialize()));
		CHECK(!std::filesystem::exists(dir / "known-features.txt.tmp"));
		std::string back;
		CHECK(LoadFile(path, back));
		Catalog loaded;
		CHECK(loaded.Parse(back) == 0);
		CHECK(loaded.Serialize() == full.Serialize() && loaded.Size() == 2);
		CHECK(loaded.Entries()[0].offBuilds.size() == 2 && loaded.Entries()[0].lastSeen == d2 && loaded.Entries()[0].title == "A title with spaces");
		CHECK(IniFile::WriteTextFile(path, loaded.Serialize()));   // overwrites an existing file
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
	ProofGateTests();
	ProofHoldTests();
	PushGateTests();

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
		CHECK(RulesText(sp, false) == "default=allow\nBest Builder.some_feature=allow\nBest Builder.other=deny\n");
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

	// Built-in default rows.
	{
		auto has = [](const std::string& text, const std::string& line) { return text.find("\n" + line + "\n") != std::string::npos || text.rfind(line + "\n", 0) == 0; };
		std::vector<std::string> dw;
		const Model none = ParseText({ "[Policy]", "Default=allow" }, dw);
		const Model deny1 = ParseText({ "[Rules]", "betterCheats.stack-size=deny" }, dw);
		const Model allow1 = ParseText({ "[Rules]", "BetterCheats.stack-size=allow" }, dw);
		const Model star = ParseText({ "[Policy]", "Default=allow", "[Rules]", "BetterCheats.*=allow" }, dw);
		PlayerFlags plain;

		// The table: every row is a deny, and a mod name matches without case, a section id exactly.
		CHECK(DefaultRows().size() == 31 && IsDefaultRow("bettercheats", "stack-size") && !IsDefaultRow("BetterCheats", "Stack-Size") && IsDefaultRow("best builder", "no-build-cost")
		      && IsDefaultRow("CameraControls", "vitals") && IsDefaultRow("WaveOptimiser", "heat-tools") && IsDefaultRow("BetterCheats", "inventory-slot-size") && !IsDefaultRow("BetterCheats", "wind-mute"));

		// A non-admin is denied; a remote set carries it, a set for no remote connection does not.
		CHECK(has(RulesText(none, true), "BetterCheats.stack-size=deny") && has(RulesText(none, true), "Best Builder.unlock-all-buildings=deny"));
		CHECK(has(RulesText(none, true), "BetterCheats.inventory-slot-size=deny") && has(PlayerRulesText(none, plain, true), "BetterCheats.inventory-slot-size=deny"));
		CHECK(RulesText(none, false) == "default=allow\n");
		CHECK(PlayerRulesText(none, plain, true) == RulesText(none, true));
		PlayerFlags sec;
		sec.sections = { "Other.thing" };
		CHECK(has(PlayerRulesText(none, sec, true), "BetterCheats.stack-size=deny") && PlayerRulesText(none, sec, false).find("BetterCheats") == std::string::npos);

		// An exact ini line (either value) replaces the default.
		CHECK(!has(RulesText(allow1, true), "BetterCheats.stack-size=deny") && has(RulesText(allow1, true), "BetterCheats.stack-size=allow"));
		CHECK(RulesText(deny1, true).find("tack-") != std::string::npos && !has(RulesText(deny1, true), "BetterCheats.stack-size=deny")
		      && has(RulesText(deny1, true), "BetterCheats.movement=deny"));
		CHECK(ActiveDefaultRows(allow1).size() == 30 && ActiveDefaultRows(deny1).size() == 30 && ActiveDefaultRows(none).size() == 31);

		// A different-case id is another section to the loader, so it does not replace the default.
		const Model oddCase = ParseText({ "[Rules]", "BetterCheats.Stack-Size=allow" }, dw);
		CHECK(ActiveDefaultRows(oddCase).size() == 31);

		// Plugin.* = allow and [Policy] Default = allow do not open it.
		CHECK(has(RulesText(star, true), "BetterCheats.*=allow") && has(RulesText(star, true), "BetterCheats.stack-size=deny"));
		CHECK(has(RulesText(none, true), "BetterCheats.stack-size=deny"));

		// An ExemptRules entry gets nothing denied, even with a Sections= key. Admin alone does not exempt (below).
		PlayerFlags exemptAll; exemptAll.exemptRules = true;
		CHECK(PlayerRulesText(none, exemptAll, true) == "default=allow\n");
		exemptAll.sections = { "BetterCheats.stack-size" };
		CHECK(PlayerRulesText(none, exemptAll, true) == "default=allow\n");
		CHECK(PlayerRulesText(none, exemptAll, true).find("inventory-slot-size") == std::string::npos);

		// Admin: the built-in default rows are "admins only", so a proven admin gets none of them
		// and the host's own lines decide. Admin exempts from no host rule.
		PlayerFlags admin; admin.admin = true;
		CHECK(NeedsOwnRules(admin));
		CHECK(PlayerRulesText(none, admin, true) == "default=allow\n");   // an explicit own set, never skipped: an admin with no host denies gets default=allow
		const Model hostAllow = ParseText({ "[Rules]", "BetterCheats.*=allow" }, dw);
		const Model hostDeny = ParseText({ "[Rules]", "BetterCheats.*=deny" }, dw);
		const Model hostExactDeny = ParseText({ "[Rules]", "BetterCheats.stack-size=deny" }, dw);
		const Model adminsRow = ParseText({ "[Rules]", "BetterCheats.stack-size=admins" }, dw);
		const Model adminsStar = ParseText({ "[Rules]", "BetterCheats.*=admins" }, dw);
		const auto count = [](const std::string& text, const std::string& line)
		{
			size_t n = 0;
			for (size_t at = text.find(line); at != std::string::npos; at = text.find(line, at + 1)) ++n;
			return n;
		};

		// Precedence proof 1: a host line such as the test server's BetterCheats.*=allow.
		// Non-admin (unchanged from rc.2): the exact default row beats Plugin.*=allow, so stack-size is denied.
		CHECK(has(PlayerRulesText(hostAllow, plain, true), "BetterCheats.*=allow") && has(PlayerRulesText(hostAllow, plain, true), "BetterCheats.stack-size=deny"));
		CHECK(PlayerRulesText(hostAllow, plain, true) == RulesText(hostAllow, true));
		// Admin: no default row, so the host's allow stands.
		CHECK(PlayerRulesText(hostAllow, admin, true) == "default=allow\nBetterCheats.*=allow\n");
		// Precedence proof 2: a host plain deny. Non-admin: denied (unchanged). Admin: the plain deny applies, admin does not override it.
		CHECK(has(PlayerRulesText(hostDeny, plain, true), "BetterCheats.*=deny") && has(PlayerRulesText(hostDeny, plain, true), "BetterCheats.stack-size=deny"));
		CHECK(PlayerRulesText(hostDeny, admin, true) == "default=allow\nBetterCheats.*=deny\n");
		CHECK(PlayerRulesText(hostExactDeny, admin, true) == "default=allow\nBetterCheats.stack-size=deny\n");
		// An exact allow from the host works for both.
		CHECK(PlayerRulesText(allow1, admin, true) == "default=allow\nBetterCheats.stack-size=allow\n");
		CHECK(has(PlayerRulesText(allow1, plain, true), "BetterCheats.stack-size=allow") && !has(PlayerRulesText(allow1, plain, true), "BetterCheats.stack-size=deny"));

		// An explicit admins-only row: deny for everyone else (once, no duplicate), allow for a proven admin.
		CHECK(count(PlayerRulesText(adminsRow, plain, true), "BetterCheats.stack-size=") == 1 && has(PlayerRulesText(adminsRow, plain, true), "BetterCheats.stack-size=deny"));
		CHECK(PlayerRulesText(adminsRow, plain, false) == "default=allow\nBetterCheats.stack-size=deny\n");
		CHECK(PlayerRulesText(adminsRow, admin, true) == "default=allow\nBetterCheats.stack-size=allow\n");
		CHECK(PlayerRulesText(adminsStar, admin, true) == "default=allow\nBetterCheats.*=allow\n");
		CHECK(has(PlayerRulesText(adminsStar, plain, true), "BetterCheats.*=deny"));
		// A proof-less admin claim is a plain player: FlagsFor drops the flag.
		CHECK(PlayerRulesText(adminsRow, FlagsFor(true, false, {}, false), true) == RulesText(adminsRow, true));
		CHECK(PlayerRulesText(adminsRow, FlagsFor(true, false, {}, true), true) == "default=allow\nBetterCheats.stack-size=allow\n");
		// A proven admin with ExemptRules still gets everything open.
		PlayerFlags both2; both2.admin = true; both2.exemptRules = true;
		CHECK(PlayerRulesText(hostDeny, both2, true) == "default=allow\n");

		// BetterDrone rows: the five cheat-like drone features are admins only; wave-shelter is a fix, not a cheat, and has no row.
		for (const char* id : { "mining-laser", "map-reveal", "drone-limits", "always-allow", "drone-interact" })
			CHECK(IsDefaultRow("BetterDrone", id) && IsDefaultRow("betterdrone", id));
		CHECK(!IsDefaultRow("BetterDrone", "wave-shelter") && !IsDefaultRow("BetterDrone", "sprint-key"));
		CHECK(has(PlayerRulesText(none, plain, true), "BetterDrone.mining-laser=deny"));            // a non-admin is denied
		CHECK(PlayerRulesText(none, admin, true).find("BetterDrone") == std::string::npos);        // a proven admin gets no deny: allowed
		CHECK(PlayerRulesText(none, FlagsFor(true, false, {}, false), true).find("BetterDrone.mining-laser=deny") != std::string::npos);   // an admin claim without proof is denied

		// A player's Sections= grant: Plugin.* leaves the default denied, the exact key opens it.
		PlayerFlags wide; wide.sections = { "BetterCheats.*" };
		const std::string wideText = PlayerRulesText(none, wide, true);
		CHECK(has(wideText, "BetterCheats.*=allow") && has(wideText, "BetterCheats.stack-size=deny"));
		PlayerFlags exact; exact.sections = { "BetterCheats.stack-size" };
		const std::string exactText = PlayerRulesText(none, exact, true);
		CHECK(has(exactText, "BetterCheats.stack-size=allow") && !has(exactText, "BetterCheats.stack-size=deny") && has(exactText, "BetterCheats.movement=deny"));
		// A Plugin.* key does not take back an exact allow the host wrote.
		const std::string wideOpened = PlayerRulesText(allow1, wide, true);
		CHECK(has(wideOpened, "BetterCheats.stack-size=allow") && !has(wideOpened, "BetterCheats.stack-size=deny"));
		PlayerFlags both; both.sections = { "BetterCheats.*", "BetterCheats.stack-size" };
		const std::string bothText = PlayerRulesText(star, both, true);
		CHECK(has(bothText, "BetterCheats.stack-size=allow") && !has(bothText, "BetterCheats.stack-size=deny"));
	}

	// The "admins" rule value: file round trip, merge on a duplicate, the plain-deny view for the loader,
	// and an old file (no admins value) loads unchanged.
	{
		std::vector<std::string> aw;
		Model am = ParseText({ "[Rules]", "BetterCheats.stack-size=Admins", "Other.x=deny", "P.f=allow" }, aw);
		CHECK(aw.empty() && am.rules.size() == 3 && am.rules[0].adminsOnly && am.rules[0].deny && !am.rules[1].adminsOnly && !am.rules[2].deny);
		CHECK(RuleLines(am)[0] == "BetterCheats.stack-size=admins" && RuleLines(am)[1] == "Other.x=deny" && RuleLines(am)[2] == "P.f=allow");
		CHECK(RulesText(am, false) == "default=allow\nBetterCheats.stack-size=deny\nOther.x=deny\nP.f=allow\n");
		// The file text re-reads to the same model.
		std::vector<std::string> file = { "[Rules]" };
		for (auto& l : RuleLines(am)) file.push_back(l);
		aw.clear();
		Model back = ParseText(file, aw);
		CHECK(aw.empty() && RuleLines(back) == RuleLines(am));
		// A duplicate: deny beats admins, admins beats allow, in either order.
		aw.clear();
		CHECK(ParseText({ "[Rules]", "A.b=admins", "A.b=deny" }, aw).rules[0].adminsOnly == false);
		CHECK(ParseText({ "[Rules]", "A.b=deny", "A.b=admins" }, aw).rules[0].adminsOnly == false);
		CHECK(ParseText({ "[Rules]", "A.b=allow", "A.b=admins" }, aw).rules[0].adminsOnly && ParseText({ "[Rules]", "A.b=admins", "A.b=allow" }, aw).rules[0].adminsOnly);
		CHECK(ParseText({ "[Rules]", "A.b=allow", "A.b=admins" }, aw).rules[0].deny);
		// An unknown value is still refused, with the new wording.
		aw.clear();
		Model bad = ParseText({ "[Rules]", "A.b=sometimes" }, aw);
		CHECK(bad.rules.empty() && aw.size() == 1 && aw[0].find("allow, deny or admins") != std::string::npos);
		// Old file: only allow and deny. It loads the same as before.
		aw.clear();
		Model old = ParseText({ "[Rules]", "A.b=deny", "C.d=allow" }, aw);
		CHECK(aw.empty() && old.rules.size() == 2 && !old.rules[0].adminsOnly && old.rules[0].deny && !old.rules[1].adminsOnly);
		// Setters: admins, then allow, then deny, then back.
		std::string err;
		CHECK(SetRuleAdminsOnly(old, "A.b", err) && old.rules[0].adminsOnly && old.rules[0].deny);
		CHECK(SetRule(old, "A.b", false, err) && !old.rules[0].adminsOnly && !old.rules[0].deny);
		CHECK(SetRuleAdminsOnly(old, "A.b", err) && SetRule(old, "A.b", true, err) && !old.rules[0].adminsOnly && old.rules[0].deny);
		CHECK(SetRuleAdminsOnly(old, "New.k", err) && old.rules.size() == 3 && old.rules[2].adminsOnly);
		CHECK(!SetRuleAdminsOnly(old, "nodot", err) && !err.empty() && old.rules.size() == 3);
		// The ServerUtility mod's own features are never ruled: no line for them reaches a player, with or without admin.
		aw.clear();
		Model own = ParseText({ "[Rules]", "ServerUtility.admin-window=deny", "serverutility.*=admins", "P.f=deny" }, aw);
		PlayerFlags adm2; adm2.admin = true;
		const auto lowered = [](std::string t) { for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return t; };
		CHECK(own.rules.size() >= 1 && lowered(RulesText(own, true)).find("serverutility") == std::string::npos && RulesText(own, true).find("P.f=deny\n") != std::string::npos);
		CHECK(lowered(PlayerRulesText(own, adm2, true)).find("serverutility") == std::string::npos);
		// The same row for a plugin the loader rules: the lowercase check above would catch it.
		Model other = ParseText({ "[Rules]", "serverother.*=admins", "P.f=deny" }, aw);
		CHECK(lowered(RulesText(other, true)).find("serverother") != std::string::npos);
	}

	// Records are kept per connection: two connections that read the same ID, one
	// leaving, and the other still gets its rules refreshed.
	{
		RulesModel::PerConnection<int, std::string> reported;
		reported.Ensure(1) = "A";
		reported.Ensure(2) = "A";   // a second connection with the same ID
		CHECK(reported.rows.size() == 2);
		reported.Erase(2);
		CHECK(reported.Find(1) != nullptr && *reported.Find(1) == "A");
		CHECK(reported.Find(2) == nullptr);
		reported.Ensure(1) = "B";   // Ensure finds the existing row
		CHECK(reported.rows.size() == 1 && *reported.Find(1) == "B");
		reported.Erase(7);          // an unknown connection changes nothing
		CHECK(reported.rows.size() == 1);
	}

	// Resolve: may a player use plugin.id. Held to PlayerRulesText by reading its text the way the loader does.
	{
		// Test-only reader of a rules text: the exact row, then Plugin.*, then default= (mod names without case).
		const auto textAllows = [](const std::string& text, const std::string& plugin, const std::string& id)
		{
			const auto low = [](std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; };
			bool def = true, haveExact = false, exact = true, haveAll = false, all = true;
			size_t pos = 0;
			while (pos < text.size())
			{
				size_t end = text.find('\n', pos);
				if (end == std::string::npos) end = text.size();
				const std::string line = text.substr(pos, end - pos);
				pos = end + 1;
				const size_t eq = line.find('=');
				if (eq == std::string::npos) continue;
				const std::string key = line.substr(0, eq), val = line.substr(eq + 1);
				if (key == "default") { def = val == "allow"; continue; }
				if (key == "message") continue;
				const size_t dot = key.rfind('.');
				if (dot == std::string::npos || low(key.substr(0, dot)) != low(plugin)) continue;
				const std::string rid = key.substr(dot + 1);
				if (rid == id) { haveExact = true; exact = val == "allow"; }
				else if (rid == "*") { haveAll = true; all = val == "allow"; }
			}
			return haveExact ? exact : haveAll ? all : def;
		};

		std::vector<std::string> rw;
		const std::vector<std::vector<std::string>> models = {
			{},
			{ "[Rules]", "BetterCheats.stack-size=allow" },
			{ "[Rules]", "BetterCheats.stack-size=deny" },
			{ "[Rules]", "BetterCheats.stack-size=admins" },
			{ "[Rules]", "BetterCheats.*=deny" },
			{ "[Rules]", "BetterCheats.*=admins" },
			{ "[Rules]", "BetterCheats.*=allow", "BetterCheats.movement=deny" },
			{ "[Policy]", "Default=deny" },
			{ "[Policy]", "Default=deny", "[Rules]", "BetterCheats.*=allow" },
			{ "[Policy]", "Default=deny", "[Rules]", "BetterCheats.skills=allow" },
			{ "[Rules]", "bettercheats.movement=allow", "Other.thing=deny" },
		};
		std::vector<PlayerFlags> flagSets;
		flagSets.push_back(PlayerFlags());
		{ PlayerFlags f; f.admin = true; flagSets.push_back(f); }
		{ PlayerFlags f; f.exemptRules = true; flagSets.push_back(f); }
		{ PlayerFlags f; f.sections = { "BetterCheats.movement" }; flagSets.push_back(f); }
		{ PlayerFlags f; f.sections = { "BetterCheats.*" }; flagSets.push_back(f); }
		{ PlayerFlags f; f.sections = { "BetterCheats.*", "BetterCheats.stack-size" }; flagSets.push_back(f); }
		{ PlayerFlags f; f.admin = true; f.sections = { "BetterCheats.skills" }; flagSets.push_back(f); }
		{ PlayerFlags f; f.admin = true; f.exemptRules = true; flagSets.push_back(f); }

		std::vector<std::string> ids = { "no-such-feature", "stack-size", "movement", "skills" };
		for (const DefaultRow& d : DefaultRows()) if (std::string(d.plugin) == "BetterCheats") ids.push_back(d.id);
		int drift = 0;
		for (const auto& lines : models)
		{
			const Model m = ParseText(lines, rw);
			for (const PlayerFlags& f : flagSets)
			{
				const std::string text = PlayerRulesText(m, f, true);
				for (const std::string& id : ids)
				{
					const bool viaText = f.exemptRules ? true : textAllows(text, "BetterCheats", id);
					if (Resolve(m, f, "BetterCheats", id).allowed != viaText) { ++drift; std::printf("drift: %s admin=%d exempt=%d\n", id.c_str(), f.admin, f.exemptRules); }
				}
			}
		}
		CHECK(drift == 0);

		const Model none = ParseText({}, rw);
		PlayerFlags admin; admin.admin = true;
		PlayerFlags exempt; exempt.exemptRules = true;
		// A non-admin is denied every default row, a proven admin gets them, a listed admin not proven is a non-admin.
		CHECK(!Resolve(none, PlayerFlags(), "BetterCheats", "movement").allowed);
		CHECK(Resolve(none, admin, "BetterCheats", "movement").allowed);
		CHECK(!Resolve(none, FlagsFor(true, false, {}, false), "BetterCheats", "movement").allowed);
		CHECK(Resolve(none, FlagsFor(true, false, {}, true), "BetterCheats", "movement").allowed);
		CHECK(Resolve(none, PlayerFlags(), "BetterCheats", "no-such-feature").allowed);   // no row, default allow
		// An exact deny beats the default "admins only" row, for a proven admin too.
		const Model exactDeny = ParseText({ "[Rules]", "BetterCheats.movement=deny" }, rw);
		CHECK(!Resolve(exactDeny, admin, "BetterCheats", "movement").allowed);
		// "admins" on a row: only a proven admin.
		const Model adminsRow = ParseText({ "[Rules]", "BetterCheats.skills=admins" }, rw);
		CHECK(!Resolve(adminsRow, PlayerFlags(), "BetterCheats", "skills").allowed);
		CHECK(Resolve(adminsRow, admin, "BetterCheats", "skills").allowed);
		// An exact allow opens a default row for everyone.
		const Model exactAllow = ParseText({ "[Rules]", "BetterCheats.movement=allow" }, rw);
		CHECK(Resolve(exactAllow, PlayerFlags(), "BetterCheats", "movement").allowed);
		// A player's own exact Sections= entry beats their own whole-mod entry and the rules.
		PlayerFlags own; own.sections = { "BetterCheats.movement" };
		CHECK(Resolve(exactDeny, own, "BetterCheats", "movement").allowed);
		CHECK(!Resolve(exactDeny, own, "BetterCheats", "skills").allowed);
		PlayerFlags ownAll; ownAll.sections = { "BetterCheats.*" };
		CHECK(Resolve(ParseText({ "[Rules]", "BetterCheats.*=deny" }, rw), ownAll, "BetterCheats", "no-such-feature").allowed);
		// BetterCheats.*=deny: a proven admin is denied, an ExemptRules player is allowed.
		const Model modDeny = ParseText({ "[Rules]", "BetterCheats.*=deny" }, rw);
		CHECK(!Resolve(modDeny, admin, "BetterCheats", "movement").allowed);
		CHECK(!Resolve(modDeny, admin, "BetterCheats", "no-such-feature").allowed);
		CHECK(Resolve(modDeny, exempt, "BetterCheats", "movement").allowed);
		// [Policy] Default=deny with no row: denied (an id with no row falls to Plugin.*, then Default).
		const Model closed = ParseText({ "[Policy]", "Default=deny" }, rw);
		CHECK(!Resolve(closed, PlayerFlags(), "BetterCheats", "no-such-feature").allowed);
		CHECK(!Resolve(closed, admin, "BetterCheats", "movement").allowed);
		CHECK(Resolve(closed, exempt, "BetterCheats", "movement").allowed);
		const Model closedMod = ParseText({ "[Policy]", "Default=deny", "[Rules]", "BetterCheats.*=allow" }, rw);
		CHECK(Resolve(closedMod, PlayerFlags(), "BetterCheats", "no-such-feature").allowed);
		CHECK(!Resolve(closedMod, PlayerFlags(), "BetterCheats", "movement").allowed);   // the default row still holds
	}

	// CheatAuthority::Decide: the rules decide, and a listed admin counts as one only once proven.
	{
		std::vector<std::string> cw;
		PlayerList::Entry listedAdmin; listedAdmin.id = "A"; listedAdmin.admin = true;
		PlayerList::Entry plain;       plain.id = "P";
		bool proven = false;
		Model rules = ParseText({ "[Rules]", "BetterCheats.movement=allow" }, cw);
		CheatAuthority::Env env;
		env.findPlayer = [&](const std::string& id, PlayerList::Entry& out) { if (id == "A") { out = listedAdmin; return true; } if (id == "P") { out = plain; return true; } return false; };
		env.isProven   = [&](const std::string&) { return proven; };
		env.model      = [&] { return rules; };
		// An explicit allow opens the feature to everyone; a default row stays admins-only.
		CHECK(CheatAuthority::Decide(env, "P", "BetterCheats", "movement").allowed);
		CHECK(!CheatAuthority::Decide(env, "P", "BetterCheats", "skills").allowed);
		CHECK(!CheatAuthority::Decide(env, "A", "BetterCheats", "skills").allowed);   // listed, not proven
		proven = true;
		CHECK(CheatAuthority::Decide(env, "A", "BetterCheats", "skills").allowed);
		CHECK(!CheatAuthority::Decide(env, "nobody", "BetterCheats", "skills").allowed);
		// Reasons: the admins-only deny and the rule deny read differently.
		const Model denyRow = ParseText({ "[Rules]", "BetterCheats.skills=deny" }, cw);
		CHECK(std::string(Resolve(ParseText({}, cw), PlayerFlags(), "BetterCheats", "skills").why) == "Only this server's admins can use this.");
		CHECK(std::string(Resolve(denyRow, PlayerFlags(), "BetterCheats", "skills").why) == "Turned off by this server's rules. Ask the server's admin.");
	}

	std::printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
