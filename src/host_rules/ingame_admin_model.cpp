#include "ingame_admin_model.h"
#include "claim_model.h"
#include "ini_file.h"
#include "../admin_wire/admin_wire.h"
#include "../admin_wire/admin_crypto.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace InGameAdminModel
{
	namespace
	{
		using IniFile::Lower;

		bool Blank(const std::string& id)
		{
			return id.empty() || std::any_of(id.begin(), id.end(), [](unsigned char c) { return std::isspace(c) != 0; });
		}
	}

	Server::Server(Env env) : env_(std::move(env))
	{
		if (!env_.random)
			env_.random = [](size_t n, std::vector<uint8_t>& out) { return AdminCrypto::RandomBytes(n, out); };
		Load();
	}

	bool Server::Ready() const
	{
		std::lock_guard<std::mutex> lk(mutex_);
		return !serverId_.empty();
	}

	std::string Server::ServerId() const
	{
		std::lock_guard<std::mutex> lk(mutex_);
		return serverId_;
	}

	bool Server::RandomHex(size_t bytes, std::string& out)
	{
		std::vector<uint8_t> b;
		if (!env_.random(bytes, b) || b.size() != bytes)
			return false;
		out = AdminCrypto::ToHex(b);
		return true;
	}

	// Plugins\ServerUtility\admin-pairing.txt: "serverid <hex>", then "<player id> <secret hex>" lines.
	bool Server::Load()
	{
		std::lock_guard<std::mutex> lk(mutex_);
		if (!env_.storePath.empty())
		{
			std::ifstream in(env_.storePath);
			std::string line;
			while (std::getline(in, line))
			{
				while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
				const size_t sp = line.find(' ');
				if (sp == std::string::npos || sp == 0) continue;
				const std::string key = line.substr(0, sp);
				const std::string value = line.substr(sp + 1);
				if (key == "serverid")
					serverId_ = value;
				else if (value.size() == 64)
					secrets_[key] = Lower(value);
			}
		}
		if (serverId_.empty())
		{
			if (!RandomHex(16, serverId_))
			{
				serverId_.clear();
				return false;
			}
			SaveLocked();
		}
		return true;
	}

	// Temp file, then rename. Never logs, never returns the content.
	bool Server::SaveLocked()
	{
		if (env_.storePath.empty()) return true;
		std::string text = "serverid " + serverId_ + "\n";
		for (const auto& kv : secrets_)
			text += kv.first + " " + kv.second + "\n";
		return IniFile::WriteTextFile(std::filesystem::path(env_.storePath).wstring(), text);
	}

	std::string Server::Welcome(const char* role, uint32_t revision) const
	{
		AdminWire::Message m;
		m.tag = AdminWire::Tag::Welcome;
		m.role = role;
		m.revision = revision;
		std::string out;
		AdminWire::Encode(m, out);
		return out;
	}

	bool Server::MakeChallengeLocked(Conn& c, std::string& message)
	{
		if (!RandomHex(16, c.nonce))
		{
			c.nonce.clear();
			return false;
		}
		c.nonceExpires = (env_.now ? env_.now() : 0) + kNonceLifetimeSeconds;
		c.challenged = true;
		c.proven = false;
		AdminWire::Message m;
		m.tag = AdminWire::Tag::Challenge;
		m.serverId = serverId_;
		m.nonce = c.nonce;
		return AdminWire::Encode(m, message);
	}

	void Server::UnproveIdLocked(const std::string& id)
	{
		for (auto& kv : conns_)
			if (kv.second.id == id)
				kv.second.proven = false;
	}

	std::vector<std::string> Server::SnapshotMessages(const std::string& sections) const
	{
		if (!env_.snapshot)
			return {};
		AdminWire::Snapshot snap = env_.snapshot();
		snap.revision = env_.revision ? env_.revision() : 0;
		snap.sections = sections.empty() ? std::string(AdminWire::kSectionAll) : sections;
		if (snap.sections == AdminWire::kSectionMods)   // the catalog alone: no rules, minimums or players
		{
			snap.rules.clear();
			snap.defaults.clear();
			snap.mins.clear();
			snap.players.clear();
		}
		return AdminWire::EncodeSnapshot(snap);
	}

	namespace
	{
		// Set while one connection's ops run: the "changed" the run causes for that
		// connection is held back so it follows that connection's "result".
		thread_local ConnId              t_originConn = 0;
		thread_local std::vector<std::string>* t_afterResult = nullptr;
	}

	std::vector<std::string> Server::CatalogPush(ConnId conn, const AdminWire::Message& m) const
	{
		AdminWire::Message r;
		r.tag = AdminWire::Tag::Result;
		r.requestId = m.requestId;
		r.revision = env_.revision ? env_.revision() : 0;
		std::vector<std::string> rows, afterResult;
		bool only = true;
		for (const AdminWire::Op& o : m.ops)
		{
			if (o.name != "catalog.push" || o.args.size() != 1) { only = false; break; }
			rows.push_back(o.args[0]);
		}
		if (!only)
		{
			r.status = AdminWire::kResultError;
			r.text = "catalog.push goes in a message of its own";
		}
		else
		{
			if (env_.catalogPush)
			{
				t_originConn = conn;
				t_afterResult = &afterResult;
				env_.catalogPush(rows);   // the host announces a change with NoteCatalogChanged; the revision stays
				t_originConn = 0;
				t_afterResult = nullptr;
			}
			r.status = AdminWire::kResultOk;
			r.revision = env_.revision ? env_.revision() : 0;
		}
		std::vector<std::string> out;
		std::string wire;
		if (AdminWire::Encode(r, wire))
			out.push_back(wire);
		out.insert(out.end(), afterResult.begin(), afterResult.end());
		return out;
	}

	std::vector<std::string> Server::RunOps(ConnId conn, const std::string& playerId, const std::string& name, const AdminWire::Message& m) const
	{
		AdminWire::Message r;
		r.tag = AdminWire::Tag::Result;
		r.requestId = m.requestId;
		OpsOutcome o;
		std::vector<std::string> afterResult;
		if (env_.runOps)
		{
			t_originConn = conn;
			t_afterResult = &afterResult;
			o = env_.runOps(playerId, name, m.ops, m.baseRevision);
			t_originConn = 0;
			t_afterResult = nullptr;
		}
		else
			o.text = "Not available";
		r.revision = o.revision;
		r.text = o.text;
		r.status = o.ok ? AdminWire::kResultOk : (o.stale ? AdminWire::kResultChanged : AdminWire::kResultError);

		std::vector<std::string> out;
		std::string wire;
		if (AdminWire::Encode(r, wire))
			out.push_back(wire);
		if (o.stale)
		{
			const std::vector<std::string> snap = SnapshotMessages(AdminWire::kSectionAll);
			out.insert(out.end(), snap.begin(), snap.end());
		}
		out.insert(out.end(), afterResult.begin(), afterResult.end());
		return out;
	}

	std::vector<Out> Server::OnChanged(const std::string& sections, uint32_t revision)
	{
		std::vector<std::pair<ConnId, std::string>> proven;
		{
			std::lock_guard<std::mutex> lk(mutex_);
			for (const auto& kv : conns_)
				if (kv.second.proven)
					proven.emplace_back(kv.first, kv.second.id);
		}
		AdminWire::Message m;
		m.tag = AdminWire::Tag::Changed;
		m.revision = revision;
		m.sections = sections;
		std::string wire;
		std::vector<Out> out;
		if (!AdminWire::Encode(m, wire))
			return out;
		for (const auto& p : proven)
			if (env_.isAdmin && env_.isAdmin(p.second))   // still an Admin, read now
			{
				if (t_afterResult && p.first == t_originConn)
					t_afterResult->push_back(wire);   // goes out after this connection's result
				else
					out.push_back(Out{ p.first, wire });
			}
		return out;
	}

	std::vector<std::string> Server::OnPacket(ConnId conn, const std::string& playerId, const std::string& name,
	                                          const uint8_t* data, size_t size)
	{
		using namespace AdminWire;
		std::vector<std::string> out;
		if (!env_.enabled || !env_.enabled() || !Ready())
			return out;

		Message m;
		const Decoded decoded = Decode(data, size, m);
		if (decoded == Decoded::Garbage)
			return out;

		// The Admin flag is read before the lock: the player list has its own lock.
		const bool admin = !playerId.empty() && env_.isAdmin && env_.isAdmin(playerId);
		const uint32_t revision = env_.revision ? env_.revision() : 0;
		const std::string who = name.empty() ? playerId : name;

		{
			std::unique_lock<std::mutex> lk(mutex_);
			Conn& c = conns_[conn];
			if (c.silent)
				return out;

			// The server reads the ID again on every packet. A different ID, or one that lost
			// the Admin flag, ends whatever this connection had earned.
			if (c.id != playerId)
			{
				c.id = playerId;
				c.proven = false;
				c.challenged = false;
				c.nonce.clear();
			}
			if (c.proven && !admin)
				c.proven = false;

			// Every decoded message takes tokens: one, or one per op in an ops message.
			// Over the limit it is dropped with no reply, and logged once per burst.
			{
				const double now = env_.now ? env_.now() : 0;
				c.tokens = std::min(kOpsBurst, c.tokens + (now - c.tokenTime) * kOpsPerSecond);
				c.tokenTime = now;
				if (c.tokens >= kOpsBurst)
					c.limitLogged = false;   // logged again only after the bucket has refilled
				double cost = 1;
				if (decoded == Decoded::Ok && m.tag == Tag::Ops && m.ops.size() > 1)
					cost = std::min(kOpsBurst, static_cast<double>(m.ops.size()));
				if (c.tokens < cost)
				{
					if (!c.limitLogged && env_.audit)
						env_.audit("[Admin] '" + who + "' sent too many requests; dropping them");
					c.limitLogged = true;
					return out;
				}
				c.tokens -= cost;
			}

			if (decoded == Decoded::WrongProtocol)
			{
				if (m.tag == Tag::Hello)
					out.push_back(Welcome(kRoleUpdate, 0));
				return out;
			}

			if (m.tag == Tag::Hello)
			{
				c.proven = false;
				c.nonce.clear();
				if (!admin)
				{
					c.silent = true;
					out.push_back(Welcome(kRoleNone, 0));
					return out;
				}
				std::string challenge;
				if (MakeChallengeLocked(c, challenge))
					out.push_back(challenge);
				return out;
			}

			if (!c.challenged)
				return out;   // nothing but a hello starts a conversation

			if (!admin)
			{
				c.challenged = false;
				c.proven = false;
				c.silent = true;
				out.push_back(Welcome(kRoleNone, 0));
				return out;
			}

			if (m.tag == Tag::Prove)
			{
				if (c.proven)
					return out;   // already proven: a second prove gets no reply, so the client keeps its admin view
				const std::string nonce = c.nonce;
				c.nonce.clear();   // one proof per challenge
				const bool fresh = (env_.now ? env_.now() : 0) < c.nonceExpires;
				const auto it = secrets_.find(playerId);
				if (m.proof.empty() || nonce.empty() || !fresh || it == secrets_.end())
				{
					out.push_back(Welcome(kRolePair, revision));
					return out;
				}
				std::string expected;
				if (AdminCrypto::MakeProof(it->second, nonce, serverId_, expected) &&
				    AdminCrypto::ConstantTimeEqual(Lower(m.proof), expected))
				{
					c.proven = true;
					out.push_back(Welcome(kRoleAdmin, revision));
					lk.unlock();   // the snapshot runs game-thread work, outside the model lock
					return WithSnapshot(std::move(out));
				}
				if (++c.wrongProofs <= kMaxWrongPerConnection && env_.audit)
					env_.audit("[Admin] '" + who + "' proof failed");
				out.push_back(Welcome(kRolePair, revision));
				return out;
			}

			if (m.tag == Tag::Pair)
			{
				const double now = env_.now ? env_.now() : 0;
				auto refuse = [&](const char* why)
				{
					if (env_.audit) env_.audit(std::string("[Admin] '") + who + "' pairing refused (" + why + ")");
					out.push_back(Welcome(kRolePair, revision));
				};

				if (c.wrongCodes >= kMaxWrongPerConnection)
				{
					out.push_back(Welcome(kRolePair, revision));   // no further log lines
					return out;
				}

				bool codeOk = false;
				if (m.code.empty())
				{
					// Only the connection that completed the claim, and only while it still
					// reads as the same ID. An ID alone is identification, not proof.
					const auto cl = claimed_.find(conn);
					codeOk = cl != claimed_.end() && cl->second.id == playerId && now < cl->second.expires;
					if (cl != claimed_.end() && now >= cl->second.expires) claimed_.erase(cl);
					if (!codeOk) ++c.wrongCodes;
				}
				else
				{
					auto it = codes_.find(playerId);
					if (it != codes_.end() && now >= it->second.expires)
					{
						ClaimModel::Wipe(it->second.code);
						codes_.erase(it);
						it = codes_.end();
					}
					std::string norm;
					if (it != codes_.end() && ClaimModel::NormalizeCode(m.code, norm) &&
					    ClaimModel::ConstantTimeEquals(norm, it->second.code))
						codeOk = true;
					else
					{
						++c.wrongCodes;
						if (it != codes_.end() && ++it->second.wrong >= kMaxWrongPerCode)
						{
							ClaimModel::Wipe(it->second.code);
							codes_.erase(it);
							if (env_.audit) env_.audit("[Admin] pairing code for an admin voided (too many wrong codes)");
						}
					}
				}

				if (!codeOk)
				{
					refuse("wrong code");
					return out;
				}

				if (Blank(playerId))
				{
					refuse("unusable ID");
					return out;
				}

				const auto previous = secrets_.find(playerId);
				const bool had = previous != secrets_.end();
				const std::string old = had ? previous->second : std::string();
				secrets_[playerId] = Lower(m.secret);
				if (!SaveLocked())
				{
					if (had) secrets_[playerId] = old; else secrets_.erase(playerId);
					refuse("could not save");
					return out;
				}

				auto it = codes_.find(playerId);
				if (it != codes_.end()) { ClaimModel::Wipe(it->second.code); codes_.erase(it); }
				EraseClaimedLocked(playerId);
				UnproveIdLocked(playerId);   // a new secret replaces the old one everywhere
				c.proven = true;
				c.nonce.clear();
				if (env_.audit) env_.audit("[Admin] '" + who + "' paired");
				out.push_back(Welcome(kRoleAdmin, revision));
				lk.unlock();   // the snapshot runs game-thread work, outside the model lock
				return WithSnapshot(std::move(out));
			}

			// Anything else is for a proven connection only.
			if (!c.proven)
				return out;
			if (m.tag != Tag::Snap && m.tag != Tag::Ops)
				return out;
		}

		// The game-thread work runs outside the model lock.
		if (m.tag == Tag::Snap)
			return SnapshotMessages(m.sections);
		if (m.tag == Tag::Ops && !m.ops.empty() && m.ops.front().name == "catalog.push")
			return CatalogPush(conn, m);
		return RunOps(conn, playerId, who, m);
	}

	std::vector<std::string> Server::WithSnapshot(std::vector<std::string> out) const
	{
		// Follows "welcome admin": the full state.
		const std::vector<std::string> snap = SnapshotMessages(AdminWire::kSectionAll);
		out.insert(out.end(), snap.begin(), snap.end());
		return out;
	}

	void Server::OnDisconnect(ConnId conn)
	{
		std::lock_guard<std::mutex> lk(mutex_);
		conns_.erase(conn);
		claimed_.erase(conn);
	}

	void Server::OnWorldEnd()
	{
		std::lock_guard<std::mutex> lk(mutex_);
		conns_.clear();
		claimed_.clear();
	}

	bool Server::IsProven(ConnId conn) const
	{
		std::lock_guard<std::mutex> lk(mutex_);
		const auto it = conns_.find(conn);
		return it != conns_.end() && it->second.proven;
	}

	bool Server::HasSecret(const std::string& playerId) const
	{
		std::lock_guard<std::mutex> lk(mutex_);
		return secrets_.count(playerId) != 0;
	}

	bool Server::IssueCode(const std::string& playerId, std::string& code, std::string& error)
	{
		code.clear();
		if (!Ready()) { error = "In-game admin is not available (no random source for the server id)"; return false; }
		if (Blank(playerId) || !env_.isAdmin || !env_.isAdmin(playerId))
		{
			error = "That player does not have the Admin flag. Add it first: admin add <id|number>";
			return false;
		}

		std::string fresh;
		auto fill = [this](unsigned char* buf, size_t size)
		{
			std::vector<uint8_t> b;
			if (!env_.random(size, b) || b.size() != size) return false;
			std::copy(b.begin(), b.end(), buf);
			return true;
		};
		if (!ClaimModel::GenerateCode(fill, fresh))
		{
			error = "Could not make a code (the system random generator failed)";
			return false;
		}

		std::lock_guard<std::mutex> lk(mutex_);
		auto it = codes_.find(playerId);
		if (it != codes_.end()) ClaimModel::Wipe(it->second.code);
		Code c;
		c.code = fresh;
		c.expires = (env_.now ? env_.now() : 0) + kCodeLifetimeSeconds;
		codes_[playerId] = c;
		code = fresh;
		ClaimModel::Wipe(fresh);
		return true;
	}

	bool Server::Unpair(const std::string& playerId)
	{
		std::lock_guard<std::mutex> lk(mutex_);
		bool did = false;
		if (secrets_.erase(playerId))
		{
			SaveLocked();
			did = true;
		}
		auto it = codes_.find(playerId);
		if (it != codes_.end()) { ClaimModel::Wipe(it->second.code); codes_.erase(it); did = true; }
		EraseClaimedLocked(playerId);
		UnproveIdLocked(playerId);
		if (did && env_.audit) env_.audit("[Admin] pairing for " + playerId + " removed (unpaired)");
		return did;
	}

	std::vector<Out> Server::OnPlayersChanged()
	{
		// Look the flags up before taking the lock.
		std::vector<std::string> ids, silentIds;
		{
			std::lock_guard<std::mutex> lk(mutex_);
			for (const auto& kv : secrets_) ids.push_back(kv.first);
			for (const auto& kv : codes_) ids.push_back(kv.first);
			for (const auto& kv : claimed_) ids.push_back(kv.second.id);
			for (const auto& kv : conns_)
			{
				if (kv.second.proven) ids.push_back(kv.second.id);
				if (kv.second.silent) silentIds.push_back(kv.second.id);
			}
		}
		std::sort(ids.begin(), ids.end());
		ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
		std::sort(silentIds.begin(), silentIds.end());
		silentIds.erase(std::unique(silentIds.begin(), silentIds.end()), silentIds.end());

		std::vector<std::string> lost, gained;
		for (const std::string& id : ids)
			if (!env_.isAdmin || !env_.isAdmin(id))
				lost.push_back(id);
		const bool on = env_.enabled && env_.enabled() && Ready();
		for (const std::string& id : silentIds)
			if (on && !Blank(id) && env_.isAdmin && env_.isAdmin(id))
				gained.push_back(id);

		std::vector<Out> out;
		if (lost.empty() && gained.empty()) return out;

		std::lock_guard<std::mutex> lk(mutex_);
		bool changed = false;
		for (const std::string& id : lost)
		{
			if (secrets_.erase(id))
			{
				changed = true;
				if (env_.audit) env_.audit("[Admin] pairing for " + id + " removed (no longer an admin)");
			}
			auto it = codes_.find(id);
			if (it != codes_.end()) { ClaimModel::Wipe(it->second.code); codes_.erase(it); }
			EraseClaimedLocked(id);
			UnproveIdLocked(id);
		}
		if (changed) SaveLocked();

		// Gained the flag while told "none": out of silence, with a challenge.
		for (auto& kv : conns_)
		{
			Conn& c = kv.second;
			if (!c.silent || !std::binary_search(gained.begin(), gained.end(), c.id)) continue;
			c.silent = false;
			std::string challenge;
			if (MakeChallengeLocked(c, challenge))
				out.push_back({ kv.first, challenge });
		}
		return out;
	}

	void Server::EraseClaimedLocked(const std::string& playerId)
	{
		for (auto it = claimed_.begin(); it != claimed_.end();)
			it = it->second.id == playerId ? claimed_.erase(it) : std::next(it);
	}

	std::vector<Out> Server::OnClaimed(ConnId claimant, const std::string& playerId)
	{
		std::vector<Out> out;
		if (!env_.enabled || !env_.enabled() || !Ready())
			return out;
		const bool admin = !playerId.empty() && env_.isAdmin && env_.isAdmin(playerId);
		if (!admin) return out;

		std::lock_guard<std::mutex> lk(mutex_);
		if (claimant)
			claimed_[claimant] = { playerId, (env_.now ? env_.now() : 0) + kCodeLifetimeSeconds };
		for (auto& kv : conns_)
		{
			Conn& c = kv.second;
			if (c.id != playerId || !c.silent) continue;
			c.silent = false;
			std::string challenge;
			if (MakeChallengeLocked(c, challenge))
				out.push_back({ kv.first, challenge });
		}
		return out;
	}
}
