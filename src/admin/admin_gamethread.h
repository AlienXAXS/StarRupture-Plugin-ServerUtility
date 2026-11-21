#pragma once

// Utility for blocking game-thread dispatch from HTTP connection threads.
//
// Usage:
//   std::string result = AdminGT::Dispatch([]() -> std::string { return R"({"ok":true})"; });
//
// The lambda runs on the game thread; this call blocks (up to 5 s).
// Called from the game thread itself, the lambda runs inline.
// On timeout, returns an error JSON string and the lambda is dropped if it
// has not started yet.

#include <string>
#include <functional>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <chrono>

#include "plugin_interface.h"
#include "../plugin_helpers.h"

namespace AdminGT
{
	// Shared by the waiting thread and the queued task, so whichever finishes
	// last frees it. All fields after fn are guarded by mu.
	struct GtPayload
	{
		std::function<std::string()> fn;
		std::string                  result;
		bool                         done = false;
		bool                         abandoned = false;
		std::mutex                   mu;
		std::condition_variable      cv;
	};

	inline std::string Dispatch(std::function<std::string()> fn)
	{
		auto* hooks = GetHooks();
		if (!hooks || !hooks->Engine)
			return R"({"ok":false,"error":"engine unavailable"})";

		// Waiting on the game thread for the game thread would block it for
		// the whole timeout.
		if (hooks->Engine->IsGameThread())
			return fn();

		auto p = std::make_shared<GtPayload>();
		p->fn = std::move(fn);

		hooks->Engine->PostToGameThread([](void* ctx)
		{
			std::unique_ptr<std::shared_ptr<GtPayload>> owner(static_cast<std::shared_ptr<GtPayload>*>(ctx));
			GtPayload& payload = **owner;
			{
				std::lock_guard<std::mutex> lk(payload.mu);
				if (payload.abandoned) return; // the caller already gave up
			}
			std::string result = payload.fn();
			{
				std::lock_guard<std::mutex> lk(payload.mu);
				payload.result = std::move(result);
				payload.done = true;
			}
			payload.cv.notify_one();
		}, new std::shared_ptr<GtPayload>(p));

		std::unique_lock<std::mutex> lk(p->mu);
		p->cv.wait_for(lk, std::chrono::seconds(5), [&]{ return p->done; });

		if (p->done)
			return p->result;

		p->abandoned = true;
		return R"({"ok":false,"error":"game thread timeout"})";
	}
}
