#ifndef IO_SCHEDULER_NOOP_H
#define IO_SCHEDULER_NOOP_H


#include <KernelExport.h>

#include <condition_variable.h>
#include <lock.h>
#include <util/OpenHashTable.h>

#include "dma_resources.h"
#include "IOScheduler.h"


class IOSchedulerNoop : public IOScheduler {
public:
								IOSchedulerNoop(DMAResource* resource);
	virtual						~IOSchedulerNoop();

	virtual	status_t			Init(const char* name);

	virtual	status_t			ScheduleRequest(IORequest* request);

	virtual	void				AbortRequest(IORequest* request,
									status_t status = B_CANCELED);
	virtual	void				OperationCompleted(IOOperation* operation,
									status_t status,
									generic_size_t transferredBytes);
									// called by the driver when the operation
									// has been completed successfully or failed
									// for some reason

	virtual	void				Dump() const;

private:
			typedef DoublyLinkedList<IORequestOwner> RequestOwnerList;


			void				_Finisher();
			bool				_FinisherWorkPending();
			bool				_NextActiveRequestOwner(IORequestOwner*& owner);
			bool				_PrepareRequestOperations(IORequest* request,
									IOOperationList& operations,
									int32& operationsPrepared);
			status_t			_Scheduler();
	static	status_t			_SchedulerThread(void* self);
			status_t			_RequestNotifier();
	static	status_t			_RequestNotifierThread(void* self);

			void				_AddRequestOwner(IORequestOwner* owner);
			IORequestOwner*		_GetRequestOwner(team_id team, thread_id thread,
									bool allocate);

private:
			spinlock			fFinisherLock;
			mutex				fLock;
			thread_id			fSchedulerThread;
			thread_id			fRequestNotifierThread;
			IORequestList		fUnscheduledRequests;
			IORequestList		fFinishedRequests;
			ConditionVariable	fNewRequestCondition;
			ConditionVariable	fFinishedOperationCondition;
			ConditionVariable	fFinishedRequestCondition;
			IOOperationList		fUnusedOperations;
			IOOperationList		fCompletedOperations;
			IORequestOwner*		fAllocatedRequestOwners;
			int32				fAllocatedRequestOwnerCount;
			RequestOwnerList	fActiveRequestOwners;
			RequestOwnerList	fUnusedRequestOwners;
			IORequestOwnerHashTable* fRequestOwners;
			generic_size_t		fBlockSize;
			int32				fPendingOperations;
	volatile bool				fTerminating;
};


#endif	// IO_SCHEDULER_NOOP_H
