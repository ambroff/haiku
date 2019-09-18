#include "IOSchedulerNoop.h"

#include "IOSchedulerRoster.h"

IOSchedulerNoop::IOSchedulerNoop(DMAResource *resource)
    : IOScheduler(resource)
      //fRequestOwners(NULL)
{
}

status_t IOSchedulerNoop::Init(const char *name) {
  status_t error = IOScheduler::Init(name);
  if (error != B_OK) {
    return error;
  }

//  fRequestOwners = new(std::nothrow) IORequestOwnerHashTable;
//  if (fRequestOwners == NULL)
//    return B_NO_MEMORY;

  return B_OK;
}

status_t IOSchedulerNoop::ScheduleRequest(IORequest *request)
{
  IOOperation *operation = new(nothrow) IOOperation;
  if (operation == NULL) {
    request->SetStatusAndNotify(B_NO_MEMORY);
    return B_NO_MEMORY;
  }

//  {
//    auto team_id = request->TeamID();
//    auto thread_id = request->ThreadID();
//    IORequestOwner *owner = fRequestOwners->Lookup(thread_id);
//    if (owner == NULL) {
//      owner = new(nothrow) IORequestOwner;
//      if (owner == NULL) {
//        panic("IOSchedulerNoop: Out of request owners!\n");
//        request->SetStatusAndNotify(B_NO_MEMORY);
//        return B_NO_MEMORY;
//      }
//
//      owner->team = team_id;
//      owner->thread = thread_id;
//      fRequestOwners->InsertUnchecked(owner);
//
//      int32 priority = thread_get_io_priority(request->ThreadID());
//      if (priority >= 0) {
//        owner->priority = priority;
//      } else {
//        owner->priority = B_IDLE_PRIORITY;
//      }
//    }
//
//    request->SetOwner(owner);
//    owner->requests.Add(request);
//  }



  IOSchedulerRoster::Default()->Notify(IO_SCHEDULER_REQUEST_SCHEDULED, this,
                                       request);

  status_t status = (fDMAResource == NULL)
                    ? operation->Prepare(request)
                    : fDMAResource->TranslateNext(request, operation, request->Length());
  if (status != B_OK) {
    //if (status == B_BUSY) {
    // unavailable for now, should enqueue to retry.
    //}
    delete operation;
    AbortRequest(request, status);
    return status;
  }

  IOSchedulerRoster::Default()->Notify(IO_SCHEDULER_OPERATION_STARTED, this, operation->Parent(), operation);

  fIOCallback(fIOCallbackData, operation);

  return B_OK;
}

void IOSchedulerNoop::AbortRequest(IORequest *request, status_t status)
{
  // FIXME: Implement this. Status B_CANCELLED?
}

void IOSchedulerNoop::OperationCompleted(IOOperation *operation,
                                         status_t status,
                                         generic_size_t transferredBytes)
{
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

  IOSchedulerRoster::Default()->Notify(
      IO_SCHEDULER_REQUEST_FINISHED, this, request);
  request->NotifyFinished();

  delete operation;
}

void IOSchedulerNoop::Dump() const {
  kprintf("IOSchedulerNoop named %s at %p\n", fName, this);
  kprintf("  DMA resource: %p\n", fDMAResource);
}
