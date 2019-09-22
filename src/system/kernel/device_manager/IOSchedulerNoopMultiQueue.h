#ifndef IO_SCHEDULER_MULTIQUEUE_H
#define IO_SCHEDULER_MULTIQUEUE_H

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

class IOSchedulerMultiQueue : public IOScheduler {
public:
	IOSchedulerMultiQueue(DMAResource *resource);

	virtual ~IOSchedulerMultiQueue();

	virtual status_t Init(const char *name);

	virtual status_t ScheduleRequest(IORequest *request);

	virtual void AbortRequest(IORequest *request, status_t status = B_CANCELED);

	virtual void OperationCompleted(IOOperation *operation, status_t status,
									generic_size_t transferredBytes);

	virtual void Dump() const;

private:
	bool _TrySubmittingRequest(IORequest *request);

	status_t _Scheduler();

	static status_t _SchedulerThread(void *self);

	status_t _RequestNotifier();

	static status_t _RequestNotifierThread(void *self);

private:
	spinlock fFinisherLock;
	mutex fLock;
	thread_id fSchedulerThread;
	thread_id fRequestNotifierThread;
	IORequestList fScheduledRequests;
	IORequestList fFinishedRequests;
	ConditionVariable fNewRequestCondition;
	ConditionVariable fFinishedOperationCondition;
	ConditionVariable fFinishedRequestCondition;
	IOOperationList fUnusedOperations;
	IOOperationList fRescheduledOperations;
	generic_size_t fBlockSize;
	volatile bool fTerminating;
};

#endif // IO_SCHEDULER_MULTIQUEUE_H
