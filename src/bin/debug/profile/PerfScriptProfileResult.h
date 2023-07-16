#ifndef PERF_SCRIPT_PROFILE_RESULT_H
#define PERF_SCRIPT_PROFILE_RESULT_H

#include "ProfileResult.h"

class PerfScriptProfileResult : public ProfileResult
{
public:
	virtual void AddSamples(ImageProfileResultContainer* container, addr_t* samples, int32 sampleCount);

	virtual void AddDroppedTicks(int32 dropped);

	virtual void PrintResults(ImageProfileResultContainer* container);

	virtual status_t GetImageProfileResult(SharedImage* image, image_id id, ImageProfileResult*& _imageResult);
};

#endif
