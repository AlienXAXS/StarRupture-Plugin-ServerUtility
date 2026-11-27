#include "op_move.h"
#include "op_power.h"

#include <optional>

namespace OpMove
{
	namespace
	{
		Env g_env;

		using CheatDispatch::Make;

		void Audit(const CheatDispatch::Context& ctx, uint32_t reqId, const std::string& what)
		{
			if (!g_env.log) return;
			const std::string who = ctx.playerId ? *ctx.playerId : std::string();
			g_env.log("req " + std::to_string(reqId) + ": " + who + " asked to " + what);
		}

		std::string SpeedText(float speed)
		{
			return speed == 1.0f ? std::string("off") : OpPower::FormatValue(speed);
		}
	}

	void SetEnv(Env env) { g_env = std::move(env); }

	bool Available()
	{
		return g_env.available && g_env.inWorld && g_env.hasCharacter && g_env.flying && g_env.speed
			&& g_env.setFly && g_env.setSpeed && g_env.available();
	}

	CheatDispatch::Fields StateFields(const std::string& playerId)
	{
		if (!g_env.flying || !g_env.speed) return {};
		return {
			{ "fly",   g_env.flying(playerId) ? "on" : "off" },
			{ "speed", SpeedText(g_env.speed(playerId)) },
		};
	}

	CheatDispatch::Result Handle(const CheatDispatch::Context& ctx, const CheatWire::Request& req)
	{
		if (!Available())
			return Make(CheatWire::Status::Refused, kTextOff);

		std::optional<bool> fly;
		if (const std::string* f = req.Get("fly"))
		{
			if (*f == "on") fly = true;
			else if (*f == "off") fly = false;
			else return Make(CheatWire::Status::Refused, CheatDispatch::kTextBadRequest);
		}

		// speed: absent, off ("off" or 1), or a multiplier.
		std::optional<bool> speedOn;
		float mult = 1.0f;
		if (const std::string* s = req.Get("speed"))
		{
			if (*s == "off") speedOn = false;
			else
			{
				double v = 0;
				if (!OpPower::ParseValue(*s, v))
					return Make(CheatWire::Status::Refused, CheatDispatch::kTextBadRequest);
				if (v == 1.0) speedOn = false;
				else if (v < kSpeedMin || v > kSpeedMax)
					return Make(CheatWire::Status::Refused, kTextRange);
				else { speedOn = true; mult = static_cast<float>(v); }
			}
		}

		if (!fly && !speedOn)
			return Make(CheatWire::Status::Refused, kTextPickOne);

		if (!g_env.inWorld())
			return Make(CheatWire::Status::Error, kTextNoWorld);

		// Taking a setting off needs no character; putting one on does.
		if ((fly.value_or(false) || speedOn.value_or(false)) && !g_env.hasCharacter(ctx))
			return Make(CheatWire::Status::Refused, kTextNoCharacter);

		if (fly)
		{
			Audit(ctx, req.id, *fly ? "turn flying on" : "turn flying off");
			if (!g_env.setFly(ctx, req.id, *fly))
				return Make(CheatWire::Status::Error, kTextFailed);
		}
		if (speedOn)
		{
			Audit(ctx, req.id, *speedOn ? "set speed to " + OpPower::FormatValue(mult) : std::string("turn speed boost off"));
			if (!g_env.setSpeed(ctx, req.id, *speedOn, mult))
				return Make(CheatWire::Status::Error, kTextFailed);
		}

		const std::string who = ctx.playerId ? *ctx.playerId : std::string();
		const bool flying = g_env.flying(who);
		const float speed = g_env.speed(who);

		// The text follows what was asked: fly first, as in the contract's example.
		std::string text;
		if (fly) text = *fly ? kTextFlyOn : kTextFlyOff;
		else text = speed == 1.0f ? std::string(kTextSpeedOff) : "Speed " + OpPower::FormatValue(speed) + "x.";

		CheatDispatch::Result res = Make(CheatWire::Status::Ok, text);
		res.fields.emplace_back("fly", flying ? "on" : "off");
		res.fields.emplace_back("speed", SpeedText(speed));
		return res;
	}
}
