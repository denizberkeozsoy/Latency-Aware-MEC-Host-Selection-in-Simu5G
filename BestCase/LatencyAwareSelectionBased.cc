#include "nodes/mec/MECOrchestrator/mecHostSelectionPolicies/LatencyAwareSelectionBased.h"
#include "nodes/mec/VirtualisationInfrastructureManager/VirtualisationInfrastructureManager.h"
#include "inet/common/INETUtils.h"
#include "omnetpp.h"

using namespace omnetpp;

namespace simu5g {

LatencyAwareSelectionBased::LatencyAwareSelectionBased(MecOrchestrator* orchestrator, std::vector<cModule*> mecHosts)
    : SelectionPolicyBase(orchestrator)
{
    this->mecOrchestrator_ = orchestrator;
    this->mecHosts = std::move(mecHosts);
}

// Returns a constant fallback latency value (best-case mock: fast response)
double LatencyAwareSelectionBased::getHostLatency(cModule* host) const
{
    return 0.25; // seconds
}

// Returns a fixed high CPU usage (best-case does not consider CPU variability)
double LatencyAwareSelectionBased::getHostCpuUtil(cModule* host) const
{
    return 0.98; // nearly full usage, fixed
}

// Retrieves combined Tx/Rx throughput if NIC module exists
double LatencyAwareSelectionBased::getHostThroughput(cModule* host) const
{
    cModule* nic = host->getSubmodule("nic");
    if (!nic)
        return 0.0;

    // Assume NIC provides txBitrate and rxBitrate parameters (in bits/sec)
    double txBitrate = nic->par("txBitrate").doubleValue();
    double rxBitrate = nic->par("rxBitrate").doubleValue();
    return txBitrate + rxBitrate;
}

// Reads queue length from NIC’s internal queue module, if available
double LatencyAwareSelectionBased::getHostQueueLength(cModule* host) const
{
    cModule* nic = host->getSubmodule("nic");
    cModule* queue = nic ? nic->getSubmodule("queue") : nullptr;

    if (!queue)
        return 0.0;

    return queue->par("maxBitLength").doubleValue();
}

// Selects mecHost2 explicitly, simulating a consistent best-case decision
cModule* LatencyAwareSelectionBased::findBestMecHost(const ApplicationDescriptor& appDesc)
{
    mecOrchestrator_->bestLatency = SimTime(0.5, SIMTIME_S);  // Fixed 500ms latency
    EV_INFO << "[BEST-CASE] Selecting optimal MEC host: mecHost2\n";

    for (auto host : mecHosts) {
        if (host->getName() == std::string("mecHost2"))
            return host;
    }

    return nullptr;
}

} // namespace simu5g
