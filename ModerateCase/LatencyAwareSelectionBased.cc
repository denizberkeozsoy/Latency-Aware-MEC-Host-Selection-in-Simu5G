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

double LatencyAwareSelectionBased::getHostLatency(cModule* host) const
{
    // MODERATE-CASE: Fetch latency based on host name from .ned configuration
    if (host->getName() == std::string("mecHost1"))
        return mecOrchestrator_->par("latencyHost1").doubleValue();
    else if (host->getName() == std::string("mecHost2"))
        return mecOrchestrator_->par("latencyHost2").doubleValue();
    return 0.05; // Default fallback latency (50 ms)
}

double LatencyAwareSelectionBased::getHostCpuUtil(cModule* host) const
{
    // Retrieve CPU utilization from the VIM module (0.0 to 1.0)
    cModule* vimSubmod = host->getSubmodule("vim");
    if (!vimSubmod)
        return 1.0; // Assume 100% utilization if VIM is missing

    auto* vim = check_and_cast<VirtualisationInfrastructureManager*>(vimSubmod);
    return vim->getUsedCpu();
}

double LatencyAwareSelectionBased::getHostThroughput(cModule* host) const
{
    // Moderate-case metric: retrieve current NIC transmit + receive bitrate
    cModule* nic = host->getSubmodule("nic");
    if (!nic)
        return 0.0;

    double txBitrate = nic->par("txBitrate").doubleValue();
    double rxBitrate = nic->par("rxBitrate").doubleValue();
    return txBitrate + rxBitrate;
}

double LatencyAwareSelectionBased::getHostQueueLength(cModule* host) const
{
    // Retrieve maximum queue length (bit length) from NIC submodule
    cModule* queue = nullptr;
    cModule* nic = host->getSubmodule("nic");
    if (nic)
        queue = nic->getSubmodule("queue");

    if (!queue)
        return 0.0;

    return queue->par("maxBitLength").doubleValue();
}

cModule* LatencyAwareSelectionBased::findBestMecHost(const ApplicationDescriptor& appDesc)
{
    EV_INFO << "\n[LatencyAware] Finding best MEC host with enhanced metrics\n";

    double bestScore = std::numeric_limits<double>::max();
    cModule* bestHost = nullptr;

    // MODERATE-CASE: Use weighted metric combination for scoring
    double wLatency = mecOrchestrator_->par("latencyWeight").doubleValue();
    double wCpu = mecOrchestrator_->par("cpuWeight").doubleValue();
    double wThroughput = mecOrchestrator_->par("throughputWeight").doubleValue();
    double wQueueLen = mecOrchestrator_->par("queueLenWeight").doubleValue();

    // Determine max values for normalization
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

    // Evaluate all candidate MEC hosts
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

        // Normalize values
        double normLatency = latency / maxLatency;
        double normCpu = cpuUtil; // already 0..1
        double normThroughput = throughput / maxThroughput;
        double normQueueLen = queueLen / maxQueueLen;

        // MODERATE-CASE: Scoring formula with weighted components
        double score = wLatency * normLatency
                     + wCpu * normCpu
                     + wQueueLen * normQueueLen
                     - wThroughput * normThroughput;

        EV_INFO << "[LatencyAware] Host " << host->getName()
                << " score components: latency=" << normLatency
                << ", cpu=" << normCpu
                << ", queueLen=" << normQueueLen
                << ", throughput=" << normThroughput
                << " => score=" << score << "\n";

        if (score < bestScore) {
            bestScore = score;
            bestHost = host;
            mecOrchestrator_->bestLatency = latency;
        }
    }

    if (!bestHost) {
        EV_ERROR << "[LatencyAware] No suitable MEC host found\n";
    } else {
        EV_INFO << "[LatencyAware] Selected host: " << bestHost->getName() << " with score " << bestScore << "\n";
    }

    return bestHost;
}

} // namespace simu5g
