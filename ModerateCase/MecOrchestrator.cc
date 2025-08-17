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

// Selection policy headers
#include "nodes/mec/MECOrchestrator/mecHostSelectionPolicies/MecServiceSelectionBased.h"
#include "nodes/mec/MECOrchestrator/mecHostSelectionPolicies/AvailableResourcesSelectionBased.h"
#include "nodes/mec/MECOrchestrator/mecHostSelectionPolicies/MecHostSelectionBased.h"
#include "nodes/mec/MECOrchestrator/mecHostSelectionPolicies/LatencyAwareSelectionBased.h"

// Debug utilities
#include <iostream>

namespace simu5g {

Define_Module(MecOrchestrator);
//-----------------------------------------------------------------------------
// Initialization
//-----------------------------------------------------------------------------
void MecOrchestrator::initialize(int stage)
{
    cSimpleModule::initialize(stage);

    // Only run initialization logic at the local stage
    if (stage != inet::INITSTAGE_LOCAL)
        return;

    EV << "MecOrchestrator::initialize - stage " << stage << endl;

    // Reference to global binder module
    binder_.reference(this, "binderModule", true);

    // Retrieve and apply the MEC host selection policy
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
        throw cRuntimeError("MecOrchestrator::initialize - Unknown selection policy: '%s'", selectionPolicyPar);

    // Load operational timing parameters
    onboardingTime = par("onboardingTime").doubleValue();
    instantiationTime = par("instantiationTime").doubleValue();
    terminationTime = par("terminationTime").doubleValue();

    // Initialize connected MEC hosts and onboard available application packages
    getConnectedMecHosts();
    onboardApplicationPackages();
}
//-----------------------------------------------------------------------------
// Message Dispatcher
//-----------------------------------------------------------------------------
void MecOrchestrator::handleMessage(cMessage *msg)
{
    if (msg->isSelfMessage()) {
        // Handle internally scheduled orchestration messages
        if (strcmp(msg->getName(), "MECOrchestratorMessage") == 0) {
            EV << "MecOrchestrator::handleMessage - internal event: " << msg->getName() << endl;

            auto *meoMsg = check_and_cast<MECOrchestratorMessage *>(msg);
            if (!strcmp(meoMsg->getType(), CREATE_CONTEXT_APP)) {
                // Completion of context creation
                if (meoMsg->getSuccess())
                    sendCreateAppContextAck(true, meoMsg->getRequestId(), meoMsg->getContextId());
                else
                    sendCreateAppContextAck(false, meoMsg->getRequestId());
            }
            else if (!strcmp(meoMsg->getType(), DELETE_CONTEXT_APP)) {
                // Completion of context deletion
                sendDeleteAppContextAck(meoMsg->getSuccess(), meoMsg->getRequestId(), meoMsg->getContextId());
            }
        }
    }
    else if (msg->arrivedOn("fromUALCMP")) {
        // Handle requests from the UALCMP module (LCM proxy)
        EV << "MecOrchestrator::handleMessage - received from UALCMP: " << msg->getName() << endl;
        handleUALCMPMessage(msg);
    }

    // Clean up
    delete msg;
}
//-----------------------------------------------------------------------------
// UALCMP Message Routing
//-----------------------------------------------------------------------------
void MecOrchestrator::handleUALCMPMessage(cMessage *msg)
{
    auto *lcmMsg = check_and_cast<UALCMPMessage *>(msg);

    // Process application context creation
    if (!strcmp(lcmMsg->getType(), CREATE_CONTEXT_APP)) {
        startMECApp(lcmMsg);  // Triggers selection and instantiation
    }
    // Process application context deletion
    else if (!strcmp(lcmMsg->getType(), DELETE_CONTEXT_APP)) {
        stopMECApp(lcmMsg);   // Triggers termination and cleanup
    }
}



void MecOrchestrator::startMECApp(UALCMPMessage *msg)
{
    // Cast and extract request parameters
    CreateContextAppMessage *contAppMsg = check_and_cast<CreateContextAppMessage *>(msg);
    unsigned int requestSno = msg->getRequestId();
    int contextId = contextIdCounter;
    contextStartTimes[contextIdCounter] = simTime();  // Store request time

    EV << "MecOrchestrator::createMeApp - processing... request id: " << requestSno << endl;

    // Retrieve UE App ID
    int ueAppID = atoi(contAppMsg->getDevAppId());

    //--------------------------------------------------------------------------
    // Check if the MEC App is already running
    //--------------------------------------------------------------------------
    for (const auto& contextApp : meAppMap) {
        if (contextApp.second.mecUeAppID == ueAppID &&
            contextApp.second.appDId == contAppMsg->getAppDId())
        {
            EV << "MecOrchestrator::startMECApp - WARNING: App already running on MEC host: "
               << contextApp.second.mecHost->getName() << endl;
            EV << "MecOrchestrator::startMECApp - Sending ACK for existing context.\n";
            sendCreateAppContextAck(true, requestSno, contextApp.first);

            // If app supports multiple UEs, notify it
            auto* existingMECApp = dynamic_cast<MultiUEMECApp*>(contextApp.second.reference);
            if (existingMECApp) {
                UE_MEC_CLIENT newUE;
                newUE.address = inet::L3Address(contAppMsg->getUeIpAddress());
                newUE.port = -1;  // Unknown at this stage
                existingMECApp->addNewUE(newUE);
            }
            return;
        }
    }

    //--------------------------------------------------------------------------
    // Handle application descriptor onboarding if needed
    //--------------------------------------------------------------------------
    std::string appDid;
    double processingTime = 0.0;

    if (!contAppMsg->getOnboarded()) {
        EV << "MecOrchestrator::startMECApp - Onboarding app from: "
           << contAppMsg->getAppPackagePath() << endl;
        const ApplicationDescriptor& appDesc = onboardApplicationPackage(contAppMsg->getAppPackagePath());
        appDid = appDesc.getAppDId();
        processingTime += onboardingTime;
    } else {
        appDid = contAppMsg->getAppDId();
    }

    // Check that the descriptor is available
    auto it = mecApplicationDescriptors_.find(appDid);
    if (it == mecApplicationDescriptors_.end()) {
        EV << "MecOrchestrator::startMECApp - AppDId [" << appDid << "] not found.\n";
        sendCreateAppContextAck(false, requestSno);
        return;
    }

    const ApplicationDescriptor& desc = it->second;

    //--------------------------------------------------------------------------
    // Select MEC Host using configured policy
    //--------------------------------------------------------------------------
    cModule *bestHost = mecHostSelectionPolicy_->findBestMecHost(desc);

    //--------------------------------------------------------------------------
    // Instantiate MEC Application on selected host
    //--------------------------------------------------------------------------
    if (bestHost != nullptr) {
        bestLatency = computeLatencyForHost(bestHost);

        CreateAppMessage *createAppMsg = new CreateAppMessage();
        createAppMsg->setUeAppID(ueAppID);
        createAppMsg->setMEModuleName(desc.getAppName().c_str());
        createAppMsg->setMEModuleType(desc.getAppProvider().c_str());
        createAppMsg->setRequiredCpu(desc.getVirtualResources().cpu);
        createAppMsg->setRequiredRam(desc.getVirtualResources().ram);
        createAppMsg->setRequiredDisk(desc.getVirtualResources().disk);
        createAppMsg->setRequiredService(
            desc.getOmnetppServiceRequired().empty() ? "NULL" : desc.getOmnetppServiceRequired().c_str()
        );
        createAppMsg->setContextId(contextIdCounter);

        //--------------------------------------------------------------------------
        // Setup internal tracking structures
        //--------------------------------------------------------------------------
        mecAppMapEntry newMecApp;
        newMecApp.appDId = appDid;
        newMecApp.mecUeAppID = ueAppID;
        newMecApp.mecHost = bestHost;
        newMecApp.ueAddress = inet::L3AddressResolver().resolve(contAppMsg->getUeIpAddress());
        newMecApp.vim = bestHost->getSubmodule("vim");
        newMecApp.mecpm = bestHost->getSubmodule("mecPlatformManager");
        newMecApp.mecAppName = desc.getAppName().c_str();

        MecPlatformManager *mecpm = check_and_cast<MecPlatformManager *>(newMecApp.mecpm);

        //--------------------------------------------------------------------------
        // Deploy app based on emulation mode
        //--------------------------------------------------------------------------
        MecAppInstanceInfo *appInfo = nullptr;
        if (desc.isMecAppEmulated()) {
            EV << "MecOrchestrator::startMECApp - Emulated MEC App\n";
            bool result = mecpm->instantiateEmulatedMEApp(createAppMsg);
            appInfo = new MecAppInstanceInfo();
            appInfo->status = result;
            appInfo->endPoint.addr = inet::L3Address(desc.getExternalAddress().c_str());
            appInfo->endPoint.port = desc.getExternalPort();
            appInfo->instanceId = "emulated_" + desc.getAppName();
            newMecApp.isEmulated = true;

            // Register MEC app address to UPF via binder
            inet::L3Address gtpAddress = inet::L3AddressResolver()
                .resolve(newMecApp.mecHost->getSubmodule("upf_mec")->getFullPath().c_str());
            binder_->registerMecHostUpfAddress(appInfo->endPoint.addr, gtpAddress);
        }
        else {
            appInfo = mecpm->instantiateMEApp(createAppMsg);
            newMecApp.isEmulated = false;
        }

        //--------------------------------------------------------------------------
        // Instantiation failed: moderate case fallback
        //--------------------------------------------------------------------------
        if (!appInfo->status) {
            EV << "MecOrchestrator::startMECApp - MEC App instantiation failed.\n";
            auto *msg = new MECOrchestratorMessage("MECOrchestratorMessage");
            msg->setType(CREATE_CONTEXT_APP);
            msg->setRequestId(requestSno);
            msg->setSuccess(false);
            processingTime += instantiationTime;
            scheduleAt(simTime() + processingTime, msg);
            return;
        }

        //--------------------------------------------------------------------------
        // Success: finalize context creation and register
        //--------------------------------------------------------------------------
        EV << "MecOrchestrator::startMECApp - App [" << appInfo->instanceId
           << "] deployed at " << appInfo->endPoint.addr.str() << ":"
           << appInfo->endPoint.port << endl;

        auto *msg = new MECOrchestratorMessage("MECOrchestratorMessage");
        msg->setContextId(contextIdCounter);
        msg->setType(CREATE_CONTEXT_APP);
        msg->setRequestId(requestSno);
        msg->setSuccess(true);

        newMecApp.mecAppAddress = appInfo->endPoint.addr;
        newMecApp.mecAppPort = appInfo->endPoint.port;
        newMecApp.mecAppInstanceId = appInfo->instanceId;
        newMecApp.contextId = contextIdCounter;
        newMecApp.reference = appInfo->reference;
        meAppMap[contextIdCounter] = newMecApp;

        processingTime += instantiationTime;
        scheduleAt(simTime() + processingTime, msg);

        delete appInfo;
    }
    else {
        //--------------------------------------------------------------------------
        // No host selected: simulate partial success with delay (moderate case)
        //--------------------------------------------------------------------------
        EV << "MecOrchestrator::startMECApp - No suitable host found.\n";
        auto *msg = new MECOrchestratorMessage("MECOrchestratorMessage");
        msg->setType(CREATE_CONTEXT_APP);
        msg->setRequestId(requestSno);
        msg->setSuccess(false);
        processingTime += instantiationTime / 2;  // Retry opportunity implied
        scheduleAt(simTime() + processingTime, msg);
        bestLatency = SIMTIME_ZERO;
    }
}


void MecOrchestrator::stopMECApp(UALCMPMessage *msg)
{
    EV << "MecOrchestrator::stopMECApp - processing..." << endl;

    //--------------------------------------------------------------------------
    // Validate and cast the incoming delete message
    //--------------------------------------------------------------------------
    auto *contAppMsg = dynamic_cast<DeleteContextAppMessage *>(msg);
    if (!contAppMsg)
        throw cRuntimeError("Invalid cast to DeleteContextAppMessage");

    int contextId = contAppMsg->getContextId();
    EV << "MecOrchestrator::stopMECApp - processing contextId: " << contextId << endl;

    //--------------------------------------------------------------------------
    // Check if the context exists before proceeding
    //--------------------------------------------------------------------------
    if (meAppMap.empty() || meAppMap.find(contextId) == meAppMap.end()) {
        EV << "MecOrchestrator::stopMECApp - WARNING: MEC App [" << contextId << "] not found!" << endl;
        sendDeleteAppContextAck(false, contAppMsg->getRequestId(), contextId);
        return;
    }

    //--------------------------------------------------------------------------
    // Deallocate resources through MEC platform manager (PM) and VIM
    //--------------------------------------------------------------------------
    MecPlatformManager *mecpm = check_and_cast<MecPlatformManager *>(meAppMap[contextId].mecpm);
    DeleteAppMessage *deleteAppMsg = new DeleteAppMessage();
    deleteAppMsg->setUeAppID(meAppMap[contextId].mecUeAppID);

    //--------------------------------------------------------------------------
    // Terminate the application depending on whether it is emulated
    //--------------------------------------------------------------------------
    bool isTerminated;
    if (meAppMap[contextId].isEmulated) {
        isTerminated = mecpm->terminateEmulatedMEApp(deleteAppMsg);
        std::cout << "terminateEmulatedMEApp with result: " << isTerminated << std::endl;
    } else {
        isTerminated = mecpm->terminateMEApp(deleteAppMsg);
    }

    //--------------------------------------------------------------------------
    // Prepare and send orchestrator's acknowledgment message
    //--------------------------------------------------------------------------
    MECOrchestratorMessage *mecoMsg = new MECOrchestratorMessage("MECOrchestratorMessage");
    mecoMsg->setType(DELETE_CONTEXT_APP);
    mecoMsg->setRequestId(contAppMsg->getRequestId());
    mecoMsg->setContextId(contextId);

    if (isTerminated) {
        EV << "MecOrchestrator::stopMECApp - MEC App [" << meAppMap[contextId].mecUeAppID << "] removed successfully." << endl;
        meAppMap.erase(contextId);
        mecoMsg->setSuccess(true);
    } else {
        EV << "MecOrchestrator::stopMECApp - MEC App [" << meAppMap[contextId].mecUeAppID << "] could not be removed." << endl;
        mecoMsg->setSuccess(false);
    }

    //--------------------------------------------------------------------------
    // Simulate moderate processing delay before confirming deletion
    //--------------------------------------------------------------------------
    double processingTime = terminationTime;
    scheduleAt(simTime() + processingTime, mecoMsg);
}


void MecOrchestrator::sendDeleteAppContextAck(bool result, unsigned int requestSno, int contextId)
{
    EV << "MecOrchestrator::sendDeleteAppContextAck - result: "
       << result << " reqSno: " << requestSno << " contextId: " << contextId << endl;

    DeleteContextAppAckMessage *ack = new DeleteContextAppAckMessage();
    ack->setType(ACK_DELETE_CONTEXT_APP);
    ack->setRequestId(requestSno);
    ack->setSuccess(result);

    send(ack, "toUALCMP");
}


void MecOrchestrator::sendCreateAppContextAck(bool result, unsigned int requestSno, int contextId)
{
    EV << "MecOrchestrator::sendCreateAppContextAck - result: "
       << result << " reqSno: " << requestSno << " contextId: " << contextId << endl;

    CreateContextAppAckMessage *ack = new CreateContextAppAckMessage();
    ack->setType(ACK_CREATE_CONTEXT_APP);

    if (result) {
        //--------------------------------------------------------------------------
        // In moderate case, verify again that the context exists before responding
        //--------------------------------------------------------------------------
        if (meAppMap.empty() || meAppMap.find(contextId) == meAppMap.end()) {
            EV << "MecOrchestrator::sendCreateAppContextAck - ERROR: meApp[" << contextId << "] does not exist!" << endl;
            return;
        }

        mecAppMapEntry mecAppStatus = meAppMap[contextId];

        ack->setSuccess(true);
        ack->setContextId(contextId);
        ack->setAppInstanceId(mecAppStatus.mecAppInstanceId.c_str());
        ack->setRequestId(requestSno);

        // Construct instance URI in format: IP:Port
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

    //--------------------------------------------------------------------------
    // Moderate-Case Scenario: Latency-Based policy with realistic resource filtering
    //--------------------------------------------------------------------------
    if (policy == "LatencyBased") {
        EV << "MecOrchestrator::findBestMecHost - Applying Latency-Based policy..." << endl;
        getSimulation()->getActiveEnvir()->alert("✅ Latency-Based policy is ACTIVE!");

        bestLatency = SIMTIME_MAX;
        double bestScore = std::numeric_limits<double>::max();
        cModule* bestHost = nullptr;

        for (auto mecHost : mecHosts) {
            // Access VIM submodule for resource data
            cModule* vimSubmod = mecHost->getSubmodule("vim");
            if (!vimSubmod)
                throw cRuntimeError("Submodule 'vim' not found in MEC host: %s", mecHost->getFullPath().c_str());

            auto* vim = check_and_cast<VirtualisationInfrastructureManager*>(vimSubmod);
            ResourceDescriptor resources = appDesc.getVirtualResources();

            // Check allocability under moderate resource pressure
            if (!vim->isAllocable(resources.ram, resources.disk, resources.cpu)) {
                EV << "  MEC host [" << mecHost->getName() << "] doesn't have enough resources.\n";
                continue;
            }

            //-------------------------------------------------------------------------
            // Simulate moderate-case latency estimates (not worst, not optimal)
            //-------------------------------------------------------------------------
            simtime_t latency;
            std::string hostName = mecHost->getName();
            if (hostName.find("mecHost1") != std::string::npos)
                latency = SimTime(0.005, SIMTIME_S);  // 5ms
            else
                latency = SimTime(0.05, SIMTIME_S);   // 50ms (moderate penalty)

            //-------------------------------------------------------------------------
            // Fetch dynamic VIM metrics (e.g., bandwidth and CPU load)
            //-------------------------------------------------------------------------
            double availableBandwidth = vim->getAvailableBandwidth();
            if (availableBandwidth < 1e-6)
                availableBandwidth = 1e-6; // Prevent division by zero

            double loadFactor = vim->getCurrentCpuLoad();

            //-------------------------------------------------------------------------
            // Composite score using latency, load, and bandwidth
            // Score = latency * (1 + w * load) / bandwidth (lower is better)
            //-------------------------------------------------------------------------
            double cpuWeight = 0.5; // CPU load moderately affects final score
            double clippedLoad = std::min(loadFactor, 0.9); // prevent CPU overload dominance
            double score = latency.dbl() * (1.0 + cpuWeight * clippedLoad);

            EV << "  Host [" << hostName << "] → Latency: " << latency
               << ", Load: " << loadFactor
               << ", Available BW: " << availableBandwidth
               << ", Score: " << score << endl;

            //-------------------------------------------------------------------------
            // Update best host if score is better
            //-------------------------------------------------------------------------
            if (score < bestScore) {
                bestScore = score;
                bestLatency = latency;
                bestHost = mecHost;
            }
        }

        if (bestHost)
            EV << "  Selected host: " << bestHost->getName() << " with latency: " << bestLatency << "s\n";
        else
            EV << "  No suitable MEC host found.\n";

        return bestHost;
    }

    //--------------------------------------------------------------------------
    // Fallback: Default policy based on available services
    //--------------------------------------------------------------------------
    cModule* bestHost = nullptr;

    for (auto mecHost : mecHosts) {
        cModule* vimSubmod = mecHost->getSubmodule("vim");
        if (!vimSubmod)
            throw cRuntimeError("Submodule 'vim' not found in MEC host: %s", mecHost->getFullPath().c_str());

        auto* vim = check_and_cast<VirtualisationInfrastructureManager*>(vimSubmod);
        ResourceDescriptor resources = appDesc.getVirtualResources();

        if (!vim->isAllocable(resources.ram, resources.disk, resources.cpu)) {
            EV << "MecOrchestrator::findBestMecHost - MEC host [" << mecHost->getName() << "] lacks resources.\n";
            continue;
        }

        cModule* mecpmSubmod = mecHost->getSubmodule("mecPlatformManager");
        if (!mecpmSubmod)
            throw cRuntimeError("Submodule 'mecPlatformManager' not found in MEC host: %s", mecHost->getFullPath().c_str());

        auto* mecpm = check_and_cast<MecPlatformManager*>(mecpmSubmod);
        auto mecServices = mecpm->getAvailableMecServices();

        std::string serviceName;
        if (!appDesc.getAppServicesRequired().empty())
            serviceName = appDesc.getAppServicesRequired()[0];
        else
            break;

        for (const auto& service : *mecServices) {
            if (serviceName == service.getName()) {
                bestHost = mecHost;
                break;
            }
        }

        if (bestHost)
            break;
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

    // Retrieve MEC host list from configuration parameter
    auto mecHostList = check_and_cast<cValueArray *>(par("mecHostList").objectValue());

    if (mecHostList->size() > 0) {
        for (int i = 0; i < mecHostList->size(); i++) {
            const char *token = mecHostList->get(i).stringValue();
            EV << "  → Discovered MEC host path: " << token << endl;
            cModule *mecHostModule = getSimulation()->getModuleByPath(token);
            mecHosts.push_back(mecHostModule);
        }
    } else {
        EV << "⚠️ MecOrchestrator::getConnectedMecHosts - No mecHostList found in parameters." << endl;
    }
}

const ApplicationDescriptor& MecOrchestrator::onboardApplicationPackage(const char *fileName)
{
    EV << "MecOrchestrator::onboardApplicationPackage - onboarding application descriptor from: " << fileName << endl;

    ApplicationDescriptor appDesc(fileName);
    std::string appDId = appDesc.getAppDId();

    if (mecApplicationDescriptors_.find(appDId) != mecApplicationDescriptors_.end()) {
        EV << "  → ApplicationDescriptor [" << appDId << "] already onboarded. Skipping.\n";
    } else {
        mecApplicationDescriptors_[appDId] = appDesc;
    }

    return mecApplicationDescriptors_[appDId];
}

void MecOrchestrator::registerMecService(ServiceDescriptor& serviceDescriptor) const
{
    EV << "MecOrchestrator::registerMecService - Registering service: [" << serviceDescriptor.name << "]" << endl;

    for (auto mecHost : mecHosts) {
        cModule *registryModule = mecHost->getSubmodule("mecPlatform")->getSubmodule("serviceRegistry");

        if (registryModule != nullptr) {
            ServiceRegistry *serviceRegistry = check_and_cast<ServiceRegistry *>(registryModule);
            serviceRegistry->registerMecService(serviceDescriptor);

            EV << "  → Registered on MEC host [" << mecHost->getName() << "]" << endl;
        } else {
            EV << "  ⚠️ Warning: serviceRegistry submodule missing in host: " << mecHost->getName() << endl;
        }
    }
}

void MecOrchestrator::onboardApplicationPackages()
{
    // Retrieve application package list from parameters
    auto mecApplicationPackageList = check_and_cast<cValueArray *>(par("mecApplicationPackageList").objectValue());

    if (mecApplicationPackageList->size() > 0) {
        for (int i = 0; i < mecApplicationPackageList->size(); i++) {
            const char *token = mecApplicationPackageList->get(i).stringValue();
            std::string fullPath = std::string("ApplicationDescriptors/") + token + ".json";

            EV << "MecOrchestrator::onboardApplicationPackages - Loading descriptor: " << fullPath << endl;
            onboardApplicationPackage(fullPath.c_str());
        }
    } else {
        EV << "⚠️ MecOrchestrator::onboardApplicationPackages - No packages defined in mecApplicationPackageList." << endl;
    }
}

const ApplicationDescriptor* MecOrchestrator::getApplicationDescriptorByAppName(const std::string& appName) const
{
    // Iterate through onboarded descriptors and match by application name
    for (const auto& appDesc : mecApplicationDescriptors_) {
        if (appDesc.second.getAppName() == appName)
            return &(appDesc.second);
    }

    // No match found
    return nullptr;
}

simtime_t MecOrchestrator::computeLatencyForHost(cModule* mecHost)
{
    std::string hostName = mecHost->getName();

    //--------------------------------------------------------------------------
    // Simulated Latency Mapping (Moderate-Case Scenario)
    //--------------------------------------------------------------------------
    if (hostName.find("mecHost1") != std::string::npos)
        return SimTime(0.005, SIMTIME_S);   // 5ms → closer host

    else if (hostName.find("mecHost2") != std::string::npos)
        return SimTime(0.05, SIMTIME_S);    // 50ms → further but still reachable

    // Fallback latency for unrecognized hosts
    return SimTime(0.1, SIMTIME_S);         // Default moderate latency
}



} //namespace
