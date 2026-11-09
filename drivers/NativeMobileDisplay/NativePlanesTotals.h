// SPDX-License-Identifier: BSD-2-Clause-Patent
// NativeLegacyDisplay's hardware-plane counters since boot, debug kernels only: kern.switch_planes_{flips,latched,maps,
// dropped}, which hw_test_app's display.planes reads around a video it plays. Included in the class body after
// NativePlanesDisplay.h. The pacing line's counters restart every stats window (noteStatsWindow); these add each closed
// window, and a read adds the open one's.

#if SWITCH_FRAME_TIMELINE
static constexpr unsigned kPlaneTotals = 4;
static inline uint32_t planeTotals[kPlaneTotals] = {};
static inline struct sysctl_oid planeTotalOids[kPlaneTotals] = {};

void planeTotalsWindow(uint32_t (&window)[kPlaneTotals]) const
{
    window[0] = stPlaneFlips;
    window[1] = stPlaneLatched;
    window[2] = stPlaneMaps;
    window[3] = stPlaneDropped;
}

void planeTotalsClose() const
{
    uint32_t window[kPlaneTotals];
    planeTotalsWindow(window);
    for (unsigned i = 0; i < kPlaneTotals; ++i)
        planeTotals[i] += window[i];
}

static int planeTotalSysctl(struct sysctl_oid *, void *, int which, struct sysctl_req *req)
{
    uint32_t open[kPlaneTotals] = {};
    if (const NativeLegacyDisplay *display = __atomic_load_n(&primaryDisplay, __ATOMIC_ACQUIRE))
        display->planeTotalsWindow(open);
    const unsigned i = static_cast<unsigned>(which) % kPlaneTotals;
    const int value = static_cast<int>(planeTotals[i] + open[i]);
    return SYSCTL_OUT(req, &value, sizeof(value));
}

static void planeTotalsRegister()
{
    static const char *const names[kPlaneTotals] = {"switch_planes_flips", "switch_planes_latched",
                                                    "switch_planes_maps", "switch_planes_dropped"};
    for (unsigned i = 0; i < kPlaneTotals; ++i) {
        if (planeTotalOids[i].oid_name)
            continue;
        switch_sysctl_oid(&planeTotalOids[i], &sysctl__kern_children, names[i],
                          CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED, planeTotalSysctl, "I",
                          "window B since boot: sets written, sets latched, slot mappings, detached layers not shown");
        planeTotalOids[i].oid_arg2 = static_cast<int>(i);
        sysctl_register_oid(&planeTotalOids[i]);
    }
}
#else
void planeTotalsClose() const
{}

static void planeTotalsRegister()
{}
#endif
