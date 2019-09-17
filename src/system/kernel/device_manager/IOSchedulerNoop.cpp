#include "IOSchedulerNoop.h"

IOSchedulerNoop::IOSchedulerNoop(DMAResource *resource)
    : IOScheduler(resource)
{
}

status_t IOSchedulerNoop::ScheduleRequest(IORequest *request) { return 0; }

void IOSchedulerNoop::AbortRequest(IORequest *request, status_t status) {}

void IOSchedulerNoop::OperationCompleted(IOOperation *operation,
                                         status_t status,
                                         generic_size_t transferredBytes) {}

void IOSchedulerNoop::Dump() const {}
