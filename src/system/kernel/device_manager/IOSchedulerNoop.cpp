#include "IOSchedulerNoop.h"

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

#include <lock.h>
#include <thread_types.h>
#include <thread.h>
#include <util/AutoLock.h>

#include "IOSchedulerRoster.h"


//#define TRACE_IO_SCHEDULER
#ifdef TRACE_IO_SCHEDULER
#	define TRACE(x...) dprintf(x)
#else
#	define TRACE(x...) ;
#endif


// #pragma mark -

IOSchedulerNoop::IOSchedulerNoop(DMAResource* resource)
	:
	IOScheduler(resource),
	fSchedulerThread(-1),
	fRequestNotifierThread(-1),
	fAllocatedRequestOwners(NULL),
	fRequestOwners(NULL),
	fBlockSize(0),
	fPendingOperations(0),
	fTerminating(false)
{
	mutex_init(&fLock, "I/O scheduler");
	B_INITIALIZE_SPINLOCK(&fFinisherLock);

	fNewRequestCondition.Init(this, "I/O new request");
	fFinishedOperationCondition.Init(this, "I/O finished operation");
	fFinishedRequestCondition.Init(this, "I/O finished request");

}


IOSchedulerNoop::~IOSchedulerNoop()
{
	// shutdown threads
	MutexLocker locker(fLock);
	InterruptsSpinLocker finisherLocker(fFinisherLock);
	fTerminating = true;

	fNewRequestCondition.NotifyAll();
	fFinishedOperationCondition.NotifyAll();
	fFinishedRequestCondition.NotifyAll();

	finisherLocker.Unlock();
	locker.Unlock();

	if (fSchedulerThread >= 0)
		wait_for_thread(fSchedulerThread, NULL);

	if (fRequestNotifierThread >= 0)
		wait_for_thread(fRequestNotifierThread, NULL);

	// destroy our belongings
	mutex_lock(&fLock);
	mutex_destroy(&fLock);

	while (IOOperation* operation = fUnusedOperations.RemoveHead())
		delete operation;

	delete fRequestOwners;
	delete[] fAllocatedRequestOwners;
}


status_t
IOSchedulerNoop::Init(const char* name)
{
	status_t error = IOScheduler::Init(name);
	if (error != B_OK)
		return error;

	size_t count = fDMAResource != NULL ? fDMAResource->BufferCount() : 16;
	for (size_t i = 0; i < count; i++) {
		IOOperation* operation = new(std::nothrow) IOOperation;
		if (operation == NULL)
			return B_NO_MEMORY;

		fUnusedOperations.Add(operation);
	}

	if (fDMAResource != NULL)
		fBlockSize = fDMAResource->BlockSize();
	if (fBlockSize == 0)
		fBlockSize = 512;

	fAllocatedRequestOwnerCount = thread_max_threads();
	fAllocatedRequestOwners
		= new(std::nothrow) IORequestOwner[fAllocatedRequestOwnerCount];
	if (fAllocatedRequestOwners == NULL)
		return B_NO_MEMORY;

	for (int32 i = 0; i < fAllocatedRequestOwnerCount; i++) {
		IORequestOwner& owner = fAllocatedRequestOwners[i];
		owner.team = -1;
		owner.thread = -1;
		owner.priority = B_IDLE_PRIORITY;
		fUnusedRequestOwners.Add(&owner);
	}

	fRequestOwners = new(std::nothrow) IORequestOwnerHashTable;
	if (fRequestOwners == NULL)
		return B_NO_MEMORY;

	error = fRequestOwners->Init(fAllocatedRequestOwnerCount);
	if (error != B_OK)
		return error;

	// start threads
	char buffer[B_OS_NAME_LENGTH];
	strlcpy(buffer, name, sizeof(buffer));
	strlcat(buffer, " scheduler ", sizeof(buffer));
	size_t nameLength = strlen(buffer);
	snprintf(buffer + nameLength, sizeof(buffer) - nameLength, "%" B_PRId32,
		fID);
	fSchedulerThread = spawn_kernel_thread(&_SchedulerThread, buffer,
		B_NORMAL_PRIORITY + 2, (void *)this);
	if (fSchedulerThread < B_OK)
		return fSchedulerThread;

	strlcpy(buffer, name, sizeof(buffer));
	strlcat(buffer, " notifier ", sizeof(buffer));
	nameLength = strlen(buffer);
	snprintf(buffer + nameLength, sizeof(buffer) - nameLength, "%" B_PRId32,
		fID);
	fRequestNotifierThread = spawn_kernel_thread(&_RequestNotifierThread,
		buffer, B_NORMAL_PRIORITY + 2, (void *)this);
	if (fRequestNotifierThread < B_OK)
		return fRequestNotifierThread;

	resume_thread(fSchedulerThread);
	resume_thread(fRequestNotifierThread);

	return B_OK;
}


