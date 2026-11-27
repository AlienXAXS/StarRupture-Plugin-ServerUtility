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
			if (!op.state || !CheatDispatch::IsOn(op) || !allowed(op))
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
			{ "ai",     "off" },
			{ "onehit", "off" },
			{ "weapon", "1000.000000,1000.000000" },
			{ "grid",   "100x100" },
			{ "plant",  "2147483647" },
			{ "mine",   "10" },
			{ "heat",   "off" },
			{ "build",  "cost,stability,windows,benches" },
			{ "unlock", "off" },
		};
	}
}
