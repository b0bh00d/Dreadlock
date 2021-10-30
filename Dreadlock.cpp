//------------------------------------------------------------------------------
// MIT License
//
// Copyright (c) 2021 Bob Hood
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to
// deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
// sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
//------------------------------------------------------------------------------

#ifdef ENABLE_DREADLOCK

#include <iostream>
#include <chrono>

#include <assert.h>

#include "Dreadlock.h"

using namespace std::chrono_literals;

uint32_t Dreadlock::next_id{0};
std::mutex Dreadlock::tracking_mutex;
Dreadlock::tracking_map_t Dreadlock::tracking;

std::mutex Dreadlock::printing_mutex;

Dreadlock::Dreadlock(std::mutex& mtx, const std::string& name, const std::string& file, int line, bool defer) : mtx(mtx), id(name)
{
	mtx_key = reinterpret_cast<size_t>(&mtx);

	std::unique_lock<std::mutex> tracking_lock(tracking_mutex);
	this_dreadlock = next_id++;
	tracking_lock.unlock();

	if (!defer)
		lock(file, line);
}

Dreadlock::~Dreadlock()
{
	// if the mutex has an entry in the tracking database,
	// then it was instantiated without an explicit unlock

	std::unique_lock<std::mutex> tracking_lock(tracking_mutex);
	auto is_locked{tracking.find(mtx_key) != tracking.end()};
	bool locked_by_me{is_locked && tracking[mtx_key].dreadlock_id == this_dreadlock};
	tracking_lock.unlock();

	if (locked_by_me)
		unlock(destruct_file.empty() ? "Dreadlock::~Dreadlock()" : destruct_file, destruct_line ? destruct_line : __LINE__);
}

#if defined(_WIN32) && defined(ENABLE_WIN32_CONSOLE)
int Dreadlock::output_win32_console(const char* format, ...)
{
	static char s_printf_buf[1024];
	va_list args;
	va_start(args, format);
	_vsnprintf_s(s_printf_buf, sizeof(s_printf_buf), sizeof(s_printf_buf), format, args);
	va_end(args);
	OutputDebugStringA(s_printf_buf);
	return 0;
}
#endif

std::string Dreadlock::get_module_name(const std::string& path)
{
	std::regex regex{R"([\\/]+)"}; // split path separator
	std::sregex_token_iterator it{path.begin(), path.end(), regex, -1};
	std::vector<std::string> items{it, {}};
	return items.back();
}

void Dreadlock::lock(const std::string& file, int line)
{
#if defined(DREADLOCK_VERBOSE)
	std::unique_lock<std::mutex> printing_lock(printing_mutex);
#if defined(_WIN32) && defined(ENABLE_WIN32_CONSOLE)
	output_win32_console("[[ Dreadlock ]] Locking %s in module %s:%d\n", id.c_str(), file.c_str(), line);
#endif
	std::cout << "[[ Dreadlock ]] Locking " << id << " in module " << file << ":" << line << std::endl;
	printing_lock.unlock();
#endif

	std::unique_lock<std::mutex> tracking_lock(tracking_mutex);

	auto module{file};
	if (ShortModuleNames)
		module = get_module_name(file);

	// if the mutex is not present in the tracking database,
	// then it is not currently lock (as far as we know, of course)
	auto is_locked{tracking.find(mtx_key) != tracking.end()};

	// is the mutex currently locked?
	if (!is_locked && mtx.try_lock())
		tracking.emplace(mtx_key, LockInfo(this_dreadlock, module, line));
	else
	{
		LockInfo* info{nullptr};

		if (is_locked)
		{
			// grab the locker's info
			info = &tracking[mtx_key];

			std::unique_lock<std::mutex> printing_lock(printing_mutex);
			if (info->dreadlock_id == this_dreadlock)
			{
				// we already hold this lock!

#if defined(_WIN32) && defined(ENABLE_WIN32_CONSOLE)
				output_win32_console(
					"[[ Dreadlock ]] Illegal lock of mutex %s in module %s:%d when already held!;\n   ... currently locked in module %s:%d.\n",
					id.c_str(),
					module.c_str(),
					line,
					info->lock_file.c_str(),
					info->lock_line);
#endif
				std::cout << "[[ Dreadlock ]] Illegal lock of mutex " << id << " in module " << module << ":" << line
						  << " when already held!;";
				if(!ShortModuleNames)
					std::cout << "\n   ...";
				std::cout << " currently locked in module " << info->lock_file << ":" << info->lock_line << std::endl;

				assert(false);
			}
#if defined(DREADLOCK_VERBOSE)
			else
			{
#if defined(_WIN32) && defined(ENABLE_WIN32_CONSOLE)
				output_win32_console(
					"[[ Dreadlock ]] Attempting to lock mutex %s in module %s:%d;\n   ... currently locked in module %s:%d.\n",
					id.c_str(),
					module.c_str(),
					line,
					info->lock_file.c_str(),
					info->lock_line);
#endif
				std::cout << "[[ Dreadlock ]] Attempting to lock mutex " << id << " in module " << module << ":" << line << ";";
				if (!ShortModuleNames)
					std::cout << "\n   ...";
				std::cout << " currently locked in module " << info->lock_file << ":" << info->lock_line << std::endl;
			}
#endif
			printing_lock.unlock();
		}

		// this mutex is already locked ... wait for it a reasonable
		// amount of time before we consider it deadlocked

		auto start{std::chrono::steady_clock::now()};
		bool reported_performance{false};
		bool reported_deadlock{false};

		tracking_lock.unlock();

		for (;;)
		{
			std::this_thread::sleep_for(500000ns);

			tracking_lock.lock();

			is_locked = tracking.find(mtx_key) != tracking.end();

			if (!is_locked)
			{
				// should be able to acquire the lock now.  if
				// we don't get it for some reason, that's a fail!
				if (mtx.try_lock())
					tracking.emplace(mtx_key, LockInfo(this_dreadlock, module, line));

				tracking_lock.unlock();

				break;
			}

			tracking_lock.unlock();

			auto elapsed{std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count()};
			if (elapsed >= DeadlockTimeout)
			{
				break; // this is a fail!
			}
			else if (PerformanceTimeout && !reported_performance && elapsed >= PerformanceTimeout)
			{
				std::unique_lock<std::mutex> printing_lock(printing_mutex);
#if defined(_WIN32) && defined(ENABLE_WIN32_CONSOLE)
				output_win32_console(
					"[[ Dreadlock ]] Waited for %s in module %s:%d longer than %dms; definite performance issue, potential deadlock.\n",
					PerformanceTimeout,
					id.c_str(),
					module.c_str(),
					line);
#endif
				std::cout << "[[ Dreadlock ]] Waited for " << id << " in module " << module << ":" << line << " longer than " << PerformanceTimeout
						  << "ms";
				if (info)
				{
					std::cout << ";";
					if (!ShortModuleNames)
						std::cout << "\n   ...";
					if (info)
						std::cout << " currently locked in module " << info->lock_file << ":" << info->lock_line;
				}
				std::cout << std::endl;

				printing_lock.unlock();

				reported_performance = true;
			}
		}

		tracking_lock.lock();

		is_locked = tracking.find(mtx_key) != tracking.end();
		bool locked_by_me{is_locked && tracking[mtx_key].dreadlock_id == this_dreadlock};

		if (!locked_by_me)
		{
			std::unique_lock<std::mutex> printing_lock(printing_mutex);
#if defined(_WIN32) && defined(ENABLE_WIN32_CONSOLE)
			output_win32_console(
				"[[ Dreadlock ]] Deadlock detected on mutex %s in module %s:%d;\n   ... currently locked in module %s:%d\n",
				id.c_str(),
				module.c_str(),
				line,
				info->lock_file.c_str(),
				info->lock_line);
#endif
			std::cout << "[[ Dreadlock ]] Deadlock detected on mutex " << id << " in module " << module << ":" << line << ";";
			if (!ShortModuleNames)
				std::cout << "\n   ...";
			std::cout << " currently locked in module " << info->lock_file << ":" << info->lock_line << std::endl;

			printing_lock.unlock();

			assert(!AssertOnDeadlock);
		}
	}
}

