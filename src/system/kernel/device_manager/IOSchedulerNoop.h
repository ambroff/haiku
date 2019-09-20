#ifndef IO_SCHEDULER_NOOP_H
#define IO_SCHEDULER_NOOP_H

#include <KernelExport.h>

#include <condition_variable.h>
#include <lock.h>
#include <util/OpenHashTable.h>

#include "IOScheduler.h"
#include "dma_resources.h"

class IOSchedulerNoop : public IOScheduler {
public:
  IOSchedulerNoop(DMAResource *resource);

  virtual ~IOSchedulerNoop();

  virtual status_t Init(const char *name);

  virtual status_t ScheduleRequest(IORequest *request);

  virtual void AbortRequest(IORequest *request, status_t status = B_CANCELED);

  virtual void OperationCompleted(IOOperation *operation, status_t status,
                                  generic_size_t transferredBytes);

  virtual void Dump() const;

private:
  status_t _Finisher();
  bool _TrySubmittingRequest(IORequest *request);
  status_t _Scheduler();
  static status_t _SchedulerThread(void *self);
  status_t _RequestNotifier();
  static status_t _RequestNotifierThread(void *self);
  static status_t _FinisherThread(void *self);

private:
  spinlock fFinisherLock;
  mutex fLock;
  thread_id fSchedulerThread;
  thread_id fFinisherThread;
  thread_id fRequestNotifierThread;
  IORequestList fScheduledRequests;
  IORequestList fFinishedRequests;
  ConditionVariable fNewRequestCondition;
  ConditionVariable fFinishedOperationCondition;
  ConditionVariable fFinishedRequestCondition;
  IOOperationList fUnusedOperations;
  IOOperationList fCompletedOperations;
  IOOperationList fRescheduledOperations;
  generic_size_t fBlockSize;
  volatile bool fTerminating;
};

#endif // IO_SCHEDULER_NOOP_H
