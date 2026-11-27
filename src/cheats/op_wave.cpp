#include "op_wave.h"

#include <cctype>

namespace OpWave
{
	namespace
	{
		Env g_env;

		using CheatDispatch::Make;

		CheatDispatch::Result Refuse(const char* text) { return Make(CheatWire::Status::Refused, text); }
		CheatDispatch::Result Failed(const char* text = kTextFailed) { return Make(CheatWire::Status::Error, text); }

		CheatDispatch::Result Done(const char* text, const char* state)
		{
			CheatDispatch::Result r = Make(CheatWire::Status::Ok, text);
			r.fields.emplace_back("state", state);
			return r;
		}

		bool EqualsNoCase(const std::string& a, const char* b)
		{
			size_t i = 0;
			for (; i < a.size() && b[i]; ++i)
				if (std::tolower(static_cast<unsigned char>(a[i])) != b[i]) return false;
			return i == a.size() && !b[i];
		}

		// The wave type a start asks for: absent means Heat; 0 when it is not one we know.
		int ParseType(const std::string* text)
		{
			if (!text) return kTypeHeat;
			if (EqualsNoCase(*text, "heat")) return kTypeHeat;
			if (EqualsNoCase(*text, "cold")) return kTypeCold;
			return 0;
		}
	}

	void SetEnv(Env env) { g_env = std::move(env); }

	bool Available()
	{
		return g_env.available && g_env.state && g_env.start && g_env.pause && g_env.resume && g_env.cancel && g_env.available();
	}

	CheatDispatch::Fields StateFields(const std::string&)
	{
		if (!g_env.state) return {};
		const std::optional<State> now = g_env.state();
		if (!now) return {};
		return { { "wave", !now->inProgress ? "none" : now->paused ? "paused" : "running" } };
	}

	CheatDispatch::Result Handle(const CheatDispatch::Context& ctx, const CheatWire::Request& req)
	{
		if (!Available())
			return Refuse(kTextOff);

		const std::string* what = req.Get("do");
		if (!what)
			return Refuse(CheatDispatch::kTextBadRequest);
		if (*what == "skip")
			return Refuse(CheatDispatch::kTextNotYet);
		const bool start  = *what == "start";
		const bool pause  = *what == "pause";
		const bool resume = *what == "resume";
		if (!start && !pause && !resume && *what != "cancel")
			return Refuse(CheatDispatch::kTextBadRequest);

		int type = 0;
		if (start)
		{
			type = ParseType(req.Get("type"));
			if (type == 0)
				return Refuse(CheatDispatch::kTextBadRequest);
		}

		const std::optional<State> now = g_env.state();
		if (!now)
			return Failed(kTextNoWorld);

		if (g_env.log)
		{
			const std::string who = ctx.playerId ? *ctx.playerId : std::string();
			g_env.log("req " + std::to_string(req.id) + ": " + who + " asked to " + *what + " the wave");
		}

		if (start)
		{
			if (now->inProgress) return Refuse(kTextAlreadyWave);
			return g_env.start(type) ? Done(kTextStarted, "running") : Failed();
		}
		if (!now->inProgress)
			return Refuse(kTextNoWave);
		if (pause)
		{
			if (now->paused) return Refuse(kTextAlreadyPause);
			return g_env.pause(ctx, req.id) ? Done(kTextPaused, "paused") : Failed();
		}
		if (resume)
		{
			if (!now->paused) return Refuse(kTextNotPaused);
			return g_env.resume(ctx, req.id) ? Done(kTextResumed, "running") : Failed();
		}
		return g_env.cancel(ctx, req.id) ? Done(kTextCancelled, "idle") : Failed();
	}
}
