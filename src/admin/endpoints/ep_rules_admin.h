#pragma once

#include "plugin_interface.h"

// The panel's host rules endpoints. Every one is a POST with a session_token.
// An optional "base_revision" makes a change refuse (409) when the rules or the
// player list changed since the page read them. Every answer carries
// "revision" and, for the state endpoints, "state" (the rules as they are now).
namespace Ep_RulesAdmin
{
	// api/rules/catalog: the features the connected players' mod loaders listed
	// (with how many players list each), the rules set so far, and the state.
	void Catalog(const PluginHttpRequest* req, PluginHttpResponse* resp);
	// api/rules/get: the state.
	void Get(const PluginHttpRequest* req, PluginHttpResponse* resp);
	// api/rules/set: {key, value: allow|admins|deny|unset}.
	void Set(const PluginHttpRequest* req, PluginHttpResponse* resp);
	// api/rules/policy: {default: allow|deny} and/or {message: text|off}, in one change.
	void Policy(const PluginHttpRequest* req, PluginHttpResponse* resp);
	// api/rules/minversion: {version: x.y.z|off}.
	void MinVersion(const PluginHttpRequest* req, PluginHttpResponse* resp);
	// api/rules/modmin: {plugin, version: x.y.z|wire|off}; without version, the state.
	void ModMin(const PluginHttpRequest* req, PluginHttpResponse* resp);
	// api/rules/stacksize: {stack_size: 0..9999|off}; without it, the state.
	void StackSize(const PluginHttpRequest* req, PluginHttpResponse* resp);
}
