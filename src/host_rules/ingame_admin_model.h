#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../admin_wire/admin_wire.h"

// The server half of the in-game admin tool, apart from the game: the handshake
// (hello, challenge, prove, welcome), admin pairing (codes, secrets, limits) and
// the per-connection state. No game or SDK types, so it runs in unit tests.
//
// A connection is a number the glue gives each player controller it sees; this
// model never holds a controller. The glue reads the sender's player ID from the
// game on every packet and passes it in, so nothing the client sends is trusted
// as its identity.
namespace InGameAdminModel
{
	using ConnId = uint64_t;

	constexpr int    kMaxWrongPerConnection = 5;
	constexpr int    kMaxWrongPerCode       = 5;
	constexpr double kCodeLifetimeSeconds   = 600.0;   // 10 minutes
	constexpr double kNonceLifetimeSeconds  = 30.0;    // a challenge is good for one proof, this long
	constexpr double kOpsPerSecond          = 20;   // refill rate per connection: one token per op, one per other message
	constexpr double kOpsBurst              = 100;  // bucket size: at least the ops one packet can hold

	// What running a batch of ops came to (a caller-side copy of AdminOps::Result).
	struct OpsOutcome
	{
		bool        ok    = false;
		bool        stale = false;   // refused because the base revision was old
		std::string text;
		uint32_t    revision = 0;
	};

	struct Env
	{
		std::function<bool()>                          enabled;    // [Policy] InGameAdmin
		std::function<bool(const std::string&)>        isAdmin;    // the player list's Admin flag
		// The ID has a flag that gives something or loosens a rule (Admin, ExemptRules, Sections=): it
		// may pair and prove. Admin counts whether or not this is set.
		std::function<bool(const std::string&)>        canProve;
		// Some connection started or stopped being proven. Called with no lock held, on the thread
		// that made the change, so the host can re-send that connection's rules.
		std::function<void()>                          proofChanged;
		std::function<uint32_t()>                      revision;   // AdminOps::Revision
		std::function<double()>                        now;        // seconds, steady
		std::function<bool(size_t, std::vector<uint8_t>&)> random; // empty = the system generator
		std::function<void(const std::string&)>        audit;      // one log line; never holds a code or secret
		std::string                                    storePath;  // admin-pairing.txt
		// Game thread. The full state for a snapshot (revision and sections set by the model).
		std::function<AdminWire::Snapshot()>           snapshot;
		// Game thread. Runs a batch as this player; applies the in-game guards.
		std::function<OpsOutcome(const std::string& playerId, const std::string& name,
		                         const std::vector<AdminWire::Op>& ops, uint32_t baseRevision)> runOps;
		// Game thread. Rows an admin client's mod loader declared (catalog.push); remembers
		// them; the host announces a change with NoteCatalogChanged (the revision stays).
		std::function<void(const std::vector<std::string>& rows)> catalogPush;
	};

	// A message for one connection.
	struct Out
	{
		ConnId      conn = 0;
		std::string data;
	};

	class Server
	{
	public:
		explicit Server(Env env);

		// False when the server id could not be made (no random source): the tool stays silent.
		bool Ready() const;
		std::string ServerId() const;

		// One packet from a connection. playerId is what the server read for that
		// controller just now (empty when unreadable). Returns the replies to send
		// to that same connection.
		std::vector<std::string> OnPacket(ConnId conn, const std::string& playerId, const std::string& name,
		                                  const uint8_t* data, size_t size);

		// A connection ended, or the world changed (travel): proven state is gone.
		void OnDisconnect(ConnId conn);
		void OnWorldEnd();

		// True when the connection proved its secret (or paired) this session and
		// its server-read ID still has a flag that needs proof as of the last check.
		bool IsProven(ConnId conn) const;
		// The same, and the connection's ID as the server reads it now is still the ID it proved.
		// What a flag that grants something must ask: the ID can change between packets.
		bool IsProvenAs(ConnId conn, const std::string& currentId) const;

		// RCON `admin pair`: a one-time code for this ID. False with a reason when
		// the ID has no flag that needs proof. Any thread.
		bool IssueCode(const std::string& playerId, std::string& code, std::string& error);

		// RCON `admin unpair`: drop the secret and un-prove the ID's connections.
		// False when the ID had neither a secret nor a code.
		bool Unpair(const std::string& playerId);

		// The player list changed: an ID that lost the Admin flag loses its secret,
		// its code and its proven connections.
		// A connection that was told "none" and now holds the Admin flag is taken out of
		// silence and gets a challenge: those are returned to send, as OnClaimed does.
		std::vector<Out> OnPlayersChanged();

		// The connection `claimant` completed the admin claim as this ID: that
		// connection alone may pair without a code, for 10 minutes or until it
		// disconnects or the world ends. Another connection with the same ID gets
		// no shortcut. Returns the challenges to send to the ID's connections that
		// were told "none" before they were an admin.
		std::vector<Out> OnClaimed(ConnId claimant, const std::string& playerId);

		bool HasSecret(const std::string& playerId) const;

		// A change was made (from any front end): the `changed` message for every
		// connection that is proven and whose ID still has the Admin flag.
		std::vector<Out> OnChanged(const std::string& sections, uint32_t revision);

	private:
		struct Conn
		{
			std::string id;          // the server-read ID at the last packet
			std::string nonce;       // hex, one challenge
			double      nonceExpires = 0;
			bool        challenged = false;
			bool        proven     = false;
			bool        silent     = false;
			int         wrongCodes = 0;
			int         wrongProofs = 0;
			double      tokens     = kOpsBurst;   // rate limit for every message
			double      tokenTime  = 0;
			bool        limitLogged = false;
		};
		struct Code
		{
			std::string code;
			double      expires = 0;
			int         wrong   = 0;
		};

		bool Load();
		bool SaveLocked();
		bool RandomHex(size_t bytes, std::string& out);
		bool MakeChallengeLocked(Conn& c, std::string& message);
		std::string Welcome(const char* role, uint32_t revision) const;
		// snap-begin ... snap-end for a proven connection.
		std::vector<std::string> SnapshotMessages(const std::string& sections) const;
		std::vector<std::string> WithSnapshot(std::vector<std::string> out) const;
		// A proven connection's ops message.
		std::vector<std::string> CatalogPush(ConnId conn, const AdminWire::Message& m) const;
		std::vector<std::string> RunOps(ConnId conn, const std::string& playerId, const std::string& name, const AdminWire::Message& m) const;
		void UnproveIdLocked(const std::string& id);
		void SetProvenLocked(Conn& c, bool proven);
		bool CanProve(const std::string& id) const;
		struct ProofWatch;   // calls env_.proofChanged when a method changed who is proven
		void EraseClaimedLocked(const std::string& playerId);

		Env                            env_;
		mutable std::mutex             mutex_;
		std::string                    serverId_;
		std::map<std::string, std::string> secrets_;   // player ID -> secret (hex)
		std::map<ConnId, Conn>         conns_;
		uint64_t                       provenEpoch_ = 0;   // counts proven/not-proven flips
		std::map<std::string, Code>    codes_;         // player ID -> open pairing code
		struct Claimed { std::string id; double expires = 0; };
		std::map<ConnId, Claimed>      claimed_;       // connection that claimed -> claim shortcut
	};
}
