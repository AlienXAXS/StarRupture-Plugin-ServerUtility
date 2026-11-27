#include "op_mine.h"
#include "op_power.h"

namespace OpMine
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

		void Audit(const CheatDispatch::Context& ctx, uint32_t reqId, const std::string& what)
		{
			if (!g_env.log) return;
			const std::string who = ctx.playerId ? *ctx.playerId : std::string();
			g_env.log("req " + std::to_string(reqId) + ": " + who + " asked to " + what);
		}
	}

	void SetEnv(Env env) { g_env = std::move(env); }

	bool Available()
	{
		return g_env.available && g_env.inWorld && g_env.current && g_env.apply && g_env.available();
	}

	CheatDispatch::Fields StateFields(const std::string& playerId)
	{
		if (!g_env.current) return {};
		const State s = g_env.current(playerId);
		return {
			{ "mine", s.boost == 1.0f ? std::string("off") : OpPower::FormatValue(s.boost) },
			{ "heat", s.heat ? "on" : "off" },
		};
	}

	CheatDispatch::Result Handle(const CheatDispatch::Context& ctx, const CheatWire::Request& req)
	{
		if (!Available())
			return Make(CheatWire::Status::Refused, kTextOff);

		// heat=on|off: on turns overheating off.
		std::optional<bool> heat;
		if (const std::string* h = req.Get("heat"))
		{
			if (*h == "on") heat = true;
			else if (*h == "off") heat = false;
			else return Make(CheatWire::Status::Refused, CheatDispatch::kTextBadRequest);
		}

		// boost=<mult>: "off" or 1 puts it back.
		std::optional<float> boost;
		if (const std::string* b = req.Get("boost"))
		{
			if (*b == "off") boost = 1.0f;
			else
			{
				double v = 0;
				if (!OpPower::ParseValue(*b, v))
					return Make(CheatWire::Status::Refused, CheatDispatch::kTextBadRequest);
				if (v < kBoostMin || v > kBoostMax)
					return Make(CheatWire::Status::Refused, kTextRange);
				boost = static_cast<float>(v);
			}
		}

		if (!heat && !boost)
			return Make(CheatWire::Status::Refused, kTextPickOne);

		if (!g_env.inWorld())
			return Make(CheatWire::Status::Error, kTextNoWorld);

		Audit(ctx, req.id, std::string("set mining") + (heat ? (*heat ? " heat off" : " heat back") : "")
			+ (boost ? " boost " + OpPower::FormatValue(*boost) : std::string()));
		if (!g_env.apply(ctx, req.id, heat, boost))
			return Make(CheatWire::Status::Error, kTextFailed);

		const std::string who = ctx.playerId ? *ctx.playerId : std::string();
		const State s = g_env.current(who);
		const bool boosted = s.boost != 1.0f;
		std::string text;
		if (!s.heat && !boosted) text = kTextBothOff;
		else if (s.heat && boosted) text = "Mining heat off, boost " + OpPower::FormatValue(s.boost) + "x.";
		else if (s.heat) text = kTextHeatOnly;
		else text = "Mining boost " + OpPower::FormatValue(s.boost) + "x.";

		CheatDispatch::Result res = Make(CheatWire::Status::Ok, text);
		res.fields.emplace_back("heat", s.heat ? "on" : "off");
		res.fields.emplace_back("boost", OpPower::FormatValue(s.boost));
		return res;
	}
}
