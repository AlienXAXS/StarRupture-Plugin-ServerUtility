#pragma once

// State model of the in-game admin tool, client half. No game or SDK types: the
// send function and the clock are injected, so the unit tests run without the game.
//
// Threads: OnPacket / OnServerReady / OnWorldEnd run on the network and game
// callbacks, the window reads and acts on the render thread. One mutex covers all
// state; the render thread reads a copy with GetView(). Nothing is sent from the
// render thread: outgoing messages queue, and Flush() (the game-thread tick)
// sends them.

#include "admin_wire/admin_wire.h"

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace AdminClient
{
	enum class Status
	{
		Waiting,          // no server answer yet (or between worlds)
		NoInGameAdmin,    // hello sent, no answer in 5 s: the server lacks the in-game admin
		NotAdmin,         // welcome none
		UpdateClient,     // protocol mismatch: update the admin client
		PairNeeded,       // Admin flag on the server, no matching secret: show the pairing box
		Ready
	};

	constexpr double kHelloTimeoutSeconds = 5.0;
	constexpr double kSnapshotTimeoutSeconds = 5.0;   // from snap-begin to snap-end
	constexpr size_t kMaxSnapshotParts = 512;         // messages between snap-begin and snap-end
	constexpr size_t kMaxSnapshotBytes = 1u << 20;
	constexpr const char* kRefreshing = "Refreshing...";
	constexpr int kMaxWrongCodes = 5;                 // the server refuses further codes after this many
	constexpr double kPairTimeoutSeconds = 10.0;      // no answer to a pair message
	constexpr const char* kUnknownAddress = "unknown";

	struct Env
	{
		std::function<void(const std::string&)> send;   // sends one encoded message to the server
		std::function<double()> now;                    // seconds, monotonic
		std::string storePath;                          // client secret store file
	};

	// Everything the render callback needs, copied out under the lock.
	struct View
	{
		Status status = Status::Waiting;
		bool haveSnapshot = false;           // a complete snapshot has been swapped in
		AdminWire::Snapshot snapshot;
		std::vector<AdminWire::Op> staged;   // staged.size() is the count
		std::string lastResult;              // text of the last result from the server, or a local note
		bool lastResultIsError = false;      // error or "Changed elsewhere"
		bool applyPending = false;           // a batch was sent and its result is awaited
		bool pairPending = false;            // a pair message was sent and its answer is awaited
	};

	class Client
	{
	public:
		explicit Client(const Env& env);

		// Network and game callbacks.
		void OnServerReady();                              // sends hello
		void OnPacket(const uint8_t* data, size_t size);
		void OnWorldEnd();                                 // back to waiting, drops snapshot and staged ops
		void Flush();                                      // game-thread tick: sends the queued messages
		// Game-thread tick, after the server is ready: the address of the client's own connection
		// as lower-case "ip:port", or kUnknownAddress (or empty) when the loader cannot give it.
		void SetServerAddress(const std::string& address);

		// Render thread.
		void GetView(View& out);                           // also applies the hello timeout
		bool Stage(const AdminWire::Op& op);               // false when the batch would not fit one message or the op is unknown
		void Unstage(size_t index);
		bool Apply();                                      // one batch, base = snapshot revision; false when nothing to send
		void Revert();                                     // drops staged ops and asks for a fresh snapshot
		bool Kick(const std::string& playerId, const std::string& reason);   // sent at once
		bool Pair(const std::string& code);                // code empty = claim shortcut

	private:
		void Send(const AdminWire::Message& m);
		void SendSnapReq();
		void CheckTimeout();
		void HandleWelcome(const AdminWire::Message& m);
		void HandleChallenge(const AdminWire::Message& m);
		void HandleSnapshotPart(const AdminWire::Message& m, size_t bytes);
		void AbortSnapshot();
		void HandleResult(const AdminWire::Message& m);
		void ResetSession();
		bool EncodeBatch(const std::vector<AdminWire::Op>& ops, uint32_t reqId, uint32_t base, std::string& out) const;
		bool SaveSecret(const std::string& serverId, const std::string& address, const std::string& secretHex);
		bool AddressKnown() const { return !address_.empty() && address_ != kUnknownAddress; }
		void LoadSecrets();

		Env env_;
		std::mutex mu_;
		struct Pin
		{
			std::string address;                        // the connection address the secret was paired on
			std::string secretHex;
		};
		std::map<std::string, Pin> secrets_;           // server id -> pin (loaded once)
		std::string address_ = kUnknownAddress;        // current connection address, fed by SetServerAddress

		Status status_ = Status::Waiting;
		bool helloSent_ = false;
		double helloAt_ = 0;
		std::vector<std::string> outbox_;              // encoded messages waiting for Flush()
		std::string serverId_;
		std::string pendingSecret_;                    // generated by Pair, saved on welcome admin
		std::string pendingAddress_;                   // the address current when Pair was pressed
		double pairAt_ = 0;

		bool assembling_ = false;
		double assemblyAt_ = 0;
		size_t assemblyParts_ = 0;
		size_t assemblyBytes_ = 0;
		bool snapshotRetried_ = false;                 // one automatic re-request per failed snapshot
		AdminWire::Snapshot building_;
		bool haveSnapshot_ = false;
		AdminWire::Snapshot snapshot_;

		std::vector<AdminWire::Op> staged_;
		uint32_t stagedBase_ = 0;                      // revision the staged ops were made on
		bool rebaseOnSnapshot_ = false;                // the next snapshot is the answer to our own Apply
		size_t applyCount_ = 0;                        // leading staged ops that went out with the pending Apply
		int wrongCodes_ = 0;
		uint32_t nextReqId_ = 1;
		uint32_t applyReqId_ = 0;                      // 0 = none pending
		std::string lastResult_;
		bool lastResultIsError_ = false;
	};
}
