#include "op_skill.h"

#include <algorithm>

namespace OpSkill
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

	std::string TextOne(int index, int level) { return "Skill " + std::to_string(index) + " set to " + std::to_string(level) + "."; }
	std::string TextAll(int level) { return "All skills set to " + std::to_string(level) + "."; }

	void SetEnv(Env env) { g_env = std::move(env); }

	bool Available()
	{
		return g_env.available && g_env.skillCount && g_env.setSkill && g_env.available();
	}

	bool ParseWhole(const std::string& text, int& out)
	{
		if (text.empty() || text.size() > 3)
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

	CheatDispatch::Result Handle(const CheatDispatch::Context& ctx, const CheatWire::Request& req)
	{
		if (!Available())
			return Make(CheatWire::Status::Refused, kTextOff);

		// Exactly one of skill=<index> and all=1, and a level.
		const std::string* skill = req.Get("skill");
		const std::string* all = req.Get("all");
		const std::string* levelText = req.Get("level");
		if (!levelText || (skill != nullptr) == (all != nullptr))
			return Make(CheatWire::Status::Refused, CheatDispatch::kTextBadRequest);
		if (all && *all != "1")
			return Make(CheatWire::Status::Refused, CheatDispatch::kTextBadRequest);

		int level = 0;
		if (!ParseWhole(*levelText, level))
			return Make(CheatWire::Status::Refused, CheatDispatch::kTextBadRequest);
		if (level > kMaxLevel)
			return Make(CheatWire::Status::Refused, kTextRange);

		int index = 0;
		if (skill && !ParseWhole(*skill, index))
			return Make(CheatWire::Status::Refused, CheatDispatch::kTextBadRequest);

		const std::optional<int> count = g_env.skillCount(ctx);
		if (!count)
			return Make(CheatWire::Status::Error, kTextNoWorld);
		const int usable = std::min(*count, kMaxSkills);
		if (usable <= 0 || (skill && index >= usable))
			return Make(CheatWire::Status::Refused, kTextNoSkill);

		if (g_env.log)
		{
			const std::string who = ctx.playerId ? *ctx.playerId : std::string();
			g_env.log("req " + std::to_string(req.id) + ": " + who + " asked to set "
				+ (skill ? "skill " + std::to_string(index) : std::string("all skills")) + " to level " + std::to_string(level));
		}

		const int first = skill ? index : 0;
		const int last = skill ? index + 1 : usable;
		for (int i = first; i < last; ++i)
		{
			if (!g_env.setSkill(ctx, req.id, i, level))
				return Make(CheatWire::Status::Error, kTextFailed);
		}

		CheatDispatch::Result r = Make(CheatWire::Status::Ok, skill ? TextOne(index, level) : TextAll(level));
		r.fields.emplace_back("level", std::to_string(level));
		return r;
	}
}
