#include "ep_rules_admin.h"
#include "../admin_json.h"
#include "../admin_request.h"
#include "../admin_gamethread.h"
#include "../../host_rules/admin_ops.h"
#include "../../host_rules/host_rules.h"
#include "../../host_rules/known_features.h"
#include "../../host_rules/player_ids.h"

#include "SDK/Engine_classes.hpp"

#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace
{
	constexpr int kMaxPlayers = 128;

	using AdminJson::Escape;

	std::string StateJson()
	{
		const RulesModel::Model m = HostRules::Get();
		std::ostringstream json;
		json << "{\"default\":\"" << (m.defaultDeny ? "deny" : "allow") << "\","
		     << "\"message\":\"" << Escape(m.message) << "\","
		     << "\"rules\":[";
		for (size_t i = 0; i < m.rules.size(); ++i)
			json << (i ? "," : "") << "{\"key\":\"" << Escape(m.rules[i].plugin + "." + m.rules[i].id)
			     << "\",\"value\":\"" << (m.rules[i].adminsOnly ? "admins" : m.rules[i].deny ? "deny" : "allow") << "\"}";
		json << "],\"min_loader\":\"" << Escape(m.minLoaderVersion) << "\",\"mod_mins\":[";
		for (size_t i = 0; i < m.modMins.size(); ++i)
			json << (i ? "," : "") << "{\"plugin\":\"" << Escape(m.modMins[i].plugin)
			     << "\",\"version\":\"" << Escape(m.modMins[i].version) << "\"}";
		json << "],\"loader_receives\":" << (HostRules::LoaderReceives() ? "true" : "false")
		     << ",\"push_status\":\"" << Escape(HostRules::PushStatus()) << "\""
		     << ",\"minimums_status\":\"" << Escape(HostRules::MinimumsStatus()) << "\"}";
		return json.str();
	}

	void SendBody(PluginHttpResponse* resp, int status, std::string& body)
	{
		resp->statusCode  = status;
		resp->contentType = "application/json";
		resp->body        = body.c_str();
		resp->bodyLen     = body.size();
	}

	// Answer for a change: ok with the note, or the refusal. A stale base
	// revision is a 409 and carries the state so the page can refresh at once.
	void Answer(PluginHttpResponse* resp, std::string& body, const AdminOps::Result& r)
	{
		std::ostringstream json;
		json << "{\"ok\":" << (r.ok ? "true" : "false")
		     << (r.ok ? ",\"text\":\"" : ",\"error\":\"") << Escape(r.text) << "\""
		     << ",\"revision\":" << r.revision
		     << ",\"state\":" << StateJson() << "}";
		body = json.str();
		SendBody(resp, r.ok ? 200 : (r.stale ? 409 : 400), body);
	}

	uint64_t BaseRevision(const PluginHttpRequest* req)
	{
		const int v = AdminJson::ExtractInt(req->body, req->bodyLen, "base_revision", 0);
		return v > 0 ? static_cast<uint64_t>(v) : 0;
	}

	std::string Field(const PluginHttpRequest* req, const char* key)
	{
		return AdminJson::ExtractString(req->body, req->bodyLen, key);
	}

	void Change(const PluginHttpRequest* req, PluginHttpResponse* resp, std::string& body, const std::vector<AdminOps::Op>& ops)
	{
		Answer(resp, body, AdminOps::Run(AdminOps::Caller::FromPanel(), ops, BaseRevision(req)));
	}

	// Game thread. One "Plugin.feature" count per connected player that lists it.
	std::string CatalogJson()
	{
		SDK::APlayerController* controllers[kMaxPlayers] = {};
		const int connected = HostRules::ConnectedControllers(controllers, kMaxPlayers);

		std::map<std::string, int> listedBy;
		int reported = 0;
		for (int i = 0; i < connected; ++i)
		{
			const std::string id = PlayerIds::OfController(controllers[i]);
			std::string text;
			if (id.empty() || HostRules::ClientFeatures(id, text) != HostRules::FeaturesState::Ok)
				continue;
			++reported;

			for (const KnownFeatures::Report& r : KnownFeatures::ParseReportText(text, std::string()))
				++listedBy[r.plugin + "." + r.id];
		}

		std::ostringstream json;
		json << "\"players\":" << connected << ",\"reported\":" << reported << ",\"features\":[";
		bool firstFeature = true;
		for (const auto& f : listedBy)
		{
			json << (firstFeature ? "" : ",") << "{\"key\":\"" << Escape(f.first) << "\",\"players\":" << f.second << "}";
			firstFeature = false;
		}
		json << "],\"known\":[";
		bool firstKnown = true;
		for (const KnownFeatures::Entry& e : KnownFeatures::Snapshot())
		{
			json << (firstKnown ? "" : ",") << "{\"plugin\":\"" << Escape(e.plugin) << "\",\"id\":\"" << Escape(e.id)
			     << "\",\"title\":\"" << Escape(e.title) << "\",\"version\":\"" << Escape(e.version)
			     << "\",\"off_builds\":[";
			bool firstBuild = true;
			for (const std::string& b : e.offBuilds)
			{
				json << (firstBuild ? "" : ",") << "\"" << Escape(b) << "\"";
				firstBuild = false;
			}
			json << "],\"last_seen\":\"" << Escape(e.lastSeen) << "\",\"source\":\"" << Escape(e.source) << "\"}";
			firstKnown = false;
		}
		json << "]";
		return json.str();
	}
}

