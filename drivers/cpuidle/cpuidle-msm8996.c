// SPDX-License-Identifier: GPL-2.0
/*
 * Cluster and system idle for Qualcomm msm8996.
 *
 * The generic PSCI driver reaches deeper-than-core idle through genpd: each CPU
 * gets a power domain, the domains nest, and the last CPU leaving a domain has
 * a composed state id handed to it. That works on firmware built for it. This
 * one is not: it wants a request that is already consistent, and it wants it
 * without the machinery genpd drags onto the idle path - runtime PM, a walk of
 * the domain hierarchy and its locks, all of it executed with interrupts off on
 * the way into a sleep state.
 *
 * The vendor kernel does the same job with a spinlock and a cpumask per
 * cluster: every core marks itself on the way in, the last one out composes its
 * cluster's state onto its own request, and if that was also the last cluster,
 * the system's state instead. This is that, on mainline APIs.
 *
 * The composed values are the vendor kernel's own, verified against its device
 * tree: affinity level in bits 25:24, the system mode in the third nibble, the
 * L2 mode in the second, the CPU mode in the first, and no power-down StateType
 * bit - this firmware refuses every request that carries one.
 */

#define pr_fmt(fmt) "cpuidle-msm8996: " fmt

#include <linux/cpu_pm.h>
#include <linux/cpuidle.h>
#include <linux/cpumask.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/percpu.h>
#include <linux/psci.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>

#include <asm/cputype.h>
#include <asm/smp_plat.h>

#include "cpuidle-msm8996.h"

#define MSM8996_MAX_CLUSTERS	4

struct msm8996_cluster {
	cpumask_t	child_cpus;
	cpumask_t	in_sync;
};

static struct msm8996_cluster msm8996_clusters[MSM8996_MAX_CLUSTERS];
static DEFINE_PER_CPU(struct msm8996_cluster *, msm8996_cpu_cluster);

/*
 * One lock for the whole topology rather than one per cluster: deciding that
 * every cluster is idle means reading all of them, and that has to be a single
 * consistent view. The vendor kernel gets the same effect by recursing into the
 * parent cluster's sync_lock.
 */
static DEFINE_RAW_SPINLOCK(msm8996_topo_lock);

/* The CPU-level suspend parameter, taken from the device tree at attach. */
static u32 msm8996_cpu_state;

/* affinity 1, L2 mode in the second nibble, CPU mode in the first */
static uint msm8996_cluster_gdhs = 0x01000034;
module_param(msm8996_cluster_gdhs, uint, 0644);
static uint msm8996_cluster_pc = 0x01000044;
module_param(msm8996_cluster_pc, uint, 0644);
/*
 * Both tiers are on by default, and both are writable at runtime so a level can
 * be taken out of the picture without a rebuild. Enabling them from the very
 * first idle was measured to be safe here: booting with both set on the kernel
 * command line comes up normally and stays up.
 */
static int msm8996_cluster_depth = 2;	/* 0 none, 1 retention, 2 power collapse */
module_param(msm8996_cluster_depth, int, 0644);

/*
 * Affinity level 2: both clusters down, so the resources they share - the L3
 * and the coherent fabric - go into retention. The vendor kernel enters this
 * hundreds of times a minute. Its deeper sibling, system power collapse
 * (0x02003444), is the one that carries "qcom,notify-rpm" and needs the wakeup
 * handed to the always-on MPM first; retention needs no such handshake, which
 * is why it can be done here and that one cannot, yet.
 */
static uint msm8996_system_ret = 0x02002344;
module_param(msm8996_system_ret, uint, 0644);

/*
 * System power collapse. The vendor kernel flags this level "qcom,notify-rpm"
 * and "qcom,is-reset", and does two things before entering it that retention
 * does not need: it tells the RPM the apps side is going down
 * (msm_rpm_enter_sleep) and it arms the always-on MPM with the next wakeup
 * (msm_mpm_enter_sleep), because with everything else powered off the MPM is
 * what is left to wake the SoC.
 *
 * Mainline has neither hook available here - the MPM driver only exposes
 * itself as a power domain, and there is no global "entering sleep" call to
 * the RPM at all. So this is offered separately from retention: if firmware
 * turns out to arrange the wakeup itself, it costs nothing, and if it does not
 * the machine simply will not come back and we will know the MPM work is
 * mandatory rather than assumed.
 */
static uint msm8996_system_fpc = 0x02003444;
module_param(msm8996_system_fpc, uint, 0644);

static int msm8996_system_depth = 2;	/* 0 none, 1 retention, 2 power collapse */
module_param(msm8996_system_depth, int, 0644);

static atomic_t msm8996_cluster_entries = ATOMIC_INIT(0);
static atomic_t msm8996_system_entries = ATOMIC_INIT(0);

static int msm8996_cluster_entries_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&msm8996_cluster_entries));
}

