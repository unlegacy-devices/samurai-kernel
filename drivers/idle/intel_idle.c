/*
 * intel_idle.c - native hardware idle loop for modern Intel processors
 *
 * Copyright (c) 2010, Intel Corporation.
 * Len Brown <len.brown@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 */

/*
 * intel_idle is a cpuidle driver that loads on specific Intel processors
 * in lieu of the legacy ACPI processor_idle driver.  The intent is to
 * make Linux more efficient on these processors, as intel_idle knows
 * more than ACPI, as well as make Linux more immune to ACPI BIOS bugs.
 */

/*
 * Design Assumptions
 *
 * All CPUs have same idle states as boot CPU
 *
 * Chipset BM_STS (bus master status) bit is a NOP
 *	for preventing entry into deep C-stats
 */

/*
 * Known limitations
 *
 * The driver currently initializes for_each_online_cpu() upon modprobe.
 * It it unaware of subsequent processors hot-added to the system.
 * This means that if you boot with maxcpus=n and later online
 * processors above n, those processors will use C1 only.
 *
 * ACPI has a .suspend hack to turn off deep c-statees during suspend
 * to avoid complications with the lapic timer workaround.
 * Have not seen issues with suspend, but may need same workaround here.
 *
 * There is currently no kernel-based automatic probing/loading mechanism
 * if the driver is built as a module.
 */

/* un-comment DEBUG to enable pr_debug() statements */
#define DEBUG

#include <linux/kernel.h>
#include <linux/cpuidle.h>
#include <linux/clockchips.h>
#include <linux/hrtimer.h>	/* ktime_get_real() */
#include <trace/events/power.h>
#include <linux/sched.h>

#define INTEL_IDLE_VERSION "0.4"
#define PREFIX "intel_idle: "

#define MWAIT_SUBSTATE_MASK	(0xf)
#define MWAIT_CSTATE_MASK	(0xf)
#define MWAIT_SUBSTATE_SIZE	(4)
#define MWAIT_MAX_NUM_CSTATES	8
#define CPUID_MWAIT_LEAF (5)
#define CPUID5_ECX_EXTENSIONS_SUPPORTED (0x1)
#define CPUID5_ECX_INTERRUPT_BREAK	(0x2)

static struct cpuidle_driver intel_idle_driver = {
	.name = "intel_idle",
	.owner = THIS_MODULE,
};
/* intel_idle.max_cstate=0 disables driver */
static int max_cstate = MWAIT_MAX_NUM_CSTATES - 1;
static int power_policy = 7; /* 0 = max perf; 15 = max powersave */

static unsigned int substates;
static int (*choose_substate)(int);

/* Reliable LAPIC Timer States, bit 1 for C1 etc.  */
static unsigned int lapic_timer_reliable_states;

static struct cpuidle_device *intel_idle_cpuidle_devices;
static int intel_idle(struct cpuidle_device *dev, struct cpuidle_state *state);

static struct cpuidle_state *cpuidle_state_table;

/*
 * States are indexed by the cstate number,
 * which is also the index into the MWAIT hint array.
 * Thus C0 is a dummy.
 */
static struct cpuidle_state nehalem_cstates[MWAIT_MAX_NUM_CSTATES] = {
	{ /* MWAIT C0 */ },
	{ /* MWAIT C1 */
		.name = "NHM-C1",
		.desc = "MWAIT 0x00",
		.driver_data = (void *) 0x00,
		.flags = CPUIDLE_FLAG_TIME_VALID,
		.exit_latency = 3,
		.power_usage = 1000,
		.target_residency = 6,
		.enter = &intel_idle },
	{ /* MWAIT C2 */
		.name = "NHM-C3",
		.desc = "MWAIT 0x10",
		.driver_data = (void *) 0x10,
		.flags = CPUIDLE_FLAG_TIME_VALID,
		.exit_latency = 20,
		.power_usage = 500,
		.target_residency = 80,
		.enter = &intel_idle },
	{ /* MWAIT C3 */
		.name = "NHM-C6",
		.desc = "MWAIT 0x20",
		.driver_data = (void *) 0x20,
		.flags = CPUIDLE_FLAG_TIME_VALID,
		.exit_latency = 200,
		.power_usage = 350,
		.target_residency = 800,
		.enter = &intel_idle },
};

