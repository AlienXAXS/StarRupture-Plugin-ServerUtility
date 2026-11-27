#include "op_enemy.h"

namespace OpEnemy
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

		// on|off only.
		bool ParseSwitch(const std::string& text, bool& out)
		{
			if (text == "on")  { out = true;  return true; }
			if (text == "off") { out = false; return true; }
			return false;
		}

		void Audit(const CheatDispatch::Context& ctx, uint32_t reqId, const std::string& what)
		{
			if (!g_env.log) return;
			const std::string who = ctx.playerId ? *ctx.playerId : std::string();
			g_env.log("req " + std::to_string(reqId) + ": " + who + " asked to " + what);
		}

		const char* OnOff(bool on) { return on ? "on" : "off"; }
	}

	void SetEnv(Env env) { g_env = std::move(env); }

	bool Available()
	{
		return g_env.available && g_env.inWorld && g_env.aiDisabled && g_env.oneHit && g_env.tracked &&
		       g_env.setAi && g_env.setOneHit && g_env.available();
	}

	CheatDispatch::Fields StateFields(const std::string&)
	{
		if (!g_env.aiDisabled || !g_env.oneHit) return {};
		return { { "ai", OnOff(g_env.aiDisabled()) }, { "onehit", OnOff(g_env.oneHit()) } };
	}

	CheatDispatch::Result Handle(const CheatDispatch::Context& ctx, const CheatWire::Request& req)
	{
		if (!Available())
			return Make(CheatWire::Status::Refused, kTextOff);

		const std::string* aiText = req.Get("ai");
		const std::string* hitText = req.Get("onehit");
		bool ai = false, hit = false;
		if ((aiText && !ParseSwitch(*aiText, ai)) || (hitText && !ParseSwitch(*hitText, hit)))
			return Make(CheatWire::Status::Refused, CheatDispatch::kTextBadRequest);
		if (!aiText && !hitText)
			return Make(CheatWire::Status::Refused, kTextPickOne);

		if (!g_env.inWorld())
			return Make(CheatWire::Status::Error, kTextNoWorld);

		std::string text;
		if (aiText)
		{
			Audit(ctx, req.id, std::string("turn Disable AI ") + OnOff(ai));
			if (!g_env.setAi(ctx, req.id, ai))
				return Make(CheatWire::Status::Error, kTextFailed);
			text = ai ? kTextAiOff : kTextAiOn;
		}
		if (hitText)
		{
			Audit(ctx, req.id, std::string("turn One-Hit Kill ") + OnOff(hit));
			if (!g_env.setOneHit(ctx, req.id, hit))
				return Make(CheatWire::Status::Error, kTextFailed);
			if (!text.empty()) text += " ";
			text += hit ? kTextOneHitOn : kTextOneHitOff;
		}

		CheatDispatch::Result r = Make(CheatWire::Status::Ok, text);
		r.fields.emplace_back("ai", OnOff(g_env.aiDisabled()));
		r.fields.emplace_back("onehit", OnOff(g_env.oneHit()));
		r.fields.emplace_back("tracked", std::to_string(g_env.tracked()));
		return r;
	}
}
