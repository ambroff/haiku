///-*-C++-*-//////////////////////////////////////////////////////////////////
//
// Hoard: A Fast, Scalable, and Memory-Efficient Allocator
//        for Shared-Memory Multiprocessors
// Contact author: Emery Berger, http://www.cs.utexas.edu/users/emery
//
// Copyright (c) 1998-2000, The University of Texas at Austin.
//
// This library is free software; you can redistribute it and/or modify
// it under the terms of the GNU Library General Public License as
// published by the Free Software Foundation, http://www.fsf.org.
//
// This library is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Library General Public License for more details.
//
//////////////////////////////////////////////////////////////////////////////

//#include <limits.h>
#include <string.h>

#include "config.h"

#include "heap.h"
#include "processheap.h"
#include "threadheap.h"
#include <user_thread.h>

#define KTRACE(format...) ktrace_printf(format)

using namespace BPrivate;


threadHeap::threadHeap(void)
	:_pHeap(0)
{
}


// malloc (sz):
//   inputs: the size of the object to be allocated.
//   returns: a pointer to an object of the appropriate size.
//   side effects: allocates a block from a superblock;
//                 may call sbrk() (via makeSuperblock).

void *
threadHeap::malloc(const size_t size)
{
	bool should_log = (size == 2000000013);

	if (should_log) {
		KTRACE("KWA: threadHeap::malloc(%lu) called\n");
	}

#if MAX_INTERNAL_FRAGMENTATION == 2
	if (size > 1063315264UL) {
		debug_printf("malloc() of %lu bytes asked\n", size);
		return NULL;
	}
#endif

	const int sizeclass = sizeClass(size);
	block *b = NULL;

	if (should_log) {
		KTRACE("KWA: threadHeap::malloc(%lu) using sizeclass=%d\n", size, sizeclass);
	}

	lock();

	// Look for a free block.
	// We usually have memory locally so we first look for space in the
	// superblock list.

	superblock *sb = findAvailableSuperblock(sizeclass, b, _pHeap);
	if (sb == NULL) {
		if (should_log) {
			KTRACE("KWA: No memory locally, we need to get more from the process heap\n");
		}
		// We don't have memory locally.
		// Try to get more from the process heap.

		assert(_pHeap);
		sb = _pHeap->acquire((int)sizeclass, this);

		// If we didn't get any memory from the process heap,
		// we'll have to allocate our own superblock.
		if (sb == NULL) {
			if (should_log) {
				KTRACE("KWA: Still not enough memory, so need to allocate own superblock\n");
			}
			sb = superblock::makeSuperblock(sizeclass, _pHeap);
			if (sb == NULL) {
				// We're out of memory!
				if (should_log) {
					KTRACE("KWA: Oops, out of memory, can't do it.\n");
				}
				unlock();
				return NULL;
			}
#if HEAP_LOG
			// Record the memory allocation.
			MemoryRequest m;
			m.allocate((int)sb->getNumBlocks() *
				(int)sizeFromClass(sb->getBlockSizeClass()));
			_pHeap->getLog(getIndex()).append(m);
#endif
#if HEAP_FRAG_STATS
			_pHeap->setAllocated(0,
				sb->getNumBlocks() * sizeFromClass(sb->getBlockSizeClass()));
#endif
		}
		// Get a block from the superblock.
		b = sb->getBlock();
		assert(b != NULL);

		// Insert the superblock into our list.
		insertSuperblock(sizeclass, sb, _pHeap);
	} else if (should_log) {
		KTRACE("KWA: found available superblock %p for malloc(%lu)\n", sb, size);
	}

	assert(b != NULL);
	assert(b->isValid());
	assert(sb->isValid());

	b->markAllocated();

#if HEAP_LOG
	MemoryRequest m;
	m.malloc((void *)(b + 1), align(size));
	_pHeap->getLog(getIndex()).append(m);
#endif
#if HEAP_FRAG_STATS
	b->setRequestedSize(align(size));
	_pHeap->setAllocated(align(size), 0);
#endif

	unlock();

	// Skip past the block header and return the pointer.
	return (void *)(b + 1);
}