static struct cpuidle_state atom_cstates[MWAIT_MAX_NUM_CSTATES] = {
	{ /* MWAIT C0 */ },
	{ /* MWAIT C1 */
		.name = "ATM-C1",
		.desc = "MWAIT 0x00",
		.driver_data = (void *) 0x00,
		.flags = CPUIDLE_FLAG_TIME_VALID,
		.exit_latency = 1,
		.power_usage = 1000,
		.target_residency = 4,
		.enter = &intel_idle },
	{ /* MWAIT C2 */
		.name = "ATM-C2",
		.desc = "MWAIT 0x10",
		.driver_data = (void *) 0x10,
		.flags = CPUIDLE_FLAG_TIME_VALID,
		.exit_latency = 20,
		.power_usage = 500,
		.target_residency = 80,
		.enter = &intel_idle },
	{ /* MWAIT C3 */ },
	{ /* MWAIT C4 */
		.name = "ATM-C4",
		.desc = "MWAIT 0x30",
		.driver_data = (void *) 0x30,
		.flags = CPUIDLE_FLAG_TIME_VALID,
		.exit_latency = 100,
		.power_usage = 250,
		.target_residency = 400,
		.enter = &intel_idle },
	{ /* MWAIT C5 */ },
	{ /* MWAIT C6 */
		.name = "ATM-C6",
		.desc = "MWAIT 0x40",
		.driver_data = (void *) 0x40,
		.flags = CPUIDLE_FLAG_TIME_VALID,
		.exit_latency = 200,
		.power_usage = 150,
		.target_residency = 800,
		.enter = NULL },	/* disabled */
};

/*
 * choose_tunable_substate()
 *
 * Run-time decision on which C-state substate to invoke
 * If power_policy = 0, choose shallowest substate (0)
 * If power_policy = 15, choose deepest substate
 * If power_policy = middle, choose middle substate etc.
 */
static int choose_tunable_substate(int cstate)
{
	unsigned int num_substates;
	unsigned int substate_choice;

	power_policy &= 0xF;	/* valid range: 0-15 */
	cstate &= 7;	/* valid range: 0-7 */

	num_substates = (substates >> ((cstate) * 4)) & MWAIT_SUBSTATE_MASK;

	if (num_substates <= 1)
		return 0;

	substate_choice = ((power_policy + (power_policy + 1) *
				(num_substates - 1)) / 16);

	return substate_choice;
}

/*
 * choose_zero_substate()
 */
static int choose_zero_substate(int cstate)
{
	return 0;
}

/**
 * intel_idle
 * @dev: cpuidle_device
 * @state: cpuidle state
 *
 */
static int intel_idle(struct cpuidle_device *dev, struct cpuidle_state *state)
{
	unsigned long ecx = 1; /* break on interrupt flag */
	unsigned long eax = (unsigned long)cpuidle_get_statedata(state);
	unsigned int cstate;
	ktime_t kt_before, kt_after;
	s64 usec_delta;
	int cpu = smp_processor_id();

	cstate = (((eax) >> MWAIT_SUBSTATE_SIZE) & MWAIT_CSTATE_MASK) + 1;

	eax = eax + (choose_substate)(cstate);

	local_irq_disable();

	if (!(lapic_timer_reliable_states & (1 << (cstate))))
		clockevents_notify(CLOCK_EVT_NOTIFY_BROADCAST_ENTER, &cpu);

	kt_before = ktime_get_real();

	stop_critical_timings();
#ifndef MODULE
	trace_power_start(POWER_CSTATE, (eax >> 4) + 1);
#endif
	if (!need_resched()) {

		__monitor((void *)&current_thread_info()->flags, 0, 0);
		smp_mb();
		if (!need_resched())
			__mwait(eax, ecx);
	}

	start_critical_timings();

	kt_after = ktime_get_real();
	usec_delta = ktime_to_us(ktime_sub(kt_after, kt_before));

	local_irq_enable();

	if (!(lapic_timer_reliable_states & (1 << (cstate))))
		clockevents_notify(CLOCK_EVT_NOTIFY_BROADCAST_EXIT, &cpu);

	return usec_delta;
}

/*
 * intel_idle_probe()
 */
static int intel_idle_probe(void)
{
	unsigned int eax, ebx, ecx, edx;

	if (max_cstate == 0) {
		pr_debug(PREFIX "disabled\n");
		return -EPERM;
	}

	if (boot_cpu_data.x86_vendor != X86_VENDOR_INTEL)
		return -ENODEV;

	if (!boot_cpu_has(X86_FEATURE_MWAIT))
		return -ENODEV;

	if (boot_cpu_data.cpuid_level < CPUID_MWAIT_LEAF)
		return -ENODEV;

	cpuid(CPUID_MWAIT_LEAF, &eax, &ebx, &ecx, &edx);

	if (!(ecx & CPUID5_ECX_EXTENSIONS_SUPPORTED) ||
		!(ecx & CPUID5_ECX_INTERRUPT_BREAK))
			return -ENODEV;
#ifdef DEBUG
	if (substates == 0)	/* can over-ride via modparam */
#endif
		substates = edx;

	pr_debug(PREFIX "MWAIT substates: 0x%x\n", substates);

	if (boot_cpu_has(X86_FEATURE_ARAT))	/* Always Reliable APIC Timer */
		lapic_timer_reliable_states = 0xFFFFFFFF;

	if (boot_cpu_data.x86 != 6)	/* family 6 */
		return -ENODEV;

	switch (boot_cpu_data.x86_model) {

	case 0x1A:	/* Core i7, Xeon 5500 series */
	case 0x1E:	/* Core i7 and i5 Processor - Lynnfield Jasper Forest */
	case 0x1F:	/* Core i7 and i5 Processor - Nehalem */
	case 0x2E:	/* Nehalem-EX Xeon */
		lapic_timer_reliable_states = (1 << 1);	 /* C1 */

	case 0x25:	/* Westmere */
	case 0x2C:	/* Westmere */
		cpuidle_state_table = nehalem_cstates;
		choose_substate = choose_tunable_substate;
		break;

	case 0x1C:	/* 28 - Atom Processor */
	case 0x26:	/* 38 - Lincroft Atom Processor */
		lapic_timer_reliable_states = (1 << 2) | (1 << 1); /* C2, C1 */
		cpuidle_state_table = atom_cstates;
		choose_substate = choose_zero_substate;
		break;
#ifdef FUTURE_USE
	case 0x17:	/* 23 - Core 2 Duo */
		lapic_timer_reliable_states = (1 << 2) | (1 << 1); /* C2, C1 */
#endif

	default:
		pr_debug(PREFIX "does not run on family %d model %d\n",
			boot_cpu_data.x86, boot_cpu_data.x86_model);
		return -ENODEV;
	}

	pr_debug(PREFIX "v" INTEL_IDLE_VERSION
		" model 0x%X\n", boot_cpu_data.x86_model);

	pr_debug(PREFIX "lapic_timer_reliable_states 0x%x\n",
		lapic_timer_reliable_states);
	return 0;
}