status_t
IOSchedulerNoop::ScheduleRequest(IORequest* request)
{
	TRACE("%p->IOSchedulerNoop::ScheduleRequest(%p)\n", this, request);

	IOBuffer* buffer = request->Buffer();

	// TODO: it would be nice to be able to lock the memory later, but we can't
	// easily do it in the I/O scheduler without being able to asynchronously
	// lock memory (via another thread or a dedicated call).

	if (buffer->IsVirtual()) {
		status_t status = buffer->LockMemory(request->TeamID(),
			request->IsWrite());
		if (status != B_OK) {
			request->SetStatusAndNotify(status);
			return status;
		}
	}

	MutexLocker locker(fLock);

	IORequestOwner* owner = _GetRequestOwner(request->TeamID(),
		request->ThreadID(), true);
	if (owner == NULL) {
		panic("IOSchedulerNoop: Out of request owners!\n");
		locker.Unlock();
		if (buffer->IsVirtual())
			buffer->UnlockMemory(request->TeamID(), request->IsWrite());
		request->SetStatusAndNotify(B_NO_MEMORY);
		return B_NO_MEMORY;
	}

	bool wasActive = owner->IsActive();
	request->SetOwner(owner);
	owner->requests.Add(request);

	int32 priority = thread_get_io_priority(request->ThreadID());
	if (priority >= 0)
		owner->priority = priority;
//dprintf("  request %p -> owner %p (thread %ld, active %d)\n", request, owner, owner->thread, wasActive);

	if (!wasActive)
		fActiveRequestOwners.Add(owner);

	IOSchedulerRoster::Default()->Notify(IO_SCHEDULER_REQUEST_SCHEDULED, this,
		request);

	fNewRequestCondition.NotifyAll();

	return B_OK;
}


void
IOSchedulerNoop::AbortRequest(IORequest* request, status_t status)
{
	// TODO:...
//B_CANCELED
}


void
IOSchedulerNoop::OperationCompleted(IOOperation* operation, status_t status,
	generic_size_t transferredBytes)
{
	InterruptsSpinLocker _(fFinisherLock);

	// finish operation only once
	if (operation->Status() <= 0)
		return;

	operation->SetStatus(status);

	// set the bytes transferred (of the net data)
	generic_size_t partialBegin
		= operation->OriginalOffset() - operation->Offset();
	operation->SetTransferredBytes(
		transferredBytes > partialBegin ? transferredBytes - partialBegin : 0);

	fCompletedOperations.Add(operation);
	fFinishedOperationCondition.NotifyAll();
}


void
IOSchedulerNoop::Dump() const
{
	kprintf("IOSchedulerNoop at %p\n", this);
	kprintf("  DMA resource:   %p\n", fDMAResource);

	kprintf("  active request owners:");
	for (RequestOwnerList::ConstIterator it
				= fActiveRequestOwners.GetIterator();
			IORequestOwner* owner = it.Next();) {
		kprintf(" %p", owner);
	}
	kprintf("\n");

	kprintf("  fBlockSize: %ld\n", fBlockSize);
}


