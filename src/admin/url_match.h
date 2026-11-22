#pragma once
#include <cstring>

// True when url ends with suffix, ignoring case. The mod loader always hands
// the url over with a trailing '/', so exactly one is ignored before matching.
inline bool UrlEndsWith(const char* url, const char* suffix)
{
	size_t ulen = strlen(url);
	const size_t slen = strlen(suffix);
	if (ulen > 0 && url[ulen - 1] == '/') --ulen;
	if (ulen < slen) return false;
	const char* tail = url + ulen - slen;
	for (size_t i = 0; i < slen; ++i)
		if ((tail[i] | 0x20) != (suffix[i] | 0x20)) return false;
	return true;
}
