// SPDX-License-Identifier: MIT
// The kernel calls TegraGM20BSharedWiring.cpp makes, on the host: a task's mapping as a map from address to physical
// page, IOMemoryDescriptor's complete/release counted, IOLock, IONew/IODelete.
#pragma once
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>

typedef struct task *task_t;
typedef struct pmap *pmap_t;

struct IOLock {
	std::mutex m;
};

inline IOLock *IOLockAlloc()
{
	return new IOLock;
}

inline void IOLockFree(IOLock *lock)
{
	delete lock;
}

inline void IOLockLock(IOLock *lock) __attribute__((no_thread_safety_analysis))
{
	lock->m.lock();
}

inline void IOLockUnlock(IOLock *lock) __attribute__((no_thread_safety_analysis))
{
	lock->m.unlock();
}

#define IONew(type, count) (new type[(count)])
#define IODelete(ptr, type, count) (delete[] (ptr))

struct IOMemoryDescriptor {
	int completed = 0, released = 0;

	void complete()
	{
		completed++;
	}

	void release()
	{
		released++;
	}
};

// Each task's mapping: user address -> physical address of its page.
std::map<std::pair<task_t, uint64_t>, uint64_t> &mappings();