/*!	Must not be called with the fLock held. */
void
IOSchedulerNoop::_Finisher()
{
	while (true) {
		InterruptsSpinLocker locker(fFinisherLock);
		IOOperation* operation = fCompletedOperations.RemoveHead();
		if (operation == NULL)
			return;

		locker.Unlock();

		TRACE("IOSchedulerNoop::_Finisher(): operation: %p\n", operation);

		bool operationFinished = operation->Finish();

		IOSchedulerRoster::Default()->Notify(IO_SCHEDULER_OPERATION_FINISHED,
			this, operation->Parent(), operation);
			// Notify for every time the operation is passed to the I/O hook,
			// not only when it is fully finished.

		if (!operationFinished) {
			TRACE("  operation: %p not finished yet\n", operation);
			MutexLocker _(fLock);
			operation->SetTransferredBytes(0);
			operation->Parent()->Owner()->operations.Add(operation);
			fPendingOperations--;
			continue;
		}

		// notify request and remove operation
		IORequest* request = operation->Parent();

		generic_size_t operationOffset
			= operation->OriginalOffset() - request->Offset();
		request->OperationFinished(operation, operation->Status(),
			operation->TransferredBytes() < operation->OriginalLength(),
			operation->Status() == B_OK
				? operationOffset + operation->OriginalLength()
				: operationOffset);

		// recycle the operation
		MutexLocker _(fLock);
		if (fDMAResource != NULL)
			fDMAResource->RecycleBuffer(operation->Buffer());

		fPendingOperations--;
		fUnusedOperations.Add(operation);

		// If the request is done, we need to perform its notifications.
		if (request->IsFinished()) {
			if (request->Status() == B_OK && request->RemainingBytes() > 0) {
				// The request has been processed OK so far, but it isn't really
				// finished yet.
				request->SetUnfinished();
			} else {
				// Remove the request from the request owner.
				IORequestOwner* owner = request->Owner();
				owner->requests.MoveFrom(&owner->completed_requests);
				owner->requests.Remove(request);
				request->SetOwner(NULL);

				if (!owner->IsActive()) {
					fActiveRequestOwners.Remove(owner);
					fUnusedRequestOwners.Add(owner);
				}

				if (request->HasCallbacks()) {
					// The request has callbacks that may take some time to
					// perform, so we hand it over to the request notifier.
					fFinishedRequests.Add(request);
					fFinishedRequestCondition.NotifyAll();
				} else {
					// No callbacks -- finish the request right now.
					IOSchedulerRoster::Default()->Notify(
						IO_SCHEDULER_REQUEST_FINISHED, this, request);
					request->NotifyFinished();
				}
			}
		}
	}
}


/*!	Called with \c fFinisherLock held.
*/
bool
IOSchedulerNoop::_FinisherWorkPending()
{
	return !fCompletedOperations.IsEmpty();
}


bool
IOSchedulerNoop::_PrepareRequestOperations(IORequest* request,
										   IOOperationList& operations,
										   int32& operationsPrepared)
{
	//dprintf("IOSchedulerNoop::_PrepareRequestOperations(%p)\n", request);
	if (fDMAResource != NULL) {
		while (request->RemainingBytes() > 0) {
			IOOperation* operation = fUnusedOperations.RemoveHead();
			if (operation == NULL)
				return false;

			status_t status = fDMAResource->TranslateNext(request, operation,
														  request->Length());
			if (status != B_OK) {
				operation->SetParent(NULL);
				fUnusedOperations.Add(operation);

				// B_BUSY means some resource (DMABuffers or
				// DMABounceBuffers) was temporarily unavailable. That's OK,
				// we'll retry later.
				if (status == B_BUSY)
					return false;

				AbortRequest(request, status);
				return true;
			}
			//dprintf("  prepared operation %p\n", operation);

			operations.Add(operation);
			operationsPrepared++;
		}
	} else {
		// TODO: If the device has block size restrictions, we might need to use
		// a bounce buffer.
		IOOperation* operation = fUnusedOperations.RemoveHead();
		if (operation == NULL)
			return false;

		status_t status = operation->Prepare(request);
		if (status != B_OK) {
			operation->SetParent(NULL);
			fUnusedOperations.Add(operation);
			AbortRequest(request, status);
			return true;
		}

		operation->SetOriginalRange(request->Offset(), request->Length());
		request->Advance(request->Length());

		operations.Add(operation);
		operationsPrepared++;
	}

	return true;
}


