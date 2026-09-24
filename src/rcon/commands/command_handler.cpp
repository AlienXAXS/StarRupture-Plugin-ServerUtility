#include "command_handler.h"
#include "plugin_helpers.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>

namespace PluginCommands
{
	static IPluginConsole* GetConsole()
	{
		IPluginHooks* hooks = GetHooks();
		return hooks ? hooks->Console : nullptr;
	}

	// -----------------------------------------------------------------------
	// Handler side: the loader calls this for every command we registered,
	// with the CommandFunc as userData.
	// -----------------------------------------------------------------------
	static void WriteLines(IPluginConsole* console, PluginConsoleSink sink, const std::string& text)
	{
		size_t start = 0;
		while (start < text.size())
		{
			size_t end = text.find('\n', start);
			if (end == std::string::npos) end = text.size();

			std::string line = text.substr(start, end - start);
			if (!line.empty() && line.back() == '\r')
				line.pop_back();

			const PluginConsoleLineKind kind = (line.rfind("Error:", 0) == 0)
				? PluginConsoleLineKind::Error
				: PluginConsoleLineKind::Output;
			console->Write(sink, kind, line.c_str());

			start = end + 1;
		}
	}

	static void Trampoline(const char* const* argv, int argc, PluginConsoleSink sink, void* userData)
	{
		IPluginConsole* console = GetConsole();
		CommandFunc fn = reinterpret_cast<CommandFunc>(userData);
		if (!console || !fn)
			return;

		std::string args;
		for (int i = 1; i < argc; ++i)
		{
			if (i > 1) args += ' ';
			args += argv[i];
		}

		std::string output;
		try
		{
			output = fn(args);
		}
		catch (const std::exception& ex)
		{
			output = std::string("Error: ") + ex.what();
		}
		catch (...)
		{
			output = "Error: unknown exception in command handler.";
		}

		WriteLines(console, sink, output);
	}

	bool Register(const char* name, const char* aliases, const char* usage,
	              const char* help, CommandFunc fn)
	{
		IPluginConsole* console = GetConsole();
		IPluginSelf* self = GetSelf();
		if (!console || !self || !fn)
			return false;

		PluginConsoleCommandDesc desc = {};
		desc.name       = name;
		desc.aliases    = aliases;
		desc.usage      = usage;
		desc.help       = help;
		desc.handler    = Trampoline;
		desc.userData   = reinterpret_cast<void*>(fn);
		desc.gameThread = true;

		if (!console->RegisterCommand(self, &desc))
		{
			LOG_WARN("[Commands] Could not register '%s' - the name or an alias is already taken", name);
			return false;
		}
		return true;
	}

	void UnregisterAll()
	{
		IPluginConsole* console = GetConsole();
		IPluginSelf* self = GetSelf();
		if (console && self)
			console->UnregisterAllCommands(self);
	}

	// -----------------------------------------------------------------------
	// Caller side: collect one ExecuteWithEngine run into a string.
	//
	// Two owners, because a timed-out wait returns while the command is still
	// queued and will write into this later: the waiting thread and the
	// completion callback each hold a reference, and whichever lets go last
	// frees it.
	// -----------------------------------------------------------------------
	struct Pending
	{
		std::mutex              mutex;
		std::condition_variable cv;
		std::string             text;
		bool                    done = false;
		std::atomic<int>        refs{ 2 };

		void Release()
		{
			if (refs.fetch_sub(1) == 1)
				delete this;
		}
	};

	static void OnLine(PluginConsoleLineKind /*kind*/, const char* text, void* userData)
	{
		Pending* p = static_cast<Pending*>(userData);
		std::lock_guard<std::mutex> lk(p->mutex);
		p->text += text ? text : "";
		p->text += '\n';
	}

	static void OnComplete(void* userData)
	{
		Pending* p = static_cast<Pending*>(userData);
		{
			std::lock_guard<std::mutex> lk(p->mutex);
			p->done = true;
			p->cv.notify_all();
		}
		p->Release();
	}

	std::string Execute(const std::string& line)
	{
		IPluginConsole* console = GetConsole();
		IPluginSelf* self = GetSelf();
		if (!console || !self)
			return "Error: the mod loader console interface is not available.\n";

		const size_t first = line.find_first_not_of(" \t");
		const std::string command = (first == std::string::npos) ? std::string("help") : line.substr(first);

		Pending* p = new Pending();
		if (!console->ExecuteWithEngine(self, command.c_str(), OnLine, OnComplete, p))
		{
			delete p;   // nothing ran, so the callback will never release it
			return "Error: the mod loader refused the command.\n";
		}

		std::string result;
		{
			std::unique_lock<std::mutex> lk(p->mutex);
			const bool finished = p->cv.wait_for(lk, std::chrono::seconds(30), [p]() { return p->done; });
			result = p->text;
			if (!finished)
			{
				LOG_WARN("[RCON] Command '%s' timed out waiting for the game thread (30s)", command.c_str());
				result += "Error: command timed out waiting for the game thread.\n";
			}
		}
		p->Release();

		if (result.empty())
			result = "(no output)\n";
		return result;
	}
}