void Dreadlock::unlock(const std::string& file, int line)
{
	std::unique_lock<std::mutex> tracking_lock(tracking_mutex);

	auto module{file};
	if (ShortModuleNames)
		module = get_module_name(file);

	auto is_locked{tracking.find(mtx_key) != tracking.end()};
	bool locked_by_me{is_locked && tracking[mtx_key].dreadlock_id == this_dreadlock};

	if (locked_by_me)
	{
#if defined(DREADLOCK_VERBOSE)
		auto info{&tracking[mtx_key]};

		std::unique_lock<std::mutex> printing_lock(printing_mutex);
#if defined(_WIN32) && defined(ENABLE_WIN32_CONSOLE)
		output_win32_console(
			"[[ Dreadlock ]] Unlocking mutex %s in module %s:%d;\n   ... locked in module %s:%d\n",
			module.c_str(),
			line,
			id.c_str(),
			info->lock_file.c_str(),
			info->lock_line);
#endif
		std::cout << "[[ Dreadlock ]] Unlocking mutex " << id << " in module " << module << ":" << line << ";";
		if (!ShortModuleNames)
			std::cout << "\n   ...";
		std::cout << " locked in module " << info->lock_file << ":" << info->lock_line << std::endl;
		printing_lock.unlock();
#endif

		mtx.unlock();

		tracking.erase(mtx_key);
	}
	else
	{
		std::unique_lock<std::mutex> printing_lock(printing_mutex);

		if (!is_locked)
		{
#if defined(_WIN32) && defined(ENABLE_WIN32_CONSOLE)
			output_win32_console("[[ Dreadlock ]] Attempt to unlock unowned mutex %s in module %s:%d\n", id.c_str(), module.c_str(), line);
#endif
			std::cout << "[[ Dreadlock ]] Attempt to unlock unowned mutex " << id << " in module " << module << ":" << line << std::endl;
		}
		else if (!locked_by_me)
		{
			// we don't hold this lock!

			auto info{&tracking[mtx_key]};

#if defined(_WIN32) && defined(ENABLE_WIN32_CONSOLE)
			output_win32_console(
				"[[ Dreadlock ]] Illegal unlock of mutex %s by %d in module %s:%d;\n   ... locked currently held by %d in module %s:%d\n",
				id.c_str(),
				this_dreadlock,
				module.c_str(),
				line,
				info->dreadlock_id,
				info->lock_file.c_str(),
				inof->lock_line);
#endif
			std::cout << "[[ Dreadlock ]] Illegal unlock of mutex " << id << " by " << this_dreadlock << " in module " << module << ":" << line
					  << ";";
			if (!ShortModuleNames)
				std::cout << "\n   ...";
			std::cout << " locked currently held by " << info->dreadlock_id << " in module " << info->lock_file << ":" << info->lock_line
					  << std::endl;
		}

		printing_lock.unlock();

		assert(false);
	}

	tracking_lock.unlock();
}

#endif // ENABLE_DREADLOCK