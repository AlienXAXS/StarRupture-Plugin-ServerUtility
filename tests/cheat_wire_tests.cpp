// Unit tests of the su.cheat message parser and reply encoder.
// No project file: from a Visual Studio developer prompt, in the repo root:
//   cl /nologo /EHsc /std:c++17 /Isrc tests\cheat_wire_tests.cpp src\cheats\cheat_wire.cpp src\admin_wire\admin_wire.cpp /Fe:cheat_wire_tests.exe
//   cheat_wire_tests.exe
#include "cheats/cheat_wire.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

static int g_checks = 0, g_failed = 0;

#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failed; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

using namespace CheatWire;

static const char US = '\x1F';

static Parsed Parse(const std::string& s)
{
	return ParseRequest(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

static std::string Req(const std::string& id, const std::string& op, const std::string& extra = "")
{
	std::string s = std::string("req") + US + "1" + US + id + US + op;
	if (!extra.empty()) s += US + extra;
	return s;
}

int main()
{
	// A good request, with and without fields.
	{
		Parsed p = Parse(Req("7", "cheat.give", "item=Iron" + std::string(1, US) + "count=5"));
		CHECK(p.status == ParseStatus::Ok && p.hasId && p.id == 7);
		CHECK(p.req.id == 7 && p.req.op == "cheat.give" && p.req.fields.size() == 2);
		CHECK(p.req.Get("item") && *p.req.Get("item") == "Iron");
		CHECK(p.req.Get("count") && *p.req.Get("count") == "5");
		CHECK(p.req.Get("nope") == nullptr);
	}
	{ Parsed p = Parse(Req("1", "cap")); CHECK(p.status == ParseStatus::Ok && p.req.fields.empty()); }
	// A value may be empty, and may hold '='.
	{
		Parsed p = Parse(Req("1", "cheat.stat", "a=" + std::string(1, US) + "b=x=y"));
		CHECK(p.status == ParseStatus::Ok && *p.req.Get("a") == "" && *p.req.Get("b") == "x=y");
	}

	// The id range.
	CHECK(Parse(Req("1", "cap")).status == ParseStatus::Ok);
	CHECK(Parse(Req("2147483647", "cap")).status == ParseStatus::Ok);
	CHECK(Parse(Req("2147483647", "cap")).id == 2147483647u);
	for (const char* bad : { "0", "2147483648", "4294967296", "99999999999", "-1", "1a", "", " 1", "+1" })
	{
		Parsed p = Parse(Req(bad, "cap"));
		CHECK(p.status == ParseStatus::Malformed && !p.hasId);
	}

	// Size limit: 1024 passes, 1025 is dropped.
	{
		std::string s = Req("1", "cheat.give", "k=");
		s.append(kMaxMessage - s.size(), 'x');
		CHECK(s.size() == 1024 && Parse(s).status == ParseStatus::Ok);
		s += 'x';
		CHECK(Parse(s).status == ParseStatus::TooBig);
	}

	// Wrong kind and wrong version are dropped.
	CHECK(Parse(std::string("res") + US + "1" + US + "1" + US + "ok").status == ParseStatus::UnknownKind);
	CHECK(Parse("garbage").status == ParseStatus::UnknownKind);
	CHECK(Parse(std::string("req") + US + "2" + US + "1" + US + "cap").status == ParseStatus::UnknownProtocol);
	CHECK(Parse(std::string("req") + US + US + "1" + US + "cap").status == ParseStatus::UnknownProtocol);
	CHECK(Parse("req").status == ParseStatus::UnknownProtocol);
	CHECK(Parse("").status == ParseStatus::UnknownKind);

	// Too few fields, empty op, bad field shape: malformed. An id that read well is kept to reply with.
	CHECK(Parse(std::string("req") + US + "1" + US + "5").status == ParseStatus::Malformed);
	{
		Parsed p = Parse(Req("5", ""));
		CHECK(p.status == ParseStatus::Malformed && p.hasId && p.id == 5);
	}
	CHECK(Parse(Req("5", "cheat.give", "novalue")).status == ParseStatus::Malformed);
	CHECK(Parse(Req("5", "cheat.give", "=v")).status == ParseStatus::Malformed);
	CHECK(Parse(Req("5", "cheat=x")).status == ParseStatus::Malformed);
	CHECK(Parse(Req("5", std::string(33, 'a'))).status == ParseStatus::Malformed);
	CHECK(Parse(Req("5", std::string(32, 'a'))).status == ParseStatus::Ok);
	CHECK(Parse(Req("5", "cap", std::string("k=a") + '\n' + "b")).status == ParseStatus::Malformed);   // control byte
	CHECK(Parse(Req("5", "cap", std::string("k=a") + '\x1E' + "b")).status == ParseStatus::Malformed);
	CHECK(ParseRequest(nullptr, 0).status == ParseStatus::Malformed);

	// Reply encoding.
	{
		const std::string m = EncodeReply(9, Status::Ok, "");
		CHECK(m == std::string("res") + US + "1" + US + "9" + US + "ok" + US);
	}
	{
		const std::string m = EncodeReply(9, Status::Ok, "done", { { "su", "0.19.0" }, { "cheat.give", "1,off" } });
		CHECK(m == std::string("res") + US + "1" + US + "9" + US + "ok" + US + "done" + US + "su=0.19.0" + US + "cheat.give=1,off");
	}
	// Refused and error never carry an empty reason.
	for (Status s : { Status::Refused, Status::Error })
	{
		const std::string m = EncodeReply(3, s, "");
		const std::string head = std::string("res") + US + "1" + US + "3" + US + StatusText(s) + US;
		CHECK(m.size() > head.size() && m.compare(0, head.size(), head) == 0);
	}
	// A reason is capped at 240 bytes, on a UTF-8 boundary, with control bytes replaced.
	{
		const std::string head = std::string("res") + US + "1" + US + "3" + US + "error" + US;
		std::string m = EncodeReply(3, Status::Error, std::string(500, 'a'));
		CHECK(m.size() == head.size() + kMaxReason);
		std::string utf;
		for (int i = 0; i < 200; ++i) utf += "\xC3\xA9";   // 2 bytes each
		m = EncodeReply(3, Status::Error, "a" + utf);       // a cut at 240 would split a character
		const std::string r = m.substr(head.size());
		CHECK(r.size() == 239);
		CHECK((static_cast<unsigned char>(r.back()) & 0xC0) == 0x80);   // ends on the second byte of a pair
		m = EncodeReply(3, Status::Error, std::string("a") + US + "b" + '\n' + "c");
		CHECK(m.substr(head.size()) == "a b c");
	}
	// Extra fields stop at the message limit; every message stays within it.
	{
		std::vector<std::pair<std::string, std::string>> f;
		for (int i = 0; i < 20; ++i) f.emplace_back("k" + std::to_string(i), std::string(200, 'v'));
		const std::string m = EncodeReply(1, Status::Ok, "", f);
		CHECK(m.size() <= kMaxMessage);
		CHECK(m.find("k0=") != std::string::npos && m.find("k19=") == std::string::npos);
	}
	// A bad field key is skipped.
	{
		const std::string m = EncodeReply(1, Status::Ok, "", { { "", "x" }, { "a=b", "x" }, { "ok", "1" } });
		CHECK(m.find("ok=1") != std::string::npos && m.find("a=b") == std::string::npos && std::count(m.begin(), m.end(), US) == 5);
	}
	CHECK(std::strcmp(StatusText(Status::Ok), "ok") == 0 && std::strcmp(StatusText(Status::Refused), "refused") == 0 && std::strcmp(StatusText(Status::Error), "error") == 0);

	std::printf("cheat_wire_tests: %d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
