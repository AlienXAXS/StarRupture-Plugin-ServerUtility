#include "client/admin_client.h"
#include "admin_wire/admin_crypto.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

using namespace AdminWire;

namespace AdminClient
{
	namespace
	{
		const char* const kNeedsLoader = "The in-game admin needs a newer mod loader.";
		const char* const kNoAddress = "Couldn't read this server's address, so pairing is off for now.";

		// Measure, then read. False for -1 or an empty text.
		bool ReadLoaderText(const std::function<int(char*, size_t)>& fn, std::string& out)
		{
			if (!fn) return false;
			const int n = fn(nullptr, 0);
			if (n <= 0) return false;
			std::string buf(static_cast<size_t>(n) + 1, '\0');
			const int got = fn(&buf[0], buf.size());
			if (got <= 0) return false;
			out.assign(buf.c_str(), std::min(static_cast<size_t>(got), static_cast<size_t>(n)));
			return true;
		}

		bool IsHexOfLength(const std::string& s, size_t n)
		{
			if (s.size() != n) return false;
			for (char c : s)
				if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
			return true;
		}
	}

	std::vector<std::string> ParseDeclaredFeatures(const std::string& text)
	{
		constexpr size_t kMaxTitle = 80;
		std::map<std::string, std::string> versions;   // plugin -> version
		std::vector<std::vector<std::string>> features;
		size_t pos = 0;
		while (pos <= text.size())
		{
			size_t eol = text.find('\n', pos);
			if (eol == std::string::npos) eol = text.size();
			std::string line = text.substr(pos, eol - pos);
			pos = eol + 1;
			if (!line.empty() && line.back() == '\r') line.pop_back();
			const std::vector<std::string> f = Split(line, '\t');   // split on tabs only: names can hold a space
			if (f[0] == "plugin" && f.size() >= 3)
				versions[f[1]] = f[2];
			else if (f[0] == "feature" && f.size() >= 5)
				features.push_back(f);
			// format, kind, game_cl, loader and anything unknown: skipped
		}

		std::vector<std::string> rows;
		for (const std::vector<std::string>& f : features)
		{
			if (rows.size() >= kMaxCatalogRows) break;
			if (f[3] != "on" && f[3] != "off") continue;
			std::string title = f[4];
			for (size_t i = 5; i < f.size(); ++i) title += " " + f[i];
			if (title.size() > kMaxTitle)
			{
				size_t cut = kMaxTitle;
				while (cut > 0 && (static_cast<unsigned char>(title[cut]) & 0xC0) == 0x80) --cut;
				title.resize(cut);
			}
			const auto v = versions.find(f[1]);
			rows.push_back(f[1] + "\t" + (v == versions.end() ? std::string() : v->second) + "\t" + f[2] + "\t" + f[3] + "\t" + title);
		}
		return rows;
	}

	Client::Client(const Env& env) : env_(env)
	{
		LoadSecrets();
	}

