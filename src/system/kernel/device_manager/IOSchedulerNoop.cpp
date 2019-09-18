#include "IOSchedulerNoop.h"

#include <stdio.h>
#include <thread.h>

#include "IOSchedulerRoster.h"

#define TRACE_IO_SCHEDULER
#ifdef TRACE_IO_SCHEDULER
#	define TRACE(x...) dprintf(x)
#else
#	define TRACE(x...) ;
#endif

IOSchedulerNoop::IOSchedulerNoop(DMAResource *resource)
    : IOScheduler(resource),
      fRetryThread(-1),
      fTerminating(false)
{
  mutex_init(&fLock, "I/O scheduler");

  fNewRetryCondition.Init(this, "I/O request retry");
}

IOSchedulerNoop::~IOSchedulerNoop() {
  MutexLocker locker(&fLock);
  fTerminating = true;

  fNewRetryCondition.NotifyAll();

  if (fRetryThread >= 0) {
    wait_for_thread(fRetryThread, NULL);
  }

  while (IOOperation *operation = fOperationPool.RemoveHead()) {
    delete operation;
  }

  mutex_lock(&fLock);
  mutex_destroy(&fLock);
}

status_t IOSchedulerNoop::Init(const char *name) {
  status_t error = IOScheduler::Init(name);
  if (error != B_OK) {
    return error;
  }

  size_t buffer_count = fDMAResource == NULL ? -1 : fDMAResource->BufferCount();
  TRACE("Initializing IOSchedulerNoop(%p) %s buffer_count=%ld\n", this, name, buffer_count);

  for (size_t i = 0; i < buffer_count; ++i) {
    IOOperation *operation = new(std::nothrow) IOOperation;
    if (operation == NULL) {
      return B_NO_MEMORY;
    }

    fOperationPool.Add(operation);
  }

  {
    char buffer[B_OS_NAME_LENGTH];
    strlcpy(buffer, name, sizeof(buffer));
    strlcat(buffer, " scheduler ", sizeof(buffer));
    size_t nameLength = strlen(buffer);
    snprintf(buffer + nameLength, sizeof(buffer) - nameLength, "%" B_PRId32, fID);
    fRetryThread = spawn_kernel_thread(&_RetryThread, buffer, B_NORMAL_PRIORITY + 2, reinterpret_cast<void *>(this));
    if (fRetryThread < B_OK) {
      return fRetryThread;
    }

    resume_thread(fRetryThread);
  }

  return B_OK;
}

status_t IOSchedulerNoop::ScheduleRequest(IORequest *request)
{
  TRACE("%p->IOSchedulerNoop::ScheduleRequest(%p) size=%ld\n", this, request, request->Length());

  MutexLocker locker(&fLock);

  {
    IOBuffer *buf = request->Buffer();
    if (buf->IsVirtual()) {
      status_t status = buf->LockMemory(request->TeamID(), request->IsWrite());
      if (status != B_OK) {
        request->SetStatusAndNotify(status);
        return status;
      }
    }
  }

  IOOperation *operation = fOperationPool.RemoveHead();
  if (operation == NULL) {
    TRACE("Failed to allocate IOOperation because the device is busy. Enqueuing for later\n");
    fRequestsToRetry.Add(request);
    fNewRetryCondition.NotifyAll();
    return B_OK;
  }

  TRACE("%p->IOSchedulerNoop: Scheduled request %p\n", this, request);
  IOSchedulerRoster::Default()->Notify(IO_SCHEDULER_REQUEST_SCHEDULED, this,
                                       request);

  if (fDMAResource != NULL) {
    status_t status = fDMAResource->TranslateNext(request, operation, request->Length());
    if (status != B_OK) {
      operation->SetParent(NULL);
      fOperationPool.Add(operation);

      if (status == B_BUSY) {
        TRACE("%p->IOSchedulerNoop: Resource is busy try %p again later\n", this, request);
        fRequestsToRetry.Add(request);
        return B_OK;
      }

      TRACE("%p->IOSchedulerNoop: Failed to translate dma request %p, aborting\n", this, request);
      AbortRequest(request, status);
      return status;
    }
  } else {
    status_t status = operation->Prepare(request);
    if (status != B_OK) {
      // FIXME:
      TRACE("%p->IOSchedulerNoop: Failed to prepare request %p, aborting\n", this, request);
      AbortRequest(request, status);
      operation->SetParent(NULL);
      fOperationPool.Add(operation);
      return status;
    }

    operation->SetOriginalRange(request->Offset(), request->Length());
    request->Advance(request->Length());
  }

  TRACE("%p->IOSchedulerNoop: Request started %p\n", this, operation->Parent());
  IOSchedulerRoster::Default()->Notify(IO_SCHEDULER_OPERATION_STARTED, this, operation->Parent(), operation);
  fIOCallback(fIOCallbackData, operation);

  return B_OK;
}

