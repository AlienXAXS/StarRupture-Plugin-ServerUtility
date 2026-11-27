#include "cheat_dispatch.h"
#include "op_give.h"
#include "op_wave.h"
#include "op_stack.h"
#include "op_corp.h"
#include "op_power.h"
#include "op_enemy.h"
#include "op_weapon.h"
#include "op_grid.h"
#include "op_mine.h"
#include "op_state.h"

#include <algorithm>
#include <cstring>

namespace CheatDispatch
{
	void Bucket::Refill(double nowSeconds)
	{
		if (m_last >= 0 && nowSeconds > m_last)
			m_tokens = std::min(m_burst, m_tokens + (nowSeconds - m_last) * m_rate);
		m_last = nowSeconds;
	}

	bool Bucket::Take(double nowSeconds)
	{
		Refill(nowSeconds);
		if (m_tokens < 1) return false;
		m_tokens -= 1;
		return true;
	}

	bool Bucket::Idle(double nowSeconds) const
	{
		Bucket copy = *this;
		copy.Refill(nowSeconds);
		return copy.m_tokens >= copy.m_burst;
	}

	Result NotYet(const Context&, const CheatWire::Request&)
	{
		Result r;
		r.status = CheatWire::Status::Refused;
		r.reason = kTextNotYet;
		return r;
	}

	const std::vector<OpEntry>& Ops()
	{
		// Feature ids are BetterCheats' own (the default rows in rules_model.cpp). cheat.build covers
		// four features; its slice picks the one a request names, and until then the strictest
		// default (admins only) applies.
		static const std::vector<OpEntry> table = {
			{ "cheat.give",  1, "item-spawner",   &OpGive::Handle, &OpGive::Available },
			{ "cheat.wave",  1, "rupture-wave",   &OpWave::Handle, &OpWave::Available, &OpWave::StateFields },
			{ "cheat.stack", 1, "stack-size",     &OpStack::Handle, &OpStack::Available, &OpStack::StateFields },
			{ "cheat.stat",  1, "player-stats",   &NotYet },
			{ "cheat.move",  1, "movement",       &NotYet },
			{ "cheat.skill", 1, "skills",         &NotYet },
			{ "cheat.corp",  1, "corporations",   &OpCorp::Handle, &OpCorp::Available, &OpCorp::StateFields },
			{ "cheat.power", 1, "machine-power",  &OpPower::Handle, &OpPower::Available, &OpPower::StateFields },
			{ "cheat.enemy", 1, "enemies",        &OpEnemy::Handle, &OpEnemy::Available, &OpEnemy::StateFields },
			{ "cheat.weapon", 1, "weapons",       &OpWeapon::Handle, &OpWeapon::Available, &OpWeapon::StateFields },
			{ "cheat.grid",   1, "inventory",     &OpGrid::Handle, &OpGrid::Available, &OpGrid::StateFields },
			{ "cheat.mine",   1, "mining-tools",  &OpMine::Handle, &OpMine::Available, &OpMine::StateFields },
			{ "cheat.build", 1, "build-free",     &NotYet },
		};
		return table;
	}

	bool IsOn(const OpEntry& op)
	{
		return op.run != &NotYet && (!op.available || op.available());
	}

	bool Server::LogOnce(const std::string& playerId, const char* what)
	{
		return m_logged.insert(playerId + "|" + what).second;
	}

	void Server::Forget(const std::string& playerId)
	{
		m_buckets.erase(playerId);
		for (auto it = m_logged.begin(); it != m_logged.end();)
			it = it->compare(0, playerId.size() + 1, playerId + "|") == 0 ? m_logged.erase(it) : std::next(it);
	}

	std::string Server::Cap(uint32_t id) const
	{
		std::vector<std::pair<std::string, std::string>> fields;
		fields.emplace_back("su", m_env.suVersion);
		for (const OpEntry& op : Ops())
			fields.emplace_back(op.name, std::to_string(op.version) + (IsOn(op) ? ",on" : ",off"));
		fields.emplace_back(OpState::kOp, std::to_string(OpState::kVersion) + ",on");
		return CheatWire::EncodeReply(id, CheatWire::Status::Ok, "", fields);
	}

