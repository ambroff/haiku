#include "IOSchedulerStupid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>

#include <lock.h>
#include <thread.h>
#include <thread_types.h>
#include <util/AutoLock.h>

#include "IOSchedulerRoster.h"

//#define TRACE_IO_SCHEDULER
#ifdef TRACE_IO_SCHEDULER
#define TRACE(x...) dprintf(x)
#else
#define TRACE(x...) ;
#endif

// #pragma mark -
IOSchedulerShard::IOSchedulerShard()
	: fTerminating(false),
	  fRequestQueue("IORequest queue")
{
}

status_t IOSchedulerShard::Init(const char *name, IOSchedulerDelegate *scheduler, int32 scheduler_id, int32 shard_id) {
	fScheduler = scheduler;
	fSchedulerId = scheduler_id;
	fShardId = shard_id;

	// FIXME: Set name properly
	status_t result = fRequestQueue.Init();
	if (result != B_OK) {
		return result;
	}

	char buffer[B_OS_NAME_LENGTH];
	strlcpy(buffer, name, sizeof(buffer));
	strlcat(buffer, " scheduler request ", sizeof(buffer));
	size_t nameLength = strlen(buffer);
	snprintf(buffer + nameLength, sizeof(buffer) - nameLength, "%" B_PRId32 " %" B_PRId32, fSchedulerId, fShardId);
	fThreadId = spawn_kernel_thread(&_MainloopThread, buffer, B_NORMAL_PRIORITY + 2, reinterpret_cast<void*>(this));
	if (fThreadId < B_OK) {
		return fThreadId;
	}

	resume_thread(fThreadId);

	return B_OK;
}

void IOSchedulerShard::Stop() {
	fTerminating = true;
	fRequestQueue.Stop();
	if (fThreadId >= 0) {
		wait_for_thread(fThreadId, NULL);
	}
}

void IOSchedulerShard::Submit(IORequest *request) {
	fRequestQueue.Enqueue(request);
}

void IOSchedulerShard::Dump() const {
	dprintf("  IOSchedulerShard(%p) id=%d shard=%d\n", this, fSchedulerId, fShardId);
	fRequestQueue.Dump();
}

status_t IOSchedulerShard::_Mainloop() {
	while (true) {
		IORequest *request = fRequestQueue.Dequeue();
		if (request == NULL && fTerminating) {
			break;
		}

		fScheduler->SubmitRequest(request);
	}

	return B_OK;
}

/*static*/ status_t IOSchedulerShard::_MainloopThread(void *self) {
	return reinterpret_cast<IOSchedulerShard*>(self)->_Mainloop();
}

// #pragma mark -
IORequestQueue::IORequestQueue(const char *queue_name)
	: fTerminating(false),
	  fQueueName(queue_name)
{
	// FIXME: should have a unique names here for different queues
	mutex_init(&fLock, "I/O scheduler IORequest queue");
	fNewRequestCondition.Init(this, "I/O scheduler request queue new request available");
}

IORequestQueue::~IORequestQueue() {
	mutex_lock(&fLock);
	mutex_destroy(&fLock);

	if (fQueue.Count() > 0) {
		panic("IOScheduler deallocated before request queue was drained!");
	}
}

status_t IORequestQueue::Init() {
	return B_OK;
}

void IORequestQueue::Stop() {
	fTerminating = true;
	fNewRequestCondition.NotifyAll();
}

void IORequestQueue::Enqueue(IORequest *request) {
	TRACE("IORequestQueue(%p)::Enqueue(%p)\n", this, request);
	MutexLocker _(fLock);
	fQueue.Add(request);
	fNewRequestCondition.NotifyAll();
}

IORequest* IORequestQueue::Dequeue() {
	while (true) {
		MutexLocker locker(fLock);

		IORequest *request = fQueue.RemoveHead();
		if (request != NULL) {
			return request;
		}

		if (fTerminating) {
			break;
		}

		TRACE("IORequestQueue(%p)::Dequeue(): Waiting for next request to arrive\n", this);
		ConditionVariableEntry entry;
		fNewRequestCondition.Add(&entry);

		locker.Unlock();
		entry.Wait(B_CAN_INTERRUPT);

		TRACE("IORequestQueue(%p)::Dequeue(): Waking up\n", this);
	}

	return NULL;
}

void IORequestQueue::Dump() const {
	// FIXME: Have a proper name here.
	kprintf("  Size of %s queue: %d\n", fQueueName, fQueue.Count());
}


// #pragma mark -
IOOperationPool::IOOperationPool()
	: fTerminating(false)
{
	mutex_init(&fLock, "I/O scheduler IOOperation pool");
	fNewOperationAvailableCondition.Init(this, "I/O schedule IOOperation pool new available");
}

