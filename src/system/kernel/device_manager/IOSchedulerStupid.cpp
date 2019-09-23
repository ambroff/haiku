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
class StupidIOOperation : public IOOperation {
public:
	StupidIOOperation(sem_id semaphore) : fSemaphore(semaphore)
	{
		acquire_sem_etc(fSemaphore, 1, B_RELATIVE_TIMEOUT, INT_MAX);
	}

	~StupidIOOperation()
	{
		release_sem_etc(fSemaphore, 1, B_DO_NOT_RESCHEDULE);
	}

private:
	sem_id fSemaphore;
};

// #pragma mark -

IOSchedulerStupid::IOSchedulerStupid(DMAResource *resource)
		: IOScheduler(resource),
		  fTerminating(false),
		  fBlockSize(512),
		  fNotifierThread(-1)
{
	mutex_init(&fNotifierLock, "I/O scheduler notifier queue lock");
	fNotifierCondition.Init(this, "I/O request notifier");
}

IOSchedulerStupid::~IOSchedulerStupid() {
	fTerminating = true;

	fNotifierCondition.NotifyAll();

	if (fNotifierThread >= 0) {
		wait_for_thread(fNotifierThread, NULL);
	}

	mutex_lock(&fNotifierLock);
	mutex_destroy(&fNotifierLock);
}

