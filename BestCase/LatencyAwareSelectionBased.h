#ifndef __SIMU5G_LATENCYAWARESELECTIONBASED_H_
#define __SIMU5G_LATENCYAWARESELECTIONBASED_H_

#include "nodes/mec/MECOrchestrator/mecHostSelectionPolicies/SelectionPolicyBase.h"
#include <vector>

namespace simu5g {

//
// A MEC host selection policy that uses latency, CPU utilization,
// throughput, and queue metrics to choose the optimal MEC host.
//
class LatencyAwareSelectionBased : public SelectionPolicyBase
{
  private:
    std::vector<cModule*> mecHosts;  // List of candidate MEC hosts

    // Metric evaluation helpers for scoring MEC hosts
    double getHostLatency(cModule* host) const;
    double getHostCpuUtil(cModule* host) const;
    double getHostThroughput(cModule* host) const;
    double getHostQueueLength(cModule* host) const;

  public:
    // Constructor
    LatencyAwareSelectionBased(MecOrchestrator* orchestrator, std::vector<cModule*> mecHosts);

    // Destructor
    virtual ~LatencyAwareSelectionBased() {}

    // Core method for selecting the best MEC host
    cModule* findBestMecHost(const ApplicationDescriptor& appDesc) override;
};

} // namespace simu5g

#endif  // __SIMU5G_LATENCYAWARESELECTIONBASED_H_
