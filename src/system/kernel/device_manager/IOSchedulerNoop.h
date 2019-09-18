#ifndef IO_SCHEDULER_NOOP_H
#define IO_SCHEDULER_NOOP_H

#include "IOScheduler.h"

class IOSchedulerNoop : public IOScheduler {
public:
  explicit IOSchedulerNoop(DMAResource* resource);
  virtual ~IOSchedulerNoop();

  status_t Init(const char *name) override;

  status_t ScheduleRequest(IORequest *request) override;

  void AbortRequest(IORequest *request, status_t status) override;

  void OperationCompleted(IOOperation *operation, status_t status,
                          generic_size_t transferredBytes) override;

  void Dump() const override;

private:
  static status_t _RetryThread(void *self);
  status_t _RetryLoop();

  mutex fLock;
  ConditionVariable fNewRetryCondition;
  IOOperationList fOperationsToRetry;
  thread_id fRetryThread;
  volatile bool	fTerminating;
};

#endif  // IO_SCHEDULER_NOOP_H
