#include "op_build.h"

#include <atomic>
#include <map>
#include <mutex>

namespace OpBuild
{
	namespace
	{
		Env g_env;

		std::mutex g_lock;
		std::map<std::string, unsigned> g_parts;   // player -> one bit per Part
		std::atomic<int> g_active[kPartCount];     // players with the part on

		constexpr Part kFree[]     = { Part::Cost, Part::Stability };
		constexpr Part kAnywhere[] = { Part::Windows, Part::Benches };

		unsigned Bit(Part p) { return 1u << static_cast<int>(p); }

		using CheatDispatch::Make;

		bool PartAvailable(Part p) { return g_env.available && g_env.available(p); }

		bool UnlockAvailable() { return g_env.unlockAvailable && g_env.unlockAvailable(); }

		void Audit(const CheatDispatch::Context& ctx, uint32_t reqId, const std::string& what)
		{
			if (!g_env.log) return;
			const std::string who = ctx.playerId ? *ctx.playerId : std::string();
			g_env.log("req " + std::to_string(reqId) + ": " + who + " asked to " + what);
		}

		std::string Join(const std::vector<Part>& parts)
		{
			std::string s;
			for (Part p : parts)
			{
				if (!s.empty()) s += ',';
				s += PartName(p);
			}
			return s;
		}

		bool ParseOn(const CheatWire::Request& req, bool& on)
		{
			const std::string* v = req.Get("on");
			if (!v || (*v != "on" && *v != "off")) return false;
			on = *v == "on";
			return true;
		}
	}

	const char* PartName(Part p)
	{
		switch (p)
		{
		case Part::Cost:      return "cost";
		case Part::Stability: return "stability";
		case Part::Windows:   return "windows";
		default:              return "benches";
		}
	}

	const char* FeatureOf(Part p)
	{
		return p == Part::Cost || p == Part::Stability ? kFeatureFree : kFeatureAnywhere;
	}

	void SetEnv(Env env) { g_env = std::move(env); }

	bool Available()
	{
		if (!g_env.inWorld || !g_env.apply) return false;
		for (int i = 0; i < kPartCount; ++i)
			if (PartAvailable(static_cast<Part>(i))) return true;
		return g_env.unlock && UnlockAvailable();
	}

	std::string Rules()
	{
		std::string out;
		const auto add = [&](const char* name) { out += (out.empty() ? "" : ",") + std::string(name); };
		if (PartAvailable(kFree[0]) || PartAvailable(kFree[1])) add("free");
		if (PartAvailable(kAnywhere[0]) || PartAvailable(kAnywhere[1])) add("anywhere");
		if (g_env.unlock && UnlockAvailable()) add("unlock");
		return out;
	}

	const char* FeatureFor(const CheatWire::Request& req)
	{
		const std::string* rule = req.Get("rule");
		if (rule && *rule == "anywhere") return kFeatureAnywhere;
		if (rule && *rule == "unlock") return kFeatureUnlock;
		return kFeatureFree;
	}

	bool PartOn(const std::string& playerId, Part part)
	{
		if (g_active[static_cast<int>(part)].load(std::memory_order_acquire) == 0)
			return false;
		std::lock_guard<std::mutex> hold(g_lock);
		const auto it = g_parts.find(playerId);
		return it != g_parts.end() && (it->second & Bit(part)) != 0;
	}

	void SetPart(const std::string& playerId, Part part, bool on)
	{
		bool first = false, last = false;
		{
			std::lock_guard<std::mutex> hold(g_lock);
			unsigned& bits = g_parts[playerId];
			if (on) bits |= Bit(part); else bits &= ~Bit(part);
			if (bits == 0) g_parts.erase(playerId);
			int now = 0;
			for (const auto& kv : g_parts)
				if (kv.second & Bit(part)) ++now;
			const int before = g_active[static_cast<int>(part)].exchange(now, std::memory_order_acq_rel);
			first = before == 0 && now > 0;
			last  = before > 0 && now == 0;
		}
		if (first && g_env.installHook) g_env.installHook(part);
		if (last && g_env.removeHook) g_env.removeHook(part);
	}

