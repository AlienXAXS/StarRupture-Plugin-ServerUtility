#include "op_stat.h"
#include "op_power.h"

#include <algorithm>

namespace OpStat
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

		std::string Title(std::string stat)
		{
			if (!stat.empty() && stat[0] >= 'a' && stat[0] <= 'z')
				stat[0] = static_cast<char>(stat[0] - 'a' + 'A');
			return stat;
		}

		bool IsLocked(const std::string& playerId, const std::string& stat)
		{
			if (!g_env.locked) return false;
			const std::vector<std::string> l = g_env.locked(playerId);
			return std::find(l.begin(), l.end(), stat) != l.end();
		}
	}

	const std::vector<std::string>& Stats()
	{
		static const std::vector<std::string> names = {
			"health", "energy", "shield", "hydration", "calories", "oxygen", "toxicity",
			"radiation", "heat", "drain", "corrosion", "infection", "temperature",
		};
		return names;
	}

	std::string SlotFor(const std::string& stat) { return std::string(kSlotPrefix) + stat; }

	void SetEnv(Env env) { g_env = std::move(env); }

	bool Available()
	{
		return g_env.available && g_env.inWorld && g_env.locked && g_env.range && g_env.read && g_env.apply && g_env.available();
	}

	CheatDispatch::Fields StateFields(const std::string& playerId)
	{
		if (!g_env.locked) return {};
		const std::vector<std::string> l = g_env.locked(playerId);
		std::string list;
		for (const std::string& s : Stats())
		{
			if (std::find(l.begin(), l.end(), s) == l.end()) continue;
			if (!list.empty()) list += ',';
			list += s;
		}
		return { { "lock", list.empty() ? std::string("none") : list } };
	}

	CheatDispatch::Result Handle(const CheatDispatch::Context& ctx, const CheatWire::Request& req)
	{
		if (!Available())
			return Make(CheatWire::Status::Refused, kTextOff);

		const std::string* statName = req.Get("stat");
		if (!statName || std::find(Stats().begin(), Stats().end(), *statName) == Stats().end())
			return Make(CheatWire::Status::Refused, CheatDispatch::kTextBadRequest);
		const std::string& stat = *statName;

		std::optional<double> wanted;
		if (const std::string* v = req.Get("value"))
		{
			double parsed = 0;
			if (!OpPower::ParseValue(*v, parsed))
				return Make(CheatWire::Status::Refused, CheatDispatch::kTextBadRequest);
			wanted = parsed;
		}

		std::optional<bool> lock;
		if (const std::string* l = req.Get("lock"))
		{
			if (*l == "on") lock = true;
			else if (*l == "off") lock = false;
			else return Make(CheatWire::Status::Refused, CheatDispatch::kTextBadRequest);
		}

		if (!wanted && !lock)
			return Make(CheatWire::Status::Refused, kTextPickOne);

		if (!g_env.inWorld())
			return Make(CheatWire::Status::Error, kTextNoWorld);

		// The value is clamped to the server's own Min..Max for this stat.
		std::optional<float> value;
		if (wanted)
		{
			const std::optional<Range> range = g_env.range(ctx, stat);
			if (!range || range->max < range->min)
				return Make(CheatWire::Status::Error, kTextFailed);
			value = static_cast<float>(std::min<double>(std::max<double>(*wanted, range->min), range->max));
		}

		Audit(ctx, req.id, "set " + stat + (value ? " to " + OpPower::FormatValue(*value) : std::string())
			+ (lock ? (*lock ? " and lock it" : " and unlock it") : std::string()));
		if (!g_env.apply(ctx, req.id, stat, value, lock))
			return Make(CheatWire::Status::Error, kTextFailed);

		const std::string who = ctx.playerId ? *ctx.playerId : std::string();
		const bool locked = IsLocked(who, stat);
		const std::optional<float> now = g_env.read(ctx, stat);

		const std::string name = Title(stat);
		const std::string shown = now ? OpPower::FormatValue(*now) : (value ? OpPower::FormatValue(*value) : std::string());
		std::string text;
		if (locked) text = name + " locked" + (shown.empty() ? std::string(".") : " at " + shown + ".");
		else if (!value) text = name + " unlocked.";
		else text = name + " set to " + shown + ".";

		CheatDispatch::Result res = Make(CheatWire::Status::Ok, text);
		if (!shown.empty()) res.fields.emplace_back("value", shown);
		res.fields.emplace_back("lock", locked ? "on" : "off");
		return res;
	}
}
