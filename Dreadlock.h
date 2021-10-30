#pragma once

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

/// @file Dreadlock.h
/// A helper class to detect mutex deadlocks.
///
/// @author Bob Hood
/// @date 10/03/2021

#include <mutex>

#ifdef ENABLE_DREADLOCK

#include <string>
#include <map>

#if defined(_WIN32) && defined(ENABLE_WIN32_CONSOLE)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdarg>
#include <debugapi.h>
#endif

using namespace std::chrono_literals;

const bool AssertOnDeadlock = true;

// this value will trigger a console message letting you know that
// it took longer than you feel it should to acquire a lock.  set
// this to zero to disable.
const int PerformanceTimeout = 1000;

// if this amount of time has elapsed since an attempt was made to
// acquire a lock, then Dreadlock will assume a deadlock condition
// exists, and notify you via console of the deadlock location, as
// well as the location where the lock was acquired.  if
// 'AssertOnDeadlock' is enabled, an assert() will be triggered to
// draw attention to the condition.
const int DeadlockTimeout = 5000;

// if all your instrumented modules are in the same location,
// you can do away with the full path
const bool ShortModuleNames = true;

/// @class Dreadlock
/// @brief Detection of mutex deadlocks
///
/// When enabled, this class tracks the locking and unlocking of
/// mutexes within a threaded environment.  If a lock attempt
/// exceeds a user-defined timeout period (default: 5000ms), then
/// the class considers the mutex deadlocked, and reports the
/// condition along with the location of the deadlock and the
/// location that currently holds the mutex lock.  By default,
/// an assert is also thrown at that point in case debugging is
/// desired.
///
/// As a bonus, Dreadlock will also track valid ownership of
/// locks, and report when an attempt is made to unlock an mutex
/// whose lock does not belong to the instance making the attempt.
///
/// When disabled, Dreadlock is replaced by std::unique_lock<>
/// semantics in the code.

class Dreadlock
{
private: // aliases and enums
	struct LockInfo
	{
		uint32_t dreadlock_id{0};
		std::string lock_file;
		int lock_line{0};

		LockInfo() {}
		LockInfo(uint32_t did, const std::string& f, int l) : dreadlock_id(did), lock_file(f), lock_line(l) {}
	};

	using tracking_map_t = std::map<size_t, LockInfo>;

#if defined(_WIN32) && defined(ENABLE_WIN32_CONSOLE)
	int output_win32_console(const char* format, ...);
#endif

private: // data members
	static uint32_t next_id;

	static std::mutex tracking_mutex;
	static tracking_map_t tracking;

	static std::mutex printing_mutex;

	uint32_t this_dreadlock{0}; // unique key for this Dreadlock instance in the tracking database

	std::string id;
	std::mutex& mtx;
	size_t mtx_key{0};

	std::string destruct_file;
	int destruct_line{0};

private: // methods
	std::string get_module_name(const std::string& path);

public:
	Dreadlock(std::mutex& mtx, const std::string& name, const std::string& file, int line, bool defer = false);
	~Dreadlock();

	/*!
	Locks the referenced mutex, tracking the location where ownership
	was acquired.

	If the mutex is already locked, Dreadlock will wait for
	acquisition of the lock.  If the wait exceeds 'DeadlockTimeout'
	then the mutex is considered deadlocked.

	\param file String value of the full path to the module attempting the lock (usually "__FILE__")
	\param line Line number within the module attempting the lock (usually "__LINE__")
	*/
	void lock(const std::string& file, int line);

	/*!
	Unlocks the referenced mutex, tracking the location where the
	unlock takes place.

	If the mutex isn't locked by this instance, a message is printed
	and an assert is triggered.

	\param file String value of the full path to the module attempting the unlock (usually "__FILE__")
	\param line Line number within the module attempting the unlock (usually "__LINE__")
	*/
	void unlock(const std::string& file = std::string(), int line = -1);

