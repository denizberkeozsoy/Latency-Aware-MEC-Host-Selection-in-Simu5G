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

// Fetch latency for a given host (configured values or fallback)
double LatencyAwareSelectionBased::getHostLatency(cModule* host) const
{
    if (host->getName() == std::string("mecHost1"))
        return mecOrchestrator_->par("latencyHost1").doubleValue();
    else if (host->getName() == std::string("mecHost2"))
        return mecOrchestrator_->par("latencyHost2").doubleValue();

    return 0.05;  // fallback latency in seconds
}

// Fetch CPU utilization from VIM submodule
double LatencyAwareSelectionBased::getHostCpuUtil(cModule* host) const
{
    cModule* vimSubmod = host->getSubmodule("vim");
    if (!vimSubmod)
        return 1.0;  // assume fully utilized if VIM is missing

    auto* vim = check_and_cast<VirtualisationInfrastructureManager*>(vimSubmod);
    return vim->getUsedCpu();  // value between 0.0 and 1.0
}

// Retrieve total throughput (tx + rx) from NIC submodule
double LatencyAwareSelectionBased::getHostThroughput(cModule* host) const
{
    cModule* nic = host->getSubmodule("nic");
    if (!nic)
        return 0.0;

    double txBitrate = nic->par("txBitrate").doubleValue();
    double rxBitrate = nic->par("rxBitrate").doubleValue();

    return txBitrate + rxBitrate;
}

// Get max bit queue length from NIC's queue submodule
double LatencyAwareSelectionBased::getHostQueueLength(cModule* host) const
{
    cModule* queue = nullptr;
    cModule* nic = host->getSubmodule("nic");

    if (nic)
        queue = nic->getSubmodule("queue");

    if (!queue)
        return 0.0;

    return queue->par("maxBitLength").doubleValue();
}

// Core logic to select the MEC host with the worst-case scoring behavior
cModule* LatencyAwareSelectionBased::findBestMecHost(const ApplicationDescriptor& appDesc)
{
    EV_WARN << "\n[LatencyAware-WORST] Selecting MEC host with degraded scoring and penalty injection\n";


    double bestScore = std::numeric_limits<double>::max();
    cModule* bestHost = nullptr;

    // Read selection policy weights from NED parameters
    double wLatency = mecOrchestrator_->par("latencyWeight").doubleValue();
    double wCpu = mecOrchestrator_->par("cpuWeight").doubleValue();
    double wThroughput = mecOrchestrator_->par("throughputWeight").doubleValue();
    double wQueueLen = mecOrchestrator_->par("queueLenWeight").doubleValue();

    // First pass to determine max values for normalization
    double maxLatency = 0.0, maxThroughput = 0.0, maxQueueLen = 0.0;

    for (auto host : mecHosts) {
        maxLatency = std::max(maxLatency, getHostLatency(host));
        maxThroughput = std::max(maxThroughput, getHostThroughput(host));
        maxQueueLen = std::max(maxQueueLen, getHostQueueLength(host));
    }

    // Avoid division by zero
    if (maxLatency == 0) maxLatency = 1.0;
    if (maxThroughput == 0) maxThroughput = 1.0;
    if (maxQueueLen == 0) maxQueueLen = 1.0;

    // Evaluate and score each candidate MEC host
    for (auto host : mecHosts) {
        EV_INFO << "[LatencyAware] Checking host " << host->getName() << "\n";

        cModule* vimSubmod = host->getSubmodule("vim");
        if (!vimSubmod) {
            EV_WARN << "[LatencyAware] No VIM submodule in " << host->getName() << ", skipping.\n";
            continue;
        }

        auto* vim = check_and_cast<VirtualisationInfrastructureManager*>(vimSubmod);

        ResourceDescriptor resources = appDesc.getVirtualResources();
        if (!vim->isAllocable(resources.ram, resources.disk, resources.cpu)) {
            EV_INFO << "[LatencyAware] Insufficient resources on " << host->getName() << ", skipping.\n";
            continue;
        }

        // Collect metric values
        double latency = getHostLatency(host);
        double cpuUtil = getHostCpuUtil(host);
        double throughput = getHostThroughput(host);
        double queueLen = getHostQueueLength(host);

        // Normalize metrics to [0,1]
        double normLatency = latency / maxLatency;
        double normCpu = cpuUtil;
        double normThroughput = throughput / maxThroughput;
        double normQueueLen = queueLen / maxQueueLen;

        // Compute selection score (lower is better)
        double score = wLatency * normLatency
                     + wCpu * normCpu
                     + wQueueLen * normQueueLen
                     - wThroughput * normThroughput;

        // Inject artificial noise to simulate metric uncertainty and degrade score
        double penaltyFactor = 1.5 + 0.5 * getEnvir()->getRNG(0)->doubleRand();
        score *= penaltyFactor;

        EV_INFO << "[LatencyAware] Host " << host->getName()
                << " score components: latency=" << normLatency
                << ", cpu=" << normCpu
                << ", queueLen=" << normQueueLen
                << ", throughput=" << normThroughput
                << " => score=" << score << "\n";

        // Update best host if current score is lower
        if (score < bestScore) {
            bestScore = score;
            bestHost = host;
            mecOrchestrator_->bestLatency = latency;
        }
    }

    if (!bestHost) {
        EV_ERROR << "[LatencyAware] No suitable MEC host found\n";
    } else {
        EV_INFO << "[LatencyAware] Selected host: " << bestHost->getName()
                << " with score " << bestScore << "\n";
    }

    return bestHost;
}

} // namespace simu5g
