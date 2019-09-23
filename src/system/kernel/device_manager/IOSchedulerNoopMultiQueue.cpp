#include "IOSchedulerNoopMultiQueue.h"

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

IOSchedulerNoopMultiQueue::IOSchedulerNoopMultiQueue(DMAResource *resource)
		: IOScheduler(resource),
		  fRequestNotifierThread(-1),
		  fBlockSize(0), fCPUCount(smp_get_num_cpus()), fTerminating(false) {
	mutex_init(&fLock, "I/O scheduler");

	fIORequestQueues = new IORequestQueue[fCPUCount];
	for (generic_size_t i = 0; i < fCPUCount; ++i) {
		IORequestQueue& queue_info = fIORequestQueues[i];
		mutex_init(&queue_info.fLock, "I/O scheduler");
		queue_info.fNewRequestCondition.Init(&queue_info, "I/O new request");
		queue_info.fSelf = this;
	}

	fFinishedOperationCondition.Init(this, "I/O finished operation");
	fFinishedRequestCondition.Init(this, "I/O finished request");
}

IOSchedulerNoopMultiQueue::~IOSchedulerNoopMultiQueue() {
	// shutdown threads
	fTerminating = true;

	for (generic_size_t i = 0; i < fCPUCount; ++i) {
		fIORequestQueues[i].fNewRequestCondition.NotifyAll();
		wait_for_thread(fIORequestQueues[i].fSchedulerThread, NULL);
	}

	fFinishedOperationCondition.NotifyAll();
	fFinishedRequestCondition.NotifyAll();

	if (fRequestNotifierThread >= 0) {
		wait_for_thread(fRequestNotifierThread, NULL);
	}

	// destroy our belongings
	mutex_lock(&fLock);
	mutex_destroy(&fLock);

	for (generic_size_t i = 0; i < fCPUCount; ++i) {
		mutex_lock(&fIORequestQueues[i].fLock);
		mutex_destroy(&fIORequestQueues[i].fLock);
	}

	while (IOOperation *operation = fUnusedOperations.RemoveHead()) {
		delete operation;
	}
}

status_t IOSchedulerNoopMultiQueue::Init(const char *name) {
	status_t error = IOScheduler::Init(name);
	if (error != B_OK)
		return error;

	size_t count = fDMAResource != NULL ? fDMAResource->BufferCount() : 16;
	for (size_t i = 0; i < count; i++) {
		IOOperation *operation = new(std::nothrow) IOOperation;
		if (operation == NULL)
			return B_NO_MEMORY;

		fUnusedOperations.Add(operation);
	}
	TRACE("%p->IOSchedulerNoopMultiQueue::Init(%s): Pre-allocated %d operations\n", this,
		  name, fUnusedOperations.Count());

	if (fDMAResource != NULL) {
		fBlockSize = fDMAResource->BlockSize();
		TRACE("%p->IOSchedulerNoopMultiQueue::Init(%s): Block size is %ld according to DMA device.\n", this, name, fBlockSize);
	}

	// FIXME: Should this be hard-coded to 512? It's set to 2KiB when formatting.
	// It should probably be probed. Linux system says 4096.
	if (fBlockSize == 0) {
		fBlockSize = 512;
	}

	// start threads
	char buffer[B_OS_NAME_LENGTH];
	size_t nameLength = strlen(buffer);

	for (generic_size_t i = 0; i < fCPUCount; ++i) {
		IORequestQueue& queue_info = fIORequestQueues[i];

		strlcpy(buffer, name, sizeof(buffer));
		strlcat(buffer, " scheduler ", sizeof(buffer));

		snprintf(buffer + nameLength, sizeof(buffer) - nameLength, "%" B_PRId32 " %" B_PRId64,
				fID, i);
		queue_info.fSchedulerThread = spawn_kernel_thread(&_SchedulerThread, buffer,
											   B_NORMAL_PRIORITY + 2,
											   reinterpret_cast<void *>(&queue_info));
		if (queue_info.fSchedulerThread < B_OK) {
			return queue_info.fSchedulerThread;
		}
	}


	strlcpy(buffer, name, sizeof(buffer));
	strlcat(buffer, " notifier ", sizeof(buffer));
	nameLength = strlen(buffer);
	snprintf(buffer + nameLength, sizeof(buffer) - nameLength, "%" B_PRId32,
			 fID);
	fRequestNotifierThread = spawn_kernel_thread(
			&_RequestNotifierThread, buffer, B_NORMAL_PRIORITY + 2,
			reinterpret_cast<void *>(this));
	if (fRequestNotifierThread < B_OK) {
		return fRequestNotifierThread;
	}

	for (generic_size_t i = 0; i < fCPUCount; ++i) {
		resume_thread(fIORequestQueues[i].fSchedulerThread);
	}

	resume_thread(fRequestNotifierThread);

	return B_OK;
}

