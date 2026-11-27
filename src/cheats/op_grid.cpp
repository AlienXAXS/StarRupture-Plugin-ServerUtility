#include "op_grid.h"
#include "op_power.h"

#include <cmath>

namespace OpGrid
{
	namespace
	{
		Env g_env;

		using CheatDispatch::Make;

		// A whole number from request text. False for anything else.
		bool ParseInt(const std::string* text, int& out)
		{
			double v = 0;
			if (!text || !OpPower::ParseValue(*text, v) || v != std::floor(v) || v < -1e6 || v > 1e6)
				return false;
			out = static_cast<int>(v);
			return true;
		}
	}

	void SetEnv(Env env) { g_env = std::move(env); }

	bool Available()
	{
		return g_env.available && g_env.inWorld && g_env.current && g_env.occupied && g_env.resize && g_env.available();
	}

	CheatDispatch::Fields StateFields(const std::string& playerId)
	{
		if (!g_env.current) return {};
		const std::optional<Size> now = g_env.current(playerId);
		if (!now) return {};
		return { { "grid", std::to_string(now->cols) + "x" + std::to_string(now->rows) } };
	}

	CheatDispatch::Result Handle(const CheatDispatch::Context& ctx, const CheatWire::Request& req)
	{
		if (!Available())
			return Make(CheatWire::Status::Refused, kTextOff);

		int cols = 0, rows = 0;
		if (!ParseInt(req.Get("cols"), cols) || !ParseInt(req.Get("rows"), rows))
			return Make(CheatWire::Status::Refused, CheatDispatch::kTextBadRequest);
		if (cols < kMin || cols > kMax || rows < kMin || rows > kMax)
			return Make(CheatWire::Status::Refused, kTextRange);

		if (!g_env.inWorld())
			return Make(CheatWire::Status::Error, kTextNoWorld);

		// Never shrink a grid that holds items: the extra slots would drop them.
		const std::optional<int> used = g_env.occupied(ctx);
		if (!used)
			return Make(CheatWire::Status::Refused, kTextNoCheck);
		if (*used > cols * rows)
			return Make(CheatWire::Status::Refused, "Your items need " + std::to_string(*used) + " slots. Pick a bigger grid.");

		if (g_env.log)
		{
			const std::string who = ctx.playerId ? *ctx.playerId : std::string();
			g_env.log("req " + std::to_string(req.id) + ": " + who + " asked to set the grid to "
			          + std::to_string(cols) + "x" + std::to_string(rows));
		}
		if (!g_env.resize(ctx, req.id, cols, rows))
			return Make(CheatWire::Status::Error, kTextFailed);

		CheatDispatch::Result res = Make(CheatWire::Status::Ok,
			"Inventory is now " + std::to_string(cols) + " by " + std::to_string(rows) + ".");
		res.fields.emplace_back("cols", std::to_string(cols));
		res.fields.emplace_back("rows", std::to_string(rows));
		return res;
	}
}
