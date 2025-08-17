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

#include <iostream>  // For emulation debug output

namespace simu5g {

Define_Module(MecOrchestrator);

void MecOrchestrator::initialize(int stage)
{
    cSimpleModule::initialize(stage);

    // Ensure this part runs only during the local initialization stage
    if (stage != inet::INITSTAGE_LOCAL)
        return;

    EV << "MecOrchestrator::initialize - stage " << stage << endl;

    // Reference to binder module
    binder_.reference(this, "binderModule", true);

    // Select MEC host selection policy (worst-case variant of LatencyAwareBased is used)
    const char *selectionPolicyPar = par("selectionPolicy");
    if (!strcmp(selectionPolicyPar, "MecServiceBased"))
        mecHostSelectionPolicy_ = new MecServiceSelectionBased(this);
    else if (!strcmp(selectionPolicyPar, "AvailableResourcesBased"))
        mecHostSelectionPolicy_ = new AvailableResourcesSelectionBased(this);
    else if (!strcmp(selectionPolicyPar, "MecHostBased"))
        mecHostSelectionPolicy_ = new MecHostSelectionBased(this, par("mecHostIndex"));
    else if (!strcmp(selectionPolicyPar, "LatencyAwareBased"))
        mecHostSelectionPolicy_ = new LatencyAwareSelectionBased(this, mecHosts);  // Worst-case scoring inside
    else
        throw cRuntimeError("MecOrchestrator::initialize - Selection policy '%s' not supported!", selectionPolicyPar);

    // Delays used to simulate worst-case MEC behavior
    onboardingTime = par("onboardingTime").doubleValue();
    instantiationTime = par("instantiationTime").doubleValue();
    terminationTime = par("terminationTime").doubleValue();

    simtime_t processingTime = SIMTIME_ZERO;  // optional delay placeholder

    // Collect host references and onboard app packages (may trigger delays/failures)
    getConnectedMecHosts();
    onboardApplicationPackages();
}

void MecOrchestrator::handleMessage(cMessage *msg)
{
    // Handle internal scheduler events
    if (msg->isSelfMessage()) {
        if (strcmp(msg->getName(), "MECOrchestratorMessage") == 0) {
            EV << "MecOrchestrator::handleMessage - " << msg->getName() << endl;
            MECOrchestratorMessage *meoMsg = check_and_cast<MECOrchestratorMessage *>(msg);

            // Handle delayed app instantiation responses (success or failure)
            if (strcmp(meoMsg->getType(), CREATE_CONTEXT_APP) == 0) {
                if (meoMsg->getSuccess())
                    sendCreateAppContextAck(true, meoMsg->getRequestId(), meoMsg->getContextId());
                else
                    sendCreateAppContextAck(false, meoMsg->getRequestId());
            }
            // Handle delayed app deletion responses
            else if (strcmp(meoMsg->getType(), DELETE_CONTEXT_APP) == 0) {
                sendDeleteAppContextAck(meoMsg->getSuccess(), meoMsg->getRequestId(), meoMsg->getContextId());
            }
        }
    }
    // Handle incoming requests from the UALCMP layer (UE-initiated control)
    else if (msg->arrivedOn("fromUALCMP")) {
        EV << "MecOrchestrator::handleMessage - " << msg->getName() << endl;
        handleUALCMPMessage(msg);
    }

    delete msg;  // Always clean up
}

void MecOrchestrator::handleUALCMPMessage(cMessage *msg)
{
    UALCMPMessage *lcmMsg = check_and_cast<UALCMPMessage *>(msg);

    // Process app deployment request (may trigger worst-case logic: failure, delay)
    if (!strcmp(lcmMsg->getType(), CREATE_CONTEXT_APP))
        startMECApp(lcmMsg);

    // Process app termination request
    else if (!strcmp(lcmMsg->getType(), DELETE_CONTEXT_APP))
        stopMECApp(lcmMsg);
}


void MecOrchestrator::startMECApp(UALCMPMessage *msg)
{
    CreateContextAppMessage *contAppMsg = check_and_cast<CreateContextAppMessage *>(msg);
    unsigned int requestSno = msg->getRequestId();
    int contextId = contextIdCounter;
    contextStartTimes[contextIdCounter] = simTime();  // Record task start time

    EV << "MecOrchestrator::createMeApp - processing... request id: " << contAppMsg->getRequestId() << endl;

    // Extract UE App ID
    int ueAppID = atoi(contAppMsg->getDevAppId());

    // Check if the MEC app is already deployed for the same UE and app descriptor
    for (const auto& contextApp : meAppMap) {
        if (contextApp.second.mecUeAppID == ueAppID &&
            contextApp.second.appDId.compare(contAppMsg->getAppDId()) == 0) {

            EV << "MecOrchestrator::startMECApp - \tWARNING: required MEC App instance ALREADY STARTED on MEC host: "
               << contextApp.second.mecHost->getName() << endl;
            EV << "MecOrchestrator::startMECApp  - sending ackMEAppPacket with " << ACK_CREATE_CONTEXT_APP << endl;

            sendCreateAppContextAck(true, contAppMsg->getRequestId(), contextApp.first);

            auto* existingMECApp = dynamic_cast<MultiUEMECApp*>(contextApp.second.reference);
            if (existingMECApp) {
                // Reuse existing app and add the new UE
                struct UE_MEC_CLIENT newUE;
                newUE.address = inet::L3Address(contAppMsg->getUeIpAddress());
                newUE.port = -1;
                existingMECApp->addNewUE(newUE);
            } else {
                return;
            }
        }
    }

    std::string appDid;
    double processingTime = 0.0;

    // Onboard application if not already onboarded
    if (!contAppMsg->getOnboarded()) {
        EV << "MecOrchestrator::startMECApp - onboarding appDescriptor from: "
           << contAppMsg->getAppPackagePath() << endl;

        const ApplicationDescriptor& appDesc = onboardApplicationPackage(contAppMsg->getAppPackagePath());
        appDid = appDesc.getAppDId();
        processingTime += onboardingTime;
    } else {
        appDid = contAppMsg->getAppDId();
    }

    auto it = mecApplicationDescriptors_.find(appDid);
    if (it == mecApplicationDescriptors_.end()) {
        EV << "MecOrchestrator::startMECApp - Application package with AppDId["
           << contAppMsg->getAppDId() << "] not onboarded." << endl;

        sendCreateAppContextAck(false, contAppMsg->getRequestId());
    }

    const ApplicationDescriptor& desc = it->second;

    // Select a MEC host using the active policy (may include degraded scoring logic)
    cModule *bestHost = mecHostSelectionPolicy_->findBestMecHost(desc);

    if (bestHost != nullptr) {
        // WORST-CASE SIMULATION: Injecting maximum artificial delay and high failure probability
        double failureProbability = 0.3;   // 30% chance of failure
        double extraDelayMs = 100.0;       // 100 ms artificial delay
        bool simulateFailure = uniform(0, 1) < failureProbability;

        if (simulateFailure) {
            EV_WARN << "🛑 [WORST-CASE] Forced MEC app instantiation failure: skipping deployment.\n";

            MECOrchestratorMessage *failMsg = new MECOrchestratorMessage("MECOrchestratorMessage");
            failMsg->setType(CREATE_CONTEXT_APP);
            failMsg->setRequestId(contAppMsg->getRequestId());
            failMsg->setSuccess(false);

            scheduleAt(simTime() + SimTime(extraDelayMs, SIMTIME_MS), failMsg);
            bestLatency = SIMTIME_ZERO;
            return;
        }

        // WORST-CASE SIMULATION: Injecting maximum artificial delay before processing
        processingTime += SimTime(extraDelayMs, SIMTIME_MS).dbl();
        EV_WARN << "🕒 [WORST-CASE] Injecting maximum artificial delay of "
                << SimTime(extraDelayMs, SIMTIME_MS) << " before processing.\n";


        bestLatency = computeLatencyForHost(bestHost);

        // Prepare MEC app creation message
        CreateAppMessage *createAppMsg = new CreateAppMessage();
        createAppMsg->setUeAppID(ueAppID);
        createAppMsg->setMEModuleName(desc.getAppName().c_str());
        createAppMsg->setMEModuleType(desc.getAppProvider().c_str());

        createAppMsg->setRequiredCpu(desc.getVirtualResources().cpu);
        createAppMsg->setRequiredRam(desc.getVirtualResources().ram);
        createAppMsg->setRequiredDisk(desc.getVirtualResources().disk);

        if (!desc.getOmnetppServiceRequired().empty())
            createAppMsg->setRequiredService(desc.getOmnetppServiceRequired().c_str());
        else
            createAppMsg->setRequiredService("NULL");

        createAppMsg->setContextId(contextIdCounter);

        // Initialize and register new MEC app in the internal map
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

        // Instantiate or emulate the MEC app
        if (desc.isMecAppEmulated()) {
            EV << "MecOrchestrator::startMECApp - MEC app is emulated" << endl;
            bool result = mecpm->instantiateEmulatedMEApp(createAppMsg);

            appInfo = new MecAppInstanceInfo();
            appInfo->status = result;
            appInfo->endPoint.addr = inet::L3Address(desc.getExternalAddress().c_str());
            appInfo->endPoint.port = desc.getExternalPort();
            appInfo->instanceId = "emulated_" + desc.getAppName();
            newMecApp.isEmulated = true;

            // Register emulated app address with Binder for traffic forwarding
            inet::L3Address gtpAddress = inet::L3AddressResolver().resolve(
                newMecApp.mecHost->getSubmodule("upf_mec")->getFullPath().c_str()
            );
            binder_->registerMecHostUpfAddress(appInfo->endPoint.addr, gtpAddress);
        } else {
            appInfo = mecpm->instantiateMEApp(createAppMsg);
            newMecApp.isEmulated = false;
        }

        // Handle failed instantiation
        if (!appInfo->status) {
            EV << "MecOrchestrator::startMECApp - something went wrong during MEC app instantiation" << endl;

            MECOrchestratorMessage *msg = new MECOrchestratorMessage("MECOrchestratorMessage");
            msg->setType(CREATE_CONTEXT_APP);
            msg->setRequestId(contAppMsg->getRequestId());
            msg->setSuccess(false);

            processingTime += instantiationTime;
            scheduleAt(simTime() + processingTime, msg);
            return;
        }

        // Log successful instantiation
        EV << "MecOrchestrator::startMECApp - new MEC application with name: "
           << appInfo->instanceId << " instantiated on MEC host ["
           << newMecApp.mecHost->getFullName() << "] at "
           << appInfo->endPoint.addr.str() << ":" << appInfo->endPoint.port << endl;

        // Create context ack message
        MECOrchestratorMessage *msg = new MECOrchestratorMessage("MECOrchestratorMessage");
        msg->setContextId(contextIdCounter);
        msg->setType(CREATE_CONTEXT_APP);
        msg->setRequestId(contAppMsg->getRequestId());
        msg->setSuccess(true);

        // Finalize MEC app record
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
        // No suitable host selected — simulate degraded system
        EV << "MecOrchestrator::startMECApp - A suitable MEC host has not been selected" << endl;

        MECOrchestratorMessage *msg = new MECOrchestratorMessage("MECOrchestratorMessage");
        msg->setType(CREATE_CONTEXT_APP);
        msg->setRequestId(contAppMsg->getRequestId());
        msg->setSuccess(false);

        processingTime += instantiationTime / 2;
        scheduleAt(simTime() + processingTime, msg);

        bestLatency = SIMTIME_ZERO;
    }
}


void MecOrchestrator::stopMECApp(UALCMPMessage *msg)
{
    EV << "MecOrchestrator::stopMECApp - processing..." << endl;

    // Validate and cast incoming message
    auto *contAppMsg = dynamic_cast<DeleteContextAppMessage *>(msg);
    if (!contAppMsg)
        throw cRuntimeError("Invalid cast to DeleteContextAppMessage");

    int contextId = contAppMsg->getContextId();
    EV << "MecOrchestrator::stopMECApp - processing contextId: " << contextId << endl;

    // WORST-CASE SIMULATION: Possible inconsistency or unexpected deletion
    if (meAppMap.empty() || (meAppMap.find(contextId) == meAppMap.end())) {
        EV << "MecOrchestrator::stopMECApp - ⚠️ MEC Application ["
           << meAppMap[contextId].mecUeAppID << "] not found! Possibly already deleted." << endl;

        sendDeleteAppContextAck(false, contAppMsg->getRequestId(), contextId);
        return;
    }

    // Attempt resource deallocation using platform manager (may fail in worst case)
    MecPlatformManager *mecpm = check_and_cast<MecPlatformManager *>(meAppMap[contextId].mecpm);
    DeleteAppMessage *deleteAppMsg = new DeleteAppMessage();
    deleteAppMsg->setUeAppID(meAppMap[contextId].mecUeAppID);

    // Terminate app depending on type
    bool isTerminated;
    if (meAppMap[contextId].isEmulated) {
        isTerminated = mecpm->terminateEmulatedMEApp(deleteAppMsg);
        std::cout << "terminateEmulatedMEApp with result: " << isTerminated << std::endl;
    } else {
        isTerminated = mecpm->terminateMEApp(deleteAppMsg);
    }

    // Build and schedule orchestrator message
    MECOrchestratorMessage *mecoMsg = new MECOrchestratorMessage("MECOrchestratorMessage");
    mecoMsg->setType(DELETE_CONTEXT_APP);
    mecoMsg->setRequestId(contAppMsg->getRequestId());
    mecoMsg->setContextId(contAppMsg->getContextId());

    if (isTerminated) {
        EV << "MecOrchestrator::stopMECApp - ✅ MEC Application ["
           << meAppMap[contextId].mecUeAppID << "] removed successfully" << endl;

        meAppMap.erase(contextId);
        mecoMsg->setSuccess(true);
    } else {
        // WORST-CASE SIMULATION: App termination may silently fail
        EV << "MecOrchestrator::stopMECApp - ❌ MEC Application ["
           << meAppMap[contextId].mecUeAppID << "] could not be removed" << endl;

        mecoMsg->setSuccess(false);
    }

    double processingTime = terminationTime;
    scheduleAt(simTime() + processingTime, mecoMsg);
}


void MecOrchestrator::sendDeleteAppContextAck(bool result, unsigned int requestSno, int contextId)
{
    EV << "MecOrchestrator::sendDeleteAppContextAck - result: " << result
       << " | reqSno: " << requestSno << " | contextId: " << contextId << endl;

    DeleteContextAppAckMessage *ack = new DeleteContextAppAckMessage();
    ack->setType(ACK_DELETE_CONTEXT_APP);
    ack->setRequestId(requestSno);
    ack->setSuccess(result);

    send(ack, "toUALCMP");
}


void MecOrchestrator::sendCreateAppContextAck(bool result, unsigned int requestSno, int contextId)
{
    EV << "MecOrchestrator::sendCreateAppContextAck - result: " << result
       << " | reqSno: " << requestSno << " | contextId: " << contextId << endl;

    CreateContextAppAckMessage *ack = new CreateContextAppAckMessage();
    ack->setType(ACK_CREATE_CONTEXT_APP);

    if (result) {
        // WORST-CASE SIMULATION: Double-check if app context was lost unexpectedly
        if (meAppMap.empty() || meAppMap.find(contextId) == meAppMap.end()) {
            EV << "MecOrchestrator::ackMEAppPacket - ❌ ERROR: meApp[" << contextId << "] does not exist!" << endl;
            return;
        }

        mecAppMapEntry mecAppStatus = meAppMap[contextId];

        ack->setSuccess(true);
        ack->setContextId(contextId);
        ack->setAppInstanceId(mecAppStatus.mecAppInstanceId.c_str());
        ack->setRequestId(requestSno);

        std::stringstream uri;
        uri << mecAppStatus.mecAppAddress.str() << ":" << mecAppStatus.mecAppPort;
        ack->setAppInstanceUri(uri.str().c_str());
    } else {
        // Negative acknowledgment (failed instantiation or internal error)
        ack->setRequestId(requestSno);
        ack->setSuccess(false);
    }

    send(ack, "toUALCMP");
}


cModule *MecOrchestrator::findBestMecHost(const ApplicationDescriptor& appDesc)
{
    EV << "MecOrchestrator::findBestMecHost - using policy: " << par("selectionPolicy").str() << endl;

    std::string policy = par("selectionPolicy").stdstringValue();

    if (policy == "LatencyBased") {
        EV << "MecOrchestrator::findBestMecHost - Applying Latency-Based policy..." << endl;
        getSimulation()->getActiveEnvir()->alert("✅ Latency-Based policy is ACTIVE!");

        bestLatency = SIMTIME_MAX;
        double bestScore = std::numeric_limits<double>::max();
        cModule* bestHost = nullptr;

        for (auto mecHost : mecHosts) {
            cModule* vimSubmod = mecHost->getSubmodule("vim");
            if (!vimSubmod) {
                // WORST-CASE: VIM module is missing (invalid MEC host)
                throw cRuntimeError("Submodule 'vim' not found in MEC host: %s", mecHost->getFullPath().c_str());
            }

            VirtualisationInfrastructureManager* vim = check_and_cast<VirtualisationInfrastructureManager*>(vimSubmod);
            ResourceDescriptor resources = appDesc.getVirtualResources();

            if (!vim->isAllocable(resources.ram, resources.disk, resources.cpu)) {
                // WORST-CASE: Insufficient resources → skip host
                EV << "  MEC host [" << mecHost->getName() << "] doesn't have enough resources.\n";
                continue;
            }

            // Estimate latency (fixed values as worst-case assumption)
            simtime_t latency;
            std::string hostName = mecHost->getName();
            if (hostName.find("mecHost1") != std::string::npos)
                latency = SimTime(0.005, SIMTIME_S);  // best-case fixed
            else
                latency = SimTime(0.05, SIMTIME_S);   // worst-case latency

            // Retrieve bandwidth and CPU load
            double availableBandwidth = vim->getAvailableBandwidth();
            if (availableBandwidth < 1e-6) // WORST-CASE: avoid divide by zero
                availableBandwidth = 1e-6;

            double loadFactor = vim->getCurrentCpuLoad();  // WORST-CASE: possibly high load

            // Latency-based scoring formula (lower is better)
            double cpuWeight = 0.5;
            double clippedLoad = std::min(loadFactor, 0.9);  // cap worst-case influence
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
            EV << "  Selected host: " << bestHost->getName() << " with latency: " << bestLatency << "s\n";
        } else {
            // WORST-CASE: No host qualifies
            EV << "  No suitable MEC host found.\n";
        }

        return bestHost;
    }

    // ─────────────────────────────────────────────────────────────
    // FALLBACK DEFAULT POLICY SECTION (e.g. MECServiceBased etc.)
    // ─────────────────────────────────────────────────────────────
    cModule *bestHost = nullptr;

    for (auto mecHost : mecHosts) {
        cModule* vimSubmod = mecHost->getSubmodule("vim");
        if (!vimSubmod) {
            throw cRuntimeError("Submodule 'vim' not found in MEC host: %s", mecHost->getFullPath().c_str());
        }

        VirtualisationInfrastructureManager* vim = check_and_cast<VirtualisationInfrastructureManager*>(vimSubmod);
        ResourceDescriptor resources = appDesc.getVirtualResources();

        bool res = vim->isAllocable(resources.ram, resources.disk, resources.cpu);
        if (!res) {
            EV << "MecOrchestrator::findBestMecHost - MEC host [" << mecHost->getName()
               << "] lacks sufficient resources. Skipping..." << endl;
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

    // WORST-CASE: Ensure dynamic fetching of MEC host modules, may be empty/missing
    auto mecHostList = check_and_cast<cValueArray *>(par("mecHostList").objectValue());
    if (mecHostList->size() > 0) {
        for (int i = 0; i < mecHostList->size(); i++) {
            const char *token = mecHostList->get(i).stringValue();
            EV << "MecOrchestrator::getConnectedMecHosts - MEC host path (param): " << token << endl;
            cModule *mecHostModule = getSimulation()->getModuleByPath(token);
            mecHosts.push_back(mecHostModule);
        }
    } else {
        // WORST-CASE: Parameter is misconfigured or missing
        EV << "MecOrchestrator::getConnectedMecHosts - ⚠️ No mecHostList found!" << endl;
    }
}


const ApplicationDescriptor& MecOrchestrator::onboardApplicationPackage(const char *fileName)
{
    EV << "MecOrchestrator::onBoardApplicationPackages - Onboarding application package (from request): " << fileName << endl;

    ApplicationDescriptor appDesc(fileName);

    // WORST-CASE: Duplicate onboarding attempt
    if (mecApplicationDescriptors_.find(appDesc.getAppDId()) != mecApplicationDescriptors_.end()) {
        EV << "MecOrchestrator::onboardApplicationPackages - Application descriptor with appName [" << fileName << "] is already present.\n" << endl;
    } else {
        // Register new descriptor into the map
        mecApplicationDescriptors_[appDesc.getAppDId()] = appDesc;
    }

    return mecApplicationDescriptors_[appDesc.getAppDId()];
}


void MecOrchestrator::registerMecService(ServiceDescriptor& serviceDescriptor) const
{
    EV << "MecOrchestrator::registerMecService - Registering MEC service [" << serviceDescriptor.name << "]" << endl;

    for (auto mecHost : mecHosts) {
        cModule *module = mecHost->getSubmodule("mecPlatform")->getSubmodule("serviceRegistry");

        // WORST-CASE: serviceRegistry may not exist
        if (module != nullptr) {
            EV << "MecOrchestrator::registerMecService - Registering MEC service [" << serviceDescriptor.name
               << "] in MEC host [" << mecHost->getName() << "]" << endl;

            ServiceRegistry *serviceRegistry = check_and_cast<ServiceRegistry *>(module);
            serviceRegistry->registerMecService(serviceDescriptor);
        } else {
            EV << "MecOrchestrator::registerMecService - ⚠️ serviceRegistry submodule not found in host ["
               << mecHost->getName() << "] — skipping.\n";
        }
    }
}


void MecOrchestrator::onboardApplicationPackages()
{
    // WORST-CASE: Missing or empty application package list parameter
    auto mecApplicationPackageList = check_and_cast<cValueArray *>(par("mecApplicationPackageList").objectValue());

    if (mecApplicationPackageList->size() > 0) {
        for (int i = 0; i < mecApplicationPackageList->size(); i++) {
            const char *token = mecApplicationPackageList->get(i).stringValue();
            std::string path = std::string("ApplicationDescriptors/") + token + ".json";
            onboardApplicationPackage(path.c_str());
        }
    } else {
        EV << "MecOrchestrator::onboardApplicationPackages - ⚠️ No mecApplicationPackageList found" << endl;
    }
}


const ApplicationDescriptor *MecOrchestrator::getApplicationDescriptorByAppName(const std::string& appName) const
{
    for (const auto& appDesc : mecApplicationDescriptors_) {
        if (appDesc.second.getAppName() == appName)
            return &(appDesc.second);
    }

    // WORST-CASE: App name not found
    return nullptr;
}


simtime_t MecOrchestrator::computeLatencyForHost(cModule* mecHost)
{
    std::string hostName = mecHost->getName();

    // Fixed latency assumptions for demo/test
    if (hostName.find("mecHost1") != std::string::npos) {
        return SimTime(0.005, SIMTIME_S);  // Best-case latency: 5 ms
    }
    else if (hostName.find("mecHost2") != std::string::npos) {
        return SimTime(0.05, SIMTIME_S);   // Worst-case latency: 50 ms
    }

    // WORST-CASE: Unknown host fallback latency
    return SimTime(0.1, SIMTIME_S);        // Default to 100 ms
}



} //namespace
