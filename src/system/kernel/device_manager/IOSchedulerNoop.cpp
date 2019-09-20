#include "IOSchedulerNoop.h"

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

#define TRACE_IO_SCHEDULER
#ifdef TRACE_IO_SCHEDULER
#define TRACE(x...) dprintf(x)
#else
#define TRACE(x...) ;
#endif

// #pragma mark -

IOSchedulerNoop::IOSchedulerNoop(DMAResource *resource)
    : IOScheduler(resource), fSchedulerThread(-1), fFinisherThread(-1),
      fRequestNotifierThread(-1),
      fBlockSize(0), fTerminating(false) {
  mutex_init(&fLock, "I/O scheduler");
  B_INITIALIZE_SPINLOCK(&fFinisherLock);

  fNewRequestCondition.Init(this, "I/O new request");
  fFinishedOperationCondition.Init(this, "I/O finished operation");
  fFinishedRequestCondition.Init(this, "I/O finished request");
}

IOSchedulerNoop::~IOSchedulerNoop() {
  // shutdown threads
  MutexLocker locker(fLock);
  InterruptsSpinLocker finisherLocker(fFinisherLock);
  fTerminating = true;

  fNewRequestCondition.NotifyAll();
  fFinishedOperationCondition.NotifyAll();
  fFinishedRequestCondition.NotifyAll();

  finisherLocker.Unlock();
  locker.Unlock();

  if (fSchedulerThread >= 0) {
    wait_for_thread(fSchedulerThread, NULL);
  }

  if (fRequestNotifierThread >= 0) {
    wait_for_thread(fRequestNotifierThread, NULL);
  }

  if (fFinisherThread >= 0) {
    wait_for_thread(fFinisherThread, NULL);
  }

  // destroy our belongings
  mutex_lock(&fLock);
  mutex_destroy(&fLock);

  while (IOOperation *operation = fUnusedOperations.RemoveHead()) {
    delete operation;
  }
}

status_t IOSchedulerNoop::Init(const char *name) {
  status_t error = IOScheduler::Init(name);
  if (error != B_OK)
    return error;

  size_t count = fDMAResource != NULL ? fDMAResource->BufferCount() : 16;
  for (size_t i = 0; i < count; i++) {
    IOOperation *operation = new (std::nothrow) IOOperation;
    if (operation == NULL)
      return B_NO_MEMORY;

    fUnusedOperations.Add(operation);
  }

  if (fDMAResource != NULL)
    fBlockSize = fDMAResource->BlockSize();
  if (fBlockSize == 0)
    fBlockSize = 512;

  // start threads
  char buffer[B_OS_NAME_LENGTH];
  strlcpy(buffer, name, sizeof(buffer));
  strlcat(buffer, " scheduler ", sizeof(buffer));
  size_t nameLength = strlen(buffer);
  snprintf(buffer + nameLength, sizeof(buffer) - nameLength, "%" B_PRId32, fID);
  fSchedulerThread = spawn_kernel_thread(&_SchedulerThread, buffer,
                                         B_NORMAL_PRIORITY + 2, reinterpret_cast<void*>(this));
  if (fSchedulerThread < B_OK) {
    return fSchedulerThread;
  }

  strlcpy(buffer, name, sizeof(buffer));
  strlcat(buffer, " notifier ", sizeof(buffer));
  nameLength = strlen(buffer);
  snprintf(buffer + nameLength, sizeof(buffer) - nameLength, "%" B_PRId32, fID);
  fRequestNotifierThread = spawn_kernel_thread(
      &_RequestNotifierThread, buffer, B_NORMAL_PRIORITY + 2, reinterpret_cast<void*>(this));
  if (fRequestNotifierThread < B_OK) {
    return fRequestNotifierThread;
  }

  strlcpy(buffer, name, sizeof(buffer));
  strlcat(buffer, " finisher ", sizeof(buffer));
  nameLength = strlen(buffer);
  snprintf(buffer + nameLength, sizeof(buffer) - nameLength, "%" B_PRId32, fID);
  fFinisherThread = spawn_kernel_thread(&_FinisherThread, buffer, B_NORMAL_PRIORITY + 2, reinterpret_cast<void*>(this));

  resume_thread(fSchedulerThread);
  resume_thread(fRequestNotifierThread);
  resume_thread(fFinisherThread);

  return B_OK;
}

