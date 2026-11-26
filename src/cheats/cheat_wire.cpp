#include "cheat_wire.h"

#include "admin_wire/admin_wire.h"

namespace CheatWire
{
	namespace
	{
		bool ParseId(const std::string& s, uint32_t& out)
		{
			if (s.empty() || s.size() > 10) return false;
			uint64_t v = 0;
			for (char c : s)
			{
				if (c < '0' || c > '9') return false;
				v = v * 10 + static_cast<uint64_t>(c - '0');
			}
			if (v < 1 || v > kMaxId) return false;
			out = static_cast<uint32_t>(v);
			return true;
		}

		// Text for one field: no byte below 0x20 (so no separator), at most `limit` bytes, cut on a
		// UTF-8 boundary.
		std::string Clean(const std::string& s, size_t limit)
		{
			std::string t;
			t.reserve(s.size());
			for (char c : s)
				t += static_cast<unsigned char>(c) < 0x20 ? ' ' : c;
			if (t.size() > limit)
			{
				size_t cut = limit;
				while (cut > 0 && (static_cast<unsigned char>(t[cut]) & 0xC0) == 0x80) --cut;
				t.resize(cut);
			}
			return t;
		}
	}

	const std::string* Request::Get(const char* key) const
	{
		for (const auto& f : fields)
			if (f.first == key) return &f.second;
		return nullptr;
	}

	const char* StatusText(Status s)
	{
		switch (s)
		{
		case Status::Ok:      return "ok";
		case Status::Refused: return "refused";
		default:              return "error";
		}
	}

	Parsed ParseRequest(const uint8_t* data, size_t size)
	{
		Parsed p;
		if (!data || size > kMaxMessage)
		{
			p.status = size > kMaxMessage ? ParseStatus::TooBig : ParseStatus::Malformed;
			return p;
		}

		const std::string text(reinterpret_cast<const char*>(data), size);
		const std::vector<std::string> f = AdminWire::Split(text, kUS);

		if (f[0] != "req") { p.status = ParseStatus::UnknownKind; return p; }
		if (f.size() < 2 || f[1] != "1") { p.status = ParseStatus::UnknownProtocol; return p; }
		if (f.size() < 4 || !ParseId(f[2], p.id)) return p;   // Malformed, no id to answer
		p.hasId = true;

		// No byte below 0x20 other than the separators.
		for (char c : text)
			if (static_cast<unsigned char>(c) < 0x20 && c != kUS) return p;

		Request r;
		r.id = p.id;
		r.op = f[3];
		if (r.op.empty() || r.op.size() > 32 || r.op.find('=') != std::string::npos) return p;
		for (size_t i = 4; i < f.size(); ++i)
		{
			const size_t eq = f[i].find('=');
			if (eq == std::string::npos || eq == 0) return p;
			r.fields.emplace_back(f[i].substr(0, eq), f[i].substr(eq + 1));
		}
		p.req = std::move(r);
		p.status = ParseStatus::Ok;
		return p;
	}

	std::string EncodeReply(uint32_t id, Status status, const std::string& reason,
	                        const std::vector<std::pair<std::string, std::string>>& fields)
	{
		std::string text = Clean(reason, kMaxReason);
		if (text.empty() && status != Status::Ok)
			text = status == Status::Refused ? "Not allowed." : "The request failed.";

		std::string m = std::string("res") + kUS + "1" + kUS + std::to_string(id) + kUS + StatusText(status) + kUS + text;
		for (const auto& kv : fields)
		{
			const std::string key = Clean(kv.first, 64);
			if (key.empty() || key.find('=') != std::string::npos) continue;
			const std::string one = std::string(1, kUS) + key + "=" + Clean(kv.second, kMaxReason);
			if (m.size() + one.size() > kMaxMessage) break;
			m += one;
		}
		return m;
	}
}
