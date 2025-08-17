#ifndef __SIMU5G_LATENCYAWARESELECTIONBASED_H_
#define __SIMU5G_LATENCYAWARESELECTIONBASED_H_

#include "nodes/mec/MECOrchestrator/mecHostSelectionPolicies/SelectionPolicyBase.h"
#include <vector>

namespace simu5g {

/**
 * LatencyAwareSelectionBased
 *
 * Implements a worst-case-aware MEC host selection policy.
 * This policy overrides scoring logic to simulate degraded MEC performance:
 * - High artificial latency
 * - High CPU utilization
 * - Limited throughput or overloaded queues
 *
 * This allows testing orchestrator robustness under poor system conditions.
 */
class LatencyAwareSelectionBased : public SelectionPolicyBase
{
  private:
    std::vector<cModule*> mecHosts;  // Connected MEC host list

    // --- Helper methods to retrieve runtime conditions (worst-case biased) ---

    // Returns artificially high latency (or default fallback)
    double getHostLatency(cModule* host) const;

    // Returns high CPU utilization to simulate system stress
    double getHostCpuUtil(cModule* host) const;

    // Returns degraded or capped throughput
    double getHostThroughput(cModule* host) const;

    // Returns maximum queue size or load estimate
    double getHostQueueLength(cModule* host) const;

  public:
    // Constructor initializes with orchestrator context and MEC host list
    LatencyAwareSelectionBased(MecOrchestrator* orchestrator, std::vector<cModule*> mecHosts);

    virtual ~LatencyAwareSelectionBased() {}

    // Main selection method — selects intentionally worst (or least optimal) MEC host
    cModule* findBestMecHost(const ApplicationDescriptor& appDesc) override;
};

} // namespace simu5g

#endif  // __SIMU5G_LATENCYAWARESELECTIONBASED_H_