status_t IOSchedulerNoop::ScheduleRequest(IORequest *request) {
  TRACE("%p->IOSchedulerNoop::ScheduleRequest(%p)\n", this, request);

  IOBuffer *buffer = request->Buffer();

  // TODO: it would be nice to be able to lock the memory later, but we can't
  // easily do it in the I/O scheduler without being able to asynchronously
  // lock memory (via another thread or a dedicated call).

  if (buffer->IsVirtual()) {
    status_t status = buffer->LockMemory(request->TeamID(), request->IsWrite());
    if (status != B_OK) {
      TRACE("%p->IOSchedulerNoop::ScheduleRequest(%p) unable to lock memory: %d\n", this, request, status);
      request->SetStatusAndNotify(status);
      return status;
    }
  }

  MutexLocker locker(fLock);

  fScheduledRequests.Add(request);

  IOSchedulerRoster::Default()->Notify(IO_SCHEDULER_REQUEST_SCHEDULED, this,
                                       request);
  TRACE("%p->IOSchedulerNoop::ScheduleRequest(%p) request scheduled\n", this, request);

  fNewRequestCondition.NotifyAll();

  return B_OK;
}

void IOSchedulerNoop::AbortRequest(IORequest *request, status_t status) {
  // TODO:...
  // B_CANCELED
  TRACE("IOSchedulerNoop::AbortRequest(%p, %d)\n", request, status);
  request->SetStatusAndNotify(status);
}

void IOSchedulerNoop::OperationCompleted(IOOperation *operation,
                                         status_t status,
                                         generic_size_t transferredBytes) {
  InterruptsSpinLocker _(fFinisherLock);

  // finish operation only once
  if (operation->Status() <= 0)
    return;

  operation->SetStatus(status);

  // set the bytes transferred (of the net data)
  generic_size_t partialBegin =
      operation->OriginalOffset() - operation->Offset();
  operation->SetTransferredBytes(
      transferredBytes > partialBegin ? transferredBytes - partialBegin : 0);

  fCompletedOperations.Add(operation);
  fFinishedOperationCondition.NotifyAll();
}

void IOSchedulerNoop::Dump() const {
  kprintf("IOSchedulerNoop at %p\n", this);
  kprintf("  DMA resource:   %p\n", fDMAResource);
  kprintf("  fBlockSize: %ld\n", fBlockSize);
  kprintf("  Scheduled requests: %d\n", fScheduledRequests.Count());
  kprintf("  Finished requests: %d\n", fFinishedRequests.Count());
  kprintf("  Rescheduled operations: %d\n", fRescheduledOperations.Count());
}