namespace Ep_RulesAdmin
{
	void Catalog(const PluginHttpRequest* req, PluginHttpResponse* resp)
	{
		std::string body;
		if (!AdminRequest::PostWithSession(req, resp, body)) return;

		const std::string catalog = AdminGT::Dispatch(CatalogJson);
		if (!catalog.empty() && catalog[0] == '{')   // Dispatch's own error JSON
		{
			body = catalog;
			SendBody(resp, 503, body);
			return;
		}
		AdminJson::SetOk(resp, body, catalog + ",\"revision\":" + std::to_string(AdminOps::Revision()) + ",\"state\":" + StateJson());
	}

	void Get(const PluginHttpRequest* req, PluginHttpResponse* resp)
	{
		std::string body;
		if (!AdminRequest::PostWithSession(req, resp, body)) return;
		AdminJson::SetOk(resp, body, "\"revision\":" + std::to_string(AdminOps::Revision()) + ",\"state\":" + StateJson());
	}

	void Set(const PluginHttpRequest* req, PluginHttpResponse* resp)
	{
		std::string body;
		if (!AdminRequest::PostWithSession(req, resp, body)) return;

		const std::string key = Field(req, "key");
		if (key.empty()) { AdminJson::SetError(resp, 400, "key required", body); return; }
		Change(req, resp, body, { AdminOps::Op{ "rule.set", { key, Field(req, "value") } } });
	}

	void Policy(const PluginHttpRequest* req, PluginHttpResponse* resp)
	{
		std::string body;
		if (!AdminRequest::PostWithSession(req, resp, body)) return;

		std::vector<AdminOps::Op> ops;
		if (AdminJson::Has(req->body, req->bodyLen, "default"))
			ops.push_back(AdminOps::Op{ "policy.default", { Field(req, "default") } });
		if (AdminJson::Has(req->body, req->bodyLen, "message"))
			ops.push_back(AdminOps::Op{ "policy.message", { Field(req, "message") } });
		if (ops.empty()) { AdminJson::SetError(resp, 400, "default or message required", body); return; }
		Change(req, resp, body, ops);
	}

	void MinVersion(const PluginHttpRequest* req, PluginHttpResponse* resp)
	{
		std::string body;
		if (!AdminRequest::PostWithSession(req, resp, body)) return;

		if (!AdminJson::Has(req->body, req->bodyLen, "version")) { AdminJson::SetError(resp, 400, "version required", body); return; }
		Change(req, resp, body, { AdminOps::Op{ "min.loader", { Field(req, "version") } } });
	}

	void ModMin(const PluginHttpRequest* req, PluginHttpResponse* resp)
	{
		std::string body;
		if (!AdminRequest::PostWithSession(req, resp, body)) return;

		if (!AdminJson::Has(req->body, req->bodyLen, "version"))
		{
			Get(req, resp);   // no version: read the state
			return;
		}
		const std::string plugin = Field(req, "plugin");
		if (plugin.empty()) { AdminJson::SetError(resp, 400, "plugin required", body); return; }
		Change(req, resp, body, { AdminOps::Op{ "min.mod", { plugin, Field(req, "version") } } });
	}
}
