#pragma once

// Utility for blocking game-thread dispatch from HTTP connection threads.
//
// Usage:
//   std::string result = AdminGT::Dispatch([]() -> std::string { return R"({"ok":true})"; });
//
// The lambda runs on the game thread; this call blocks (up to 5 s).
// On timeout, returns an error JSON string.

#include <string>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <chrono>

#include "plugin_interface.h"
#include "../plugin_helpers.h"

namespace AdminGT
{
	struct GtPayload
	{
		std::function<std::string()> fn;
		std::string                  result;
		bool                         done = false;
		std::mutex                   mu;
		std::condition_variable      cv;
	};

	inline std::string Dispatch(std::function<std::string()> fn)
	{
		auto* hooks = GetHooks();
		if (!hooks || !hooks->Engine)
			return R"({"ok":false,"error":"engine unavailable"})";

		GtPayload* p = new GtPayload();
		p->fn = std::move(fn);

		hooks->Engine->PostToGameThread([](void* ctx)
		{
			GtPayload* payload = static_cast<GtPayload*>(ctx);
			payload->result = payload->fn();
			{
				std::lock_guard<std::mutex> lk(payload->mu);
				payload->done = true;
			}
			payload->cv.notify_one();
		}, p);

		std::unique_lock<std::mutex> lk(p->mu);
		p->cv.wait_for(lk, std::chrono::seconds(5), [&]{ return p->done; });

		std::string result = p->done
			? p->result
			: R"({"ok":false,"error":"game thread timeout"})";
		delete p;
		return result;
	}
}
