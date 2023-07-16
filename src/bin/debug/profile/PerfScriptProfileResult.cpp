#include "PerfScriptProfileResult.h"

#include "Options.h"

void PerfScriptProfileResult::AddSamples(ImageProfileResultContainer *container, addr_t *samples,
                        int32 sampleCount)
{
	fprintf(stderr, "KWA: AddSamples(container, samples, %d)\n", sampleCount);
}

void PerfScriptProfileResult::AddDroppedTicks(int32 dropped)
{
	fprintf(stderr, "KWA: AddDroppedTicks(%d)\n", dropped);
}

void PerfScriptProfileResult::PrintResults(ImageProfileResultContainer *container)
{
	fprintf(gOptions.output, "Fake results!\n");
}

status_t PerfScriptProfileResult::GetImageProfileResult(SharedImage *image, image_id id,
                                       ImageProfileResult *&_imageResult)
{
	return B_OK;
}