	std::string Server::Handle(void* controller, const std::string& playerId, const uint8_t* data, size_t size)
	{
		const auto log = [&](const std::string& line) { if (m_env.log) m_env.log("[Cheat] " + line); };
		const auto reply = [](uint32_t id, CheatWire::Status s, const std::string& reason, const Result* r = nullptr)
		{
			return CheatWire::EncodeReply(id, s, reason, r ? r->fields : std::vector<std::pair<std::string, std::string>>());
		};

		const CheatWire::Parsed p = CheatWire::ParseRequest(data, size);

		// Every message takes a token, a bad one too.
		const double now = m_env.now();
		auto bucket = m_buckets.find(playerId);
		if (bucket == m_buckets.end())
		{
			// Buckets that are full again hold nothing worth keeping.
			if (m_buckets.size() >= 256)
				for (auto it = m_buckets.begin(); it != m_buckets.end();)
					it = it->second.Idle(now) ? m_buckets.erase(it) : std::next(it);
			bucket = m_buckets.emplace(playerId, Bucket()).first;
		}
		const bool allowed = bucket->second.Take(now);

		switch (p.status)
		{
		case CheatWire::ParseStatus::TooBig:
			if (LogOnce(playerId, "toobig")) log("Dropped a request over " + std::to_string(CheatWire::kMaxMessage) + " bytes from " + playerId);
			return {};
		case CheatWire::ParseStatus::UnknownKind:
			if (LogOnce(playerId, "kind")) log("Dropped a message of an unknown kind from " + playerId);
			return {};
		case CheatWire::ParseStatus::UnknownProtocol:
			if (LogOnce(playerId, "proto")) log("Dropped a message of an unknown protocol from " + playerId);
			return {};
		case CheatWire::ParseStatus::Malformed:
			if (!allowed) return {};
			if (p.hasId) return reply(p.id, CheatWire::Status::Error, kTextBadRequest);
			if (LogOnce(playerId, "malformed")) log("Dropped an unreadable request from " + playerId);
			return {};
		case CheatWire::ParseStatus::Ok:
			break;
		}

		const CheatWire::Request& req = p.req;
		if (!allowed)
		{
			if (LogOnce(playerId, "rate")) log("Rate limit hit by " + playerId);
			return reply(req.id, CheatWire::Status::Refused, kTextTooMany);
		}

		if (req.op == "cap")
			return Cap(req.id);

		if (req.op == OpState::kOp)
		{
			const Result r = OpState::Handle(req, [&](const OpEntry& op) { return m_env.check(controller, playerId, op.feature).allowed; }, playerId);
			return reply(req.id, r.status, r.reason, &r);
		}

		const OpEntry* entry = nullptr;
		for (const OpEntry& op : Ops())
			if (req.op == op.name) { entry = &op; break; }
		if (!entry)
		{
			log("Unknown op '" + req.op + "' from " + playerId);
			return reply(req.id, CheatWire::Status::Refused, kTextUnknownOp);
		}

		const RulesModel::Decision d = m_env.check(controller, playerId, entry->feature);
		if (!d.allowed)
		{
			log("Refused " + req.op + " (" + entry->feature + ") for " + playerId + ": " + (d.why ? d.why : ""));
			const bool adminsOnly = d.why && std::strcmp(d.why, kTextAdminsOnly) == 0;
			return reply(req.id, CheatWire::Status::Refused, adminsOnly ? kTextAdminsOnly : kTextRuleDeny);
		}

		Context ctx;
		ctx.controller = controller;
		ctx.playerId   = &playerId;
		const Result r = entry->run(ctx, req);
		return reply(req.id, r.status, r.reason, &r);
	}
}