status_t IOSchedulerStupid::Init(const char *name) {
	status_t error = IOScheduler::Init(name);
	if (error != B_OK)
		return error;

	TRACE("%p->IOSchedulerStupid::Init(%s)\n", this, name);

	size_t concurrent_buffer_count = 16;

	if (fDMAResource != NULL) {
		concurrent_buffer_count = fDMAResource->BufferCount();
		fBlockSize = fDMAResource->BlockSize();
	}

	// FIXME: Concatenate name with this string.
	fConcurrentRequests = create_sem(concurrent_buffer_count, "IOScheduler concurrent requests");

	// FIXME: Should this be hard-coded to 512? It's set to 2KiB when formatting.
	// It should probably be probed. Linux system says 4096.
	if (fBlockSize == 0) {
		fBlockSize = 512;
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

	return B_OK;
}

status_t IOSchedulerStupid::ScheduleRequest(IORequest *request) {
	TRACE("%p->IOSchedulerStupid::ScheduleRequest(%p)\n", this, request);

	IOSchedulerRoster::Default()->Notify(IO_SCHEDULER_REQUEST_SCHEDULED, this,
										 request);
	TRACE("%p->IOSchedulerStupid::ScheduleRequest(%p) request scheduled\n", this,
		  request);

	IOOperation *operation = new(std::nothrow) StupidIOOperation(fConcurrentRequests);
	if (operation == NULL) {
		AbortRequest(request, B_NO_MEMORY);
		return B_NO_MEMORY;
	}
	
	// Code from _Scheduler thread.
	if (fDMAResource != NULL) {
		generic_size_t max_operation_length = fBlockSize * 1024;
		TRACE("%p->IOSchedulerStupid::_TrySubmittingRequest(%p): Translating next batch with %ld remaining bytes, limiting operation length to %ld\n",
				this,
				request,
				request->RemainingBytes(),
				max_operation_length);
		status_t status =
				fDMAResource->TranslateNext(request, operation,
											max_operation_length);
		if (status != B_OK) {
			delete operation;

			// B_BUSY means some resource (DMABuffers or
			// DMABounceBuffers) was temporarily unavailable. That's OK,
			// we'll retry later.
			if (status == B_BUSY) {
				AbortRequest(request, status);
			}

			AbortRequest(request, status);
			return status;
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
			delete operation;
			AbortRequest(request, status);
			return true;
		}

		operation->SetOriginalRange(request->Offset(), request->Length());
		request->Advance(request->Length());

		IOSchedulerRoster::Default()->Notify(IO_SCHEDULER_OPERATION_STARTED, this,
											 request, operation);

	}

	TRACE("%p->IOSchedulerStupid::ScheduleRequest(%p): Invoking fIOCallback for operation %p.\n", this, request, operation);
	{
		IOBuffer *buffer = request->Buffer();
		if (buffer->IsVirtual()) {
			status_t status = buffer->LockMemory(request->TeamID(),
												 request->IsWrite());
			if (status != B_OK) {
				TRACE("%p->IOSchedulerStupid::ScheduleRequest(%p) unable to lock memory: %d\n",
					  this, request, status);
				delete operation;
				request->SetStatusAndNotify(status);
				return status;
			}
		}

		fIOCallback(fIOCallbackData, operation);
	}
	
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

		TRACE("%p->IOSchedulerStupid::_Finisher(): Operation %p finished? %d\n",
			  this, operation, operationFinished);

		IOSchedulerRoster::Default()->Notify(IO_SCHEDULER_OPERATION_FINISHED,
											 this,
											 request, operation);

		TRACE("%p->IOSchedulerStupid::_Finisher(): Operation %p notified to roster\n",
			  this, operation);

		// Notify for every time the operation is passed to the I/O hook,
		// not only when it is fully finished.

		if (!operationFinished) {
			TRACE("%p->IOSchedulerStupid::_Finisher(): Operation: %p not finished yet\n",
				  this, operation);
			operation->SetTransferredBytes(0);
			// FIXME: Schedule for retry
			//fRescheduledOperations.Add(operation);
			//fNewRequestCondition.NotifyAll();
			panic("Not implemented to reschedule here");
			return;
		}
	
		// notify request and remove operation
		TRACE("%p->IOSchedulerStupid::_Finisher(): Request %p from operation %p\n",
			  this, request, operation);

		generic_size_t operationOffset =
				operation->OriginalOffset() - request->Offset();
		request->OperationFinished(
				operation, operation->Status(),
				operation->TransferredBytes() < operation->OriginalLength(),
				operation->Status() == B_OK
				? operationOffset + operation->OriginalLength()
				: operationOffset);

		TRACE("%p->IOSchedulerStupid::_Finisher(): operation %p finished, recycling buffer\n",
			  this, operation);
		if (fDMAResource != NULL) {
			fDMAResource->RecycleBuffer(operation->Buffer());
		}

		delete operation;

		// If the request is done, we need to perform its notifications.
		if (request->IsFinished()) {
			TRACE("%p->IOSchedulerStupid::_Finisher(): request %p is finished\n",
				  this, request);
			if (request->Status() == B_OK && request->RemainingBytes() > 0) {
				// The request has been processed OK so far, but it isn't really
				// finished yet.
				TRACE("%p->IOSchedulerStupid::_Finisher(): Setting request %p as unfinished cause remaining bytes is %ld\n",
					  this, request, request->RemainingBytes());
				request->SetUnfinished();
				// FIXME: Need to reschedule this
				//fScheduledRequests.Add(request);
				panic("Not implemented to reschedule unfinished");
			} else {
				if (request->HasCallbacks()) {
					TRACE("%p->IOSchedulerNoop::_Finisher(): request %p has callbacks, enqueuing for notifier thread\n",
						  this, request);
					// The request has callbacks that may take some time to
					// perform, so we hand it over to the request notifier.
					MutexLocker _(&fNotifierLock);
					fNotifierQueue.Add(request);
					fNotifierCondition.NotifyAll();
				} else {
					TRACE("%p->IOSchedulerStupid::_Finisher(): request %p has no callbacks. Notifying now.\n",
						  this, request);
					IOSchedulerRoster::Default()->Notify(IO_SCHEDULER_REQUEST_FINISHED,	this, request);
					request->NotifyFinished();

					TRACE("%p->IOSchedulerStupid::_Finisher(): request %p notified\n",
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
	kprintf("  fNotifierQueue size: %d\n", fNotifierQueue.Count());
}

/*static*/ status_t IOSchedulerStupid::_NotifierThread(void *self) {
	return reinterpret_cast<IOSchedulerStupid*>(self)->_Notifier();
}

status_t IOSchedulerStupid::_Notifier() {
	TRACE("%p->IOSchedulerStupid::_Notifier(): starting request notifier thread\n",
		  this);

	while (true) {
		MutexLocker locker(fNotifierLock);

		// get a request
		IORequest *request = fNotifierQueue.RemoveHead();
		if (request == NULL) {
			if (fTerminating) {
				break;
			}

			TRACE("%p->IOSchedulerStupid::_RequestNotifier(): No finished requests. Waiting...\n",
				  this);

			ConditionVariableEntry entry;
			fNotifierCondition.Add(&entry);

			locker.Unlock();

			entry.Wait();
			continue;
		}

		locker.Unlock();

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
