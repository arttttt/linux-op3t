/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Cluster and system idle for Qualcomm msm8996 - see cpuidle-msm8996.c.
 */

#ifndef __CPUIDLE_MSM8996_H
#define __CPUIDLE_MSM8996_H

struct cpuidle_driver;

#ifdef CONFIG_ARM_MSM8996_CPUIDLE
/*
 * Take over the deepest CPU idle state so that the last core out of a cluster
 * composes the cluster's - or the whole system's - state onto its request.
 * @cpu_state is that state's own suspend parameter, which the composed value is
 * built on top of.
 */
void msm8996_cpuidle_attach(struct cpuidle_driver *drv, unsigned int state_count,
			    u32 cpu_state);
#else
static inline void msm8996_cpuidle_attach(struct cpuidle_driver *drv,
					  unsigned int state_count, u32 cpu_state)
{
}
#endif

#endif /* __CPUIDLE_MSM8996_H */
