#pragma once

// The su.cheat messages between a client mod (BetterCheats) and the ServerUtility server half.
//
//   request  req|1|<id>|<op>|key=value|...
//   reply    res|1|<id>|<status>|<reason>|key=value|...
//
// Fields are joined by US (0x1F), as in the admin wire. Pure C++17, no game or SDK types, so the
// parser and the encoder run in unit tests.
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace CheatWire
{
	constexpr const char* kPacketTag  = "su.cheat";
	constexpr const char* kPeerPlugin = "BetterCheats";   // the plugin a reply is addressed to
	constexpr char        kUS         = '\x1F';
	constexpr size_t      kMaxMessage = 1024;             // a whole message, at most
	constexpr size_t      kMaxReason  = 240;              // bytes of reply text
	constexpr uint32_t    kMaxId      = 2147483647u;

	enum class Status { Ok, Refused, Error };

	struct Request
	{
		uint32_t id = 0;
		std::string op;
		std::vector<std::pair<std::string, std::string>> fields;   // key, value; in the order sent

		// The value of the first field with this key, or nullptr.
		const std::string* Get(const char* key) const;
	};

	enum class ParseStatus
	{
		Ok,
		TooBig,          // over kMaxMessage: dropped, no reply
		UnknownKind,     // field 0 is not "req": dropped
		UnknownProtocol, // field 1 is not "1": dropped
		Malformed,       // anything else wrong; hasId says whether an id could be read to reply with
	};

	struct Parsed
	{
		ParseStatus status = ParseStatus::Malformed;
		bool        hasId  = false;
		uint32_t    id     = 0;
		Request     req;      // filled when status is Ok
	};

	Parsed ParseRequest(const uint8_t* data, size_t size);

	// One reply. The reason loses bytes below 0x20, is cut to kMaxReason on a UTF-8 boundary, and is
	// never empty for refused or error. Extra fields that would push the message past kMaxMessage
	// are left off from the end.
	std::string EncodeReply(uint32_t id, Status status, const std::string& reason,
	                        const std::vector<std::pair<std::string, std::string>>& fields = {});

	const char* StatusText(Status s);
}
