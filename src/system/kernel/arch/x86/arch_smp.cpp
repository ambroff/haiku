/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2002-2005, Axel Dörfler, axeld@pinc-software.de. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Copyright 2001-2002, Travis Geiselbrecht. All rights reserved.
 * Distributed under the terms of the NewOS License.
 */


#include <boot/kernel_args.h>
#include <vm/vm.h>
#include <cpu.h>
#include <int.h>
#include <smp.h>
#include <smp_priv.h>

#include <arch/atomic.h>
#include <arch/cpu.h>
#include <arch/vm.h>
#include <arch/smp.h>

#include <arch/x86/apic.h>
#include <arch/x86/arch_smp.h>
#include <arch/x86/smp_priv.h>
#include <arch/x86/timer.h>

#include <string.h>
#include <stdio.h>

#include <algorithm>


//#define TRACE_ARCH_SMP
#ifdef TRACE_ARCH_SMP
#	define TRACE(x) dprintf x
#else
#	define TRACE(x) ;
#endif


#define	ICI_VECTOR		0xfd


static uint32 sCPUAPICIds[SMP_MAX_CPUS];
static uint32 sAPICVersions[SMP_MAX_CPUS];


static int32
x86_ici_interrupt(void *data)
{
	// genuine inter-cpu interrupt
	int cpu = smp_get_current_cpu();
	TRACE(("inter-cpu interrupt on cpu %d\n", cpu));
	return smp_intercpu_int_handler(cpu);
}


static int32
x86_spurious_interrupt(void *data)
{
	// spurious interrupt
	TRACE(("spurious interrupt on cpu %" B_PRId32 "\n", smp_get_current_cpu()));

	// spurious interrupts must not be acknowledged as it does not expect
	// a end of interrupt - if we still do it we would loose the next best
	// interrupt
	return B_HANDLED_INTERRUPT;
}


static int32
x86_smp_error_interrupt(void *data)
{
	// smp error interrupt
	TRACE(("smp error interrupt on cpu %" B_PRId32 "\n", smp_get_current_cpu()));
	return B_HANDLED_INTERRUPT;
}


uint32
x86_get_cpu_apic_id(int32 cpu)
{
	ASSERT(cpu >= 0 && cpu < SMP_MAX_CPUS);
	return sCPUAPICIds[cpu];
}

static int apic_stats(int argc, char **argv);

status_t
arch_smp_init(kernel_args *args)
{
	TRACE(("%s: entry\n", __func__));

	add_debugger_command_etc("apicstats", &apic_stats, "Show APIC command stats", 0);

	if (!apic_available()) {
		// if we don't have an apic we can't do smp
		TRACE(("%s: apic not available for smp\n", __func__));
		return B_OK;
	}

	// setup some globals
	memcpy(sCPUAPICIds, args->arch_args.cpu_apic_id, sizeof(args->arch_args.cpu_apic_id));
	memcpy(sAPICVersions, args->arch_args.cpu_apic_version, sizeof(args->arch_args.cpu_apic_version));

	// set up the local apic on the boot cpu
	arch_smp_per_cpu_init(args, 0);

	if (args->num_cpus > 1) {
		// I/O interrupts start at ARCH_INTERRUPT_BASE, so all interrupts are shifted
		reserve_io_interrupt_vectors(3, 0xfd - ARCH_INTERRUPT_BASE,
			INTERRUPT_TYPE_ICI);
		install_io_interrupt_handler(0xfd - ARCH_INTERRUPT_BASE, &x86_ici_interrupt, NULL, B_NO_LOCK_VECTOR);
		install_io_interrupt_handler(0xfe - ARCH_INTERRUPT_BASE, &x86_smp_error_interrupt, NULL, B_NO_LOCK_VECTOR);
		install_io_interrupt_handler(0xff - ARCH_INTERRUPT_BASE, &x86_spurious_interrupt, NULL, B_NO_LOCK_VECTOR);
	}

	return B_OK;
}


status_t
arch_smp_per_cpu_init(kernel_args *args, int32 cpu)
{
	// set up the local apic on the current cpu
	TRACE(("arch_smp_init_percpu: setting up the apic on cpu %" B_PRId32 "\n",
		cpu));
	apic_per_cpu_init(args, cpu);

	// setup FPU and SSE if supported
	x86_init_fpu();

	return B_OK;
}