status_t IOSchedulerNoopMultiQueue::ScheduleRequest(IORequest *request) {
	TRACE("%p->IOSchedulerNoopMultiQueue::ScheduleRequest(%p)\n", this, request);

	IOBuffer *buffer = request->Buffer();

	// TODO: it would be nice to be able to lock the memory later, but we can't
	// easily do it in the I/O scheduler without being able to asynchronously
	// lock memory (via another thread or a dedicated call).

	if (buffer->IsVirtual()) {
		status_t status = buffer->LockMemory(request->TeamID(),
											 request->IsWrite());
		if (status != B_OK) {
			TRACE("%p->IOSchedulerNoopMultiQueue::ScheduleRequest(%p) unable to lock memory: %d\n",
				  this, request, status);
			request->SetStatusAndNotify(status);
			return status;
		}
	}

	IORequestQueue& queue_info = fIORequestQueues[smp_get_current_cpu()];
	{
		MutexLocker locker(queue_info.fLock);

		queue_info.fScheduledRequests.Add(request);
		queue_info.fNewRequestCondition.NotifyAll();
	}

	IOSchedulerRoster::Default()->Notify(IO_SCHEDULER_REQUEST_SCHEDULED, this,
										 request);
	TRACE("%p->IOSchedulerNoopMultiQueue::ScheduleRequest(%p) request scheduled\n", this,
		  request);

	return B_OK;
}

void IOSchedulerNoopMultiQueue::AbortRequest(IORequest *request, status_t status) {
	TRACE("%p->IOSchedulerNoopMultiQueue::AbortRequest(%p, %d)\n", this, request, status);
	request->SetStatusAndNotify(status);
}

