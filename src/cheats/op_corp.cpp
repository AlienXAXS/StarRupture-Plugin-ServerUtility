#include "op_corp.h"

namespace OpCorp
{
	namespace
	{
		Env g_env;

		CheatDispatch::Result Make(CheatWire::Status status, const std::string& text)
		{
			CheatDispatch::Result r;
			r.status = status;
			r.reason = text;
			return r;
		}
	}

	std::string TextSet(int points) { return "Data points set to " + std::to_string(points) + "."; }

	void SetEnv(Env env) { g_env = std::move(env); }

	bool Available()
	{
		return g_env.available && g_env.current && g_env.set && g_env.available();
	}

	bool ParsePoints(const std::string& text, int& out)
	{
		if (text.empty() || text.size() > 9)
			return false;
		int n = 0;
		for (char c : text)
		{
			if (c < '0' || c > '9') return false;
			n = n * 10 + (c - '0');
		}
		out = n;
		return true;
	}

	CheatDispatch::Fields StateFields(const std::string&)
	{
		if (!g_env.current) return {};
		const std::optional<int> now = g_env.current();
		if (!now) return {};
		return { { "points", std::to_string(*now) } };
	}

	CheatDispatch::Result Handle(const CheatDispatch::Context& ctx, const CheatWire::Request& req)
	{
		if (!Available())
			return Make(CheatWire::Status::Refused, kTextOff);

		const std::string* text = req.Get("points");
		if (!text)
			return Make(CheatWire::Status::Refused, CheatDispatch::kTextBadRequest);
		int points = 0;
		if (!ParsePoints(*text, points))
			return Make(CheatWire::Status::Refused, kTextBounds);

		if (!g_env.current())
			return Make(CheatWire::Status::Error, kTextNoWorld);

		if (g_env.log)
		{
			const std::string who = ctx.playerId ? *ctx.playerId : std::string();
			g_env.log("req " + std::to_string(req.id) + ": " + who + " asked for " + std::to_string(points) + " data points");
		}

		if (!g_env.set(ctx, req.id, points))
			return Make(CheatWire::Status::Error, kTextFailed);

		const int now = g_env.current().value_or(points);
		CheatDispatch::Result r = Make(CheatWire::Status::Ok, TextSet(now));
		r.fields.emplace_back("points", std::to_string(now));
		return r;
	}
}