static const struct kernel_param_ops msm8996_cluster_entries_ops = {
	.get = msm8996_cluster_entries_get,
};
module_param_cb(cluster_entries, &msm8996_cluster_entries_ops, NULL, 0444);

static int msm8996_system_entries_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&msm8996_system_entries));
}

static const struct kernel_param_ops msm8996_system_entries_ops = {
	.get = msm8996_system_entries_get,
};
module_param_cb(system_entries, &msm8996_system_entries_ops, NULL, 0444);

/*
 * True from the moment an IPI is raised for a CPU until that CPU takes it;
 * kept by smp_cross_call()/do_handle_IPI() in arch/arm64/kernel/smp.c. A
 * cluster must not be collapsed with one in flight - the vendor kernel makes
 * the same refusal from cluster_configure() via is_IPI_pending().
 */
DECLARE_PER_CPU(bool, pending_ipi);

static bool msm8996_ipi_pending(const struct cpumask *mask)
{
	unsigned int cpu;

	for_each_cpu(cpu, mask)
		if (per_cpu(pending_ipi, cpu))
			return true;

	return false;
}

/* Caller holds msm8996_topo_lock. */
static bool msm8996_all_clusters_idle(void)
{
	int i;

	for (i = 0; i < MSM8996_MAX_CLUSTERS; i++) {
		struct msm8996_cluster *c = &msm8996_clusters[i];

		if (cpumask_empty(&c->child_cpus))
			continue;
		if (!cpumask_equal(&c->in_sync, &c->child_cpus))
			return false;
	}

	return true;
}

static __cpuidle int msm8996_enter_idle_state(struct cpuidle_device *dev,
					      struct cpuidle_driver *drv,
					      int idx)
{
	struct msm8996_cluster *cluster = this_cpu_read(msm8996_cpu_cluster);
	u32 state = msm8996_cpu_state;
	bool composed = false;
	bool system = false;
	int ret;

	if (cluster) {
		raw_spin_lock(&msm8996_topo_lock);
		cpumask_set_cpu(dev->cpu, &cluster->in_sync);

		/*
		 * Only the last core out of a cluster composes, and not while
		 * an IPI is on its way to one of them. If this is also the last
		 * core of the last cluster, the state that belongs here is the
		 * system one.
		 */
		if (msm8996_cluster_depth &&
		    cpumask_equal(&cluster->in_sync, &cluster->child_cpus) &&
		    !msm8996_ipi_pending(&cluster->child_cpus)) {
			if (msm8996_system_depth && msm8996_all_clusters_idle()) {
				state |= (msm8996_system_depth >= 2) ?
					  msm8996_system_fpc : msm8996_system_ret;
				system = true;
			} else {
				state |= (msm8996_cluster_depth >= 2) ?
					  msm8996_cluster_pc :
					  msm8996_cluster_gdhs;
			}
			composed = true;
		}
		raw_spin_unlock(&msm8996_topo_lock);
	}

	if (composed) {
		cpu_cluster_pm_enter();
		atomic_inc(system ? &msm8996_system_entries :
				    &msm8996_cluster_entries);
	}

	ret = psci_cpu_suspend_enter(state) ? -1 : idx;

	if (composed)
		cpu_cluster_pm_exit();

	if (cluster) {
		raw_spin_lock(&msm8996_topo_lock);
		cpumask_clear_cpu(dev->cpu, &cluster->in_sync);
		raw_spin_unlock(&msm8996_topo_lock);
	}

	return ret;
}

static void msm8996_topology_init(void)
{
	static bool done;
	unsigned int cpu;
	int i;

	if (done)
		return;

	for (i = 0; i < MSM8996_MAX_CLUSTERS; i++) {
		cpumask_clear(&msm8996_clusters[i].child_cpus);
		cpumask_clear(&msm8996_clusters[i].in_sync);
	}

	for_each_possible_cpu(cpu) {
		int aff1 = MPIDR_AFFINITY_LEVEL(cpu_logical_map(cpu), 1);

		if (aff1 >= MSM8996_MAX_CLUSTERS)
			continue;

		cpumask_set_cpu(cpu, &msm8996_clusters[aff1].child_cpus);
		per_cpu(msm8996_cpu_cluster, cpu) = &msm8996_clusters[aff1];
	}

	for (i = 0; i < MSM8996_MAX_CLUSTERS; i++)
		if (!cpumask_empty(&msm8996_clusters[i].child_cpus))
			pr_info("cluster %d: cpus %*pbl\n", i,
				cpumask_pr_args(&msm8996_clusters[i].child_cpus));

	done = true;
}

void msm8996_cpuidle_attach(struct cpuidle_driver *drv, unsigned int state_count,
			    u32 cpu_state)
{
	msm8996_topology_init();
	msm8996_cpu_state = cpu_state;
	drv->states[state_count - 1].enter = msm8996_enter_idle_state;
}
