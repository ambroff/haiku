#ifndef IO_SCHEDULER_NOOP_H
#define IO_SCHEDULER_NOOP_H

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

	IOOperationPool(const IOOperationPool& rhs) = delete;
	
	status_t Init(generic_size_t size);

	void Stop();
	
	IOOperation* GetFreeOperation();

	IOOperation* GetFreeOperationNonBlocking();

	void ReleaseIOOperation(IOOperation *operation);

	void Dump() const;

private:
	// FIXME: Following the pattern of using volatile, but is it even correct?
	// It should probably use atomic primitives instead.
	volatile bool fTerminating;
	spinlock fLock;
	IOOperationList fUnusedOperations;
	ConditionVariable fNewOperationAvailableCondition;
};

class IORequestQueue {
public:
	IORequestQueue(const char *queue_name);

	~IORequestQueue();

	IORequestQueue(const IORequestQueue& rhs) = delete;

	// FIXME: This should be bounded.
	status_t Init();

	void Stop();

	void Enqueue(IORequest *request);

	IORequest* Dequeue();

	void Dump() const;

private:
	volatile bool fTerminating;
	const char *fQueueName;
	IORequestList fQueue;
	spinlock fLock;
	ConditionVariable fNewRequestCondition;
};

class IOSchedulerShard {
public:
	IOSchedulerShard();

	IOSchedulerShard(const IOSchedulerShard& rhs) = delete;

	status_t Init(const char *name,
				  IOScheduler *scheduler,
				  int32 scheduler_id,
				  int32 shard_id);

	void Stop();

	void Submit(IORequest *request);

	void Dump() const;

private:
	IOScheduler *fScheduler;
	int32 fSchedulerId;
	int32 fShardId;
	volatile bool fTerminating;
	thread_id fThreadId;

	IORequestQueue fRequestQueue;

	status_t _Mainloop();

	static status_t _MainloopThread(void *self);
};

class IOSchedulerNoop : public IOScheduler {
public:
	IOSchedulerNoop(DMAResource *resource);

	virtual ~IOSchedulerNoop();

	virtual status_t Init(const char *name);

	virtual status_t SubmitRequest(IORequest *request);

	virtual status_t ScheduleRequest(IORequest *request);

	virtual void AbortRequest(IORequest *request, status_t status = B_CANCELED);

	virtual void OperationCompleted(IOOperation *operation, status_t status,
									generic_size_t transferredBytes);

	virtual void Dump() const;

private:
	volatile bool fTerminating;

	generic_size_t fBlockSize;

	IOOperationPool fOperationPool;

	generic_size_t fCpuCount;
	IOSchedulerShard *fIOSchedulerShards;

	status_t _SubmitRequest(IORequest *request, IOOperation *operation);
};

#endif // IO_SCHEDULER_NOOP_H
