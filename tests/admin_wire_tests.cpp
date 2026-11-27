// Unit tests of the admin message codec.
// No project file: from a Visual Studio developer prompt, in the repo root:
//   cl /nologo /EHsc /std:c++17 /Isrc tests\admin_wire_tests.cpp src\admin_wire\admin_wire.cpp src\admin_wire\admin_crypto.cpp /Fe:admin_wire_tests.exe
//   admin_wire_tests.exe
#include "admin_wire/admin_wire.h"
#include "admin_wire/admin_crypto.h"

#include <cstdio>
#include <cstring>

static int g_checks = 0, g_failed = 0;

#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failed; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

using namespace AdminWire;

static Message RoundTrip(const Message& in, Decoded* res = nullptr)
{
	std::string enc;
	Message out;
	CHECK(Encode(in, enc));
	CHECK(enc.size() < kMaxMessage);
	Decoded d = Decode(enc, out);
	if (res) *res = d;
	CHECK(d == Decoded::Ok);
	CHECK(out.tag == in.tag);
	return out;
}

static bool HasSep(const std::string& s) { return s.find(kUS) != std::string::npos || s.find(kRS) != std::string::npos; }

int main()
{
	// Plain tags.
	{ Message m; m.tag = Tag::Hello; RoundTrip(m); }
	{ Message m; m.tag = Tag::Welcome; m.role = kRoleAdmin; m.revision = 42; Message o = RoundTrip(m); CHECK(o.role == "admin" && o.revision == 42); }
	{ Message m; m.tag = Tag::Welcome; m.role = kRoleUpdate; Message o = RoundTrip(m); CHECK(o.role == "update"); }
	{ Message m; m.tag = Tag::Snap; m.sections = "players"; Message o = RoundTrip(m); CHECK(o.sections == "players"); }
	{ Message m; m.tag = Tag::SnapBegin; m.revision = 7; m.sections = "all"; Message o = RoundTrip(m); CHECK(o.revision == 7 && o.sections == "all"); }
	{ Message m; m.tag = Tag::SnapPolicy; m.defaultPolicy = "deny"; m.text = "Some mods are off"; Message o = RoundTrip(m); CHECK(o.defaultPolicy == "deny" && o.text == m.text); }
	{ Message m; m.tag = Tag::SnapPolicy; m.defaultPolicy = "allow"; Message o = RoundTrip(m); CHECK(o.text.empty()); }
	{ Message m; m.tag = Tag::SnapRule; m.rules = { { "BetterCheats.*", "allow" }, { "BetterCheats.movement", "deny" } };
	  Message o = RoundTrip(m); CHECK(o.rules.size() == 2 && o.rules[1].key == "BetterCheats.movement" && o.rules[1].value == "deny"); }
	{ Message m; m.tag = Tag::SnapMin; m.mins = { { "", "v1.27.0" }, { "RuptureTimer", "wire" } };
	  Message o = RoundTrip(m); CHECK(o.mins.size() == 2 && o.mins[0].plugin.empty() && o.mins[1].version == "wire"); }
	{ Message m; m.tag = Tag::SnapDefault; m.keys = { "BetterCheats.god", "Best Builder.no-build-cost" };
	  Message o = RoundTrip(m); CHECK(o.keys.size() == 2 && o.keys[0] == "BetterCheats.god" && o.keys[1] == "Best Builder.no-build-cost"); }
	{ Message m; m.tag = Tag::SnapAdmins; m.keys = { "BetterCheats.god", "BetterCheats.*" };
	  Message o = RoundTrip(m); CHECK(o.keys.size() == 2 && o.keys[0] == "BetterCheats.god" && o.keys[1] == "BetterCheats.*"); }
	{ Message m; m.tag = Tag::SnapMod; m.mod.plugin = "BetterCheats"; m.mod.version = "1.4.0"; m.mod.wire = "3";
	  m.mod.features = { { "movement", "Movement" }, { "god", "" } }; m.cont = true;
	  Message o = RoundTrip(m);
	  CHECK(o.mod.plugin == "BetterCheats" && o.mod.wire == "3" && o.cont && o.mod.features.size() == 2 && o.mod.features[1].label.empty()); }
	{ Message m; m.tag = Tag::SnapPlayer; m.player.id = "76561198000000001"; m.player.name = "Mike"; m.player.flags = "admin -exemptversion label=Some Name";
	  m.player.online = true; m.player.mods = { { "BetterCheats", "1.4.0" } };
	  Message o = RoundTrip(m);
	  CHECK(o.player.id == m.player.id && o.player.flags == m.player.flags && o.player.online && !o.cont && o.player.mods.size() == 1); }
	{ Message m; m.tag = Tag::SnapEnd; m.revision = 9; m.ruleCount = 1; m.minCount = 2; m.modCount = 3; m.playerCount = 4;
	  Message o = RoundTrip(m); CHECK(o.revision == 9 && o.ruleCount == 1 && o.minCount == 2 && o.modCount == 3 && o.playerCount == 4); }
	{ Message m; m.tag = Tag::Result; m.requestId = 5; m.status = kResultChanged; m.text = "Changed elsewhere, refreshed"; m.revision = 11;
	  Message o = RoundTrip(m); CHECK(o.requestId == 5 && o.status == "changed" && o.text == m.text && o.revision == 11); }
	{ Message m; m.tag = Tag::Changed; m.revision = 12; m.sections = "players"; Message o = RoundTrip(m); CHECK(o.revision == 12 && o.sections == "players"); }

	// Pairing tags.
	{
		Message m; m.tag = Tag::Welcome; m.role = kRolePair; m.revision = 1; Message o = RoundTrip(m); CHECK(o.role == "pair");
		Message c; c.tag = Tag::Challenge; c.serverId = std::string(32, 'a'); c.nonce = "00ff10cd"; Message oc = RoundTrip(c);
		CHECK(oc.serverId == c.serverId && oc.nonce == "00ff10cd");
		Message p; p.tag = Tag::Prove; p.proof = std::string(64, 'b'); CHECK(RoundTrip(p).proof == p.proof);
		Message pe; pe.tag = Tag::Prove; CHECK(RoundTrip(pe).proof.empty());
		Message pr; pr.tag = Tag::Pair; pr.code = "K7M2QX9A"; pr.secret = std::string(64, 'c'); Message op = RoundTrip(pr);
		CHECK(op.code == "K7M2QX9A" && op.secret == pr.secret);
		Message pc; pc.tag = Tag::Pair; pc.secret = std::string(64, 'c'); CHECK(RoundTrip(pc).code.empty());   // claim shortcut
		const std::string bad[] = {
			std::string("challenge") + kUS + "1" + kUS + "zz" + kUS + "00",
			std::string("challenge") + kUS + "1" + kUS + "aa",
			std::string("prove") + kUS + "1" + kUS + "abcd",
			std::string("pair") + kUS + "1" + kUS + "K7M2QX9A" + kUS + "abcd",
			std::string("pair") + kUS + "1" + kUS + "k7m2qx9a" + kUS + std::string(64, 'c'),
			std::string("pair") + kUS + "1" + kUS + "K7M2QX9AB" + kUS + std::string(64, 'c'),
			std::string("pair") + kUS + "1" + kUS + "O0I1L" + kUS + std::string(64, 'c') };
		for (const std::string& b : bad) { Message o; CHECK(Decode(b, o) == Decoded::Garbage); }
	}

	// HMAC-SHA256 against RFC 4231 test cases 1 and 2, and the proof helper.
	{
		using namespace AdminCrypto;
		Bytes mac;
		CHECK(HmacSha256(Bytes(20, 0x0b), ToBytes("Hi There"), mac));
		CHECK(ToHex(mac) == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
		CHECK(HmacSha256(ToBytes("Jefe"), ToBytes("what do ya want for nothing?"), mac));
		CHECK(ToHex(mac) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
		Bytes r1, r2; CHECK(RandomBytes(32, r1) && RandomBytes(32, r2) && r1.size() == 32 && !ConstantTimeEqual(r1, r2));
		Bytes back; CHECK(FromHex(ToHex(r1), back) && ConstantTimeEqual(back, r1));
		CHECK(!FromHex("abc", back) && !FromHex("zz", back));
		CHECK(!ConstantTimeEqual(Bytes{ 1, 2 }, Bytes{ 1, 2, 3 }) && ConstantTimeEqual(std::string("ab"), std::string("ab")) && !ConstantTimeEqual(std::string("ab"), std::string("ac")));
		std::string proof, secret = ToHex(r1);
		CHECK(MakeProof(secret, "00ff", "aabb", proof) && proof.size() == 64);
		HmacSha256(r1, ToBytes("00ffaabb"), mac); CHECK(ToHex(mac) == proof);
		std::string other; MakeProof(secret, "00fe", "aabb", other); CHECK(other != proof);
		CHECK(!MakeProof("nothex", "a", "b", proof));
	}

	// Ops: every op record.
	{
		Message m; m.tag = Tag::Ops; m.requestId = 3; m.baseRevision = 8;
		m.ops = { { "rule.set", { "BetterCheats.movement", "deny" } }, { "policy.default", { "deny" } }, { "policy.message", { "Rules apply" } },
			{ "min.loader", { "v1.27.0" } }, { "min.mod", { "RuptureTimer", "wire" } }, { "player.set", { "76561198000000001", "admin" } },
			{ "player.remove", { "76561198000000002" } }, { "player.kick", { "76561198000000003", "AFK" } }, { "catalog.push", { "row" } } };
		Message o = RoundTrip(m);
		CHECK(o.requestId == 3 && o.baseRevision == 8 && o.ops.size() == 9);
		for (size_t i = 0; i < m.ops.size() && i < o.ops.size(); ++i) CHECK(o.ops[i].name == m.ops[i].name && o.ops[i].args == m.ops[i].args);
		CHECK(o.ops[7].args[1] == "AFK");
	}
	{ Message m; m.tag = Tag::Ops; m.ops = { { "rule.set", { "only-one" } } }; std::string e; CHECK(!Encode(m, e)); }       // wrong arg count
	{ Message m; m.tag = Tag::Ops; m.ops = { { "wave.start", {} } }; std::string e; CHECK(!Encode(m, e)); }                 // unknown op

	// Protocol and tag constants.
	{ Message m; m.tag = Tag::Hello; std::string e; Encode(m, e); CHECK(e == std::string("hello") + kUS + "1"); CHECK(kProtocol == 1); CHECK(std::strcmp(kPacketTag, "su.admin") == 0); }

	// Wrong protocol.
	{
		std::string e = std::string("welcome") + kUS + "2" + kUS + "admin" + kUS + "1";
		Message o; CHECK(Decode(e, o) == Decoded::WrongProtocol && o.tag == Tag::Welcome && o.role.empty());
		std::string e2 = std::string("snap-mod") + kUS + "99";
		CHECK(Decode(e2, o) == Decoded::WrongProtocol && o.tag == Tag::SnapMod);
	}

	// Garbage.
	{
		const char* bad[] = { "", "x", "hello", "hello\x1F", "hello\x1F" "x", "hello\x1F" "1\x1F" "extra", "nope\x1F" "1", "welcome\x1F" "1\x1F" "boss\x1F" "1",
			"welcome\x1F" "1\x1F" "admin", "welcome\x1F" "1\x1F" "admin\x1F" "-1", "welcome\x1F" "1\x1F" "admin\x1F" "99999999999",
			"snap-rule\x1F" "1", "snap-rule\x1F" "1\x1F" "k\x1F" "maybe", "snap-mod\x1F" "1\x1F" "p\x1F" "v\x1F" "w\x1F" "2", "snap-mod\x1F" "1\x1F" "p\x1F" "v\x1F" "w\x1F" "0\x1F" "odd",
			"snap-player\x1F" "1\x1F" "id", "snap-end\x1F" "1\x1F" "1", "result\x1F" "1\x1F" "1\x1F" "weird\x1F" "t\x1F" "1",
			"hello\x1F" "1\x1E" "extra", "ops\x1F" "1\x1F" "1\x1F" "1\x1E" "bogus\x1F" "a", "ops\x1F" "1\x1F" "1\x1F" "1\x1E", "ops\x1F" "1\x1F" "1\x1E" "rule.set\x1F" "a\x1F" "allow" };
		for (const char* b : bad) { Message o; Decoded d = Decode(std::string(b), o); CHECK(d == Decoded::Garbage); }
		Message o;
		CHECK(Decode(nullptr, 0, o) == Decoded::Garbage);
		std::string big(kMaxPacket + 1, 'a');
		CHECK(Decode(big, o) == Decoded::Garbage);
		// Random bytes never crash and never decode as Ok with a wrong shape.
		unsigned seed = 12345;
		for (int i = 0; i < 2000; ++i)
		{
			std::string s;
			size_t len = (seed = seed * 1103515245u + 12345u) % 80;
			for (size_t j = 0; j < len; ++j)
			{
				seed = seed * 1103515245u + 12345u;
				unsigned c = (seed >> 16) % 6;
				s += c == 0 ? kUS : c == 1 ? kRS : c == 2 ? '1' : static_cast<char>('a' + (seed >> 20) % 26);
			}
			Decode(s, o);
		}
	}

	// Separators and length in text.
	{
		Message m; m.tag = Tag::SnapPolicy; m.defaultPolicy = "allow";
		m.text = std::string("a") + kUS + "b" + kRS + "c";
		Message o = RoundTrip(m);
		CHECK(o.text == "a b c" && !HasSep(o.text));
		m.text = std::string(5000, 'x');
		Message o2 = RoundTrip(m);
		CHECK(o2.text.size() == kMaxMessageText);
		// Cut on a UTF-8 boundary: 3-byte characters, limit not a multiple of 3.
		std::string cut = CleanText("\xE2\x82\xAC\xE2\x82\xAC\xE2\x82\xAC", 5);
		CHECK(cut == "\xE2\x82\xAC");
		Message k; k.tag = Tag::Ops; k.ops = { { "player.kick", { "id", std::string("why") + kRS + kUS + "x" } } };
		Message ko = RoundTrip(k);
		CHECK(ko.ops[0].args[1] == "why  x");
	}

	// A batch too big for one message is refused.
	{
		Message m; m.tag = Tag::Ops;
		for (int i = 0; i < 40; ++i) m.ops.push_back({ "player.kick", { "76561198000000001", std::string(60, 'r') } });
		std::string e; CHECK(!Encode(m, e));
	}

	// 30-mod snapshot with long feature lists and players: every message under 1024 and the content survives.
	{
		Snapshot s; s.revision = 77; s.defaultPolicy = "deny"; s.message = std::string(400, 'm');
		for (int i = 0; i < 150; ++i) s.rules.push_back({ "Plugin" + std::to_string(i) + ".feature-" + std::to_string(i), i % 2 ? "deny" : "allow" });
		s.mins.push_back({ "", "v1.27.0" });
		for (int i = 0; i < 30; ++i)
		{
			ModInfo m; m.plugin = "Plugin" + std::to_string(i); m.version = "1.2." + std::to_string(i); m.wire = std::to_string(i);
			int nf = (i == 5) ? 120 : 6;   // one mod with a feature list far over a packet
			for (int f = 0; f < nf; ++f) m.features.push_back({ "feature-" + std::to_string(f), std::string(60, 'L') + std::to_string(f) });
			s.mods.push_back(m);
		}
		for (int i = 0; i < 20; ++i)
		{
			PlayerInfo p; p.id = "7656119800000" + std::to_string(1000 + i); p.name = std::string(100, 'n'); p.flags = std::string(300, 'f'); p.online = i % 2 == 0;
			for (int k = 0; k < (i == 3 ? 60 : 4); ++k) p.mods.push_back({ "Plugin" + std::to_string(k), "1.0." + std::to_string(k) });
			s.players.push_back(p);
		}
		std::vector<std::string> msgs = EncodeSnapshot(s);
		CHECK(msgs.size() > 30);
		size_t maxSize = 0, mods = 0, ends = 0, feats5 = 0;
		size_t rules = 0, players = 0;
		bool allOk = true, first = true;
		for (const std::string& e : msgs)
		{
			if (e.size() > maxSize) maxSize = e.size();
			Message o;
			if (Decode(e, o) != Decoded::Ok) { allOk = false; continue; }
			if (first) { CHECK(o.tag == Tag::SnapBegin && o.revision == 77); first = false; }
			if (o.tag == Tag::SnapRule) rules += o.rules.size();
			if (o.tag == Tag::SnapMod) { if (!o.cont) ++mods; if (o.mod.plugin == "Plugin5") feats5 += o.mod.features.size(); }
			if (o.tag == Tag::SnapPlayer && !o.cont) ++players;
			if (o.tag == Tag::SnapEnd) { ++ends; CHECK(o.modCount == 30 && o.playerCount == 20 && o.ruleCount == 150 && o.minCount == 1); }
		}
		CHECK(allOk);
		CHECK(maxSize < kMaxMessage);
		CHECK(mods == 30 && players == 20 && rules == 150 && ends == 1 && feats5 == 120);
		Message last; CHECK(Decode(msgs.back(), last) == Decoded::Ok && last.tag == Tag::SnapEnd);
		std::printf("snapshot: %zu messages, largest %zu bytes\n", msgs.size(), maxSize);
	}

	// snap-default sits between snap-min and snap-mod, is absent when there are no defaults,
	// and a decoder without the tag sees an unknown tag, which decodes as Garbage and is dropped.
	{
		Snapshot s; s.revision = 1; s.sections = "all";
		s.mins.push_back({ "", "v1.27.0" });
		ModInfo mod; mod.plugin = "P"; mod.version = "1"; mod.wire = "1"; s.mods.push_back(mod);
		for (int withDefaults = 0; withDefaults < 2; ++withDefaults)
		{
			if (withDefaults) s.defaults = { "P.f1", "P.f2" };
			size_t minAt = 0, defAt = 0, modAt = 0, defKeys = 0, i = 0;
			for (const std::string& e : EncodeSnapshot(s))
			{
				Message o; CHECK(Decode(e, o) == Decoded::Ok);
				if (o.tag == Tag::SnapMin) minAt = i;
				if (o.tag == Tag::SnapDefault) { defAt = i; defKeys += o.keys.size(); }
				if (o.tag == Tag::SnapMod) modAt = i;
				++i;
			}
			if (withDefaults) CHECK(defKeys == 2 && minAt < defAt && defAt < modAt);
			else CHECK(defKeys == 0 && defAt == 0);
		}
		Message o;
		CHECK(Decode(std::string("snap-future") + kUS + "1" + kUS + "P.f1", o) == Decoded::Garbage);
		CHECK(Decode(std::string("snap-default") + kUS + "1", o) == Decoded::Garbage);
		CHECK(Decode(std::string("snap-default") + kUS + "1" + kUS + "", o) == Decoded::Garbage);
	}

	// snap-admins: an admins-only rule goes out as a plain deny in snap-rule (what a client that does
	// not know the value shows) and its key goes in snap-admins. The record is always sent, even with
	// no keys, and sits after snap-default and before snap-mod. A decoder without the tag drops it.
	{
		Snapshot s; s.revision = 1; s.sections = "all";
		s.rules = { { "BetterCheats.god", "admins" }, { "BetterCheats.movement", "deny" }, { "P.f", "allow" } };
		s.defaults = { "P.f1" };
		ModInfo mod; mod.plugin = "P"; mod.version = "1"; mod.wire = "1"; s.mods.push_back(mod);
		for (int withAdmins = 0; withAdmins < 2; ++withAdmins)
		{
			if (!withAdmins) s.rules[0].value = "deny";
			size_t defAt = 0, admAt = 0, modAt = 0, admKeys = 0, admRecords = 0, i = 0;
			bool godDeny = false, wrongValue = false;
			for (const std::string& e : EncodeSnapshot(s))
			{
				Message o; CHECK(Decode(e, o) == Decoded::Ok);
				if (o.tag == Tag::SnapDefault) defAt = i;
				if (o.tag == Tag::SnapAdmins) { admAt = i; ++admRecords; admKeys += o.keys.size(); if (!o.keys.empty()) CHECK(o.keys[0] == "BetterCheats.god"); }
				if (o.tag == Tag::SnapMod) modAt = i;
				if (o.tag == Tag::SnapRule)
					for (const Rule& r : o.rules)
					{
						if (r.key == "BetterCheats.god") godDeny = r.value == "deny";
						if (r.value == "admins") wrongValue = true;
					}
				++i;
			}
			CHECK(godDeny && !wrongValue);
			CHECK(admRecords == 1 && defAt < admAt && admAt < modAt);
			CHECK(admKeys == (withAdmins ? 1u : 0u));
			s.rules[0].value = "admins";
		}
		Message o;
		CHECK(Decode(std::string("snap-admins") + kUS + "1", o) == Decoded::Ok && o.keys.empty());   // empty is valid
		CHECK(Decode(std::string("snap-admins") + kUS + "1" + kUS + "", o) == Decoded::Garbage);
		// A decoder from before rc.3 knows no snap-admins: the unknown tag decodes as Garbage and the client drops it.
		CHECK(Decode(std::string("snap-future") + kUS + "1" + kUS + "P.f1", o) == Decoded::Garbage);
	}

	// snap-server: round trip, placed after snap-default, absent for mods-only and when the server has no value,
	// garbage forms refused, the op is known, and a snapshot with it stays under kMaxMessage.
	{
		Message in; in.tag = Tag::SnapServer; in.stackSize = 500; in.stackAvailable = true;
		Message out = RoundTrip(in);
		CHECK(out.stackSize == 500 && out.stackAvailable);
		in.stackSize = 0; in.stackAvailable = false;
		out = RoundTrip(in);
		CHECK(out.stackSize == 0 && !out.stackAvailable);
		in.stackSize = 9999; in.stackAvailable = true;
		out = RoundTrip(in);
		CHECK(out.stackSize == 9999 && out.stackAvailable);
		in.stackSize = 10000;
		std::string enc;
		CHECK(!Encode(in, enc));

		Snapshot s; s.revision = 3; s.sections = kSectionAll; s.stackSize = 500; s.stackAvailable = true;
		s.defaults = { "P.f1" };
		ModInfo mod; mod.plugin = "P"; mod.version = "1"; mod.wire = "1"; s.mods.push_back(mod);
		auto where = [](const Snapshot& snap, size_t& defAt, size_t& srvAt, size_t& modAt, int& servers, uint32_t& value, bool& avail)
		{
			defAt = srvAt = modAt = 0; servers = 0; value = 0; avail = false;
			size_t i = 0;
			for (const std::string& e : EncodeSnapshot(snap))
			{
				Message o; CHECK(Decode(e, o) == Decoded::Ok);
				CHECK(e.size() < kMaxMessage);
				if (o.tag == Tag::SnapDefault) defAt = i;
				if (o.tag == Tag::SnapServer) { srvAt = i; ++servers; value = o.stackSize; avail = o.stackAvailable; }
				if (o.tag == Tag::SnapMod) modAt = i;
				++i;
			}
		};
		size_t defAt, srvAt, modAt; int servers; uint32_t value; bool avail;
		where(s, defAt, srvAt, modAt, servers, value, avail);
		CHECK(servers == 1 && value == 500 && avail && defAt < srvAt && srvAt < modAt);

		s.sections = kSectionMods;   // never for a mods-only snapshot
		where(s, defAt, srvAt, modAt, servers, value, avail);
		CHECK(servers == 0);

		s.sections = kSectionAll; s.stackSize = -1;   // the server has no value: no record
		where(s, defAt, srvAt, modAt, servers, value, avail);
		CHECK(servers == 0);
		CHECK(Snapshot().stackSize == -1 && !Snapshot().stackAvailable);

		Message o;
		const std::string head = std::string("snap-server") + kUS + "1";
		CHECK(Decode(head, o) == Decoded::Garbage);                                  // no fields
		CHECK(Decode(head + kUS + "500", o) == Decoded::Garbage);                    // missing available
		CHECK(Decode(head + kUS + "10000" + kUS + "1", o) == Decoded::Garbage);      // too big
		CHECK(Decode(head + kUS + "500" + kUS + "2", o) == Decoded::Garbage);        // available 2
		CHECK(Decode(head + kUS + "-1" + kUS + "1", o) == Decoded::Garbage);
		CHECK(Decode(head + kUS + "abc" + kUS + "1", o) == Decoded::Garbage);
		CHECK(Decode(head + kUS + "" + kUS + "1", o) == Decoded::Garbage);
		CHECK(Decode(head + kUS + "500" + kUS + "1" + kUS + "x", o) == Decoded::Garbage);
		CHECK(Decode(head + kUS + "500" + kUS + "1", o) == Decoded::Ok && o.stackSize == 500 && o.stackAvailable);

		// The op: one argument; it encodes and decodes in an ops message.
		CHECK(OpArgCount("server.stacksize") == 1);
		Message ops; ops.tag = Tag::Ops; ops.requestId = 1; ops.baseRevision = 2;
		ops.ops.push_back({ "server.stacksize", { "500" } });
		Message back = RoundTrip(ops);
		CHECK(back.ops.size() == 1 && back.ops[0].name == "server.stacksize" && back.ops[0].args[0] == "500");
		ops.ops[0].args.clear();
		CHECK(!Encode(ops, enc));
	}

	std::printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
