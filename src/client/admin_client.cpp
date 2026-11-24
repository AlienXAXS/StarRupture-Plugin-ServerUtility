#include "client/admin_client.h"
#include "admin_wire/admin_crypto.h"

#include <filesystem>
#include <fstream>

using namespace AdminWire;

namespace AdminClient
{
	namespace
	{
		bool IsHexOfLength(const std::string& s, size_t n)
		{
			if (s.size() != n) return false;
			for (char c : s)
				if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
			return true;
		}
	}

	Client::Client(const Env& env) : env_(env)
	{
		LoadSecrets();
	}

	// Store format: one line per server, "<serverId> <secret hex>".
	void Client::LoadSecrets()
	{
		if (env_.storePath.empty()) return;
		std::ifstream in(env_.storePath);
		std::string line;
		while (std::getline(in, line))
		{
			if (!line.empty() && line.back() == '\r') line.pop_back();
			size_t sp = line.find(' ');
			if (sp == std::string::npos || sp == 0) continue;
			std::string id = line.substr(0, sp), secret = line.substr(sp + 1);
			if (IsHexOfLength(secret, 64)) secrets_[id] = secret;
		}
	}

	bool Client::SaveSecret(const std::string& serverId, const std::string& secretHex)
	{
		secrets_[serverId] = secretHex;
		if (env_.storePath.empty()) return false;
		namespace fs = std::filesystem;
		std::error_code ec;
		fs::path target(env_.storePath);
		if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
		fs::path tmp = target;
		tmp += ".tmp";
		{
			std::ofstream out(tmp, std::ios::trunc);
			if (!out) return false;
			for (const auto& kv : secrets_) out << kv.first << ' ' << kv.second << '\n';
			out.flush();
			if (!out) return false;
		}
		fs::rename(tmp, target, ec);
		if (ec) { fs::remove(tmp, ec); return false; }
		return true;
	}

	void Client::Send(const Message& m)
	{
		std::string enc;
		if (!Encode(m, enc) || !env_.send) return;
		env_.send(enc);
	}

	void Client::SendSnapReq()
	{
		Message m;
		m.tag = Tag::Snap;
		m.sections = kSectionAll;
		Send(m);
	}

	void Client::ResetSession()
	{
		status_ = Status::Waiting;
		helloSent_ = false;
		serverId_.clear();
		pendingSecret_.clear();
		assembling_ = false;
		building_ = Snapshot();
		haveSnapshot_ = false;
		snapshot_ = Snapshot();
		staged_.clear();
		applyReqId_ = 0;
		lastResult_.clear();
		lastResultIsError_ = false;
	}

	void Client::OnServerReady()
	{
		std::lock_guard<std::mutex> lock(mu_);
		ResetSession();
		Message m;
		m.tag = Tag::Hello;
		Send(m);
		helloSent_ = true;
		helloAt_ = env_.now ? env_.now() : 0;
	}

	void Client::OnWorldEnd()
	{
		std::lock_guard<std::mutex> lock(mu_);
		ResetSession();
	}

	void Client::CheckTimeout()
	{
		if (status_ == Status::Waiting && helloSent_ && env_.now && env_.now() - helloAt_ >= kHelloTimeoutSeconds)
			status_ = Status::NoInGameAdmin;
	}

	void Client::GetView(View& out)
	{
		std::lock_guard<std::mutex> lock(mu_);
		CheckTimeout();
		out.status = status_;
		out.haveSnapshot = haveSnapshot_;
		out.snapshot = snapshot_;
		out.staged = staged_;
		out.lastResult = lastResult_;
		out.lastResultIsError = lastResultIsError_;
		out.applyPending = applyReqId_ != 0;
	}

	void Client::OnPacket(const uint8_t* data, size_t size)
	{
		Message m;
		Decoded d = Decode(data, size, m);
		std::lock_guard<std::mutex> lock(mu_);
		if (d == Decoded::WrongProtocol) { status_ = Status::UpdateClient; return; }
		if (d != Decoded::Ok) return;
		switch (m.tag)
		{
		case Tag::Welcome: HandleWelcome(m); break;
		case Tag::Challenge: HandleChallenge(m); break;
		case Tag::Changed: if (status_ == Status::Ready) SendSnapReq(); break;
		case Tag::Result: HandleResult(m); break;
		case Tag::SnapBegin: case Tag::SnapPolicy: case Tag::SnapRule: case Tag::SnapMin:
		case Tag::SnapMod: case Tag::SnapPlayer: case Tag::SnapEnd:
			HandleSnapshotPart(m); break;
		default: break;
		}
	}

	void Client::HandleChallenge(const Message& m)
	{
		serverId_ = m.serverId;
		Message p;
		p.tag = Tag::Prove;
		auto it = secrets_.find(m.serverId);
		if (it != secrets_.end())
		{
			std::string proof;
			if (AdminCrypto::MakeProof(it->second, m.nonce, m.serverId, proof)) p.proof = proof;
		}
		Send(p);
	}

	void Client::HandleWelcome(const Message& m)
	{
		if (m.role == kRoleAdmin)
		{
			if (!pendingSecret_.empty() && !serverId_.empty() && !SaveSecret(serverId_, pendingSecret_))
			{
				lastResult_ = "Paired, but the secret could not be saved on this PC";
				lastResultIsError_ = true;
			}
			pendingSecret_.clear();
			status_ = Status::Ready;
			SendSnapReq();
			return;
		}
		pendingSecret_.clear();
		if (m.role == kRoleNone) status_ = Status::NotAdmin;
		else if (m.role == kRoleUpdate) status_ = Status::UpdateClient;
		else if (m.role == kRolePair) status_ = Status::PairNeeded;
	}

