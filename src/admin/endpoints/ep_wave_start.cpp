#include "ep_wave_start.h"
#include "../admin_json.h"
#include "../session/session_store.h"
#include "../admin_gamethread.h"
#include "wave_common.h"

namespace Ep_WaveStart
{
	void Handle(const PluginHttpRequest* req, PluginHttpResponse* resp)
	{
		std::string body;

		if (!AdminJson::RequirePost(req, resp, body)) return;

		const std::string token = AdminJson::ExtractString(req->body, req->bodyLen, "session_token");
		if (!AdminSessionStore::Get().ValidateSession(token))
		{
			AdminJson::SetError(resp, 401, "unauthorized", body);
			return;
		}

		const int waveType = AdminJson::ExtractInt(req->body, req->bodyLen, "wave_type");
		if (waveType < 0)
		{
			AdminJson::SetError(resp, 400, "wave_type required", body);
			return;
		}

		const std::string result = AdminGT::Dispatch([waveType]() -> std::string
		{
#if WAVE_HAS_SDK
			__try
			{
				SDK::UCrEnviroWaveSubsystem* ws = GetWaveSubsystem();
				if (!ws) return R"({"ok":false,"error":"wave subsystem unavailable"})";

				ws->StartWave(static_cast<SDK::EEnviroWave>(waveType));
				return R"({"ok":true})";
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return R"({"ok":false,"error":"exception starting wave"})";
			}
#else
			return R"({"ok":false,"error":"SDK not available in this build"})";
#endif
		});

		body = result;
		resp->statusCode  = 200;
		resp->contentType = "application/json";
		resp->body        = body.c_str();
		resp->bodyLen     = body.size();
	}
}