void IOSchedulerNoop::AbortRequest(IORequest *request, status_t status)
{
  TRACE("%p->IOSchedulerNoop: AbortRequest called %p status=%d\n", this, request, status);
  // FIXME: Implement this. Status B_CANCELLED?
}

void IOSchedulerNoop::OperationCompleted(IOOperation *operation,
                                         status_t status,
                                         generic_size_t transferredBytes)
{
  TRACE("%p->IOSchedulerNoop: Operation completed %p status=%d transferedBytes=%ld\n", this, operation->Parent(), status, transferredBytes);
  if (operation->Status() <= 0) {
    return;
  }

  operation->SetStatus(status);

  generic_size_t partialBegin = operation->OriginalOffset() - operation->Offset();
  operation->SetTransferredBytes(transferredBytes > partialBegin ? transferredBytes - partialBegin : 0);

  IOSchedulerRoster::Default()->Notify(IO_SCHEDULER_OPERATION_FINISHED, this, operation->Parent(), operation);

  IORequest *request = operation->Parent();

  generic_size_t operationOffset = operation->OriginalOffset() - request->Offset();
  request->OperationFinished(
      operation,
      operation->Status(),
      operation->TransferredBytes() < operation->OriginalLength(),
      operation->Status() == B_OK ? operationOffset + operation->OriginalLength() : operationOffset);

  if (fDMAResource) {
    fDMAResource->RecycleBuffer(operation->Buffer());
  }

  TRACE("%p->IOSchedulerNoop: Request completed %p\n", this, operation->Parent());
  IOSchedulerRoster::Default()->Notify(
      IO_SCHEDULER_REQUEST_FINISHED, this, request);
  request->NotifyFinished();

  operation->SetParent(NULL);

  MutexLocker locker(&fLock);
  fOperationPool.Add(operation);
}

void IOSchedulerNoop::Dump() const {
  kprintf("IOSchedulerNoop named %s at %p\n", fName, this);
  kprintf("  DMA resource: %p\n", fDMAResource);
}

status_t IOSchedulerNoop::_RetryThread(void *self) {
  return reinterpret_cast<IOSchedulerNoop*>(self)->_RetryLoop();
}

status_t IOSchedulerNoop::_RetryLoop() {
  while (!fTerminating) {
    MutexLocker locker(fLock);

    IORequest *request = fRequestsToRetry.RemoveHead();
    if (request == NULL) {
      ConditionVariableEntry entry;
      fNewRetryCondition.Add(&entry);

      locker.Unlock();
      entry.Wait();
      continue;
    }

    IOOperation *operation = fOperationPool.RemoveHead();

    if (fDMAResource != NULL) {
      status_t status = fDMAResource->TranslateNext(
          operation->Parent(), operation, operation->Parent()->Length());
      if (status != B_OK) {
        if (status == B_BUSY) {
          TRACE("%p->IOSchedulerNoop: Resource is busy try %p again later\n",
                this, operation->Parent());
          fRequestsToRetry.Add(request);
          continue;
        }

        TRACE("%p->IOSchedulerNoop: Failed to translate dma request %p, aborting\n",
              this, operation->Parent());
        AbortRequest(operation->Parent(), status);
        operation->SetParent(NULL);
        fOperationPool.Add(operation);
        return status;
      }
    } else {

    }
  }

  return B_OK;
}