/*!	Must not be called with the fLock held. */
status_t IOSchedulerNoop::_Finisher() {
  while (!fTerminating) {
    TRACE("IOSchedulerNoop::_Finisher(): Waiting for operations to finish\n");
    ConditionVariableEntry entry;
    fFinishedOperationCondition.Add(&entry);
    entry.Wait(B_CAN_INTERRUPT);

    TRACE("IOSchedulerNoop::_Finisher(): Woken up, acquiring spinlock\n");
    InterruptsSpinLocker locker(fFinisherLock);
    IOOperation *operation = fCompletedOperations.RemoveHead();
    if (operation == NULL) {
      TRACE("IOSchedulerNoop::_Finisher(): Nothing to do\n");
      continue;
    }

    locker.Unlock();

    TRACE("IOSchedulerNoop::_Finisher(): operation: %p\n", operation);

    bool operationFinished = operation->Finish();

    TRACE("IOSchedulerNoop::_Finisher(): Operation %p finished? %d\n", operation, operationFinished);

    IOSchedulerRoster::Default()->Notify(IO_SCHEDULER_OPERATION_FINISHED, this,
                                         operation->Parent(), operation);

    TRACE("IOSchedulerNoop::_Finisher(): Operation %p notified\n", operation);

    // Notify for every time the operation is passed to the I/O hook,
    // not only when it is fully finished.

    if (!operationFinished) {
      TRACE("  operation: %p not finished yet\n", operation);
      MutexLocker _(fLock);
      operation->SetTransferredBytes(0);
      fRescheduledOperations.Add(operation);
      fNewRequestCondition.NotifyAll();
      continue;
    }

    // notify request and remove operation
    IORequest *request = operation->Parent();
    TRACE("IOSchedulerNoop::_Finisher(): Request %p from operation %p\n", request, operation);

    generic_size_t operationOffset =
        operation->OriginalOffset() - request->Offset();
    request->OperationFinished(
        operation, operation->Status(),
        operation->TransferredBytes() < operation->OriginalLength(),
        operation->Status() == B_OK
            ? operationOffset + operation->OriginalLength()
            : operationOffset);

    // recycle the operation
    TRACE("IOSchedulerNoop::_Finisher(): operation %p finished so recycling buffer\n", operation);
    MutexLocker _(fLock);
    if (fDMAResource != NULL) {
      fDMAResource->RecycleBuffer(operation->Buffer());
    }

    TRACE("IOSchedulerNoop::_Finisher(): operation %p finished so recycled buffer\n", operation);

    fUnusedOperations.Add(operation);
    fNewRequestCondition.NotifyAll();

    // If the request is done, we need to perform its notifications.
    if (request->IsFinished()) {
      TRACE("IOSchedulerNoop::_Finisher(): request %p is finished\n", request);
      if (request->Status() == B_OK && request->RemainingBytes() > 0) {
        // The request has been processed OK so far, but it isn't really
        // finished yet.
        TRACE("IOSchedulerNoop::_Finisher(): Setting request %p as unfinished cause remaining bytes is %ld\n", request, request->RemainingBytes());
        request->SetUnfinished();
      } else {
        if (request->HasCallbacks()) {
          TRACE("IOSchedulerNoop::_Finisher(): request %p has callbacks, enqueuing for notifier thread\n", request);
          // The request has callbacks that may take some time to
          // perform, so we hand it over to the request notifier.
          fFinishedRequests.Add(request);
          fFinishedRequestCondition.NotifyAll();
        } else {
          TRACE("IOSchedulerNoop::_Finisher(): request %p has no callbacks. Notifying now.\n", request);
          // No callbacks -- finish the request right now.
          IOSchedulerRoster::Default()->Notify(IO_SCHEDULER_REQUEST_FINISHED,
                                               this, request);
          request->NotifyFinished();

          TRACE("IOSchedulerNoop::_Finisher(): request %p notified\n", request);
        }
      }
    }
  }

  TRACE("IOSchedulerNoop::_Finisher(): exiting finisher function\n");
  return B_OK;
}

bool IOSchedulerNoop::_TrySubmittingRequest(IORequest *request) {
  // dprintf("IOSchedulerNoop::_PrepareRequestOperations(%p)\n", request);
  IOOperation *operation = fUnusedOperations.RemoveHead();
  if (operation == NULL) {
    return false;
  }

  if (fDMAResource != NULL) {
      status_t status =
          fDMAResource->TranslateNext(request, operation, request->Length());
      if (status != B_OK) {
        operation->SetParent(NULL);
        fUnusedOperations.Add(operation);
        fNewRequestCondition.NotifyAll();

        // B_BUSY means some resource (DMABuffers or
        // DMABounceBuffers) was temporarily unavailable. That's OK,
        // we'll retry later.
        if (status == B_BUSY) {
          return false;
        }

        AbortRequest(request, status);
        return true;
      }
  } else {
    // TODO: If the device has block size restrictions, we might need to use
    // a bounce buffer.
    status_t status = operation->Prepare(request);
    if (status != B_OK) {
      operation->SetParent(NULL);
      fUnusedOperations.Add(operation);
      fNewRequestCondition.NotifyAll();
      AbortRequest(request, status);
      return true;
    }

    operation->SetOriginalRange(request->Offset(), request->Length());
    request->Advance(request->Length());
  }

  IOSchedulerRoster::Default()->Notify(IO_SCHEDULER_OPERATION_STARTED, this,
                                       operation->Parent(), operation);

  fIOCallback(fIOCallbackData, operation);

  return true;
}

