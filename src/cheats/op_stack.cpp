#include "op_stack.h"

namespace OpStack
{
	namespace
	{
		Env g_env;

		using CheatDispatch::Make;

		CheatDispatch::Result Done(const std::string& text, int size)
		{
			CheatDispatch::Result r = Make(CheatWire::Status::Ok, text);
			r.fields.emplace_back("size", std::to_string(size));
			return r;
		}
	}

	std::string TextSet(int size) { return "Stack size set to " + std::to_string(size) + "."; }

	void SetEnv(Env env) { g_env = std::move(env); }

	bool Available()
	{
		return g_env.available && g_env.current && g_env.set && g_env.clear && g_env.available();
	}

	bool ParseSize(const std::string& text, int& out)
	{
		if (text.empty() || text.size() > 6)
			return false;
		int n = 0;
		for (char c : text)
		{
			if (c < '0' || c > '9') return false;
			n = n * 10 + (c - '0');
		}
		if (n > kMaxStack)
			return false;
		out = n;
		return true;
	}

	CheatDispatch::Fields StateFields(const std::string&)
	{
		if (!g_env.current || !g_env.overridden) return {};
		return { { "stack", g_env.overridden() ? std::to_string(g_env.current()) : "off" } };
	}

	CheatDispatch::Result Handle(const CheatDispatch::Context& ctx, const CheatWire::Request& req)
	{
		if (!Available())
			return Make(CheatWire::Status::Refused, kTextOff);

		const std::string* text = req.Get("size");
		int size = 0;
		if (!text || !ParseSize(*text, size))
			return Make(CheatWire::Status::Refused, CheatDispatch::kTextBadRequest);

		if (g_env.log)
		{
			const std::string who = ctx.playerId ? *ctx.playerId : std::string();
			g_env.log("req " + std::to_string(req.id) + ": " + who + " asked for stack size " + std::to_string(size));
		}

		if (size == 0)
			return g_env.clear(ctx, req.id) ? Done(kTextCleared, g_env.current()) : Make(CheatWire::Status::Error, kTextFailed);
		return g_env.set(ctx, req.id, size) ? Done(TextSet(size), g_env.current()) : Make(CheatWire::Status::Error, kTextFailed);
	}

	Action Decide(const TickState& s)
	{
		if (!s.featureAvailable || s.target <= 0)
			return (s.lastApplied != 0 || s.itemsRaised > 0) ? Action::Restore : Action::None;
		if (s.itemsTracked <= 0 || s.worldChanged || s.catalogueChanged) return Action::Rescan;
		if (s.target != s.lastApplied) return Action::Apply;
		return Action::None;
	}
}