void
arch_smp_send_multicast_ici(CPUSet& cpuSet)
{
#if KDEBUG
	if (are_interrupts_enabled())
		panic("arch_smp_send_multicast_ici: called with interrupts enabled");
#endif

	memory_write_barrier();

	int32 i = 0;
	int32 cpuCount = smp_get_num_cpus();

	int32 logicalModeCPUs;
	if (x2apic_available())
		logicalModeCPUs = cpuCount;
	else
		logicalModeCPUs = std::min(cpuCount, int32(8));

	uint32 destination = 0;
	for (; i < logicalModeCPUs; i++) {
		if (cpuSet.GetBit(i) && i != smp_get_current_cpu())
			destination |= gCPU[i].arch.logical_apic_id;
	}

	uint32 mode = ICI_VECTOR | APIC_DELIVERY_MODE_FIXED
			| APIC_INTR_COMMAND_1_ASSERT
			| APIC_INTR_COMMAND_1_DEST_MODE_LOGICAL
			| APIC_INTR_COMMAND_1_DEST_FIELD;

	while (!apic_interrupt_delivered())
		cpu_pause();
	apic_set_interrupt_command(destination, mode);

	for (; i < cpuCount; i++) {
		if (cpuSet.GetBit(i)) {
			uint32 destination = sCPUAPICIds[i];
			uint32 mode = ICI_VECTOR | APIC_DELIVERY_MODE_FIXED
					| APIC_INTR_COMMAND_1_ASSERT
					| APIC_INTR_COMMAND_1_DEST_MODE_PHYSICAL
					| APIC_INTR_COMMAND_1_DEST_FIELD;

			while (!apic_interrupt_delivered())
				cpu_pause();
			apic_set_interrupt_command(destination, mode);
		}
	}
}


void
arch_smp_send_broadcast_ici(void)
{
#if KDEBUG
	if (are_interrupts_enabled())
		panic("arch_smp_send_broadcast_ici: called with interrupts enabled");
#endif

	memory_write_barrier();

	uint32 mode = ICI_VECTOR | APIC_DELIVERY_MODE_FIXED
			| APIC_INTR_COMMAND_1_ASSERT
			| APIC_INTR_COMMAND_1_DEST_MODE_PHYSICAL
			| APIC_INTR_COMMAND_1_DEST_ALL_BUT_SELF;

	while (!apic_interrupt_delivered())
		cpu_pause();
	apic_set_interrupt_command(0, mode);
}

static const size_t RECORD_LENGTH = 4096;
static uint32 gPauses[RECORD_LENGTH];
static nanotime_t gWaitTime[RECORD_LENGTH];
static nanotime_t gSetInterruptCommandTime[RECORD_LENGTH];

static int64 gIndex = -1;

static int apic_stats(int argc, char **argv) {
	int64 total_recordings = gIndex % RECORD_LENGTH;

	uint32 total_waits = 0;
	uint32 max_waits = 0;

	nanotime_t total_wait_time = 0;
	nanotime_t max_wait_time = 0;

	nanotime_t total_set_interrupt_cmd_time = 0;
	nanotime_t max_set_interrupt_cmd_time = 0;

	for (int64 i = 0; i < total_recordings; i++) {
		max_waits = std::max(max_waits, gPause[i]);
		total_waits += gPauses[i];

		max_wait_time = std::max(max_wait_time, gWaitTime[i]);
		total_wait_time += gWaitTime[i];

		max_set_interrupt_cmd_time = std::max(max_set_interrupt_cmd_time, gSetInterruptCommandTime[i]);
		total_set_interrupt_cmd_time += gSetInterruptCommandTime[i];
	}

	kprintf("APIC delivery waits: avg=%d, max=%d\n", total_waits / total_recordings, max_waits);
	kprintf("APIC delivery wait time: avg=%dns, max=%dns\n", total_wait_time / total_recordings, max_wait_time);
	kprintf("APIC set command time: avg=%dns, max=%dns\n", total_set_interrupt_cmd_time / total_recordings, max_set_interrupt_cmd_time);
}

void
arch_smp_send_ici(int32 target_cpu)
{
#if KDEBUG
	if (are_interrupts_enabled())
		panic("arch_smp_send_ici: called with interrupts enabled");
#endif

	memory_write_barrier();

	uint32 destination = sCPUAPICIds[target_cpu];
	uint32 mode = ICI_VECTOR | APIC_DELIVERY_MODE_FIXED
			| APIC_INTR_COMMAND_1_ASSERT
			| APIC_INTR_COMMAND_1_DEST_MODE_PHYSICAL
			| APIC_INTR_COMMAND_1_DEST_FIELD;

	nanotime_t wait_start_time = system_time_nsecs();
	uint32 pause_count = 0;
	while (!apic_interrupt_delivered()) {
		cpu_pause();
		++pause_count;
	}
	nanotime_t wait_time = system_time_nsecs() - wait_start_time;

	nanotime_t set_command_start_time = system_time_nsecs();
	apic_set_interrupt_command(destination, mode);
	nanotime_t set_command_time = system_time_nsecs() - set_command_start_time;

	int64 idx = atomic_add64(gIndex, 1) % RECORD_LENGTH;
	gPauses[idx] = pause_count;
	gWaitTime[idx] = wait_time;
	gSetInterruptCommandTime[idx] = set_command_time;
}
