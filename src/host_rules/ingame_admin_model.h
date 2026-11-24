#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

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
	constexpr size_t kCodeLength            = 8;

	struct Env
	{
		std::function<bool()>                          enabled;    // [Policy] InGameAdmin
		std::function<bool(const std::string&)>        isAdmin;    // the player list's Admin flag
		std::function<uint32_t()>                      revision;   // AdminOps::Revision
		std::function<double()>                        now;        // seconds, steady
		std::function<bool(size_t, std::vector<uint8_t>&)> random; // empty = the system generator
		std::function<void(const std::string&)>        audit;      // one log line; never holds a code or secret
		std::string                                    storePath;  // admin-pairing.txt
	};

	// A message for one connection.
	struct Out
	{
		ConnId      conn = 0;
		std::string data;
	};

	// What a proven connection's other messages (snap, ops) are handed to (SA3b).
	// Returns the messages to send back to that connection.
	using ProvenHandler = std::function<std::vector<std::string>(ConnId, const std::string& playerId, const std::string& name,
	                                                             const uint8_t* data, size_t size)>;

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

		void SetProvenHandler(ProvenHandler handler);

		// A connection ended, or the world changed (travel): proven state is gone.
		void OnDisconnect(ConnId conn);
		void OnWorldEnd();

		// True when the connection proved its secret (or paired) this session and
		// its server-read ID still has the Admin flag as of the last check.
		bool IsProven(ConnId conn) const;
		std::vector<ConnId> ProvenConnections() const;
		std::string PlayerIdOf(ConnId conn) const;

		// RCON `admin pair`: a one-time code for this ID. False with a reason when
		// the ID has no Admin flag. Any thread.
		bool IssueCode(const std::string& playerId, std::string& code, std::string& error);

		// RCON `admin unpair`: drop the secret and un-prove the ID's connections.
		// False when the ID had neither a secret nor a code.
		bool Unpair(const std::string& playerId);

		// The player list changed: an ID that lost the Admin flag loses its secret,
		// its code and its proven connections.
		void OnPlayersChanged();

		// The connection `claimant` completed the admin claim as this ID: that
		// connection alone may pair without a code, for 10 minutes or until it
		// disconnects or the world ends. Another connection with the same ID gets
		// no shortcut. Returns the challenges to send to the ID's connections that
		// were told "none" before they were an admin.
		std::vector<Out> OnClaimed(ConnId claimant, const std::string& playerId);

		bool HasSecret(const std::string& playerId) const;

	private:
		struct Conn
		{
			std::string id;          // the server-read ID at the last packet
			std::string nonce;       // hex, one challenge
			bool        challenged = false;
			bool        proven     = false;
			bool        silent     = false;
			int         wrongCodes = 0;
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
		void UnproveIdLocked(const std::string& id);
		void EraseClaimedLocked(const std::string& playerId);

		Env                            env_;
		mutable std::mutex             mutex_;
		std::string                    serverId_;
		std::map<std::string, std::string> secrets_;   // player ID -> secret (hex)
		std::map<ConnId, Conn>         conns_;
		std::map<std::string, Code>    codes_;         // player ID -> open pairing code
		struct Claimed { std::string id; double expires = 0; };
		std::map<ConnId, Claimed>      claimed_;       // connection that claimed -> claim shortcut
		ProvenHandler                  proven_;
	};
}