bool
IOSchedulerNoop::_NextActiveRequestOwner(IORequestOwner*& owner)
{
	while (true) {
		if (fTerminating)
			return false;

		if (owner != NULL)
			owner = fActiveRequestOwners.GetNext(owner);
		if (owner == NULL)
			owner = fActiveRequestOwners.Head();

		if (owner != NULL) {
			return true;
		}

		// Wait for new requests owners. First check whether any finisher work
		// has to be done.
		InterruptsSpinLocker finisherLocker(fFinisherLock);
		if (_FinisherWorkPending()) {
			finisherLocker.Unlock();
			mutex_unlock(&fLock);
			_Finisher();
			mutex_lock(&fLock);
			continue;
		}

		// Wait for new requests.
		ConditionVariableEntry entry;
		fNewRequestCondition.Add(&entry);

		finisherLocker.Unlock();
		mutex_unlock(&fLock);

		entry.Wait(B_CAN_INTERRUPT);
		_Finisher();
		mutex_lock(&fLock);
	}
}


struct OperationComparator {
	inline bool operator()(const IOOperation* a, const IOOperation* b)
	{
		off_t offsetA = a->Offset();
		off_t offsetB = b->Offset();
		return offsetA < offsetB
			|| (offsetA == offsetB && a->Length() > b->Length());
	}
};


status_t
IOSchedulerNoop::_Scheduler()
{
	IORequestOwner marker;
	marker.thread = -1;
	{
		MutexLocker locker(fLock);
		fActiveRequestOwners.Add(&marker, false);
	}

	IORequestOwner* owner = NULL;

	while (!fTerminating) {
		MutexLocker locker(fLock);

		IOOperationList operations;
		int32 operationCount = 0;
		bool resourcesAvailable = true;

		if (owner == NULL) {
			owner = fActiveRequestOwners.GetPrevious(&marker);
			fActiveRequestOwners.Remove(&marker);
		}

		if (owner == NULL) {
			if (!_NextActiveRequestOwner(owner)) {
				// we've been asked to terminate
				return B_OK;
			}
		}

		while (resourcesAvailable) {
//dprintf("IOSchedulerNoop::_Scheduler(): request owner: %p (thread %ld)\n",
//owner, owner->thread);
			// Prepare operations for the owner.

			// There might still be unfinished ones.
			while (IOOperation* operation = owner->operations.RemoveHead()) {
				// TODO: We might actually grant the owner more bandwidth than
				// it deserves.
				// TODO: We should make sure that after the first read operation
				// of a partial write, no other write operation to the same
				// location is scheduled!
				operations.Add(operation);
				operationCount++;
			}

			while (resourcesAvailable) {
				IORequest* request = owner->requests.Head();
				if (request == NULL) {
					resourcesAvailable = false;
					if (operationCount == 0) {
						panic("no more requests for owner %p (thread %" B_PRId32 ")", owner, owner->thread);
					}

					break;
				}

				resourcesAvailable = _PrepareRequestOperations(request,	operations, operationCount);
				if (request->RemainingBytes() == 0 || request->Status() <= 0) {
					// If the request has been completed, move it to the
					// completed list, so we don't pick it up again.
					owner->requests.Remove(request);
					owner->completed_requests.Add(request);
				}
			}

			// Get the next owner.
			if (resourcesAvailable)
				_NextActiveRequestOwner(owner);
		}

		// If the current owner doesn't have anymore requests, we have to
		// insert our marker, since the owner will be gone in the next
		// iteration.
		if (owner->requests.IsEmpty()) {
			fActiveRequestOwners.Insert(owner, &marker);
			owner = NULL;
		}

		if (operations.IsEmpty())
			continue;

		fPendingOperations = operationCount;

		locker.Unlock();

		// execute the operations
#ifdef TRACE_IO_SCHEDULER
		int32 i = 0;
#endif
		while (IOOperation* operation = operations.RemoveHead()) {
			TRACE("IOSchedulerNoop::_Scheduler(): calling callback for "
				"operation %d: %p\n", i++, operation);

			IOSchedulerRoster::Default()->Notify(IO_SCHEDULER_OPERATION_STARTED,
				this, operation->Parent(), operation);

			fIOCallback(fIOCallbackData, operation);

			_Finisher();
		}

		// wait for all operations to finish
		while (!fTerminating) {
			locker.Lock();

			if (fPendingOperations == 0)
				break;

			// Before waiting first check whether any finisher work has to be
			// done.
			InterruptsSpinLocker finisherLocker(fFinisherLock);
			if (_FinisherWorkPending()) {
				finisherLocker.Unlock();
				locker.Unlock();
				_Finisher();
				continue;
			}

			// wait for finished operations
			ConditionVariableEntry entry;
			fFinishedOperationCondition.Add(&entry);

			finisherLocker.Unlock();
			locker.Unlock();

			entry.Wait(B_CAN_INTERRUPT);
			_Finisher();
		}
	}

	return B_OK;
}


