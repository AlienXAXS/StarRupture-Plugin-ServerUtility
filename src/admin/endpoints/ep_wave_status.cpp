#include "ep_wave_status.h"
#include "../admin_json.h"
#include "../session/session_store.h"
#include "../admin_gamethread.h"
#include "wave_common.h"

#include <cstdio>

#if WAVE_HAS_SDK
// No C++ objects with destructors in this frame — SEH is legal.
// Uses a static buffer; safe because AdminGT::Dispatch is synchronous on the game thread.
static const char* WaveStatus_SEH()
{
	static char s_buf[256];
	__try
	{
		SDK::UCrEnviroWaveSubsystem* ws = GetWaveSubsystem();
		if (!ws) return R"({"ok":false,"error":"wave subsystem unavailable"})";

		const bool  inProgress = ws->IsWaveInProgress();
		const bool  paused     = ws->IsWavePaused();
		const int   type       = static_cast<int>(ws->GetCurrentType());
		const int   stage      = static_cast<int>(ws->GetCurrentStage());
		const float progress   = ws->GetCurrentStageProgress();

		snprintf(s_buf, sizeof(s_buf),
			"{\"ok\":true,\"in_progress\":%s,\"paused\":%s,\"type\":%d,\"stage\":%d,\"progress\":%.4f}",
			inProgress ? "true" : "false",
			paused     ? "true" : "false",
			type, stage, static_cast<double>(progress));
		return s_buf;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return R"({"ok":false,"error":"exception reading wave status"})";
	}
}
#endif

namespace Ep_WaveStatus
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

		const std::string result = AdminGT::Dispatch([]() -> std::string
		{
#if WAVE_HAS_SDK
			return WaveStatus_SEH();
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
