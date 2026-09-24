#pragma once

#include "plugin_interface.h"

#include <cstdint>

// ---------------------------------------------------------------------------
// scan_helpers.h
//
// One place to build a PluginScanRequest, because ServerUtility resolves its
// patterns from six different files and they should not each grow their own
// copy of the boilerplate.
//
// Every pattern in game_signatures.h is a function prologue whose address is
// either called directly or has a detour written over it, so every request
// declares PLUGIN_SCAN_FUNCTION_START. That is the part that matters: a
// pattern which drifted into the middle of an unrelated function still
// matches, and without a declared kind the loader hands that address back and
// the plugin writes a jump over it. FUNCTION_START makes the loader check the
// match against the executable's exception directory first -- a real function
// entry, long enough to hold the 14-byte detour -- and turns a drifted pattern
// into a named line in the failure report.
//
// Optional is a label on that report line, not a lighter verdict: a miss
// refuses the plugin either way. It marks the addresses whose null return the
// calling code genuinely handles, which is all of these.
//
// Callable only from OnPluginLoadHooks; IPluginHookScanner refuses any call
// made outside that event.
// ---------------------------------------------------------------------------
namespace ScanUtil
{
	inline uintptr_t ResolveFunction(IPluginSelf* self, IPluginHookScanner* scanner,
		const char* hookName, const char* pattern)
	{
		if (!self || !scanner)
			return 0;

		PluginScanRequest req = PLUGIN_SCAN_REQUEST_INIT;
		req.hookName = hookName;
		req.pattern  = pattern;
		req.kind     = PLUGIN_SCAN_FUNCTION_START;
		req.flags    = PLUGIN_SCAN_FLAG_OPTIONAL;

		return scanner->Resolve(self, &req);
	}
}
