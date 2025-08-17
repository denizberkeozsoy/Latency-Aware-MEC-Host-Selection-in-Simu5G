#ifndef __SIMU5G_LATENCYAWARESELECTIONBASED_H_
#define __SIMU5G_LATENCYAWARESELECTIONBASED_H_

#include "nodes/mec/MECOrchestrator/mecHostSelectionPolicies/SelectionPolicyBase.h"
#include <vector>

namespace simu5g {

/**
 * LatencyAwareSelectionBased
 *
 * Implements a moderate-case MEC host selection policy that considers:
 *  - Network latency (from .ned parameters)
 *  - CPU utilization
 *  - Throughput (from NIC)
 *  - Queue length (from NIC queue)
 *
 * This policy performs weighted scoring using these runtime metrics to choose
 * the most suitable MEC host under typical, non-extreme conditions.
 */
class LatencyAwareSelectionBased : public SelectionPolicyBase
{
  private:
    std::vector<cModule*> mecHosts;  // List of candidate MEC hosts

    // MODERATE-CASE METRIC HELPERS

    // Fetch the configured or measured latency of a MEC host
    double getHostLatency(cModule* host) const;

    // Fetch the current CPU utilization (0.0 - 1.0) from the host's VIM module
    double getHostCpuUtil(cModule* host) const;

    // Retrieve the total NIC throughput (tx + rx bitrate)
    double getHostThroughput(cModule* host) const;

    // Retrieve the NIC queue's maximum bit length (approximate congestion level)
    double getHostQueueLength(cModule* host) const;

  public:
    // Constructor accepting orchestrator pointer and list of MEC hosts
    LatencyAwareSelectionBased(MecOrchestrator* orchestrator, std::vector<cModule*> mecHosts);
    virtual ~LatencyAwareSelectionBased() {}

    // Selects the best host for app instantiation using multi-metric scoring
    cModule* findBestMecHost(const ApplicationDescriptor& appDesc) override;
};

} // namespace simu5g

#endif  // __SIMU5G_LATENCYAWARESELECTIONBASED_H_