/*
 * intel_idle_cpuidle_devices_uninit()
 * unregister, free cpuidle_devices
 */
static void intel_idle_cpuidle_devices_uninit(void)
{
	int i;
	struct cpuidle_device *dev;

	for_each_online_cpu(i) {
		dev = per_cpu_ptr(intel_idle_cpuidle_devices, i);
		cpuidle_unregister_device(dev);
	}

	free_percpu(intel_idle_cpuidle_devices);
	return;
}
/*
 * intel_idle_cpuidle_devices_init()
 * allocate, initialize, register cpuidle_devices
 */
static int intel_idle_cpuidle_devices_init(void)
{
	int i, cstate;
	struct cpuidle_device *dev;

	intel_idle_cpuidle_devices = alloc_percpu(struct cpuidle_device);
	if (intel_idle_cpuidle_devices == NULL)
		return -ENOMEM;

	for_each_online_cpu(i) {
		dev = per_cpu_ptr(intel_idle_cpuidle_devices, i);

		dev->state_count = 1;

		for (cstate = 1; cstate < MWAIT_MAX_NUM_CSTATES; ++cstate) {
			int num_substates;

			if (cstate > max_cstate) {
				printk(PREFIX "max_cstate %d reached\n",
					max_cstate);
				break;
			}

			/* does the state exist in CPUID.MWAIT? */
			num_substates = (substates >> ((cstate) * 4))
						& MWAIT_SUBSTATE_MASK;
			if (num_substates == 0)
				continue;
			/* is the state not enabled? */
			if (cpuidle_state_table[cstate].enter == NULL) {
				/* does the driver not know about the state? */
				if (*cpuidle_state_table[cstate].name == '\0')
					pr_debug(PREFIX "unaware of model 0x%x"
						" MWAIT %d please"
						" contact lenb@kernel.org",
					boot_cpu_data.x86_model, cstate);
				continue;
			}

			if ((cstate > 2) &&
				!boot_cpu_has(X86_FEATURE_NONSTOP_TSC))
				mark_tsc_unstable("TSC halts in idle"
					" states deeper than C2");

			dev->states[dev->state_count] =	/* structure copy */
				cpuidle_state_table[cstate];

			dev->state_count += 1;
		}

		dev->cpu = i;
		if (cpuidle_register_device(dev)) {
			pr_debug(PREFIX "cpuidle_register_device %d failed!\n",
				 i);
			intel_idle_cpuidle_devices_uninit();
			return -EIO;
		}
	}

	return 0;
}


static int __init intel_idle_init(void)
{
	int retval;

	retval = intel_idle_probe();
	if (retval)
		return retval;

	retval = cpuidle_register_driver(&intel_idle_driver);
	if (retval) {
		printk(KERN_DEBUG PREFIX "intel_idle yielding to %s",
			cpuidle_get_driver()->name);
		return retval;
	}

	retval = intel_idle_cpuidle_devices_init();
	if (retval) {
		cpuidle_unregister_driver(&intel_idle_driver);
		return retval;
	}

	return 0;
}

static void __exit intel_idle_exit(void)
{
	intel_idle_cpuidle_devices_uninit();
	cpuidle_unregister_driver(&intel_idle_driver);

	return;
}

module_init(intel_idle_init);
module_exit(intel_idle_exit);

module_param(power_policy, int, 0644);
module_param(max_cstate, int, 0444);
#ifdef DEBUG
module_param(substates, int, 0444);
#endif

MODULE_AUTHOR("Len Brown <len.brown@intel.com>");
MODULE_DESCRIPTION("Cpuidle driver for Intel Hardware v" INTEL_IDLE_VERSION);
MODULE_LICENSE("GPL");