	/*!
	This is a tracking function.  It takes the file/line where the
	Dreadlock instance will go out of scope and automatically trigger
	the destructor.  The information provided may be used in printing
	diagnostic messages.  It's use is optional, but can be helpful.

	\param file String value of the full path to the module where the instance is going out of scope (usually "__FILE__")
	\param line Line number within the module where the instance is going out of scope (usually "__LINE__")
	*/
	void destruct(const std::string& file, int line)
	{
		destruct_file = file;
		destruct_line = line;
	}
};

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#define DREADLOCK(mtx) Dreadlock dreadlock_##mtx(mtx, TOSTRING(mtx), __FILE__, __LINE__)
#define DREADLOCK_DEFER(mtx) Dreadlock dreadlock_##mtx(mtx, TOSTRING(mtx), __FILE__, __LINE__, true)
#define DREADLOCK_LOCK(mtx) dreadlock_##mtx.lock(__FILE__, __LINE__)
#define DREADLOCK_UNLOCK(mtx) dreadlock_##mtx.unlock(__FILE__, __LINE__)
#define DREADLOCK_UNLOCK_AND_DESTRUCT(mtx)                                                                                                           \
	dreadlock_##mtx.unlock(__FILE__, __LINE__);                                                                                                      \
	dreadlock_##mtx.destruct(__FILE__, __LINE__)
#define DREADLOCK_DESTRUCT(mtx) dreadlock_##mtx.destruct(__FILE__, __LINE__)

// in cases where the mutex name contains characters that are invalid
// for a C variable, these macros allow you to specify the tag

#define DREADLOCK_ID(mtx, id) Dreadlock dreadlock_##id(mtx, TOSTRING(mtx), __FILE__, __LINE__)
#define DREADLOCK_DEFER_ID(mtx, id) Dreadlock dreadlock_##id(mtx, TOSTRING(mtx), __FILE__, __LINE__, true)
#define DREADLOCK_LOCK_ID(mtx, id) dreadlock_##id.lock(__FILE__, __LINE__)
#define DREADLOCK_UNLOCK_ID(mtx, id) dreadlock_##id.unlock(__FILE__, __LINE__)
#define DREADLOCK_UNLOCK_AND_DESTRUCT_ID(mtx, id)                                                                                                    \
	dreadlock_##id.unlock(__FILE__, __LINE__);                                                                                                       \
	dreadlock_##id.destruct(__FILE__, __LINE__)
#define DREADLOCK_DESTRUCT_ID(mtx, id) dreadlock_##id.destruct(__FILE__, __LINE__)

#else // ENABLE_DREADLOCK

// Production builds replace Dreadlock with std::unique_lock functionality

#define DREADLOCK(mtx) std::unique_lock<std::mutex> lock_##mtx(mtx)
#define DREADLOCK_DEFER(mtx) std::unique_lock<std::mutex> lock_##mtx(mtx, std::defer_lock)
#define DREADLOCK_LOCK(mtx) lock_##mtx.lock()
#define DREADLOCK_UNLOCK(mtx) lock_##mtx.unlock()
#define DREADLOCK_UNLOCK_AND_DESTRUCT(mtx) lock_##mtx.unlock()
#define DREADLOCK_DESTRUCT(mtx)

#define DREADLOCK_ID(mtx, id) std::unique_lock<std::mutex> lock_##id(mtx);
#define DREADLOCK_DEFER_ID(mtx, id) std::unique_lock<std::mutex> lock_##id(mtx, std::defer_lock);
#define DREADLOCK_LOCK_ID(mtx, id) lock_##id.lock()
#define DREADLOCK_UNLOCK_ID(mtx, id) lock_##id.unlock()
#define DREADLOCK_UNLOCK_AND_DESTRUCT_ID(mtx, id) lock_##id.unlock()
#define DREADLOCK_DESTRUCT_ID(mtx, id)

#endif // ENABLE_DREADLOCK