IOOperationPool::~IOOperationPool() {
	mutex_lock(&fLock);
	mutex_destroy(&fLock);

	while (IOOperation *operation = fUnusedOperations.RemoveHead()) {
		delete operation;
	}
}

status_t IOOperationPool::Init(generic_size_t size) {
	for (generic_size_t i = 0; i < size; ++i) {
		IOOperation *operation = new(std::nothrow) IOOperation;
		if (operation == NULL) {
			return B_NO_MEMORY;
		}

		fUnusedOperations.Add(operation);
	}

	return B_OK;
}

void IOOperationPool::Stop() {
	fTerminating = true;
	fNewOperationAvailableCondition.NotifyAll();
}

IOOperation* IOOperationPool::GetFreeOperation() {
	while (!fTerminating) {
		MutexLocker locker(fLock);
		IOOperation *operation = fUnusedOperations.RemoveHead();
		if (operation != NULL) {
			return operation;
		}

		ConditionVariableEntry entry;
		fNewOperationAvailableCondition.Add(&entry);

		locker.Unlock();
		entry.Wait(B_CAN_INTERRUPT);
	}

	return NULL;
}

IOOperation* IOOperationPool::GetFreeOperationNonBlocking() {
	MutexLocker _(fLock);
	return fUnusedOperations.RemoveHead();
}

void IOOperationPool::ReleaseIOOperation(IOOperation *operation) {
	operation->SetParent(NULL);
	MutexLocker _(fLock);
	fUnusedOperations.Add(operation);
	fNewOperationAvailableCondition.NotifyAll();
}

void IOOperationPool::Dump() const {
	kprintf("  Free IOOperations in pool: %d\n", fUnusedOperations.Count());
}

// #pragma mark -

IOSchedulerStupid::IOSchedulerStupid(DMAResource *resource)
		: IOScheduler(resource),
		  fTerminating(false),
		  fBlockSize(512),
		  fCpuCount(smp_get_num_cpus()),
		  fNotifierQueue("finished requests"),
		  fNotifierThread(-1)
{
}

IOSchedulerStupid::~IOSchedulerStupid() {
	fTerminating = true;

	for (generic_size_t i = 0; i < fCpuCount; ++i) {
		fIOSchedulerShards[i].Stop();
	}
	delete[] fIOSchedulerShards;

	fOperationPool.Stop();

	fNotifierQueue.Stop();

	if (fNotifierThread >= 0) {
		wait_for_thread(fNotifierThread, NULL);
	}
}

status_t IOSchedulerStupid::Init(const char *name) {
	status_t error = IOScheduler::Init(name);
	if (error != B_OK)
		return error;

	TRACE("%p->IOSchedulerStupid::Init(%s)\n", this, name);

	fIOSchedulerShards = new(std::nothrow) IOSchedulerShard[fCpuCount];
	if (fIOSchedulerShards == NULL) {
		return B_NO_MEMORY;
	}

	for (generic_size_t i = 0; i < fCpuCount; ++i) {
		TRACE("%p->IOSchedulerStupid::Init(%s): Initializing shard %ld\n", this, name, i);
		status_t result = fIOSchedulerShards[i].Init(name, this, fID, i);
		if (result != B_OK) {
			delete[] fIOSchedulerShards;
			return result;
		}
	}

	size_t concurrent_buffer_count = 16;

	if (fDMAResource != NULL) {
		concurrent_buffer_count = fDMAResource->BufferCount();
		fBlockSize = fDMAResource->BlockSize();
	}

	status_t init_result = fOperationPool.Init(concurrent_buffer_count);
	if (init_result != B_OK) {
		return init_result;
	}

	init_result = fNotifierQueue.Init();
	if (init_result != B_OK) {
		return init_result;
	}

	if (fBlockSize == 0) {
		fBlockSize = 512;
		TRACE("%p->IOSchedulerStupid::Init(%s) Overriding block_size to %ld since it wasn't provided by the DMAResource\n", this, name, fBlockSize);
	}

	// Start notifier thread
	{
		char buffer[B_OS_NAME_LENGTH];
		strlcpy(buffer, name, sizeof(buffer));
		strlcat(buffer, " scheduler notifier ", sizeof(buffer));
		size_t nameLength = strlen(buffer);
		snprintf(buffer + nameLength, sizeof(buffer) - nameLength, "%" B_PRId32, fID);
		fNotifierThread = spawn_kernel_thread(&_NotifierThread, buffer, B_NORMAL_PRIORITY + 2, reinterpret_cast<void*>(this));
		if (fNotifierThread < B_OK) {
			return fNotifierThread;
		}
	}

	resume_thread(fNotifierThread);

	TRACE("%p->IOSchedulerStupid::Init(%s) Initialization complete\n", this, name);

	return B_OK;
}

