#include "admin_wire/admin_wire.h"
#include "host_rules/claim_model.h"

#include <cstdlib>
#include <cstring>

namespace AdminWire
{
	std::vector<std::string> Split(const std::string& s, char sep)
	{
		std::vector<std::string> r;
		size_t start = 0;
		for (;;)
		{
			size_t p = s.find(sep, start);
			if (p == std::string::npos) { r.push_back(s.substr(start)); break; }
			r.push_back(s.substr(start, p - start));
			start = p + 1;
		}
		return r;
	}

namespace
{
	using Fields = std::vector<std::string>;

	struct TagName { Tag tag; const char* name; };
	const TagName kTags[] = {
		{ Tag::Hello, "hello" }, { Tag::Welcome, "welcome" }, { Tag::Snap, "snap" },
		{ Tag::SnapBegin, "snap-begin" }, { Tag::SnapPolicy, "snap-policy" }, { Tag::SnapRule, "snap-rule" },
		{ Tag::SnapMin, "snap-min" }, { Tag::SnapMod, "snap-mod" }, { Tag::SnapPlayer, "snap-player" },
		{ Tag::SnapEnd, "snap-end" }, { Tag::Ops, "ops" }, { Tag::Result, "result" }, { Tag::Changed, "changed" },
		{ Tag::Challenge, "challenge" }, { Tag::Prove, "prove" }, { Tag::Pair, "pair" },
		{ Tag::SnapDefault, "snap-default" }, { Tag::SnapAdmins, "snap-admins" },
		{ Tag::SnapServer, "snap-server" },
	};

	const char* NameOf(Tag t)
	{
		for (const TagName& e : kTags) if (e.tag == t) return e.name;
		return "";
	}

	bool TagOf(const std::string& s, Tag& t)
	{
		for (const TagName& e : kTags) if (s == e.name) { t = e.tag; return true; }
		return false;
	}

	std::string Num(uint32_t v) { return std::to_string(v); }

	bool ParseNum(const std::string& s, uint32_t& v)
	{
		if (s.empty() || s.size() > 10) return false;
		uint64_t n = 0;
		for (char c : s) { if (c < '0' || c > '9') return false; n = n * 10 + (c - '0'); }
		if (n > 0xFFFFFFFFull) return false;
		v = static_cast<uint32_t>(n);
		return true;
	}

	bool ParseBool(const std::string& s, bool& v)
	{
		if (s == "0") { v = false; return true; }
		if (s == "1") { v = true; return true; }
		return false;
	}

	std::string Join(const Fields& f)
	{
		std::string r;
		for (size_t i = 0; i < f.size(); ++i) { if (i) r += kUS; r += f[i]; }
		return r;
	}

	size_t Size(const Fields& f)
	{
		size_t n = f.empty() ? 0 : f.size() - 1;
		for (const std::string& s : f) n += s.size();
		return n;
	}

	bool IsHex(const std::string& s, size_t minLen, size_t maxLen)
	{
		if (s.size() < minLen || s.size() > maxLen || s.size() % 2) return false;
		for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
		return true;
	}

	bool IsPairCode(const std::string& s)
	{
		if (s.size() > 8) return false;
		for (char c : s) if (!std::strchr(ClaimModel::kAlphabet, c) || !c) return false;
		return true;
	}

	bool IsAllowDeny(const std::string& s) { return s == "allow" || s == "deny"; }

	Fields Head(Tag t) { return Fields{ NameOf(t), Num(kProtocol) }; }