	void Client::HandleSnapshotPart(const Message& m)
	{
		if (m.tag == Tag::SnapBegin)
		{
			building_ = Snapshot();
			building_.revision = m.revision;
			building_.sections = m.sections;
			assembling_ = true;
			return;
		}
		if (!assembling_) return;
		switch (m.tag)
		{
		case Tag::SnapPolicy:
			building_.defaultPolicy = m.defaultPolicy;
			building_.message = m.text;
			break;
		case Tag::SnapRule:
			building_.rules.insert(building_.rules.end(), m.rules.begin(), m.rules.end());
			break;
		case Tag::SnapMin:
			building_.mins.insert(building_.mins.end(), m.mins.begin(), m.mins.end());
			break;
		case Tag::SnapMod:
			if (m.cont && !building_.mods.empty() && building_.mods.back().plugin == m.mod.plugin)
			{
				auto& f = building_.mods.back().features;
				f.insert(f.end(), m.mod.features.begin(), m.mod.features.end());
			}
			else building_.mods.push_back(m.mod);
			break;
		case Tag::SnapPlayer:
			if (m.cont && !building_.players.empty() && building_.players.back().id == m.player.id)
			{
				auto& mods = building_.players.back().mods;
				mods.insert(mods.end(), m.player.mods.begin(), m.player.mods.end());
			}
			else building_.players.push_back(m.player);
			break;
		case Tag::SnapEnd:
			assembling_ = false;
			if (m.revision == building_.revision
				&& building_.rules.size() == m.ruleCount && building_.mins.size() == m.minCount
				&& building_.mods.size() == m.modCount && building_.players.size() == m.playerCount)
			{
				snapshot_ = std::move(building_);
				haveSnapshot_ = true;
			}
			building_ = Snapshot();
			break;
		default: break;
		}
	}

	void Client::HandleResult(const Message& m)
	{
		const bool isApply = applyReqId_ != 0 && m.requestId == applyReqId_;
		if (isApply) applyReqId_ = 0;
		if (m.status == kResultOk)
		{
			lastResult_ = m.text;
			lastResultIsError_ = false;
			if (isApply) staged_.clear();
			return;
		}
		lastResultIsError_ = true;
		if (m.status == kResultChanged)
		{
			lastResult_ = m.text.empty() ? "Changed elsewhere" : m.text;   // staged ops stay
			if (status_ == Status::Ready) SendSnapReq();
		}
		else lastResult_ = m.text;
	}

	bool Client::EncodeBatch(const std::vector<Op>& ops, uint32_t reqId, uint32_t base, std::string& out) const
	{
		Message m;
		m.tag = Tag::Ops;
		m.requestId = reqId;
		m.baseRevision = base;
		m.ops = ops;
		return Encode(m, out);
	}

	bool Client::Stage(const Op& op)
	{
		std::lock_guard<std::mutex> lock(mu_);
		if (OpArgCount(op.name) != static_cast<int>(op.args.size())) return false;
		std::vector<Op> trial = staged_;
		trial.push_back(op);
		std::string enc;
		if (!EncodeBatch(trial, 0, snapshot_.revision, enc)) return false;
		staged_ = std::move(trial);
		return true;
	}

	void Client::Unstage(size_t index)
	{
		std::lock_guard<std::mutex> lock(mu_);
		if (index < staged_.size()) staged_.erase(staged_.begin() + index);
	}

	bool Client::Apply()
	{
		std::lock_guard<std::mutex> lock(mu_);
		if (status_ != Status::Ready || !haveSnapshot_ || staged_.empty() || applyReqId_ != 0) return false;
		std::string enc;
		const uint32_t id = nextReqId_;
		if (!EncodeBatch(staged_, id, snapshot_.revision, enc) || !env_.send) return false;
		++nextReqId_;
		applyReqId_ = id;
		env_.send(enc);
		return true;
	}

	void Client::Revert()
	{
		std::lock_guard<std::mutex> lock(mu_);
		staged_.clear();
		lastResult_.clear();
		lastResultIsError_ = false;
		if (status_ == Status::Ready) SendSnapReq();
	}

	bool Client::Kick(const std::string& playerId, const std::string& reason)
	{
		std::lock_guard<std::mutex> lock(mu_);
		if (status_ != Status::Ready || !env_.send) return false;
		Op op;
		op.name = "player.kick";
		op.args = { playerId, reason };
		std::string enc;
		if (!EncodeBatch({ op }, nextReqId_, snapshot_.revision, enc)) return false;
		++nextReqId_;
		env_.send(enc);
		return true;
	}

	bool Client::Pair(const std::string& code)
	{
		std::lock_guard<std::mutex> lock(mu_);
		if (status_ != Status::PairNeeded) return false;
		AdminCrypto::Bytes raw;
		if (!AdminCrypto::RandomBytes(32, raw)) return false;
		Message m;
		m.tag = Tag::Pair;
		m.code = code;
		m.secret = AdminCrypto::ToHex(raw);
		std::string enc;
		if (!Encode(m, enc) || !env_.send) return false;
		pendingSecret_ = m.secret;
		env_.send(enc);
		return true;
	}
}