void IOSchedulerNoopMultiQueue::OperationCompleted(IOOperation *operation,
										 status_t status,
										 generic_size_t transferredBytes) {
	TRACE("%p->IOSchedulerNoopMultiQueue::OperationCompleted(%p, %d, %ld)\n", this, operation, status, transferredBytes);

	// finish operation only once
	if (operation->Status() <= 0) {
		TRACE("%p->IOSchedulerNoopMultiQueue::OperationCompleted(%p, %d, %ld): Dropping operation because status is %d\n", this, operation, status, transferredBytes, operation->Status());
		return;
	}

	operation->SetStatus(status);

	// set the bytes transferred (of the net data)
	generic_size_t partialBegin =
			operation->OriginalOffset() - operation->Offset();
	operation->SetTransferredBytes(
			transferredBytes > partialBegin ? transferredBytes - partialBegin
											: 0);

	TRACE("%p->IOSchedulerNoopMultiQueue::OperationCompleted(%p, %d, %ld): Operation enqueued for finishing.\n", this, operation, status, transferredBytes);

	bool operationFinished = operation->Finish();

	TRACE("%p->IOSchedulerNoopMultiQueue::_Finisher(): Operation %p finished? %d\n",
		  this, operation, operationFinished);

	IOSchedulerRoster::Default()->Notify(IO_SCHEDULER_OPERATION_FINISHED,
										 this,
										 operation->Parent(), operation);

	TRACE("%p->IOSchedulerNoopMultiQueue::_Finisher(): Operation %p notified to roster\n",
		  this, operation);

	// Notify for every time the operation is passed to the I/O hook,
	// not only when it is fully finished.

	if (!operationFinished) {
		TRACE("%p->IOSchedulerNoopMultiQueue::_Finisher(): Operation: %p not finished yet\n",
			  this, operation);
		MutexLocker _(fLock);
		operation->SetTransferredBytes(0);

		IORequestQueue& queue_info = fIORequestQueues[smp_get_current_cpu()];
		queue_info.fRescheduledOperations.Add(operation);
		queue_info.fNewRequestCondition.NotifyAll();
		return;
	}

	// notify request and remove operation
	IORequest *request = operation->Parent();
	TRACE("%p->IOSchedulerNoopMultiQueue::_Finisher(): Request %p from operation %p\n",
		  this, request, operation);

	generic_size_t operationOffset =
		operation->OriginalOffset() - request->Offset();
	request->OperationFinished(
		operation,
		operation->Status(),
		operation->TransferredBytes() < operation->OriginalLength(),
		operation->Status() == B_OK
		? operationOffset + operation->OriginalLength()
		: operationOffset);

	MutexLocker _(fLock);
	TRACE("%p->IOSchedulerNoopMultiQueue::_Finisher(): operation %p finished, recycling buffer\n",
		  this, operation);
	if (fDMAResource != NULL) {
		fDMAResource->RecycleBuffer(operation->Buffer());
	}

	fUnusedOperations.Add(operation);

	// FIXME: We should probably not have a single shared pool.
	for (generic_size_t i = 0; i < fCPUCount; ++i) {
		fIORequestQueues[i].fNewRequestCondition.NotifyAll();
	}

	// If the request is done, we need to perform its notifications.
	if (request->IsFinished()) {
		TRACE("%p->IOSchedulerNoopMultiQueue::_Finisher(): request %p is finished\n",
			  this, request);
		if (request->Status() == B_OK && request->RemainingBytes() > 0) {
			// The request has been processed OK so far, but it isn't really
			// finished yet.
			TRACE("%p->IOSchedulerNoopMultiQueue::_Finisher(): Setting request %p as unfinished cause remaining bytes is %ld\n",
				  this, request, request->RemainingBytes());
			request->SetUnfinished();

			IORequestQueue& queue_info = fIORequestQueues[smp_get_current_cpu()];
			queue_info.fScheduledRequests.Add(request);
			queue_info.fNewRequestCondition.NotifyAll();
		} else {
			if (request->HasCallbacks()) {
				TRACE("%p->IOSchedulerNoopMultiQueue::_Finisher(): request %p has callbacks, enqueuing for notifier thread\n",
					  this, request);
				// The request has callbacks that may take some time to
				// perform, so we hand it over to the request notifier.
				fFinishedRequests.Add(request);
				fFinishedRequestCondition.NotifyAll();
			} else {
				TRACE("%p->IOSchedulerNoopMultiQueue::_Finisher(): request %p has no callbacks. Notifying now.\n",
					  this, request);
				// No callbacks -- finish the request right now.
				IOSchedulerRoster::Default()->Notify(
					IO_SCHEDULER_REQUEST_FINISHED,
					this,
					request);
				request->NotifyFinished();

				TRACE("%p->IOSchedulerNoopMultiQueue::_Finisher(): request %p notified\n",
					  this, request);
			}
		}
	}
}

