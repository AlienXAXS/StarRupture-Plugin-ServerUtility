#pragma once

// The su.cheat request dispatch, apart from the game: the rate bucket, the op table, `cap` and
// the replies. The game-facing parts (who sent it, the authority check, the send) come in
// through Env, so this runs in unit tests. Everything here runs on the game thread.
#include "cheat_wire.h"
#include "host_rules/rules_model.h"

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace CheatDispatch
{
	constexpr double kRequestsPerSecond = 5;    // refill rate per player
	constexpr double kBurst             = 20;   // bucket size

	// Texts the player sees. The detail of a refusal goes to the server log only.
	constexpr const char* kTextRuleDeny    = "Turned off by this server's rules. Ask the server's admin.";
	constexpr const char* kTextAdminsOnly  = "Only this server's admins can use this.";
	constexpr const char* kTextTooMany     = "Too many requests. Wait a moment.";
	constexpr const char* kTextNotYet      = "Not available yet.";
	constexpr const char* kTextBadRequest  = "That request could not be read.";
	// The ServerUtility version that has these ops. An op this server does not know was added later.
	constexpr const char* kTextUnknownOp   = "This server's ServerUtility is too old for this. It needs 0.19.0 or newer.";

	// A token bucket. Starts full; time is passed in (seconds, steady) so tests need no clock.
	class Bucket
	{
	public:
		Bucket(double perSecond = kRequestsPerSecond, double burst = kBurst) : m_rate(perSecond), m_burst(burst), m_tokens(burst) {}
		bool Take(double nowSeconds);
		// True when the bucket is full as of nowSeconds (nothing to remember).
		bool Idle(double nowSeconds) const;
	private:
		void Refill(double nowSeconds);
		double m_rate, m_burst, m_tokens;
		double m_last = -1;
	};

	struct Context
	{
		void*              controller = nullptr;   // opaque here; the game glue gave it
		const std::string* playerId   = nullptr;
	};

	struct Result
	{
		CheatWire::Status status = CheatWire::Status::Refused;
		std::string       reason;
		std::vector<std::pair<std::string, std::string>> fields;
	};

	using Handler = Result (*)(const Context&, const CheatWire::Request&);

	// What a slice with no executor yet puts in the table.
	Result NotYet(const Context&, const CheatWire::Request&);

	struct OpEntry
	{
		const char* name;
		int         version;   // handler version, as `cap` reports it
		const char* feature;   // the BetterCheats feature id the rules decide on
		Handler     run;
		// Optional: whether the op can run on this server right now. `cap` reports it off when this says no.
		bool      (*available)() = nullptr;
	};

	// The one table. A later slice replaces `NotYet` in its entry.
	const std::vector<OpEntry>& Ops();

	struct Env
	{
		// Game thread. May this player use this feature (CheatAuthority::Check).
		std::function<RulesModel::Decision(void* controller, const std::string& playerId, const char* feature)> check;
		std::function<double()>                    now;   // seconds, steady
		std::function<void(const std::string&)>    log;   // one server log line
		std::string                                suVersion;
	};

	class Server
	{
	public:
		explicit Server(Env env) : m_env(std::move(env)) {}

		// One packet from one player. Returns the reply to send to that player, or an empty string
		// when nothing is sent (a dropped message).
		std::string Handle(void* controller, const std::string& playerId, const uint8_t* data, size_t size);

		// A player left: forget the bucket and the log-once marks.
		void Forget(const std::string& playerId);

	private:
		bool LogOnce(const std::string& playerId, const char* what);
		std::string Cap(uint32_t id) const;

		Env                          m_env;
		std::map<std::string, Bucket> m_buckets;
		std::set<std::string>        m_logged;
	};
}