status_t IOSchedulerNoop::_Scheduler() {
  while (!fTerminating) {
    // Wait for new requests.
    TRACE("IOSchedulerNoop::_Scheduler(): Waiting for next scheduled request\n");
    ConditionVariableEntry entry;
    fNewRequestCondition.Add(&entry);
    entry.Wait(B_CAN_INTERRUPT);

    TRACE("IOSchedulerNoop::_Scheduler(): Woken up, resubmitting pending operations\n");
    // First thing's first, try to re-submit some unfinished
    while (true) {
      MutexLocker locker(fLock);
      IOOperation *operation = fRescheduledOperations.RemoveHead();
      if (operation == NULL) {
        break;
      }

      locker.Unlock();

      TRACE("%p->IOSchedulerNoop::_Scheduler(): Re-submitting re-scheduled operation %p to device\n", this, operation);
      IOSchedulerRoster::Default()->Notify(IO_SCHEDULER_OPERATION_STARTED,
                                           this, operation->Parent(), operation);
      fIOCallback(fIOCallbackData, operation);
    }

    TRACE("IOSchedulerNoop::_Scheduler(): Finished with resubmitted operations, acquiring lock\n");
    MutexLocker locker(fLock);

    bool resourcesAvailable = true;
    while (resourcesAvailable) {
      IORequest *request = fScheduledRequests.RemoveHead();
      if (request == NULL) {
        // No requests pending.
        TRACE("IOSchedulerNoop::_Scheduler(): No pending requests to schedule\n");
        break;
      }

      TRACE("IOSchedulerNoop::_Scheduer(): Submitting request %p\n", request);
      locker.Unlock();
      resourcesAvailable = _TrySubmittingRequest(request);
      if (resourcesAvailable) {
        // Successfully submitted request. Remove it from queue.
        TRACE("IOSchedulerNoop::_Scheduer(): Request %p submitted\n", request);
      } else {
        TRACE("IOSchedulerNoop::_Scheduler(): Putting request %p back onto the queue because there are no more buffers available\n", request);
        locker.Lock();
        fScheduledRequests.RemoveHead();
      }
    }
  }

  return B_OK;
}

/*static*/ status_t IOSchedulerNoop::_SchedulerThread(void *_self) {
  IOSchedulerNoop *self = (IOSchedulerNoop *)_self;
  return self->_Scheduler();
}

/*static*/ status_t IOSchedulerNoop::_FinisherThread(void *_self) {
  IOSchedulerNoop *self = reinterpret_cast<IOSchedulerNoop*>(_self);
  return self->_Finisher();
}

status_t IOSchedulerNoop::_RequestNotifier() {
  while (true) {
    MutexLocker locker(fLock);

    // get a request
    IORequest *request = fFinishedRequests.RemoveHead();

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

    IOSchedulerRoster::Default()->Notify(IO_SCHEDULER_REQUEST_FINISHED, this,
                                         request);

    // notify the request
    request->NotifyFinished();
  }

  // never can get here
  return B_OK;
}

/*static*/ status_t IOSchedulerNoop::_RequestNotifierThread(void *_self) {
  IOSchedulerNoop *self = (IOSchedulerNoop *)_self;
  return self->_RequestNotifier();
}