	CheatDispatch::Fields StateFieldsFor(const std::string& playerId, const std::function<bool(const char*)>& allowed)
	{
		CheatDispatch::Fields out;
		if (!g_env.available && !g_env.unlockAvailable) return out;

		if (allowed(kFeatureFree) || allowed(kFeatureAnywhere))
		{
			std::vector<Part> on;
			for (int i = 0; i < kPartCount; ++i)
			{
				const Part p = static_cast<Part>(i);
				if (allowed(FeatureOf(p)) && PartOn(playerId, p)) on.push_back(p);
			}
			out.emplace_back("build", on.empty() ? std::string("none") : Join(on));
		}
		out.emplace_back("unlock", g_env.unlocked && g_env.unlocked() ? "on" : "off");
		return out;
	}

	CheatDispatch::Result Handle(const CheatDispatch::Context& ctx, const CheatWire::Request& req)
	{
		if (!Available())
			return Make(CheatWire::Status::Refused, kTextOff);

		const std::string* rule = req.Get("rule");
		bool on = false;
		if (!rule || !ParseOn(req, on))
			return Make(CheatWire::Status::Refused, CheatDispatch::kTextBadRequest);
		const std::string* part = req.Get("part");

		if (*rule == "unlock")
		{
			if (part)
				return Make(CheatWire::Status::Refused, CheatDispatch::kTextBadRequest);
			if (!on)
				return Make(CheatWire::Status::Refused, kTextNoRelock);
			if (!g_env.unlock || !UnlockAvailable())
				return Make(CheatWire::Status::Refused, kTextOff);
			if (!g_env.inWorld())
				return Make(CheatWire::Status::Error, kTextNoWorld);

			if (!(g_env.unlocked && g_env.unlocked()))
			{
				Audit(ctx, req.id, "unlock all buildings");
				if (!g_env.unlock(ctx, req.id))
					return Make(CheatWire::Status::Error, kTextFailed);
			}
			CheatDispatch::Result res = Make(CheatWire::Status::Ok, kTextUnlocked);
			res.fields.emplace_back("rule", "unlock");
			res.fields.emplace_back("on", "on");
			return res;
		}

		const bool isFree = *rule == "free";
		if (!isFree && *rule != "anywhere")
			return Make(CheatWire::Status::Refused, CheatDispatch::kTextBadRequest);

		// The parts this request touches: the one named, or every part of the rule that works here.
		std::vector<Part> parts;
		const Part* group = isFree ? kFree : kAnywhere;
		if (part)
		{
			for (int i = 0; i < 2; ++i)
				if (*part == PartName(group[i])) parts.push_back(group[i]);
			if (parts.empty())
				return Make(CheatWire::Status::Refused, CheatDispatch::kTextBadRequest);
			if (!PartAvailable(parts[0]))
				return Make(CheatWire::Status::Refused, kTextOff);
		}
		else
		{
			for (int i = 0; i < 2; ++i)
				if (PartAvailable(group[i])) parts.push_back(group[i]);
			if (parts.empty())
				return Make(CheatWire::Status::Refused, kTextOff);
		}

		if (!g_env.inWorld())
			return Make(CheatWire::Status::Error, kTextNoWorld);

		Audit(ctx, req.id, std::string("set ") + (isFree ? "free building" : "building anywhere") + " " + (on ? "on" : "off")
			+ " (" + Join(parts) + ")");
		if (!g_env.apply(ctx, req.id, parts, on))
			return Make(CheatWire::Status::Error, kTextFailed);

		const std::string who = ctx.playerId ? *ctx.playerId : std::string();
		for (Part p : parts)
			SetPart(who, p, on);

		CheatDispatch::Result res = Make(CheatWire::Status::Ok,
			isFree ? (on ? kTextFreeOn : kTextFreeOff) : (on ? kTextAnywhereOn : kTextAnywhereOff));
		res.fields.emplace_back("rule", *rule);
		res.fields.emplace_back("on", on ? "on" : "off");
		res.fields.emplace_back("parts", Join(parts));
		return res;
	}
}
