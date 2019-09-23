#ifndef IO_SCHEDULER_NOOP_MULTIQUEUE_H
#define IO_SCHEDULER_NOOP_MULTIQUEUE_H

#include <KernelExport.h>

#include <condition_variable.h>
#include <lock.h>
#include <util/OpenHashTable.h>

#include "IOScheduler.h"
#include "dma_resources.h"

// TODO:
// src/system/kernel/timer.cpp:    per_cpu_timer_data& cpuData = sPerCPU[smp_get_current_cpu()];
//
// Have a mutex and queue per core.
//
// TODO: Switch to a spinlock?

class IOSchedulerNoopMultiQueue : public IOScheduler {
public:
	IOSchedulerNoopMultiQueue(DMAResource *resource);

	virtual ~IOSchedulerNoopMultiQueue();

	virtual status_t Init(const char *name);

	virtual status_t ScheduleRequest(IORequest *request);

	virtual void AbortRequest(IORequest *request, status_t status = B_CANCELED);

	virtual void OperationCompleted(IOOperation *operation, status_t status,
									generic_size_t transferredBytes);

	virtual void Dump() const;

	// Rename to shard?
	struct IORequestQueue {
		mutex fLock;
		thread_id fSchedulerThread;
		IORequestList fScheduledRequests;
		IOOperationList fRescheduledOperations;
		ConditionVariable fNewRequestCondition;
		IOSchedulerNoopMultiQueue *fSelf;
	};

private:
	bool _TrySubmittingRequest(IORequest *request);

	status_t _Scheduler(IORequestQueue *request_queue);

	static status_t _SchedulerThread(void *self);

	status_t _RequestNotifier();

	static status_t _RequestNotifierThread(void *self);

	IORequestQueue *fIORequestQueues;

	mutex fLock;
	spinlock fFinisherLock;
	thread_id fRequestNotifierThread;
	IORequestList fFinishedRequests;
	ConditionVariable fFinishedOperationCondition;
	ConditionVariable fFinishedRequestCondition;
	IOOperationList fUnusedOperations;
	generic_size_t fBlockSize;
	generic_size_t fCPUCount;
	volatile bool fTerminating;
};

#endif // IO_SCHEDULER_NOOP_MULTIQUEUE_H
