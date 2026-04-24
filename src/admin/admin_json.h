#pragma once

#include "plugin_interface.h"
#include <string>
#include <cstring>

// Minimal JSON helpers for admin panel endpoints.
// No external library — only used by admin endpoint .cpp files.
namespace AdminJson
{
	// Extract a string value for "key" from a JSON body.
	// Returns empty string if not found.
	inline std::string ExtractString(const char* body, size_t len, const char* key)
	{
		if (!body || len == 0 || !key) return {};

		std::string haystack(body, len);

		// Look for "key":
		std::string needle = std::string("\"") + key + "\"";
		size_t pos = haystack.find(needle);
		if (pos == std::string::npos) return {};

		pos += needle.size();
		while (pos < haystack.size() && (haystack[pos] == ' ' || haystack[pos] == ':' || haystack[pos] == '\t')) ++pos;
		if (pos >= haystack.size() || haystack[pos] != '"') return {};
		++pos; // skip opening quote

		std::string result;
		while (pos < haystack.size() && haystack[pos] != '"')
		{
			if (haystack[pos] == '\\' && pos + 1 < haystack.size())
			{
				++pos;
				switch (haystack[pos])
				{
				case '"':  result += '"';  break;
				case '\\': result += '\\'; break;
				case '/':  result += '/';  break;
				case 'n':  result += '\n'; break;
				case 'r':  result += '\r'; break;
				case 't':  result += '\t'; break;
				default:   result += haystack[pos]; break;
				}
			}
			else
			{
				result += haystack[pos];
			}
			++pos;
		}
		return result;
	}

	// Extract an integer value for "key" from a JSON body.
	// Returns defaultVal if not found or not a valid integer.
	inline int ExtractInt(const char* body, size_t len, const char* key, int defaultVal = -1)
	{
		if (!body || len == 0 || !key) return defaultVal;

		std::string haystack(body, len);
		std::string needle = std::string("\"") + key + "\"";
		size_t pos = haystack.find(needle);
		if (pos == std::string::npos) return defaultVal;

		pos += needle.size();
		while (pos < haystack.size() && (haystack[pos] == ' ' || haystack[pos] == ':' || haystack[pos] == '\t')) ++pos;
		if (pos >= haystack.size()) return defaultVal;

		// Handle negative numbers
		bool negative = false;
		if (haystack[pos] == '-') { negative = true; ++pos; }
		if (pos >= haystack.size() || haystack[pos] < '0' || haystack[pos] > '9') return defaultVal;

		int value = 0;
		while (pos < haystack.size() && haystack[pos] >= '0' && haystack[pos] <= '9')
		{
			value = value * 10 + (haystack[pos] - '0');
			++pos;
		}
		return negative ? -value : value;
	}

	// Escape a string for embedding in a JSON string value.
	inline std::string Escape(const std::string& s)
	{
		std::string out;
		out.reserve(s.size() + 4);
		for (char c : s)
		{
			switch (c)
			{
			case '"':  out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n";  break;
			case '\r': out += "\\r";  break;
			case '\t': out += "\\t";  break;
			default:   out += c;      break;
			}
		}
		return out;
	}

	// Fill resp with an error JSON body. The body string is stored in outBody (caller keeps alive until callback returns).
	inline void SetError(PluginHttpResponse* resp, int statusCode, const char* msg, std::string& outBody)
	{
		outBody = std::string(R"({"ok":false,"error":")") + Escape(msg) + "\"}";
		resp->statusCode  = statusCode;
		resp->contentType = "application/json";
		resp->body        = outBody.c_str();
		resp->bodyLen     = outBody.size();
	}

	// Fill resp with a success JSON body, optionally appending extra fields (without leading comma).
	// e.g. extra = R"("session_token":"abc123")"
	inline void SetOk(PluginHttpResponse* resp, std::string& outBody, const std::string& extra = {})
	{
		if (extra.empty())
			outBody = R"({"ok":true})";
		else
			outBody = std::string(R"({"ok":true,)") + extra + "}";

		resp->statusCode  = 200;
		resp->contentType = "application/json";
		resp->body        = outBody.c_str();
		resp->bodyLen     = outBody.size();
	}

	// Helper: check that the request method is POST. Sets resp + outBody and returns false if not.
	inline bool RequirePost(const PluginHttpRequest* req, PluginHttpResponse* resp, std::string& outBody)
	{
		if (req->method == HttpMethod::Post) return true;
		SetError(resp, 405, "method not allowed", outBody);
		return false;
	}
}
