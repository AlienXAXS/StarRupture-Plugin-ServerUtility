#pragma once

// Messages between the in-game admin client and the ServerUtility server half.
//
// A message is one or more records joined by RS (0x1E); a record is fields
// joined by US (0x1F). Field 0 is the tag, field 1 the protocol number.
// Every message stays under kMaxMessage bytes. Text is stripped of US and RS and
// cut to a fixed length when encoded, so the separators never appear inside a field.
//
// Pure C++17, no game or SDK types, so both halves and the unit tests share it.

#include <cstdint>
#include <string>
#include <vector>

namespace AdminWire
{
	constexpr uint32_t    kProtocol   = 1;
	constexpr const char* kPacketTag  = "su.admin";   // the network packet type tag
	constexpr size_t      kMaxMessage = 1024;         // every message is strictly smaller
	constexpr size_t      kMaxPacket  = 4096;         // decode refuses anything larger

	constexpr char kUS = '\x1F';
	constexpr char kRS = '\x1E';

	// Splits on every `sep` and keeps empty parts: "a\t\tb" on a tab gives three.
	std::vector<std::string> Split(const std::string& s, char sep);

	// Text limits applied by the encoder (bytes, cut on a UTF-8 boundary).
	constexpr size_t kMaxName    = 64;    // plugin, version, id, key
	constexpr size_t kMaxLabel   = 96;
	constexpr size_t kMaxFlags   = 240;
	constexpr size_t kMaxMessageText = 240;

	enum class Tag
	{
		Hello, Welcome, Snap,
		SnapBegin, SnapPolicy, SnapRule, SnapMin, SnapMod, SnapPlayer, SnapEnd,
		Ops, Result, Changed,
		Challenge, Prove, Pair,
		SnapDefault   // after Pair: the enum is append-only
	};

	// Welcome roles, result statuses, section names.
	constexpr const char* kRoleAdmin  = "admin";
	constexpr const char* kRoleNone   = "none";
	constexpr const char* kRolePair   = "pair";     // Admin flag but not proven: show the pairing box
	constexpr const char* kRoleUpdate = "update";   // protocol mismatch: update the admin client
	constexpr const char* kResultOk      = "ok";
	constexpr const char* kResultChanged = "changed"; // base revision stale: "Changed elsewhere"
	constexpr const char* kResultError   = "error";
	constexpr const char* kSectionAll = "all";        // also mods, players, versions
	constexpr const char* kSectionMods = "mods";      // Snap: only the mods list. Changed: the catalog changed, the revision did not

	struct Rule      { std::string key, value; };            // value: allow | deny
	struct MinEntry  { std::string plugin, version; };       // plugin empty = the loader; version may be "wire"
	struct Feature   { std::string id, label; };
	struct ModInfo   { std::string plugin, version, wire; std::vector<Feature> features; };
	struct PlayerMod { std::string name, version; };
	struct PlayerInfo
	{
		std::string id, name, flags;   // flags: the player list flags text ("admin -exemptversion ...")
		bool online = false;
		std::vector<PlayerMod> mods;
	};

	struct Op
	{
		std::string name;                // rule.set policy.default policy.message min.loader min.mod
		std::vector<std::string> args;   // player.set player.remove player.kick catalog.push
	};

	// Number of arguments an op takes, or -1 for an unknown op.
	int OpArgCount(const std::string& name);

	struct Snapshot
	{
		uint32_t revision = 0;
		std::string sections = kSectionAll;
		std::string defaultPolicy = "allow";   // allow | deny
		std::string message;
		std::vector<Rule> rules;
		std::vector<MinEntry> mins;
		std::vector<std::string> defaults;   // "Plugin.feature" keys the server denies unless a rule names them
		std::vector<ModInfo> mods;
		std::vector<PlayerInfo> players;
	};

	// One decoded message. Which members are set depends on tag:
	//   Welcome: role, revision      Snap: sections          Changed: revision, sections
	//   SnapBegin: revision, sections   SnapPolicy: defaultPolicy, text
	//   SnapRule: rules   SnapMin: mins   SnapDefault: keys   SnapMod: mod, cont   SnapPlayer: player, cont
	//   SnapEnd: revision, counts      Ops: requestId, baseRevision, ops
	//   Result: requestId, status, text, revision
	//   Challenge: serverId, nonce (hex)   Prove: proof (64 hex, or empty)   Pair: code (empty or 8 chars), secret (64 hex)
	struct Message
	{
		Tag tag = Tag::Hello;
		uint32_t revision = 0, requestId = 0, baseRevision = 0;
		std::string role, sections, status, text, defaultPolicy;
		std::string serverId, nonce, proof, code, secret;
		std::vector<Rule> rules;
		std::vector<MinEntry> mins;
		std::vector<std::string> keys;   // SnapDefault
		ModInfo mod;
		PlayerInfo player;
		bool cont = false;   // SnapMod / SnapPlayer: continues the previous message of the same mod or player
		uint32_t ruleCount = 0, minCount = 0, modCount = 0, playerCount = 0;
		std::vector<Op> ops;
	};

	enum class Decoded { Ok, Garbage, WrongProtocol };

	// Encode one message. Returns false when the result would not fit kMaxMessage
	// or an op is unknown. Snapshot messages come from EncodeSnapshot, which pages.
	bool Encode(const Message& m, std::string& out);

	// Encode a snapshot as snap-begin, snap-policy, snap-rule*, snap-min*, snap-default*, snap-mod*,
	// snap-player*, snap-end. Every message is under kMaxMessage.
	std::vector<std::string> EncodeSnapshot(const Snapshot& s);

	// Decode. On anything but Ok the output is left empty (WrongProtocol sets only the tag
	// when the tag is known). Never throws, never reads out of range.
	Decoded Decode(const uint8_t* data, size_t size, Message& out);
	Decoded Decode(const std::string& data, Message& out);

	// Strip US and RS (to a space), cut to max bytes on a UTF-8 boundary.
	std::string CleanText(const std::string& s, size_t max);
}