status_t IOSchedulerStupid::ScheduleRequest(IORequest *request) {
	TRACE("%p->IOSchedulerStupid::ScheduleRequest(%p)\n", this, request);

	IOSchedulerRoster::Default()->Notify(IO_SCHEDULER_REQUEST_SCHEDULED, this,
										 request);
	TRACE("%p->IOSchedulerStupid::ScheduleRequest(%p) request scheduled\n", this,
		  request);

	// This is the experimental option.
	IOOperation *operation = NULL;
	if (request->HasCallbacks()) {
		operation = fOperationPool.GetFreeOperationNonBlocking();
		if (operation == NULL) {
			TRACE("%p->IOSchedulerStupid::ScheduleRequest(%p) Request has callbacks and operation pool is empty. Enqueuing request for later.", this, request);
			fIOSchedulerShards[smp_get_current_cpu()].Submit(request);
			return B_OK;
		}
	} else {
		// It's assumed that it's OK to block this thread.
		// FIXME: What we really need is a ScheduleRequest() that is explicitly not async since we have
		// many callsights which call ScheduleRequest() and then immediately call request->Wait();
		operation = fOperationPool.GetFreeOperation();
	}

	SubmitRequest(request, operation);

	// This is the safe option
	//fIOSchedulerShards[smp_get_current_cpu()].Submit(request);

	return B_OK;
}

void IOSchedulerStupid::AbortRequest(IORequest *request, status_t status) {
	TRACE("%p->IOSchedulerStupid::AbortRequest(%p, %d)\n", this, request, status);
	request->SetStatusAndNotify(status);
}

void IOSchedulerStupid::OperationCompleted(IOOperation *operation,
										 status_t status,
										 generic_size_t transferredBytes) {
	TRACE("%p->IOSchedulerStupid::OperationCompleted(%p, %d, %ld)\n", this, operation, status, transferredBytes);

	// finish operation only once
	if (operation->Status() <= 0) {
		TRACE("%p->IOSchedulerStupid::OperationCompleted(%p, %d, %ld): Dropping operation because status is %d\n", this, operation, status, transferredBytes, operation->Status());
		return;
	}

	operation->SetStatus(status);

	// set the bytes transferred (of the net data)
	generic_size_t partialBegin =
			operation->OriginalOffset() - operation->Offset();
	operation->SetTransferredBytes(
			transferredBytes > partialBegin ? transferredBytes - partialBegin
											: 0);

	TRACE("%p->IOSchedulerStupid::OperationCompleted(%p, %d, %ld): Operation enqueued for finishing.\n", this, operation, status, transferredBytes);

	// _Finisher()
	IORequest *request = operation->Parent();
	{
		bool operationFinished = operation->Finish();

		TRACE("%p->IOSchedulerStupid::OperationCompleted(): Operation %p finished? %d\n",
			  this, operation, operationFinished);

		IOSchedulerRoster::Default()->Notify(IO_SCHEDULER_OPERATION_FINISHED,
											 this,
											 request, operation);

		TRACE("%p->IOSchedulerStupid::OperationCompleted(): Operation %p notified to roster\n",
			  this, operation);

		// Notify for every time the operation is passed to the I/O hook,
		// not only when it is fully finished.

		if (!operationFinished) {
			TRACE("%p->IOSchedulerStupid::OperationCompleted(): Operation: %p not finished yet\n",
				  this, operation);
			operation->SetTransferredBytes(0);
			// FIXME: Schedule for retry
			//fRescheduledOperations.Add(operation);
			//fNewRequestCondition.NotifyAll();
			panic("Not implemented to reschedule here");
			return;
		}
	
		// notify request and remove operation
		TRACE("%p->IOSchedulerStupid::OperationCompleted(): Request %p from operation %p\n",
			  this, request, operation);

		generic_size_t operationOffset =
				operation->OriginalOffset() - request->Offset();
		request->OperationFinished(
				operation, operation->Status(),
				operation->TransferredBytes() < operation->OriginalLength(),
				operation->Status() == B_OK
				? operationOffset + operation->OriginalLength()
				: operationOffset);

		TRACE("%p->IOSchedulerStupid::OperationCompleted(): operation %p finished, recycling buffer\n",
			  this, operation);
		if (fDMAResource != NULL) {
			fDMAResource->RecycleBuffer(operation->Buffer());
		}

		fOperationPool.ReleaseIOOperation(operation);

		// If the request is done, we need to perform its notifications.
		if (request->IsFinished()) {
			TRACE("%p->IOSchedulerStupid::OperationCompleted(): request %p is finished\n",
				  this, request);
			if (request->Status() == B_OK && request->RemainingBytes() > 0) {
				// The request has been processed OK so far, but it isn't really
				// finished yet.
				TRACE("%p->IOSchedulerStupid::OperationCompleted(): Setting request %p as unfinished cause remaining bytes is %ld\n",
					  this, request, request->RemainingBytes());
				request->SetUnfinished();
				fIOSchedulerShards[smp_get_current_cpu()].Submit(request);
			} else {
				if (request->HasCallbacks()) {
					TRACE("%p->IOSchedulerStupid::OperationCompleted(): request %p has callbacks, enqueuing for notifier thread\n",
						  this, request);
					// The request has callbacks that may take some time to
					// perform, so we hand it over to the request notifier.
					fNotifierQueue.Enqueue(request);
				} else {
					TRACE("%p->IOSchedulerStupid::OperationCompleted(): request %p has no callbacks. Notifying now.\n",
						  this, request);
					IOSchedulerRoster::Default()->Notify(IO_SCHEDULER_REQUEST_FINISHED,	this, request);
					request->NotifyFinished();

					TRACE("%p->IOSchedulerStupid::OperationCompleted(): request %p notified\n",
						  this, request);
				}
			}
		}
	}
}

