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

package simu5g.nodes.mec.MECOrchestrator;

//
// This module models the functionalities of the MEC Orchestrator within a
// Multi-access Edge Computing (MEC) system.
//
// ▸ In worst-case simulation setups, this orchestrator triggers the
//   selection of the *least optimal* MEC host on purpose.
// ▸ It responds to app instantiation requests from the ~UALCMP module,
//   handles onboarding, selection, deployment, and teardown of MEC applications.
//
simple MecOrchestrator
{
    parameters:
        @display("i=device/mainframe;bgb=1006,692");

        string binderModule = default("binder");

        // MEC host selection policy (e.g., "LatencyAwareBased")
        string selectionPolicy = default("LatencyAwareBased");

        // Weights used by latency-aware policies (influences worst/best scoring)
        double latencyWeight = default(0.7);    // Stronger penalty from delay
        double cpuWeight = default(0.3);        // CPU utilization penalty

        // Predefined host-specific latency values (used in score calculation)
        volatile double latencyHost1 @unit(ms) = default(10ms);
        volatile double latencyHost2 @unit(ms) = default(20ms);

        // Optional weights for more complex scoring (unused in worst-case)
        double throughputWeight = default(0.0);
        double queueLenWeight = default(0.0);

        int mecHostIndex = default(0);
        object mecHostList = default([]);
        object mecApplicationPackageList = default([]);
        double onboardingTime @unit(s) = default(50ms);       // Time to onboard application
        double instantiationTime @unit(s) = default(50ms);    // Time to instantiate MEC app
        double terminationTime @unit(s) = default(50ms);      // Time to terminate MEC app

    gates:
        output toUALCMP;     
        input fromUALCMP;    
}
