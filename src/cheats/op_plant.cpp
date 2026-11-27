#include "op_plant.h"
#include "op_power.h"

#include <atomic>
#include <cmath>
#include <map>
#include <mutex>

namespace OpPlant
{
	namespace
	{
		Env g_env;

		std::mutex g_lock;
		std::map<std::string, int> g_mult;
		std::atomic<int> g_active{ 0 };

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
		return g_env.available && g_env.inWorld && g_env.apply && g_env.available();
	}

	int MultiplierFor(const std::string& playerId)
	{
		if (g_active.load(std::memory_order_acquire) == 0)
			return 1;
		std::lock_guard<std::mutex> hold(g_lock);
		const auto it = g_mult.find(playerId);
		return it == g_mult.end() ? 1 : it->second;
	}

	void SetMultiplier(const std::string& playerId, int mult)
	{
		bool first = false, last = false;
		{
			std::lock_guard<std::mutex> hold(g_lock);
			if (mult > 1) g_mult[playerId] = mult;
			else g_mult.erase(playerId);
			const int now = static_cast<int>(g_mult.size());
			const int before = g_active.exchange(now, std::memory_order_acq_rel);
			first = before == 0 && now > 0;
			last  = before > 0 && now == 0;
		}
		if (first && g_env.installHook) g_env.installHook();
		if (last && g_env.removeHook) g_env.removeHook();
	}

	CheatDispatch::Fields StateFields(const std::string& playerId)
	{
		if (!g_env.available) return {};
		const int m = MultiplierFor(playerId);
		return { { "plant", m > 1 ? std::to_string(m) : std::string("off") } };
	}

	CheatDispatch::Result Handle(const CheatDispatch::Context& ctx, const CheatWire::Request& req)
	{
		if (!Available())
			return Make(CheatWire::Status::Refused, kTextOff);

		const std::string* m = req.Get("mult");
		if (!m)
			return Make(CheatWire::Status::Refused, CheatDispatch::kTextBadRequest);

		int mult = 1;
		if (*m != "off")
		{
			double v = 0;
			if (!OpPower::ParseValue(*m, v))
				return Make(CheatWire::Status::Refused, CheatDispatch::kTextBadRequest);
			if (v < kMultMin || v > kMultMax || v != std::floor(v))
				return Make(CheatWire::Status::Refused, kTextRange);
			mult = static_cast<int>(v);
		}

		if (!g_env.inWorld())
			return Make(CheatWire::Status::Error, kTextNoWorld);

		Audit(ctx, req.id, "set plant pickup " + std::to_string(mult) + "x");
		if (!g_env.apply(ctx, req.id, mult))
			return Make(CheatWire::Status::Error, kTextFailed);

		const std::string who = ctx.playerId ? *ctx.playerId : std::string();
		SetMultiplier(who, mult);

		CheatDispatch::Result res = Make(CheatWire::Status::Ok,
			mult > 1 ? "Plant pickup " + std::to_string(mult) + "x." : std::string(kTextNormal));
		res.fields.emplace_back("mult", std::to_string(mult));
		return res;
	}
}