	// Append items to a header and flush whenever the next item would not fit.
	// contIdx is the header field set to "1" on every message after the first, or -1.
	void Pack(std::vector<std::string>& out, const Fields& header, int contIdx,
		const std::vector<Fields>& items, bool alwaysEmit)
	{
		Fields cur = header;
		size_t sz = Size(cur);
		bool any = false;
		for (const Fields& item : items)
		{
			size_t isz = 0;
			for (const std::string& s : item) isz += s.size() + 1;
			if (any && sz + isz >= kMaxMessage)
			{
				out.push_back(Join(cur));
				cur = header;
				if (contIdx >= 0) cur[contIdx] = "1";
				sz = Size(cur);
				any = false;
			}
			for (const std::string& s : item) cur.push_back(s);
			sz += isz;
			any = true;
		}
		if (any || alwaysEmit) out.push_back(Join(cur));
	}
}

int OpArgCount(const std::string& name)
{
	if (name == "rule.set") return 2;
	if (name == "policy.default") return 1;
	if (name == "policy.message") return 1;
	if (name == "min.loader") return 1;
	if (name == "min.mod") return 2;
	if (name == "player.set") return 2;
	if (name == "player.remove") return 1;
	if (name == "player.kick") return 2;
	if (name == "catalog.push") return 1;
	if (name == "server.stacksize") return 1;
	return -1;
}

std::string CleanText(const std::string& s, size_t max)
{
	std::string r;
	r.reserve(s.size() < max ? s.size() : max);
	for (char c : s) r += (c == kUS || c == kRS) ? ' ' : c;
	if (r.size() > max)
	{
		size_t cut = max;
		// Do not leave half a UTF-8 sequence: step back over continuation bytes.
		while (cut > 0 && (static_cast<unsigned char>(r[cut]) & 0xC0) == 0x80) --cut;
		r.resize(cut);
	}
	return r;
}

bool Encode(const Message& m, std::string& out)
{
	out.clear();
	Fields h = Head(m.tag);
	switch (m.tag)
	{
	case Tag::Hello: break;
	case Tag::Welcome: h.push_back(CleanText(m.role, kMaxName)); h.push_back(Num(m.revision)); break;
	case Tag::Snap: h.push_back(CleanText(m.sections, kMaxName)); break;
	case Tag::SnapBegin: h.push_back(Num(m.revision)); h.push_back(CleanText(m.sections, kMaxName)); break;
	case Tag::SnapPolicy: h.push_back(CleanText(m.defaultPolicy, kMaxName)); h.push_back(CleanText(m.text, kMaxMessageText)); break;
	case Tag::SnapRule:
		for (const Rule& r : m.rules) { h.push_back(CleanText(r.key, kMaxName)); h.push_back(CleanText(r.value, kMaxName)); }
		break;
	case Tag::SnapMin:
		for (const MinEntry& e : m.mins) { h.push_back(CleanText(e.plugin, kMaxName)); h.push_back(CleanText(e.version, kMaxName)); }
		break;
	case Tag::SnapDefault: case Tag::SnapAdmins:
		for (const std::string& k : m.keys) h.push_back(CleanText(k, kMaxName));
		break;
	case Tag::SnapServer:
		if (m.stackSize > static_cast<uint32_t>(kMaxStackSize)) return false;
		h.push_back(Num(m.stackSize)); h.push_back(m.stackAvailable ? "1" : "0");
		break;
	case Tag::SnapMod:
		h.push_back(CleanText(m.mod.plugin, kMaxName)); h.push_back(CleanText(m.mod.version, kMaxName));
		h.push_back(CleanText(m.mod.wire, kMaxName)); h.push_back(m.cont ? "1" : "0");
		for (const Feature& f : m.mod.features) { h.push_back(CleanText(f.id, kMaxName)); h.push_back(CleanText(f.label, kMaxLabel)); }
		break;
	case Tag::SnapPlayer:
		h.push_back(CleanText(m.player.id, kMaxName)); h.push_back(CleanText(m.player.name, kMaxLabel));
		h.push_back(CleanText(m.player.flags, kMaxFlags)); h.push_back(m.player.online ? "1" : "0"); h.push_back(m.cont ? "1" : "0");
		for (const PlayerMod& p : m.player.mods) { h.push_back(CleanText(p.name, kMaxName)); h.push_back(CleanText(p.version, kMaxName)); }
		break;
	case Tag::SnapEnd:
		h.push_back(Num(m.revision)); h.push_back(Num(m.ruleCount)); h.push_back(Num(m.minCount));
		h.push_back(Num(m.modCount)); h.push_back(Num(m.playerCount));
		break;
	case Tag::Ops:
	{
		h.push_back(Num(m.requestId)); h.push_back(Num(m.baseRevision));
		std::string s = Join(h);
		for (const Op& op : m.ops)
		{
			if (OpArgCount(op.name) != static_cast<int>(op.args.size())) return false;
			Fields r{ op.name };
			for (const std::string& a : op.args) r.push_back(CleanText(a, kMaxMessageText));
			s += kRS;
			s += Join(r);
		}
		if (s.size() >= kMaxMessage) return false;
		out = s;
		return true;
	}
	case Tag::Result:
		h.push_back(Num(m.requestId)); h.push_back(CleanText(m.status, kMaxName));
		h.push_back(CleanText(m.text, kMaxMessageText)); h.push_back(Num(m.revision));
		break;
	case Tag::Changed: h.push_back(Num(m.revision)); h.push_back(CleanText(m.sections, kMaxName)); break;
	case Tag::Challenge: h.push_back(CleanText(m.serverId, kMaxName)); h.push_back(CleanText(m.nonce, kMaxName)); break;
	case Tag::Prove: h.push_back(CleanText(m.proof, kMaxName)); break;
	case Tag::Pair: h.push_back(CleanText(m.code, kMaxName)); h.push_back(CleanText(m.secret, kMaxName)); break;
	}
	std::string s = Join(h);
	if (s.size() >= kMaxMessage) return false;
	out = s;
	return true;
}

std::vector<std::string> EncodeSnapshot(const Snapshot& s)
{
	std::vector<std::string> out;
	std::string enc;

	Message b; b.tag = Tag::SnapBegin; b.revision = s.revision; b.sections = s.sections;
	if (Encode(b, enc)) out.push_back(enc);

	Message p; p.tag = Tag::SnapPolicy; p.defaultPolicy = s.defaultPolicy; p.text = s.message;
	if (Encode(p, enc)) out.push_back(enc);

	std::vector<Fields> items;
	// An admins-only rule goes out as a deny, which is what a client that does not know the value
	// shows; the key also goes in the snap-admins record below.
	for (const Rule& r : s.rules) items.push_back({ CleanText(r.key, kMaxName), r.value == "admins" ? "deny" : CleanText(r.value, kMaxName) });
	Pack(out, Head(Tag::SnapRule), -1, items, false);

	items.clear();
	for (const MinEntry& e : s.mins) items.push_back({ CleanText(e.plugin, kMaxName), CleanText(e.version, kMaxName) });
	Pack(out, Head(Tag::SnapMin), -1, items, false);

	// Older clients drop this record as unknown, so it adds nothing they have to understand.
	items.clear();
	for (const std::string& k : s.defaults) items.push_back({ CleanText(k, kMaxName) });
	Pack(out, Head(Tag::SnapDefault), -1, items, false);

	// Always sent, even with no keys: its arrival tells the client this server knows "admins only".
	// Older clients drop it as an unknown tag.
	items.clear();
	for (const Rule& r : s.rules) if (r.value == "admins") items.push_back({ CleanText(r.key, kMaxName) });
	Pack(out, Head(Tag::SnapAdmins), -1, items, true);

	// Append-only: an older client drops this record as unknown, and an older server never sends
	// it. A mods-only snapshot leaves it out.
	if (s.stackSize >= 0 && s.sections != kSectionMods)
	{
		Message sv; sv.tag = Tag::SnapServer;
		sv.stackSize = static_cast<uint32_t>(s.stackSize); sv.stackAvailable = s.stackAvailable;
		if (Encode(sv, enc)) out.push_back(enc);
	}

	for (const ModInfo& m : s.mods)
	{
		Fields h = Head(Tag::SnapMod);
		h.push_back(CleanText(m.plugin, kMaxName)); h.push_back(CleanText(m.version, kMaxName));
		h.push_back(CleanText(m.wire, kMaxName)); h.push_back("0");
		items.clear();
		for (const Feature& f : m.features) items.push_back({ CleanText(f.id, kMaxName), CleanText(f.label, kMaxLabel) });
		Pack(out, h, 5, items, true);
	}

	for (const PlayerInfo& pl : s.players)
	{
		Fields h = Head(Tag::SnapPlayer);
		h.push_back(CleanText(pl.id, kMaxName)); h.push_back(CleanText(pl.name, kMaxLabel));
		h.push_back(CleanText(pl.flags, kMaxFlags)); h.push_back(pl.online ? "1" : "0"); h.push_back("0");
		items.clear();
		for (const PlayerMod& pm : pl.mods) items.push_back({ CleanText(pm.name, kMaxName), CleanText(pm.version, kMaxName) });
		Pack(out, h, 6, items, true);
	}

	Message e; e.tag = Tag::SnapEnd; e.revision = s.revision;
	e.ruleCount = static_cast<uint32_t>(s.rules.size()); e.minCount = static_cast<uint32_t>(s.mins.size());
	e.modCount = static_cast<uint32_t>(s.mods.size()); e.playerCount = static_cast<uint32_t>(s.players.size());
	if (Encode(e, enc)) out.push_back(enc);
	return out;
}

Decoded Decode(const std::string& data, Message& out)
{
	out = Message();
	if (data.empty() || data.size() > kMaxPacket) return Decoded::Garbage;

	std::vector<std::string> records = Split(data, kRS);
	Fields f = Split(records[0], kUS);
	if (f.size() < 2) return Decoded::Garbage;

	Tag tag;
	if (!TagOf(f[0], tag)) return Decoded::Garbage;
	uint32_t proto = 0;
	if (!ParseNum(f[1], proto)) return Decoded::Garbage;
	if (proto != kProtocol) { out.tag = tag; return Decoded::WrongProtocol; }
	if (tag != Tag::Ops && records.size() != 1) return Decoded::Garbage;

	Message m;
	m.tag = tag;
	const size_t n = f.size();
	f.resize(n < 12 ? 12 : n);   // padding so a short message never indexes out of range
	bool ok = false;
	switch (tag)
	{
	case Tag::Hello: ok = n == 2; break;
	case Tag::Welcome:
		ok = n == 4 && (f[2] == kRoleAdmin || f[2] == kRolePair || f[2] == kRoleNone || f[2] == kRoleUpdate || f[2] == kRolePaired) && ParseNum(f[3], m.revision);
		m.role = f[2];
		break;
	case Tag::Snap: ok = n == 3; m.sections = f[2]; break;
	case Tag::SnapBegin: ok = n == 4 && ParseNum(f[2], m.revision); m.sections = f[3]; break;
	case Tag::SnapPolicy:
		ok = n == 4 && IsAllowDeny(f[2]);
		m.defaultPolicy = f[2]; m.text = f[3];
		break;
	case Tag::SnapRule:
		ok = n >= 4 && n % 2 == 0;
		for (size_t i = 2; ok && i + 1 < n; i += 2)
		{
			if (!IsAllowDeny(f[i + 1]) || f[i].empty()) ok = false;
			else m.rules.push_back({ f[i], f[i + 1] });
		}
		break;
	case Tag::SnapMin:
		ok = n >= 4 && n % 2 == 0;
		for (size_t i = 2; ok && i + 1 < n; i += 2)
		{
			if (f[i + 1].empty()) ok = false;
			else m.mins.push_back({ f[i], f[i + 1] });
		}
		break;
	case Tag::SnapDefault:
		ok = n >= 3;
		for (size_t i = 2; ok && i < n; ++i)
		{
			if (f[i].empty()) ok = false;
			else m.keys.push_back(f[i]);
		}
		break;
	case Tag::SnapAdmins:
		ok = n >= 2;
		for (size_t i = 2; ok && i < n; ++i)
		{
			if (f[i].empty()) ok = false;
			else m.keys.push_back(f[i]);
		}
		break;
	case Tag::SnapServer:
		ok = n == 4 && ParseNum(f[2], m.stackSize) && m.stackSize <= static_cast<uint32_t>(kMaxStackSize)
			&& ParseBool(f[3], m.stackAvailable);
		break;
	case Tag::SnapMod:
		ok = n >= 6 && n % 2 == 0 && !f[2].empty() && ParseBool(f[5], m.cont);
		for (size_t i = 6; ok && i + 1 < n; i += 2)
		{
			if (f[i].empty()) ok = false;
			else m.mod.features.push_back({ f[i], f[i + 1] });
		}
		m.mod.plugin = f[2]; m.mod.version = f[3]; m.mod.wire = f[4];
		break;
	case Tag::SnapPlayer:
		ok = n >= 7 && n % 2 == 1 && !f[2].empty() && ParseBool(f[5], m.player.online) && ParseBool(f[6], m.cont);
		for (size_t i = 7; ok && i + 1 < n; i += 2)
		{
			if (f[i].empty()) ok = false;
			else m.player.mods.push_back({ f[i], f[i + 1] });
		}
		m.player.id = f[2]; m.player.name = f[3]; m.player.flags = f[4];
		break;
	case Tag::SnapEnd:
		ok = n == 7 && ParseNum(f[2], m.revision) && ParseNum(f[3], m.ruleCount) && ParseNum(f[4], m.minCount)
			&& ParseNum(f[5], m.modCount) && ParseNum(f[6], m.playerCount);
		break;
	case Tag::Ops:
		ok = n == 4 && ParseNum(f[2], m.requestId) && ParseNum(f[3], m.baseRevision);
		for (size_t r = 1; ok && r < records.size(); ++r)
		{
			Fields o = Split(records[r], kUS);
			int want = OpArgCount(o[0]);
			if (want < 0 || o.size() != static_cast<size_t>(want) + 1) { ok = false; break; }
			Op op; op.name = o[0];
			op.args.assign(o.begin() + 1, o.end());
			m.ops.push_back(op);
		}
		break;
	case Tag::Result:
		ok = n == 6 && ParseNum(f[2], m.requestId) && (f[3] == kResultOk || f[3] == kResultChanged || f[3] == kResultError)
			&& ParseNum(f[5], m.revision);
		m.status = f[3]; m.text = f[4];
		break;
	case Tag::Changed:
		ok = n == 4 && ParseNum(f[2], m.revision);
		m.sections = f[3];
		break;
	case Tag::Challenge:
		ok = n == 4 && IsHex(f[2], 2, 64) && IsHex(f[3], 2, 64);
		m.serverId = f[2]; m.nonce = f[3];
		break;
	case Tag::Prove:
		ok = n == 3 && (f[2].empty() || IsHex(f[2], 64, 64));
		m.proof = f[2];
		break;
	case Tag::Pair:
		ok = n == 4 && IsPairCode(f[2]) && IsHex(f[3], 64, 64);
		m.code = f[2]; m.secret = f[3];
		break;
	}
	if (!ok) return Decoded::Garbage;
	out = m;
	return Decoded::Ok;
}

Decoded Decode(const uint8_t* data, size_t size, Message& out)
{
	out = Message();
	if (!data || size == 0 || size > kMaxPacket) return Decoded::Garbage;
	return Decode(std::string(reinterpret_cast<const char*>(data), size), out);
}
}
