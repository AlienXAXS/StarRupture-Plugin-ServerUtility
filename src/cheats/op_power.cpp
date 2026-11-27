#include "op_power.h"

#include <cstdio>
#include <cstdlib>

namespace OpPower
{
	namespace
	{
		Env g_env;

		using CheatDispatch::Make;

		bool ValidName(const std::string* name)
		{
			if (!name || name->empty() || name->size() > kMaxName) return false;
			for (char c : *name)
				if (static_cast<unsigned char>(c) < 0x20) return false;
			return true;
		}

		void Audit(const CheatDispatch::Context& ctx, uint32_t reqId, const std::string& what)
		{
			if (!g_env.log) return;
			const std::string who = ctx.playerId ? *ctx.playerId : std::string();
			g_env.log("req " + std::to_string(reqId) + ": " + who + " asked to " + what);
		}

		CheatDispatch::Result Changed(const std::string& text, const Building& b)
		{
			CheatDispatch::Result r = Make(CheatWire::Status::Ok, text);
			r.fields.emplace_back("building", b.display);
			r.fields.emplace_back("value", FormatValue(b.value));
			r.fields.emplace_back("default", FormatValue(b.defaultValue));
			return r;
		}

		std::string Entry(const Building& b)
		{
			return b.asset + "~" + b.type + "~" + FormatValue(b.value) + "~" + FormatValue(b.defaultValue);
		}

		// count, then as many buildings as fit (at most kMaxListed, and the reply stays inside the
		// message limit), then more=1 when some were left out.
		CheatDispatch::Result List(const CheatWire::Request& req)
		{
			std::optional<std::vector<Building>> all = g_env.list();
			if (!all)
				return Make(CheatWire::Status::Error, kTextNoWorld);

			CheatDispatch::Result r = Make(CheatWire::Status::Ok, std::string());
			r.fields.emplace_back("count", std::to_string(all->size()));
			bool cut = false;
			for (size_t i = 0; i < all->size(); ++i)
			{
				// Room for the cut marker is always kept.
				CheatDispatch::Fields next = r.fields;
				next.emplace_back("b" + std::to_string(i), Entry((*all)[i]));
				next.emplace_back("more", "1");
				// The encoder drops fields past the limit from the end, so the marker missing from
				// the end of the encoded reply means this building does not fit.
				const std::string wire = CheatWire::EncodeReply(req.id, CheatWire::Status::Ok, std::string(), next);
				const std::string tail = "more=1";
				if (i >= kMaxListed || wire.size() < tail.size() || wire.compare(wire.size() - tail.size(), tail.size(), tail) != 0)
				{
					cut = true;
					break;
				}
				next.pop_back();
				r.fields = std::move(next);
			}
			if (cut) r.fields.emplace_back("more", "1");
			return r;
		}
	}

	std::string Slot(const std::string& asset) { return "power:" + asset; }

	std::string FormatValue(float value)
	{
		char buf[48];
		std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(value));
		std::string s = buf;
		while (!s.empty() && s.back() == '0') s.pop_back();
		if (!s.empty() && s.back() == '.') s.pop_back();
		return s;
	}

	std::string TextSet(const std::string& building, float value)
	{
		return building + " power set to " + FormatValue(value) + ".";
	}

	std::string TextReset(const std::string& building, float defaultValue)
	{
		return building + " power back to " + FormatValue(defaultValue) + ".";
	}

	void SetEnv(Env env) { g_env = std::move(env); }

	bool Available()
	{
		return g_env.available && g_env.find && g_env.list && g_env.edited && g_env.set && g_env.reset &&
		       g_env.resetAll && g_env.available();
	}

	bool ParseValue(const std::string& text, double& out)
	{
		if (text.empty() || text.size() > 24) return false;
		size_t i = (text[0] == '-') ? 1 : 0;
		size_t digits = 0, fraction = 0;
		bool dot = false;
		for (; i < text.size(); ++i)
		{
			const char c = text[i];
			if (c == '.' && !dot) { dot = true; continue; }
			if (c < '0' || c > '9') return false;
			++(dot ? fraction : digits);
		}
		if (digits == 0 || (dot && fraction == 0)) return false;
		out = std::strtod(text.c_str(), nullptr);
		return true;
	}

	CheatDispatch::Fields StateFields(const std::string&)
	{
		if (!g_env.edited) return {};
		const int n = g_env.edited();
		return { { "power", n > 0 ? std::to_string(n) : "off" } };
	}

	CheatDispatch::Result Handle(const CheatDispatch::Context& ctx, const CheatWire::Request& req)
	{
		if (!Available())
			return Make(CheatWire::Status::Refused, kTextOff);

		const std::string* action = req.Get("do");
		if (!action)
			return Make(CheatWire::Status::Refused, CheatDispatch::kTextBadRequest);

		if (*action == "list")
			return List(req);

		if (*action == "reset-all")
		{
			Audit(ctx, req.id, "reset every building's power");
			return g_env.resetAll(ctx, req.id) ? Make(CheatWire::Status::Ok, kTextResetAll)
			                                   : Make(CheatWire::Status::Error, kTextFailed);
		}

		const bool isSet = *action == "set";
		if (!isSet && *action != "reset")
			return Make(CheatWire::Status::Refused, CheatDispatch::kTextBadRequest);

		const std::string* name = req.Get("building");
		if (!ValidName(name))
			return Make(CheatWire::Status::Refused, CheatDispatch::kTextBadRequest);

		float value = 0;
		if (isSet)
		{
			const std::string* text = req.Get("value");
			double parsed = 0;
			if (!text || !ParseValue(*text, parsed))
				return Make(CheatWire::Status::Refused, CheatDispatch::kTextBadRequest);
			if (parsed < kMinPower || parsed > kMaxPower)
				return Make(CheatWire::Status::Refused, kTextBounds);
			value = static_cast<float>(parsed);
		}

		Building b;
		switch (g_env.find(*name, b))
		{
		case Lookup::NoWorld:  return Make(CheatWire::Status::Error, kTextNoWorld);
		case Lookup::NotFound: return Make(CheatWire::Status::Refused, kTextNoSuch);
		case Lookup::Found:    break;
		}

		if (isSet)
		{
			Audit(ctx, req.id, "set " + *name + " power to " + FormatValue(value));
			if (!g_env.set(ctx, req.id, *name, value))
				return Make(CheatWire::Status::Error, kTextFailed);
		}
		else
		{
			Audit(ctx, req.id, "reset " + *name + " power");
			if (!g_env.reset(ctx, req.id, *name))
				return Make(CheatWire::Status::Error, kTextFailed);
		}

		// The values in effect now, as the game has them.
		if (g_env.find(*name, b) != Lookup::Found)
			return Make(CheatWire::Status::Error, kTextFailed);
		return isSet ? Changed(TextSet(b.display, b.value), b) : Changed(TextReset(b.display, b.defaultValue), b);
	}
}
