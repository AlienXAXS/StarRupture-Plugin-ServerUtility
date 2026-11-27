#include "op_state.h"

namespace OpState
{
	CheatDispatch::Result Handle(const CheatWire::Request& req,
	                             const std::function<bool(const CheatDispatch::OpEntry&)>& allowed,
	                             const std::string& playerId)
	{
		CheatDispatch::Result r;
		if (!req.fields.empty())
		{
			r.reason = CheatDispatch::kTextBadRequest;
			return r;
		}
		r.status = CheatWire::Status::Ok;
		for (const CheatDispatch::OpEntry& op : CheatDispatch::Ops())
		{
			if (!(op.state || op.stateEx) || !CheatDispatch::IsOn(op))
				continue;
			if (op.stateEx)
			{
				// The op decides per feature itself, so its own feature is not a gate here.
				const auto featureAllowed = [&](const char* feature)
				{
					CheatDispatch::OpEntry probe = op;
					probe.feature = feature;
					return allowed(probe);
				};
				for (auto& field : op.stateEx(playerId, featureAllowed))
					r.fields.push_back(std::move(field));
				continue;
			}
			if (!allowed(op))
				continue;
			for (auto& field : op.state(playerId))
				r.fields.push_back(std::move(field));
		}
		return r;
	}

	CheatDispatch::Fields WorstCaseFields()
	{
		return {
			{ "stack",  "9999" },
			{ "wave",   "running" },
			{ "power",  "2147483647" },
			{ "points", "999999999" },
			{ "ai",     "off" },
			{ "onehit", "off" },
			{ "weapon", "1000.000000,1000.000000" },
			{ "grid",   "100x100" },
			{ "plant",  "2147483647" },
			{ "mine",   "10" },
			{ "heat",   "off" },
			{ "fly",    "off" },
			{ "speed",  "25" },
			{ "lock",   "health,energy,shield,hydration,calories,oxygen,toxicity,radiation,heat,drain,corrosion,infection,temperature" },
			{ "build",  "cost,stability,windows,benches" },
			{ "unlock", "off" },
		};
	}
}