	// Store format: one line per server, "<serverId> <address> <secret hex>".
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
			size_t sp2 = line.find(' ', sp + 1);
			if (sp2 == std::string::npos || sp2 == sp + 1) continue;
			std::string id = line.substr(0, sp), addr = line.substr(sp + 1, sp2 - sp - 1), secret = line.substr(sp2 + 1);
			if (IsHexOfLength(secret, 64)) secrets_[id] = Pin{ addr, secret };
		}
	}

	bool Client::SaveSecret(const std::string& serverId, const std::string& address, const std::string& secretHex)
	{
		secrets_[serverId] = Pin{ address, secretHex };
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
			for (const auto& kv : secrets_) out << kv.first << ' ' << kv.second.address << ' ' << kv.second.secretHex << '\n';
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
		if (Encode(m, enc)) outbox_.push_back(std::move(enc));
	}

	void Client::Flush()
	{
		std::vector<std::string> out;
		{
			std::lock_guard<std::mutex> lock(mu_);
			out.swap(outbox_);
		}
		if (env_.send)
			for (const std::string& msg : out) env_.send(msg);
	}

	void Client::Tick()
	{
		bool wantAddress = false, wantCatalog = false;
		{
			std::lock_guard<std::mutex> lock(mu_);
			wantAddress = helloSent_ && !addressRead_;
			wantCatalog = status_ == Status::Ready && !catalogSent_;
		}
		std::string text;
		if (wantAddress && ReadLoaderText(env_.readAddress, text))
		{
			std::lock_guard<std::mutex> lock(mu_);
			address_ = text;
			addressRead_ = true;
			if (haveHeldChallenge_)
			{
				haveHeldChallenge_ = false;
				HandleChallenge(heldChallenge_);
			}
		}
		text.clear();
		if (wantCatalog && ReadLoaderText(env_.readDeclaredFeatures, text))
		{
			const std::vector<std::string> rows = ParseDeclaredFeatures(text);
			std::lock_guard<std::mutex> lock(mu_);
			if (status_ == Status::Ready && !catalogSent_)
			{
				catalogSent_ = true;
				QueueCatalog(rows);
			}
		}
	}

	// One or more ops messages, each as many rows as fit. Their results are not shown.
	void Client::QueueCatalog(const std::vector<std::string>& rows)
	{
		std::vector<Op> batch;
		auto flush = [&]
		{
			if (batch.empty()) return;
			std::string enc;
			if (EncodeBatch(batch, nextReqId_, 0, enc))
			{
				catalogReqIds_.insert(nextReqId_);
				++nextReqId_;
				outbox_.push_back(std::move(enc));
			}
			batch.clear();
		};
		for (const std::string& row : rows)
		{
			Op op;
			op.name = "catalog.push";
			op.args = { row };
			batch.push_back(op);
			std::string enc;
			if (!EncodeBatch(batch, nextReqId_, 0, enc))
			{
				batch.pop_back();
				flush();
				batch.push_back(op);
			}
		}
		flush();
	}

	void Client::SendSnapReq(const char* sections)
	{
		Message m;
		m.tag = Tag::Snap;
		m.sections = sections;
		Send(m);
	}

	void Client::ResetSession()
	{
		status_ = Status::Waiting;
		helloSent_ = false;
		addressRead_ = false;
		address_ = kUnknownAddress;
		haveHeldChallenge_ = false;
		catalogSent_ = false;
		catalogReqIds_.clear();
		outbox_.clear();
		serverId_.clear();
		pendingSecret_.clear();
		pendingAddress_.clear();
		wrongCodes_ = 0;
		assembling_ = false;
		snapshotRetried_ = false;
		building_ = Snapshot();
		haveSnapshot_ = false;
		snapshot_ = Snapshot();
		staged_.clear();
		rebaseOnSnapshot_ = false;
		applyCount_ = 0;
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
		if (!env_.now) return;
		const double now = env_.now();
		if (status_ == Status::Waiting && helloSent_ && !haveHeldChallenge_ && now - helloAt_ >= kHelloTimeoutSeconds)
			status_ = Status::NoInGameAdmin;
		if (assembling_ && now - assemblyAt_ >= kSnapshotTimeoutSeconds) AbortSnapshot();
		if (haveHeldChallenge_ && now - heldAt_ >= kPairTimeoutSeconds)
		{
			haveHeldChallenge_ = false;
			lastResult_ = kNoAddress;
			lastResultIsError_ = true;
			status_ = Status::PairNeeded;
		}
		if (!pendingSecret_.empty() && now - pairAt_ >= kPairTimeoutSeconds)
		{
			pendingSecret_.clear();
			pendingAddress_.clear();
			lastResult_ = "No answer from the server. Try again.";
			lastResultIsError_ = true;
		}
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
		out.pairPending = !pendingSecret_.empty();
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
		case Tag::Changed:
			if (status_ == Status::Ready)
				SendSnapReq(m.sections == kSectionMods ? kSectionMods : kSectionAll);   // the catalog alone, or everything
			break;
		case Tag::Result: HandleResult(m); break;
		case Tag::SnapBegin: case Tag::SnapPolicy: case Tag::SnapRule: case Tag::SnapMin: case Tag::SnapDefault:
		case Tag::SnapMod: case Tag::SnapPlayer: case Tag::SnapEnd:
			HandleSnapshotPart(m, size); break;
		default: break;
		}
	}

	void Client::HandleChallenge(const Message& m)
	{
		// One held challenge at a time: a server that keeps challenging must not restart the timeout.
		if (haveHeldChallenge_) return;
		serverId_ = m.serverId;
		Message p;
		p.tag = Tag::Prove;
		auto it = secrets_.find(m.serverId);
		if (it != secrets_.end())
		{
			// The server id comes from the server, so it proves nothing: answer only on the
			// address the secret was paired on, or a hostile server could relay a challenge.
			if (!AddressKnown())
			{
				if (env_.readAddress)
				{
					// The address is read on the game thread's next tick: answer then, or give up after the timeout.
					heldChallenge_ = m;
					haveHeldChallenge_ = true;
					heldAt_ = env_.now ? env_.now() : 0;
					return;
				}
				lastResult_ = kNeedsLoader;
				lastResultIsError_ = true;
				status_ = Status::PairNeeded;
				return;
			}
			if (it->second.address != address_)
			{
				lastResult_ = "This server's address doesn't match the one you paired with. Ask the host for a new code.";
				lastResultIsError_ = true;
				status_ = Status::PairNeeded;
				return;
			}
			std::string proof;
			if (AdminCrypto::MakeProof(it->second.secretHex, m.nonce, m.serverId, proof)) p.proof = proof;
		}
		Send(p);
	}

	void Client::HandleWelcome(const Message& m)
	{
		if (m.role == kRoleAdmin)
		{
			if (!pendingSecret_.empty() && !serverId_.empty() && !SaveSecret(serverId_, pendingAddress_, pendingSecret_))
			{
				lastResult_ = "Paired, but the secret could not be saved on this PC";
				lastResultIsError_ = true;
			}
			pendingSecret_.clear();
			pendingAddress_.clear();
			status_ = Status::Ready;   // the server pushes the snapshot with this welcome
			return;
		}
		if (m.role == kRolePair && !pendingSecret_.empty())
		{
			lastResult_ = ++wrongCodes_ >= kMaxWrongCodes
				? "Too many wrong codes. Reconnect, or ask the host for a new code"
				: "Wrong code, try again";
			lastResultIsError_ = true;
		}
		pendingSecret_.clear();
		pendingAddress_.clear();
		if (m.role == kRoleNone) status_ = Status::NotAdmin;
		else if (m.role == kRoleUpdate) status_ = Status::UpdateClient;
		else if (m.role == kRolePair) status_ = Status::PairNeeded;
	}

	void Client::AbortSnapshot()
	{
		assembling_ = false;
		building_ = Snapshot();
		if (status_ != Status::Ready) return;
		if (!snapshotRetried_)
		{
			snapshotRetried_ = true;
			lastResult_ = kRefreshing;
			lastResultIsError_ = false;
			SendSnapReq();
		}
		else
		{
			lastResult_ = "The server's settings could not be loaded. Press Revert to try again";
			lastResultIsError_ = true;
		}
	}

	void Client::HandleSnapshotPart(const Message& m, size_t bytes)
	{
		if (m.tag == Tag::SnapBegin)
		{
			building_ = Snapshot();
			building_.revision = m.revision;
			building_.sections = m.sections;
			assembling_ = true;
			assemblyAt_ = env_.now ? env_.now() : 0;
			assemblyParts_ = 0;
			assemblyBytes_ = 0;
			return;
		}
		if (!assembling_) return;
		assemblyBytes_ += bytes;
		if (++assemblyParts_ > kMaxSnapshotParts || assemblyBytes_ > kMaxSnapshotBytes)
		{
			AbortSnapshot();
			return;
		}
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
		case Tag::SnapDefault:
			building_.defaults.insert(building_.defaults.end(), m.keys.begin(), m.keys.end());
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
				if (building_.sections == kSectionMods)
				{
					// Only the mods list: the rules, the revision and any staged edits stay as they are.
					snapshot_.mods = std::move(building_.mods);
					building_ = Snapshot();
					break;
				}
				snapshot_ = std::move(building_);
				haveSnapshot_ = true;
				snapshotRetried_ = false;
				if (lastResult_ == kRefreshing) lastResult_.clear();
				if (!staged_.empty())
				{
					if (rebaseOnSnapshot_) stagedBase_ = snapshot_.revision;
					else if (snapshot_.revision != stagedBase_)
					{
						lastResult_ = "The server changed since you started editing. Apply will check your edits.";
						lastResultIsError_ = true;
					}
				}
				rebaseOnSnapshot_ = false;
			}
			else AbortSnapshot();
			building_ = Snapshot();
			break;
		default: break;
		}
	}

	void Client::HandleResult(const Message& m)
	{
		if (catalogReqIds_.erase(m.requestId) > 0) return;   // our own catalog push: nothing to show
		const bool isApply = applyReqId_ != 0 && m.requestId == applyReqId_;
		const size_t sent = isApply ? applyCount_ : 0;
		if (isApply) { applyReqId_ = 0; applyCount_ = 0; }
		if (m.status == kResultOk)
		{
			lastResult_ = m.text;
			lastResultIsError_ = false;
			if (isApply)
			{
				// Only the ops that went out; edits made while the Apply was pending stay.
				staged_.erase(staged_.begin(), staged_.begin() + std::min(sent, staged_.size()));
				rebaseOnSnapshot_ = !staged_.empty();   // the refresh that follows is our own change
			}
			return;
		}
		lastResultIsError_ = true;
		if (m.status == kResultChanged)
		{
			lastResult_ = m.text.empty() ? "Changed elsewhere" : m.text;   // staged ops stay, on the refreshed revision
			rebaseOnSnapshot_ = true;
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
		if (staged_.empty()) stagedBase_ = snapshot_.revision;
		staged_ = std::move(trial);
		return true;
	}

	void Client::Unstage(size_t index)
	{
		std::lock_guard<std::mutex> lock(mu_);
		if (index >= staged_.size()) return;
		staged_.erase(staged_.begin() + index);
		if (index < applyCount_) --applyCount_;
	}

	bool Client::Apply()
	{
		std::lock_guard<std::mutex> lock(mu_);
		if (status_ != Status::Ready || !haveSnapshot_ || staged_.empty() || applyReqId_ != 0) return false;
		std::string enc;
		const uint32_t id = nextReqId_;
		if (!EncodeBatch(staged_, id, stagedBase_, enc)) return false;
		++nextReqId_;
		applyReqId_ = id;
		applyCount_ = staged_.size();
		outbox_.push_back(std::move(enc));
		return true;
	}

	void Client::Revert()
	{
		std::lock_guard<std::mutex> lock(mu_);
		staged_.clear();
		applyCount_ = 0;
		snapshotRetried_ = false;
		lastResult_.clear();
		lastResultIsError_ = false;
		if (status_ == Status::Ready) SendSnapReq();
	}

	bool Client::Kick(const std::string& playerId, const std::string& reason)
	{
		std::lock_guard<std::mutex> lock(mu_);
		if (status_ != Status::Ready) return false;
		Op op;
		op.name = "player.kick";
		op.args = { playerId, reason };
		std::string enc;
		if (!EncodeBatch({ op }, nextReqId_, snapshot_.revision, enc)) return false;
		++nextReqId_;
		outbox_.push_back(std::move(enc));
		return true;
	}

	bool Client::Pair(const std::string& code)
	{
		std::lock_guard<std::mutex> lock(mu_);
		if (status_ != Status::PairNeeded || !pendingSecret_.empty() || wrongCodes_ >= kMaxWrongCodes) return false;
		if (!AddressKnown())
		{
			lastResult_ = env_.readAddress ? kNoAddress : kNeedsLoader;
			lastResultIsError_ = true;
			return false;
		}
		AdminCrypto::Bytes raw;
		if (!AdminCrypto::RandomBytes(32, raw)) return false;
		Message m;
		m.tag = Tag::Pair;
		m.code = code;
		m.secret = AdminCrypto::ToHex(raw);
		std::string enc;
		if (!Encode(m, enc)) return false;
		pendingSecret_ = m.secret;
		pendingAddress_ = address_;
		pairAt_ = env_.now ? env_.now() : 0;
		outbox_.push_back(std::move(enc));
		return true;
	}
}
