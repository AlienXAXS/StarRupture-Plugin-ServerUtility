#include "admin_panel.h"
#include "admin_json.h"
#include "../plugin_helpers.h"
#include "../plugin_config.h"

#include "endpoints/ep_auth_login.h"
#include "endpoints/ep_players_list.h"
#include "endpoints/ep_players_kick.h"
#include "endpoints/ep_players_ban.h"
#include "endpoints/ep_wave_status.h"
#include "endpoints/ep_wave_start.h"
#include "endpoints/ep_wave_cancel.h"
#include "endpoints/ep_wave_pause.h"
#include "endpoints/ep_wave_resume.h"
#include "endpoints/ep_inventory_give.h"

#include <cstring>

static IPluginSelf* g_adminSelf = nullptr;

// ---------------------------------------------------------------------------
// Global raw-request filter: reject non-POST on all /ServerUtility/api/ paths.
// ---------------------------------------------------------------------------
static HttpRequestAction AdminApiFilter(const PluginHttpRequest* req)
{
	if (!req || !req->url) return HttpRequestAction::Approve;

	// Only guard our own api paths
	// URL format: /ServerUtility/api/...  (case-insensitive per SDK docs)
	// We do a simple case-insensitive prefix check.
	const char* url = req->url;

	auto ciStartsWith = [](const char* s, const char* prefix) -> bool
	{
		while (*prefix)
		{
			if ((*s | 0x20) != (*prefix | 0x20)) return false;
			++s; ++prefix;
		}
		return true;
	};

	if (!ciStartsWith(url, "/serverutility/api/"))
		return HttpRequestAction::Approve;

	// All admin API endpoints require POST
	if (req->method != HttpMethod::Post)
	{
		LOG_WARN("[AdminPanel] Rejected non-POST request to %s", url);
		return HttpRequestAction::Deny;
	}

	return HttpRequestAction::Approve;
}

// ---------------------------------------------------------------------------
// Route dispatchers (one per URL sub-prefix group)
// ---------------------------------------------------------------------------

// /ServerUtility/api/auth/...
static void AuthRouteHandler(const PluginHttpRequest* req, PluginHttpResponse* resp)
{
	// Currently only one auth endpoint: /api/auth/login
	Ep_AuthLogin::Handle(req, resp);
}

// /ServerUtility/api/players/...
static void PlayersRouteHandler(const PluginHttpRequest* req, PluginHttpResponse* resp)
{
	if (!req->url) return;

	auto ciEndsWith = [](const char* url, const char* suffix) -> bool
	{
		const size_t ulen = strlen(url);
		const size_t slen = strlen(suffix);
		if (ulen < slen) return false;
		const char* tail = url + ulen - slen;
		while (*suffix)
		{
			if ((*tail | 0x20) != (*suffix | 0x20)) return false;
			++tail; ++suffix;
		}
		return true;
	};

	if (ciEndsWith(req->url, "/list"))
		Ep_PlayersList::Handle(req, resp);
	else if (ciEndsWith(req->url, "/kick"))
		Ep_PlayersKick::Handle(req, resp);
	else if (ciEndsWith(req->url, "/ban"))
		Ep_PlayersBan::Handle(req, resp);
	else
	{
		std::string body;
		AdminJson::SetError(resp, 404, "unknown players endpoint", body);
		resp->body    = body.c_str();
		resp->bodyLen = body.size();
	}
}

// /ServerUtility/api/wave/...
static void WaveRouteHandler(const PluginHttpRequest* req, PluginHttpResponse* resp)
{
	if (!req->url) return;

	auto ciEndsWith = [](const char* url, const char* suffix) -> bool
	{
		const size_t ulen = strlen(url);
		const size_t slen = strlen(suffix);
		if (ulen < slen) return false;
		const char* tail = url + ulen - slen;
		while (*suffix)
		{
			if ((*tail | 0x20) != (*suffix | 0x20)) return false;
			++tail; ++suffix;
		}
		return true;
	};

	if (ciEndsWith(req->url, "/status"))
		Ep_WaveStatus::Handle(req, resp);
	else if (ciEndsWith(req->url, "/start"))
		Ep_WaveStart::Handle(req, resp);
	else if (ciEndsWith(req->url, "/cancel"))
		Ep_WaveCancel::Handle(req, resp);
	else if (ciEndsWith(req->url, "/pause"))
		Ep_WavePause::Handle(req, resp);
	else if (ciEndsWith(req->url, "/resume"))
		Ep_WaveResume::Handle(req, resp);
	else
	{
		std::string body;
		AdminJson::SetError(resp, 404, "unknown wave endpoint", body);
		resp->body    = body.c_str();
		resp->bodyLen = body.size();
	}
}

// /ServerUtility/api/inventory/...
static void InventoryRouteHandler(const PluginHttpRequest* req, PluginHttpResponse* resp)
{
	Ep_InventoryGive::Handle(req, resp);
}

// ---------------------------------------------------------------------------
// AdminPanel public API
// ---------------------------------------------------------------------------
namespace AdminPanel
{
	void Init(IPluginSelf* self)
	{
		if (!ServerUtilityConfig::Config::IsAdminPanelEnabled())
		{
			LOG_INFO("[AdminPanel] Disabled in config - skipping");
			return;
		}

		const std::string apiKey = ServerUtilityConfig::Config::GetAdminApiKey();
		if (apiKey.empty())
		{
			LOG_WARN("[AdminPanel] No ApiKey configured - admin panel will not start");
			return;
		}

		IPluginHttpServer* http = self->hooks->HttpServer;
		if (!http)
		{
			LOG_WARN("[AdminPanel] HttpServer interface not available - skipping");
			return;
		}

		g_adminSelf = self;

		// Static file route: serves Plugins\ServerUtility\admin\ at /ServerUtility/admin/
		http->AddRoute(self, "admin");

		// Raw API routes (sub-prefix groups)
		http->AddRawRoute(self, "api/auth",      AuthRouteHandler);
		http->AddRawRoute(self, "api/players",   PlayersRouteHandler);
		http->AddRawRoute(self, "api/wave",       WaveRouteHandler);
		http->AddRawRoute(self, "api/inventory",  InventoryRouteHandler);

		// Global filter: reject non-POST on API paths
		http->RegisterOnRawRequest(AdminApiFilter);

		LOG_INFO("[AdminPanel] Admin panel ready — static UI at /ServerUtility/admin/");
		LOG_INFO("[AdminPanel] API endpoints: /ServerUtility/api/{auth,players,wave,inventory}/...");
	}

	void Shutdown(IPluginSelf* self)
	{
		IPluginHttpServer* http = self ? self->hooks->HttpServer : nullptr;
		if (!http || !g_adminSelf)
			return;

		http->UnregisterOnRawRequest(AdminApiFilter);
		http->RemoveRawRoute(self, "api/auth");
		http->RemoveRawRoute(self, "api/players");
		http->RemoveRawRoute(self, "api/wave");
		http->RemoveRawRoute(self, "api/inventory");
		http->RemoveRoute(self, "admin");

		g_adminSelf = nullptr;
		LOG_INFO("[AdminPanel] Shutdown complete");
	}
}