/*static*/ status_t
IOSchedulerNoop::_SchedulerThread(void *_self)
{
	IOSchedulerNoop *self = (IOSchedulerNoop *)_self;
	return self->_Scheduler();
}


status_t
IOSchedulerNoop::_RequestNotifier()
{
	while (true) {
		MutexLocker locker(fLock);

		// get a request
		IORequest* request = fFinishedRequests.RemoveHead();

		if (request == NULL) {
			if (fTerminating)
				return B_OK;

			ConditionVariableEntry entry;
			fFinishedRequestCondition.Add(&entry);

			locker.Unlock();

			entry.Wait();
			continue;
		}

		locker.Unlock();

		IOSchedulerRoster::Default()->Notify(IO_SCHEDULER_REQUEST_FINISHED,
			this, request);

		// notify the request
		request->NotifyFinished();
	}

	// never can get here
	return B_OK;
}


/*static*/ status_t
IOSchedulerNoop::_RequestNotifierThread(void *_self)
{
	IOSchedulerNoop *self = (IOSchedulerNoop*)_self;
	return self->_RequestNotifier();
}


IORequestOwner*
IOSchedulerNoop::_GetRequestOwner(team_id team, thread_id thread,
	bool allocate)
{
	// lookup in table
	IORequestOwner* owner = fRequestOwners->Lookup(thread);
	if (owner != NULL && !owner->IsActive())
		fUnusedRequestOwners.Remove(owner);
	if (owner != NULL || !allocate)
		return owner;

	// not in table -- allocate an unused one
	RequestOwnerList existingOwners;

	while ((owner = fUnusedRequestOwners.RemoveHead()) != NULL) {
		if (owner->thread < 0 || !Thread::IsAlive(owner->thread)) {
			if (owner->thread >= 0)
				fRequestOwners->RemoveUnchecked(owner);
			owner->team = team;
			owner->thread = thread;
			owner->priority = B_IDLE_PRIORITY;
			fRequestOwners->InsertUnchecked(owner);
			break;
		}

		existingOwners.Add(owner);
	}

	fUnusedRequestOwners.MoveFrom(&existingOwners);
	return owner;
}
