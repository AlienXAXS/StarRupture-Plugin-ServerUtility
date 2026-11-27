#include "op_weapon.h"
#include "op_power.h"

namespace OpWeapon
{
	namespace
	{
		Env g_env;

		using CheatDispatch::Make;

		// One slot's request: absent, off ("off" or 1), or a multiplier.
		enum class Want { Absent, Off, Mult, BadText, OutOfRange };

		Want ParseSlot(const std::string* text, double lo, double hi, float& mult)
		{
			if (!text) return Want::Absent;
			if (*text == "off") return Want::Off;
			double v = 0;
			if (!OpPower::ParseValue(*text, v)) return Want::BadText;
			if (v == 1.0) return Want::Off;
			if (v < lo || v > hi) return Want::OutOfRange;
			mult = static_cast<float>(v);
			return Want::Mult;
		}

		void Audit(const CheatDispatch::Context& ctx, uint32_t reqId, const std::string& what)
		{
			if (!g_env.log) return;
			const std::string who = ctx.playerId ? *ctx.playerId : std::string();
			g_env.log("req " + std::to_string(reqId) + ": " + who + " asked to " + what);
		}

		bool Apply(const CheatDispatch::Context& ctx, uint32_t reqId, Slot slot, Want want, float mult)
		{
			if (want == Want::Absent) return true;
			const char* name = slot == Slot::Damage ? "damage" : "fire rate";
			Audit(ctx, reqId, want == Want::Off ? std::string("turn weapon ") + name + " off"
			                                    : std::string("set weapon ") + name + " to " + OpPower::FormatValue(mult));
			return g_env.set(ctx, reqId, slot, want == Want::Mult, mult);
		}
	}

	void SetEnv(Env env) { g_env = std::move(env); }

	bool Available()
	{
		return g_env.available && g_env.inWorld && g_env.hasWeapon && g_env.current && g_env.set && g_env.available();
	}

	CheatDispatch::Fields StateFields(const std::string& playerId)
	{
		if (!g_env.current) return {};
		const float d = g_env.current(playerId, Slot::Damage);
		const float r = g_env.current(playerId, Slot::Rate);
		if (d == 1.0f && r == 1.0f) return { { "weapon", "off" } };
		return { { "weapon", OpPower::FormatValue(d) + "," + OpPower::FormatValue(r) } };
	}

	CheatDispatch::Result Handle(const CheatDispatch::Context& ctx, const CheatWire::Request& req)
	{
		if (!Available())
			return Make(CheatWire::Status::Refused, kTextOff);

		float damage = 1.0f, rate = 1.0f;
		const Want dw = ParseSlot(req.Get("damage"), kDamageMin, kDamageMax, damage);
		const Want rw = ParseSlot(req.Get("rate"), kRateMin, kRateMax, rate);
		if (dw == Want::BadText || rw == Want::BadText)
			return Make(CheatWire::Status::Refused, CheatDispatch::kTextBadRequest);
		if (dw == Want::OutOfRange || rw == Want::OutOfRange)
			return Make(CheatWire::Status::Refused, kTextRange);
		if (dw == Want::Absent && rw == Want::Absent)
			return Make(CheatWire::Status::Refused, kTextPickOne);

		if (!g_env.inWorld())
			return Make(CheatWire::Status::Error, kTextNoWorld);

		// Taking a boost off needs no weapon; putting one on does.
		if ((dw == Want::Mult || rw == Want::Mult) && !g_env.hasWeapon(ctx))
			return Make(CheatWire::Status::Refused, kTextNoWeapon);

		if (!Apply(ctx, req.id, Slot::Damage, dw, damage) || !Apply(ctx, req.id, Slot::Rate, rw, rate))
			return Make(CheatWire::Status::Error, kTextFailed);

		const std::string who = ctx.playerId ? *ctx.playerId : std::string();
		const float d = g_env.current(who, Slot::Damage);
		const float r = g_env.current(who, Slot::Rate);
		const bool allOff = d == 1.0f && r == 1.0f;
		CheatDispatch::Result res = Make(CheatWire::Status::Ok, allOff
			? std::string(kTextBoostOff)
			: "Weapon damage " + OpPower::FormatValue(d) + "x, fire rate " + OpPower::FormatValue(r) + "x.");
		res.fields.emplace_back("damage", OpPower::FormatValue(d));
		res.fields.emplace_back("rate", OpPower::FormatValue(r));
		return res;
	}
}