void IOSchedulerNoopMultiQueue::Dump() const {
	kprintf("IOSchedulerNoopMultiQueue at %p\n", this);
	kprintf("  DMA resource:   %p\n", fDMAResource);
	kprintf("  fBlockSize: %ld\n", fBlockSize);
	kprintf("  Number of scheduler queues: %ld\n", fCPUCount);
	for (generic_size_t i = 0; i < fCPUCount; ++i) {
		kprintf("  Scheduled requests (queue %ld): %d\n", i, fIORequestQueues[i].fScheduledRequests.Count());
		kprintf("  Rescheduled operations (queue %ld): %d\n", i, fIORequestQueues[i].fRescheduledOperations.Count());
	}
	kprintf("  Finished requests: %d\n", fFinishedRequests.Count());
	kprintf("  Free operations in pool: %d\n", fUnusedOperations.Count());
}

bool IOSchedulerNoopMultiQueue::_TrySubmittingRequest(IORequest *request) {
	TRACE("%p->IOSchedulerNoopMultiQueue::_TrySubmittingRequest(%p)\n", this, request);

	if (fDMAResource != NULL) {
		// FIXME: The original code did this in a loop until request->RemainingBytes() <= 0
		IOOperation *operation = fUnusedOperations.RemoveHead();
		if (operation == NULL) {
			return false;
		}

		generic_size_t max_operation_length = fBlockSize * 1024;
		TRACE("%p->IOSchedulerNoopMultiQueue::_TrySubmittingRequest(%p): Translating next batch with %ld remaining bytes, limiting operation length to %ld\n",
				this,
				request,
				request->RemainingBytes(),
				max_operation_length);
		status_t status =
				fDMAResource->TranslateNext(request, operation,
											max_operation_length);
		if (status != B_OK) {
			operation->SetParent(NULL);

			{
				MutexLocker _(fLock);
				fUnusedOperations.Add(operation);
			}

			// FIXME: We shouldn't have a single shared operation pool
			for (generic_size_t i = 0; i < fCPUCount; ++i) {
				fIORequestQueues[i].fNewRequestCondition.NotifyAll();
			}

			// B_BUSY means some resource (DMABuffers or
			// DMABounceBuffers) was temporarily unavailable. That's OK,
			// we'll retry later.
			if (status == B_BUSY) {
				return false;
			}

			AbortRequest(request, status);
			return true;
		}

		IOSchedulerRoster::Default()->Notify(IO_SCHEDULER_OPERATION_STARTED,
											 this,
											 operation->Parent(),
											 operation);

		TRACE("%p->IOSchedulerNoopMultiQueue::_TrySubmittingRequest(%p): Invoking fIOCallback for operation %p.\n", this, request, operation);
		fIOCallback(fIOCallbackData, operation);
	} else {
		IOOperation *operation = fUnusedOperations.RemoveHead();
		if (operation == NULL) {
			return false;
		}

		// TODO: If the device has block size restrictions, we might need to use
		// a bounce buffer.
		status_t status = operation->Prepare(request);
		if (status != B_OK) {
			operation->SetParent(NULL);

			{
				MutexLocker _(fLock);
				fUnusedOperations.Add(operation);
			}

			// FIXME: We shouldn't have a shared pool of operations.
			for (generic_size_t i = 0; i < fCPUCount; ++i) {
				fIORequestQueues[i].fNewRequestCondition.NotifyAll();
			}

			AbortRequest(request, status);
			return true;
		}

		operation->SetOriginalRange(request->Offset(), request->Length());
		request->Advance(request->Length());

		IOSchedulerRoster::Default()->Notify(IO_SCHEDULER_OPERATION_STARTED, this,
											 operation->Parent(), operation);

		fIOCallback(fIOCallbackData, operation);
	}

	return true;
}

