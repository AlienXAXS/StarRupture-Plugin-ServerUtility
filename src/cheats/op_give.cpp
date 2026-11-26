#include "op_give.h"

namespace OpGive
{
	namespace
	{
		Env g_env;

		CheatDispatch::Result Refuse(const char* text)
		{
			CheatDispatch::Result r;
			r.status = CheatWire::Status::Refused;
			r.reason = text;
			return r;
		}

		// 1 to 9999 as plain digits; anything else (empty, a sign, a letter, too long) is -1.
		int ParseCount(const std::string* text)
		{
			if (!text || text->empty() || text->size() > 5)
				return -1;
			int n = 0;
			for (const char c : *text)
			{
				if (c < '0' || c > '9') return -1;
				n = n * 10 + (c - '0');
			}
			return (n >= kMinCount && n <= kMaxCount) ? n : -1;
		}
	}

	void SetEnv(Env env) { g_env = std::move(env); }

	bool Available()
	{
		return g_env.available && g_env.give && g_env.available();
	}

	CheatDispatch::Result Handle(const CheatDispatch::Context& ctx, const CheatWire::Request& req)
	{
		if (!Available())
			return Refuse(kTextOff);

		const int count = ParseCount(req.Get("count"));
		if (count < 0)
			return Refuse(kTextBadCount);

		const std::string* item = req.Get("item");
		if (!item || item->empty() || item->size() > kMaxItemBytes)
			return Refuse(kTextNoItem);

		const GiveResult g = g_env.give(ctx, *item, count, req.id);
		switch (g.outcome)
		{
		case GiveResult::Outcome::Gave:
		{
			CheatDispatch::Result r;
			r.status = CheatWire::Status::Ok;
			r.reason = "Gave " + std::to_string(g.given) + " " + g.name + ".";
			r.fields.emplace_back("item", g.name);
			r.fields.emplace_back("count", std::to_string(g.given));
			return r;
		}
		case GiveResult::Outcome::NoItem:       return Refuse(kTextNoItem);
		case GiveResult::Outcome::NoCharacter:  return Refuse(kTextNoCharacter);
		case GiveResult::Outcome::NothingAdded: return Refuse(kTextNothingAdded);
		case GiveResult::Outcome::Failed:       break;
		}
		CheatDispatch::Result r;
		r.status = CheatWire::Status::Error;
		r.reason = kTextFailed;
		return r;
	}
}
