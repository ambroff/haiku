#include "IOSchedulerNoop.h"

#include "IOSchedulerRoster.h"

#define TRACE_IO_SCHEDULER
#ifdef TRACE_IO_SCHEDULER
#	define TRACE(x...) dprintf(x)
#else
#	define TRACE(x...) ;
#endif

IOSchedulerNoop::IOSchedulerNoop(DMAResource *resource)
    : IOScheduler(resource)
{
}

status_t IOSchedulerNoop::Init(const char *name) {
  TRACE("Initializing IOSchedulerNoop(%p) %s\n", this, name);

  status_t error = IOScheduler::Init(name);
  if (error != B_OK) {
    return error;
  }

  return B_OK;
}

status_t IOSchedulerNoop::ScheduleRequest(IORequest *request)
{
  TRACE("%p->IOSchedulerNoop::ScheduleRequest(%p) size=%ld\n", this, request, request->Length());

  IOOperation *operation = new(nothrow) IOOperation;
  if (operation == NULL) {
    TRACE("Failed to allocate IOOperation\n");
    request->SetStatusAndNotify(B_NO_MEMORY);
    return B_NO_MEMORY;
  }

  TRACE("%p->IOSchedulerNoop: Scheduled request %p\n", this, request);
  IOSchedulerRoster::Default()->Notify(IO_SCHEDULER_REQUEST_SCHEDULED, this,
                                       request);

  if (fDMAResource != NULL) {
    status_t status = fDMAResource->TranslateNext(request, operation, request->Length());
    if (status != B_OK) {
      // FIXME:
      if (status == B_BUSY) {
        TRACE("%p->IOSchedulerNoop: Resource is busy try %p again later\n", this, request);
        //unavailable for now, should enqueue to retry.
      }
      TRACE("%p->IOSchedulerNoop: Failed to prepare/translate request %p dma=%d, aborting\n", this, request, fDMAResource == NULL ? 0 : 1);
      delete operation;
      AbortRequest(request, status);
      return status;
    }
  } else {
    status_t status = operation->Prepare(request);
    if (status != B_OK) {
      // FIXME:
      if (status == B_BUSY) {
        TRACE("%p->IOSchedulerNoop: Resource is busy try %p again later\n", this, request);
        //unavailable for now, should enqueue to retry.
      }
      TRACE("%p->IOSchedulerNoop: Failed to prepare/translate request %p dma=%d, aborting\n", this, request, fDMAResource == NULL ? 0 : 1);
      delete operation;
      AbortRequest(request, status);
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

  delete operation;
}

void IOSchedulerNoop::Dump() const {
  kprintf("IOSchedulerNoop named %s at %p\n", fName, this);
  kprintf("  DMA resource: %p\n", fDMAResource);
}
