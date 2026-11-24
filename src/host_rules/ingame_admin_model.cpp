#include "ingame_admin_model.h"
#include "claim_model.h"
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
		std::string Lower(std::string s)
		{
			std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return s;
		}

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
		namespace fs = std::filesystem;
		std::error_code ec;
		const fs::path target(env_.storePath);
		if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
		fs::path tmp = target;
		tmp += ".tmp";
		{
			std::ofstream out(tmp, std::ios::trunc);
			if (!out) return false;
			out << "serverid " << serverId_ << "\n";
			for (const auto& kv : secrets_)
				out << kv.first << " " << kv.second << "\n";
			out.flush();
			if (!out) { out.close(); fs::remove(tmp, ec); return false; }
		}
		fs::rename(tmp, target, ec);
		if (ec) { fs::remove(tmp, ec); return false; }
		return true;
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

	void Server::SetProvenHandler(ProvenHandler handler)
	{
		std::lock_guard<std::mutex> lk(mutex_);
		proven_ = std::move(handler);
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

		ProvenHandler handler;
		bool forward = false;
		{
			std::lock_guard<std::mutex> lk(mutex_);
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
				const std::string nonce = c.nonce;
				c.nonce.clear();   // one proof per challenge
				const auto it = secrets_.find(playerId);
				if (m.proof.empty() || nonce.empty() || it == secrets_.end())
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
					return out;
				}
				if (env_.audit) env_.audit("[Admin] '" + who + "' proof failed");
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
					refuse("too many wrong codes");
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
				return out;
			}

			if (!c.proven)
				return out;
			handler = proven_;
			forward = true;
		}

		if (forward && handler)
			out = handler(conn, playerId, who, data, size);
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

	std::vector<ConnId> Server::ProvenConnections() const
	{
		std::vector<ConnId> out;
		std::lock_guard<std::mutex> lk(mutex_);
		for (const auto& kv : conns_)
			if (kv.second.proven)
				out.push_back(kv.first);
		return out;
	}

	std::string Server::PlayerIdOf(ConnId conn) const
	{
		std::lock_guard<std::mutex> lk(mutex_);
		const auto it = conns_.find(conn);
		return it == conns_.end() ? std::string() : it->second.id;
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

	void Server::OnPlayersChanged()
	{
		// Look the flags up before taking the lock.
		std::vector<std::string> ids;
		{
			std::lock_guard<std::mutex> lk(mutex_);
			for (const auto& kv : secrets_) ids.push_back(kv.first);
			for (const auto& kv : codes_) ids.push_back(kv.first);
			for (const auto& kv : claimed_) ids.push_back(kv.second.id);
			for (const auto& kv : conns_) if (kv.second.proven) ids.push_back(kv.second.id);
		}
		std::sort(ids.begin(), ids.end());
		ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

		std::vector<std::string> lost;
		for (const std::string& id : ids)
			if (!env_.isAdmin || !env_.isAdmin(id))
				lost.push_back(id);
		if (lost.empty()) return;

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
