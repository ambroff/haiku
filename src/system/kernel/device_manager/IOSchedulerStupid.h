#ifndef IO_SCHEDULER_STUPID_H
#define IO_SCHEDULER_STUPID_H

#include <KernelExport.h>

#include <condition_variable.h>
#include <lock.h>
#include <util/OpenHashTable.h>

#include "IOScheduler.h"
#include "dma_resources.h"

class IOSchedulerStupid : public IOScheduler {
public:
	IOSchedulerStupid(DMAResource *resource);

	virtual ~IOSchedulerStupid();

	virtual status_t Init(const char *name);

	virtual status_t ScheduleRequest(IORequest *request);

	virtual void AbortRequest(IORequest *request, status_t status = B_CANCELED);

	virtual void OperationCompleted(IOOperation *operation, status_t status,
									generic_size_t transferredBytes);

	virtual void Dump() const;

private:
	generic_size_t fBlockSize;
};

#endif // IO_SCHEDULER_STUPID_H
