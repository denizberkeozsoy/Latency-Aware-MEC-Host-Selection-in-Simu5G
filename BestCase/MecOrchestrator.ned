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
// This module models the functionalities of the MEC orchestrator in a Multi-access Edge
// Computing (MEC) system. It handles dynamic instantiation and termination of MEC applications
// on available MEC hosts.
//
// Upon receiving a request from the ~UALCMP module, it selects an appropriate MEC host
// based on a configurable selection policy.
//
simple MecOrchestrator
{
    parameters:
        @display("i=device/mainframe;bgb=1006,692");

        string binderModule = default("binder");

        // Host selection policy: e.g., "LatencyAwareBased", "LatencyBased", etc.
        string selectionPolicy = default("LatencyAwareBased");

        // Policy weights (used for scoring MEC hosts)
        double latencyWeight = default(0.7);       // Higher means latency is more important
        double cpuWeight = default(0.3);           // CPU load impact on score

        // Manually configured per-host latencies (optional override values)
        volatile double latencyHost1 @unit(ms) = default(10ms);
        volatile double latencyHost2 @unit(ms) = default(20ms);

        // Optional weights for extended metrics
        double throughputWeight = default(0.0);    // If > 0, throughput affects host selection
        double queueLenWeight = default(0.0);      // If > 0, queue length affects host selection

        int mecHostIndex = default(0);
        object mecHostList = default([]);
        object mecApplicationPackageList = default([]);
        double onboardingTime @unit(s) = default(50ms);
        double instantiationTime @unit(s) = default(50ms);
        double terminationTime @unit(s) = default(50ms);

    gates:
        output toUALCMP;
        input fromUALCMP;
}
