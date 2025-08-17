//
//                  Simu5G
//
// Authors: Giovanni Nardini, Giovanni Stea, Antonio Virdis (University of Pisa)
//
// This file is part of a software released under the license included in file
// "license.pdf". Please read LICENSE and README files before using it.
// The above files and the present reference are part of the software itself,
// and cannot be removed from it.
//

#include "nodes/mec/MECOrchestrator/MecOrchestrator.h"

#include "nodes/mec/MECPlatformManager/MecPlatformManager.h"
#include "nodes/mec/VirtualisationInfrastructureManager/VirtualisationInfrastructureManager.h"
#include "nodes/mec/MECPlatform/ServiceRegistry/ServiceRegistry.h"
#include "apps/mec/MecApps/MultiUEMECApp.h"

#include "nodes/mec/MECOrchestrator/MECOMessages/MECOrchestratorMessages_m.h"
#include "nodes/mec/UALCMP/UALCMPMessages/UALCMPMessages_m.h"
#include "nodes/mec/UALCMP/UALCMPMessages/UALCMPMessages_types.h"
#include "nodes/mec/UALCMP/UALCMPMessages/CreateContextAppMessage.h"
#include "nodes/mec/UALCMP/UALCMPMessages/CreateContextAppAckMessage.h"

#include "nodes/mec/MECOrchestrator/mecHostSelectionPolicies/MecServiceSelectionBased.h"
#include "nodes/mec/MECOrchestrator/mecHostSelectionPolicies/AvailableResourcesSelectionBased.h"
#include "nodes/mec/MECOrchestrator/mecHostSelectionPolicies/MecHostSelectionBased.h"
#include "nodes/mec/MECOrchestrator/mecHostSelectionPolicies/LatencyAwareSelectionBased.h"

#include <iostream>  // For emulation debugging output

