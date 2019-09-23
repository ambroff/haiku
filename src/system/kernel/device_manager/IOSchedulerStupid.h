#ifndef IO_SCHEDULER_STUPID_H
#define IO_SCHEDULER_STUPID_H

#include <KernelExport.h>

#include <condition_variable.h>
#include <lock.h>
#include <util/OpenHashTable.h>

#include "IOScheduler.h"
#include "dma_resources.h"

class IOOperationPool {
public:
	IOOperationPool();

	~IOOperationPool();
	
	status_t Init(generic_size_t size);
	
	IOOperation* GetFreeOperation();

	void ReleaseIOOperation(IOOperation *operation);

	void Dump() const;

private:
	volatile bool fTerminating;
	mutex fLock;
	IOOperationList fUnusedOperations;
	ConditionVariable fNewOperationAvailableCondition;
};

class IOSchedulerStupid : public IOScheduler {
public:
	IOSchedulerStupid(DMAResource *resource);

	virtual ~IOSchedulerStupid();

	virtual status_t Init(const char *name);

	virtual status_t ScheduleRequest(IORequest *request);

	virtual void AbortRequest(IORequest *request, status_t status = B_CANCELED);

	virtual void OperationCompleted(IOOperation *operation, status_t status,
									generic_size_t transferredBytes);

	virtual void Dump() const;

private:
	volatile bool fTerminating;

	generic_size_t fBlockSize;

	IOOperationPool fOperationPool;

	thread_id fNotifierThread;
	mutex fNotifierLock;
	ConditionVariable fNotifierCondition;
	IORequestList fNotifierQueue;

	static status_t _NotifierThread(void *self);
	status_t _Notifier();
};

#endif // IO_SCHEDULER_STUPID_H