status_t IOSchedulerNoopMultiQueue::_Scheduler(IORequestQueue* request_queue) {
	while (!fTerminating) {
		// First thing's first, try to re-submit some unfinished
		while (true) {
			MutexLocker locker(request_queue->fLock);
			IOOperation *operation = request_queue->fRescheduledOperations.RemoveHead();
			if (operation == NULL) {
				break;
			}

			if (operation->Parent() == NULL) {
				TRACE("%p->IOSchedulerNoopMultiQueue::_Scheduler(): Something is wrong. Operation %p was re-enqueued but has no parent request.\n", this, operation);
				continue;
			}

			locker.Unlock();

			TRACE("%p->IOSchedulerNoopMultiQueue::_Scheduler(): Re-submitting re-scheduled operation %p to device\n",
				  this, operation);
			IOSchedulerRoster::Default()->Notify(IO_SCHEDULER_OPERATION_STARTED,
												 this, operation->Parent(),
												 operation);
			fIOCallback(fIOCallbackData, operation);
		}

		TRACE("%p->IOSchedulerNoopMultiQueue::_Scheduler(): Finished with resubmitted operations, acquiring lock\n",
			  this);
		MutexLocker locker(request_queue->fLock);

		bool resourcesAvailable = true;
		while (resourcesAvailable) {
			IORequest *request = request_queue->fScheduledRequests.RemoveHead();
			if (request == NULL) {
				// No requests pending.
				TRACE("%p->IOSchedulerNoopMultiQueue::_Scheduler(): No pending requests to schedule\n",
					  this);

				locker.Unlock();

				ConditionVariableEntry entry;
				request_queue->fNewRequestCondition.Add(&entry);
				entry.Wait(B_CAN_INTERRUPT);

				TRACE("%p->IOSchedulerNoopMultiQueue::_Scheduler(): Woken up, resubmitting pending operations\n",
					  this);

				break;
			}

			TRACE("%p->IOSchedulerNoopMultiQueue::_Scheduer(): Submitting request %p\n",
				  this, request);
			locker.Unlock();
			resourcesAvailable = _TrySubmittingRequest(request);
			if (resourcesAvailable) {
				// Successfully submitted request. Remove it from queue.
				TRACE("%p->IOSchedulerNoopMultiQueue::_Scheduer(): Request %p submitted\n",
					  this, request);
			} else {
				TRACE("%p->IOSchedulerNoopMultiQueue::_Scheduler(): Putting request %p back onto the queue because there are no more buffers available\n",
					  this, request);
				locker.Lock();
				request_queue->fScheduledRequests.Add(request);
				request_queue->fNewRequestCondition.NotifyAll();
			}
		}
	}

	return B_OK;
}

/*static*/ status_t IOSchedulerNoopMultiQueue::_SchedulerThread(void *_self) {
	IORequestQueue *request_queue = reinterpret_cast<IORequestQueue*>(_self);
	return request_queue->fSelf->_Scheduler(request_queue);
}

status_t IOSchedulerNoopMultiQueue::_RequestNotifier() {
	TRACE("%p->IOSchedulerLoop::_RequestNotifier(): starting request notifier thread\n",
		  this);

	while (true) {
		MutexLocker locker(fLock);

		// get a request
		IORequest *request = fFinishedRequests.RemoveHead();
		if (request == NULL) {
			if (fTerminating) {
				break;
			}

			TRACE("%p->IOSchedulerNoopMultiQueue::_RequestNotifier(): No finished requests. Waiting...\n",
				  this);

			ConditionVariableEntry entry;
			fFinishedRequestCondition.Add(&entry);

			locker.Unlock();

			entry.Wait();
			continue;
		}

		locker.Unlock();

		IOSchedulerRoster::Default()->Notify(IO_SCHEDULER_REQUEST_FINISHED,
											 this,
											 request);

		// notify the request
		TRACE("%p->IOSchedulerNoopMultiQueue::_RequestNotifier(): Calling NotifyFinish() for request %p\n",
			  this, request);
		request->NotifyFinished();
	}

	return B_OK;
}

/*static*/ status_t IOSchedulerNoopMultiQueue::_RequestNotifierThread(void *_self) {
	IOSchedulerNoopMultiQueue *self = reinterpret_cast<IOSchedulerNoopMultiQueue *>(_self);
	return self->_RequestNotifier();
}