void IOSchedulerStupid::Dump() const {
	kprintf("IOSchedulerStupid at %p\n", this);
	kprintf("  DMA resource:   %p\n", fDMAResource);
	kprintf("  fBlockSize: %ld\n", fBlockSize);
	fOperationPool.Dump();
	fNotifierQueue.Dump();
	for (generic_size_t i = 0; i < fCpuCount; ++i) {
		fIOSchedulerShards[i].Dump();
	}
}

/*static*/ status_t IOSchedulerStupid::_NotifierThread(void *self) {
	return reinterpret_cast<IOSchedulerStupid*>(self)->_Notifier();
}

status_t IOSchedulerStupid::_Notifier() {
	TRACE("%p->IOSchedulerStupid::_Notifier(): starting request notifier thread\n",
		  this);

	while (true) {
		TRACE("%p->IOSchedulerStupid::_Notifier(): Waiting for next finished request to notify\n", this);
		IORequest *request = fNotifierQueue.Dequeue();
		if (request == NULL && fTerminating) {
			break;
		}

		IOSchedulerRoster::Default()->Notify(IO_SCHEDULER_REQUEST_FINISHED,
											 this,
											 request);

		// notify the request
		TRACE("%p->IOSchedulerStupid::_Notifier(): Calling NotifyFinish() for request %p\n",
			  this, request);
		request->NotifyFinished();
	}

	return B_OK;
}

void IOSchedulerStupid::SubmitRequest(IORequest *request) {
	IOOperation *operation = fOperationPool.GetFreeOperation();
	SubmitRequest(request, operation);
}

void IOSchedulerStupid::SubmitRequest(IORequest *request, IOOperation *operation) {
	// Code from _Scheduler thread.
	if (fDMAResource != NULL) {
		generic_size_t max_operation_length = fBlockSize * 1024;
		TRACE("%p->IOSchedulerStupid::ScheduleRequest(%p): Translating next batch with %ld remaining bytes, limiting operation length to %ld\n",
				this,
				request,
				request->RemainingBytes(),
				max_operation_length);

		IOBuffer *buffer = request->Buffer();
		if (!buffer->IsMemoryLocked() && buffer->IsVirtual()) {
			status_t status = buffer->LockMemory(request->TeamID(),
												 request->IsWrite());
			if (status != B_OK) {
				TRACE("%p->IOSchedulerStupid::ScheduleRequest(%p) unable to lock memory: %d\n",
					  this, request, status);
				fOperationPool.ReleaseIOOperation(operation);
				request->SetStatusAndNotify(status);
				return; // Used to be return `status`
			}
		}

		status_t status =
				fDMAResource->TranslateNext(request, operation,
											max_operation_length);
		if (status != B_OK) {
			fOperationPool.ReleaseIOOperation(operation);

			// B_BUSY means some resource (DMABuffers or
			// DMABounceBuffers) was temporarily unavailable. That's OK,
			// we'll retry later.
			if (status == B_BUSY) {
				AbortRequest(request, status);
			}

			AbortRequest(request, status);
			return; // used to be `return status;`
		}

		IOSchedulerRoster::Default()->Notify(IO_SCHEDULER_OPERATION_STARTED,
											 this,
											 request,
											 operation);
	} else {
		// TODO: If the device has block size restrictions, we might need to use
		// a bounce buffer.
		status_t status = operation->Prepare(request);
		if (status != B_OK) {
			fOperationPool.ReleaseIOOperation(operation);
			AbortRequest(request, status);
			return; // Used to be `return true;`
		}

		operation->SetOriginalRange(request->Offset(), request->Length());
		request->Advance(request->Length());

		IOSchedulerRoster::Default()->Notify(IO_SCHEDULER_OPERATION_STARTED, this,
											 request, operation);

	}

	TRACE("%p->IOSchedulerStupid::ScheduleRequest(%p): Invoking fIOCallback for operation %p.\n", this, request, operation);
	fIOCallback(fIOCallbackData, operation);
}