namespace simu5g {

Define_Module(MecOrchestrator);

void MecOrchestrator::initialize(int stage) {
    cSimpleModule::initialize(stage);
    if (stage != inet::INITSTAGE_LOCAL)
        return;

    EV << "MecOrchestrator::initialize - stage " << stage << endl;

    binder_.reference(this, "binderModule", true);

    // Initialize selection policy
    const char *selectionPolicyPar = par("selectionPolicy");
    if (!strcmp(selectionPolicyPar, "MecServiceBased"))
        mecHostSelectionPolicy_ = new MecServiceSelectionBased(this);
    else if (!strcmp(selectionPolicyPar, "AvailableResourcesBased"))
        mecHostSelectionPolicy_ = new AvailableResourcesSelectionBased(this);
    else if (!strcmp(selectionPolicyPar, "MecHostBased"))
        mecHostSelectionPolicy_ = new MecHostSelectionBased(this, par("mecHostIndex"));
    else if (!strcmp(selectionPolicyPar, "LatencyAwareBased"))
        mecHostSelectionPolicy_ = new LatencyAwareSelectionBased(this, mecHosts);
    else
        throw cRuntimeError("Selection policy '%s' not found!", selectionPolicyPar);

    // Initialize time parameters
    onboardingTime = par("onboardingTime").doubleValue();
    instantiationTime = par("instantiationTime").doubleValue();
    terminationTime = par("terminationTime").doubleValue();

    getConnectedMecHosts();
    onboardApplicationPackages();
}

void MecOrchestrator::handleMessage(cMessage *msg) {
    if (msg->isSelfMessage()) {
        if (!strcmp(msg->getName(), "MECOrchestratorMessage")) {
            auto *meoMsg = check_and_cast<MECOrchestratorMessage *>(msg);

            if (!strcmp(meoMsg->getType(), CREATE_CONTEXT_APP))
                sendCreateAppContextAck(meoMsg->getSuccess(), meoMsg->getRequestId(), meoMsg->getContextId());
            else if (!strcmp(meoMsg->getType(), DELETE_CONTEXT_APP))
                sendDeleteAppContextAck(meoMsg->getSuccess(), meoMsg->getRequestId(), meoMsg->getContextId());
        }
    } else if (msg->arrivedOn("fromUALCMP")) {
        handleUALCMPMessage(msg);
    }

    delete msg;
}

void MecOrchestrator::handleUALCMPMessage(cMessage *msg) {
    auto *lcmMsg = check_and_cast<UALCMPMessage *>(msg);

    if (!strcmp(lcmMsg->getType(), CREATE_CONTEXT_APP))
        startMECApp(lcmMsg);
    else if (!strcmp(lcmMsg->getType(), DELETE_CONTEXT_APP))
        stopMECApp(lcmMsg);
}

void MecOrchestrator::startMECApp(UALCMPMessage *msg)
{
    CreateContextAppMessage *contAppMsg = check_and_cast<CreateContextAppMessage *>(msg);
    unsigned int requestSno = msg->getRequestId();
    int contextId = contextIdCounter;

    // Store the start time of the application context for delay measurement
    contextStartTimes[contextIdCounter] = simTime();

    EV << "MecOrchestrator::createMeApp - processing... request id: " << requestSno << endl;

    // Retrieve UE App ID from message
    int ueAppID = atoi(contAppMsg->getDevAppId());

    // Check if the MEC app is already running for this UE
    for (const auto& contextApp : meAppMap) {
        if (contextApp.second.mecUeAppID == ueAppID &&
            contextApp.second.appDId == contAppMsg->getAppDId()) {

            EV << "MecOrchestrator::startMECApp - WARNING: App already running on host "
               << contextApp.second.mecHost->getName() << endl;

            // Acknowledge app already running
            sendCreateAppContextAck(true, requestSno, contextApp.first);

            // If the app supports multiple UEs, register this new one
            if (auto* existingMECApp = dynamic_cast<MultiUEMECApp*>(contextApp.second.reference)) {
                UE_MEC_CLIENT newUE;
                newUE.address = inet::L3Address(contAppMsg->getUeIpAddress());
                newUE.port = -1;  // Port not known at this stage
                existingMECApp->addNewUE(newUE);
            }

            return; // No need to start a new instance
        }
    }

    std::string appDid;
    double processingTime = 0.0;

    // Onboard application descriptor if needed
    if (!contAppMsg->getOnboarded()) {
        EV << "MecOrchestrator::startMECApp - onboarding app from: "
           << contAppMsg->getAppPackagePath() << endl;

        const ApplicationDescriptor& appDesc = onboardApplicationPackage(contAppMsg->getAppPackagePath());
        appDid = appDesc.getAppDId();
        processingTime += onboardingTime;
    } else {
        appDid = contAppMsg->getAppDId();
    }

    // Retrieve descriptor
    auto it = mecApplicationDescriptors_.find(appDid);
    if (it == mecApplicationDescriptors_.end()) {
        EV << "MecOrchestrator::startMECApp - App package with AppDId ["
           << appDid << "] not onboarded." << endl;

        sendCreateAppContextAck(false, requestSno);
        return;
    }

    const ApplicationDescriptor& desc = it->second;

    // Select the best MEC host using the configured policy
    cModule *bestHost = mecHostSelectionPolicy_->findBestMecHost(desc);

    if (bestHost != nullptr) {
        processingTime += 0.0; // No artificial delay in best-case

        bestLatency = computeLatencyForHost(bestHost);

        // Prepare message to create MEC application
        CreateAppMessage *createAppMsg = new CreateAppMessage();
        createAppMsg->setUeAppID(ueAppID);
        createAppMsg->setMEModuleName(desc.getAppName().c_str());
        createAppMsg->setMEModuleType(desc.getAppProvider().c_str());
        createAppMsg->setRequiredCpu(desc.getVirtualResources().cpu);
        createAppMsg->setRequiredRam(desc.getVirtualResources().ram);
        createAppMsg->setRequiredDisk(desc.getVirtualResources().disk);
        createAppMsg->setContextId(contextIdCounter);
        createAppMsg->setRequiredService(
            desc.getOmnetppServiceRequired().empty() ? "NULL" : desc.getOmnetppServiceRequired().c_str()
        );

        // Build new app mapping entry
        mecAppMapEntry newMecApp;
        newMecApp.appDId = appDid;
        newMecApp.mecUeAppID = ueAppID;
        newMecApp.mecHost = bestHost;
        newMecApp.ueAddress = inet::L3AddressResolver().resolve(contAppMsg->getUeIpAddress());
        newMecApp.vim = bestHost->getSubmodule("vim");
        newMecApp.mecpm = bestHost->getSubmodule("mecPlatformManager");
        newMecApp.mecAppName = desc.getAppName().c_str();

        MecPlatformManager *mecpm = check_and_cast<MecPlatformManager *>(newMecApp.mecpm);
        MecAppInstanceInfo *appInfo = nullptr;

        // Launch the application (emulated or simulated)
        if (desc.isMecAppEmulated()) {
            EV << "MecOrchestrator::startMECApp - App is emulated." << endl;
            mecpm->instantiateEmulatedMEApp(createAppMsg);

            appInfo = new MecAppInstanceInfo();
            appInfo->status = true;
            appInfo->endPoint.addr = inet::L3Address(desc.getExternalAddress().c_str());
            appInfo->endPoint.port = desc.getExternalPort();
            appInfo->instanceId = "emulated_" + desc.getAppName();
            newMecApp.isEmulated = true;

            // Register MEC app in GTP Binder
            inet::L3Address gtpAddress =
                inet::L3AddressResolver().resolve(newMecApp.mecHost->getSubmodule("upf_mec")->getFullPath().c_str());

            binder_->registerMecHostUpfAddress(appInfo->endPoint.addr, gtpAddress);
        } else {
            appInfo = mecpm->instantiateMEApp(createAppMsg);
            newMecApp.isEmulated = false;
        }

        if (!appInfo->status) {
            EV << "MecOrchestrator::startMECApp - ERROR: App instantiation failed." << endl;

            MECOrchestratorMessage *failMsg = new MECOrchestratorMessage("MECOrchestratorMessage");
            failMsg->setType(CREATE_CONTEXT_APP);
            failMsg->setRequestId(requestSno);
            failMsg->setSuccess(false);

            processingTime += instantiationTime;
            scheduleAt(simTime() + processingTime, failMsg);
            delete appInfo;
            return;
        }

        // App successfully created, populate map and send ACK
        EV << "MecOrchestrator::startMECApp - App instantiated successfully with ID: "
           << appInfo->instanceId << " on host: " << newMecApp.mecHost->getName()
           << " at " << appInfo->endPoint.addr.str() << ":" << appInfo->endPoint.port << endl;

        MECOrchestratorMessage *ackMsg = new MECOrchestratorMessage("MECOrchestratorMessage");
        ackMsg->setContextId(contextIdCounter);
        ackMsg->setType(CREATE_CONTEXT_APP);
        ackMsg->setRequestId(requestSno);
        ackMsg->setSuccess(true);

        newMecApp.mecAppAddress = appInfo->endPoint.addr;
        newMecApp.mecAppPort = appInfo->endPoint.port;
        newMecApp.mecAppInstanceId = appInfo->instanceId;
        newMecApp.contextId = contextIdCounter;
        newMecApp.reference = appInfo->reference;

        meAppMap[contextIdCounter] = newMecApp;

        processingTime += instantiationTime;
        scheduleAt(simTime() + processingTime, ackMsg);

        delete appInfo;
    }
    else {
        // No suitable host found (unexpected in best-case, but fallback needed)
        EV << "MecOrchestrator::startMECApp - ERROR: No suitable MEC host selected." << endl;

        MECOrchestratorMessage *failMsg = new MECOrchestratorMessage("MECOrchestratorMessage");
        failMsg->setType(CREATE_CONTEXT_APP);
        failMsg->setRequestId(requestSno);
        failMsg->setSuccess(false);

        processingTime += instantiationTime / 2;
        scheduleAt(simTime() + processingTime, failMsg);
        bestLatency = SIMTIME_ZERO;
    }
}


void MecOrchestrator::stopMECApp(UALCMPMessage *msg)
{
    EV << "MecOrchestrator::stopMECApp - processing..." << endl;

    // Ensure correct message type
    auto *contAppMsg = dynamic_cast<DeleteContextAppMessage *>(msg);
    if (!contAppMsg)
        throw cRuntimeError("Invalid cast to DeleteContextAppMessage");

    int contextId = contAppMsg->getContextId();
    EV << "MecOrchestrator::stopMECApp - processing contextId: " << contextId << endl;

    // Check whether the MEC application context exists
    if (meAppMap.empty() || meAppMap.find(contextId) == meAppMap.end()) {
        EV << "MecOrchestrator::stopMECApp - WARNING: MEC App with contextId [" << contextId << "] not found!" << endl;
        sendDeleteAppContextAck(false, contAppMsg->getRequestId(), contextId);
        return;
    }

    // Retrieve platform manager and prepare the delete message
    MecPlatformManager *mecpm = check_and_cast<MecPlatformManager *>(meAppMap[contextId].mecpm);
    DeleteAppMessage *deleteAppMsg = new DeleteAppMessage();
    deleteAppMsg->setUeAppID(meAppMap[contextId].mecUeAppID);

    // Terminate application based on its type (emulated or simulated)
    bool isTerminated;
    if (meAppMap[contextId].isEmulated) {
        isTerminated = mecpm->terminateEmulatedMEApp(deleteAppMsg);
        std::cout << "terminateEmulatedMEApp with result: " << isTerminated << std::endl;
    } else {
        isTerminated = mecpm->terminateMEApp(deleteAppMsg);
    }

    // Create and configure the response message
    MECOrchestratorMessage *mecoMsg = new MECOrchestratorMessage("MECOrchestratorMessage");
    mecoMsg->setType(DELETE_CONTEXT_APP);
    mecoMsg->setRequestId(contAppMsg->getRequestId());
    mecoMsg->setContextId(contextId);

    if (isTerminated) {
        EV << "MecOrchestrator::stopMECApp - MEC App [" << meAppMap[contextId].mecUeAppID << "] terminated successfully" << endl;
        meAppMap.erase(contextId);
        mecoMsg->setSuccess(true);
    } else {
        EV << "MecOrchestrator::stopMECApp - Failed to terminate MEC App [" << meAppMap[contextId].mecUeAppID << "]" << endl;
        mecoMsg->setSuccess(false);
    }

    // Schedule the final response after simulated termination time
    double processingTime = terminationTime;
    scheduleAt(simTime() + processingTime, mecoMsg);
}


void MecOrchestrator::sendDeleteAppContextAck(bool result, unsigned int requestSno, int contextId)
{
    EV << "MecOrchestrator::sendDeleteAppContextAck - result: " << result
       << " | requestId: " << requestSno << " | contextId: " << contextId << endl;

    DeleteContextAppAckMessage *ack = new DeleteContextAppAckMessage();
    ack->setType(ACK_DELETE_CONTEXT_APP);
    ack->setRequestId(requestSno);
    ack->setSuccess(result);

    send(ack, "toUALCMP");
}

void MecOrchestrator::sendCreateAppContextAck(bool result, unsigned int requestSno, int contextId)
{
    EV << "MecOrchestrator::sendCreateAppContextAck - result: " << result
       << " | requestId: " << requestSno << " | contextId: " << contextId << endl;

    CreateContextAppAckMessage *ack = new CreateContextAppAckMessage();
    ack->setType(ACK_CREATE_CONTEXT_APP);

    if (result) {
        // Validate that the contextId exists in the MEC app map
        if (meAppMap.empty() || meAppMap.find(contextId) == meAppMap.end()) {
            EV << "MecOrchestrator::sendCreateAppContextAck - ERROR: meApp[" << contextId << "] does not exist!" << endl;
            return;
        }

        mecAppMapEntry &mecAppStatus = meAppMap[contextId];

        ack->setSuccess(true);
        ack->setContextId(contextId);
        ack->setAppInstanceId(mecAppStatus.mecAppInstanceId.c_str());
        ack->setRequestId(requestSno);

        // Construct the URI (e.g., IP:Port) of the instantiated MEC app
        std::stringstream uri;
        uri << mecAppStatus.mecAppAddress.str() << ":" << mecAppStatus.mecAppPort;
        ack->setAppInstanceUri(uri.str().c_str());
    }
    else {
        ack->setRequestId(requestSno);
        ack->setSuccess(false);
    }

    send(ack, "toUALCMP");
}


cModule* MecOrchestrator::findBestMecHost(const ApplicationDescriptor& appDesc)
{
    EV << "MecOrchestrator::findBestMecHost - using policy: " << par("selectionPolicy").str() << endl;

    std::string policy = par("selectionPolicy").stdstringValue();

    // ==============================
    // Latency-Based Selection Policy
    // ==============================
    if (policy == "LatencyBased") {
        EV << "MecOrchestrator::findBestMecHost - Applying Latency-Based policy..." << endl;
        getSimulation()->getActiveEnvir()->alert("✅ Latency-Based policy is ACTIVE!");

        bestLatency = SIMTIME_MAX;
        double bestScore = std::numeric_limits<double>::max();
        cModule* bestHost = nullptr;

        for (auto mecHost : mecHosts) {
            cModule* vimSubmod = mecHost->getSubmodule("vim");
            if (!vimSubmod) {
                throw cRuntimeError("Submodule 'vim' not found in MEC host: %s", mecHost->getFullPath().c_str());
            }

            VirtualisationInfrastructureManager* vim = check_and_cast<VirtualisationInfrastructureManager*>(vimSubmod);
            ResourceDescriptor resources = appDesc.getVirtualResources();

            if (!vim->isAllocable(resources.ram, resources.disk, resources.cpu)) {
                EV << "  MEC host [" << mecHost->getName() << "] doesn't have enough resources.\n";
                continue;
            }

            // Estimate latency based on host name
            simtime_t latency;
            std::string hostName = mecHost->getName();
            if (hostName.find("mecHost1") != std::string::npos)
                latency = SimTime(0.005, SIMTIME_S);  // 5ms
            else
                latency = SimTime(0.05, SIMTIME_S);   // 50ms (default)

            // Bandwidth and load checks
            double availableBandwidth = vim->getAvailableBandwidth();
            if (availableBandwidth < 1e-6)
                availableBandwidth = 1e-6;  // prevent division by zero

            double loadFactor = vim->getCurrentCpuLoad();

            // Score = latency * (1 + weighted load)
            double cpuWeight = 0.5;
            double clippedLoad = std::min(loadFactor, 0.9);
            double score = latency.dbl() * (1.0 + cpuWeight * clippedLoad);

            EV << "  Host [" << hostName << "] → Latency: " << latency
               << ", Load: " << loadFactor
               << ", Available BW: " << availableBandwidth
               << ", Score: " << score << endl;

            if (score < bestScore) {
                bestScore = score;
                bestLatency = latency;
                bestHost = mecHost;
            }
        }

        if (bestHost) {
            EV << "  Selected host: " << bestHost->getName()
               << " with latency: " << bestLatency << "s\n";
        } else {
            EV << "  No suitable MEC host found.\n";
        }

        return bestHost;
    }

    // ===========================================
    // Default Behavior: Service-Based Selection
    // ===========================================
    cModule* bestHost = nullptr;

    for (auto mecHost : mecHosts) {
        cModule* vimSubmod = mecHost->getSubmodule("vim");
        if (!vimSubmod) {
            throw cRuntimeError("Submodule 'vim' not found in MEC host: %s", mecHost->getFullPath().c_str());
        }

        VirtualisationInfrastructureManager* vim = check_and_cast<VirtualisationInfrastructureManager*>(vimSubmod);
        ResourceDescriptor resources = appDesc.getVirtualResources();

        if (!vim->isAllocable(resources.ram, resources.disk, resources.cpu)) {
            EV << "MecOrchestrator::findBestMecHost - MEC host ["
               << mecHost->getName() << "] has not got enough resources. Searching again..." << endl;
            continue;
        }

        cModule* mecpmSubmod = mecHost->getSubmodule("mecPlatformManager");
        if (!mecpmSubmod) {
            throw cRuntimeError("Submodule 'mecPlatformManager' not found in MEC host: %s", mecHost->getFullPath().c_str());
        }

        MecPlatformManager* mecpm = check_and_cast<MecPlatformManager*>(mecpmSubmod);
        auto mecServices = mecpm->getAvailableMecServices();

        std::string serviceName;
        if (!appDesc.getAppServicesRequired().empty()) {
            serviceName = appDesc.getAppServicesRequired()[0];
        } else {
            break;
        }

        for (const auto& service : *mecServices) {
            if (serviceName == service.getName()) {
                bestHost = mecHost;
                break;
            }
        }

        if (bestHost) break;  // stop loop if a suitable host is found
    }

    if (bestHost)
        EV << "MecOrchestrator::findBestMecHost - best MEC host: " << bestHost->getName() << endl;
    else
        EV << "MecOrchestrator::findBestMecHost - no MEC host found" << endl;

    return bestHost;
}



void MecOrchestrator::getConnectedMecHosts()
{
    EV << "MecOrchestrator::getConnectedMecHosts - mecHostList: " << par("mecHostList").str() << endl;

    // Retrieve the list of MEC host paths from the NED parameter
    auto mecHostList = check_and_cast<cValueArray *>(par("mecHostList").objectValue());

    if (mecHostList->size() > 0) {
        for (int i = 0; i < mecHostList->size(); ++i) {
            const char *token = mecHostList->get(i).stringValue();
            EV << "  → Found MEC host path: " << token << endl;

            cModule *mecHostModule = getSimulation()->getModuleByPath(token);
            if (mecHostModule)
                mecHosts.push_back(mecHostModule);
            else
                EV << "  ⚠️ Invalid MEC host path: " << token << " (skipped)" << endl;
        }
    } else {
        EV << "⚠️ MecOrchestrator::getConnectedMecHosts - No MEC hosts defined in parameter." << endl;
    }
}


const ApplicationDescriptor& MecOrchestrator::onboardApplicationPackage(const char *fileName)
{
    EV << "MecOrchestrator::onboardApplicationPackage - Onboarding app package: " << fileName << endl;

    // Create app descriptor from the given file
    ApplicationDescriptor appDesc(fileName);
    std::string appDId = appDesc.getAppDId();

    // Check if the descriptor is already known
    if (mecApplicationDescriptors_.count(appDId)) {
        EV << "  ⚠️ App descriptor [" << appDId << "] already onboarded.\n";
    } else {
        // Register the new app descriptor
        mecApplicationDescriptors_[appDId] = appDesc;
        EV << "  ✅ Successfully onboarded [" << appDId << "]\n";
    }

    return mecApplicationDescriptors_[appDId];
}


void MecOrchestrator::registerMecService(ServiceDescriptor& serviceDescriptor) const
{
    EV << "MecOrchestrator::registerMecService - Registering service: " << serviceDescriptor.name << endl;

    for (auto mecHost : mecHosts) {
        cModule *serviceRegistryMod = mecHost->getSubmodule("mecPlatform")->getSubmodule("serviceRegistry");

        if (serviceRegistryMod) {
            ServiceRegistry *registry = check_and_cast<ServiceRegistry *>(serviceRegistryMod);

            EV << "  → Registering service [" << serviceDescriptor.name
               << "] on host [" << mecHost->getName() << "]" << endl;

            registry->registerMecService(serviceDescriptor);
        } else {
            EV << "  ⚠️ ServiceRegistry not found in host: " << mecHost->getName() << endl;
        }
    }
}


void MecOrchestrator::onboardApplicationPackages()
{
    EV << "MecOrchestrator::onboardApplicationPackages - Loading application packages..." << endl;

    // Retrieve application package list from parameter
    auto mecApplicationPackageList = check_and_cast<cValueArray *>(par("mecApplicationPackageList").objectValue());

    if (mecApplicationPackageList->size() > 0) {
        for (int i = 0; i < mecApplicationPackageList->size(); ++i) {
            const char *token = mecApplicationPackageList->get(i).stringValue();

            // Construct file path and onboard
            std::string filePath = std::string("ApplicationDescriptors/") + token + ".json";
            EV << "  → Onboarding package: " << filePath << endl;

            onboardApplicationPackage(filePath.c_str());
        }
    } else {
        EV << "⚠️ MecOrchestrator::onboardApplicationPackages - No application packages defined in parameter." << endl;
    }
}


const ApplicationDescriptor* MecOrchestrator::getApplicationDescriptorByAppName(const std::string& appName) const
{
    // Search through onboarded descriptors by name
    for (const auto& entry : mecApplicationDescriptors_) {
        if (entry.second.getAppName() == appName)
            return &(entry.second);
    }

    // App not found
    return nullptr;
}


simtime_t MecOrchestrator::computeLatencyForHost(cModule* mecHost)
{
    std::string hostName = mecHost->getName();

    // Use simple static latency mapping (can be replaced by dynamic measurements)
    if (hostName.find("mecHost1") != std::string::npos) {
        return SimTime(0.005, SIMTIME_S);  // 5 ms
    }
    else if (hostName.find("mecHost2") != std::string::npos) {
        return SimTime(0.05, SIMTIME_S);   // 50 ms
    }

    // Default case: higher fallback latency
    return SimTime(0.1, SIMTIME_S);        // 100 ms
}


} //namespace
