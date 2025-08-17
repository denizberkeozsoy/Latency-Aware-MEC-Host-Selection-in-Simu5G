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
// This module models the functionalities of the MEC orchestrator within a Multi-access Edge
// Computing (MEC) system.
//
// In a moderate-case simulation scenario, this orchestrator receives application instantiation
// and termination requests via the ~UALCMP interface. It selects an appropriate MEC host
// based on latency, CPU usage, and other factors — using a configurable selection policy.
// Timing parameters (onboarding, instantiation, termination) reflect moderate system responsiveness.
//
simple MecOrchestrator
{
    parameters:
        @display("i=device/mainframe;bgb=1006,692");

        string binderModule = default("binder");

        // Policy used to select MEC host (can be changed at runtime)
        string selectionPolicy = default("LatencyAwareBased");

        // Selection weights for latency and CPU utilization (moderate balance)
        double latencyWeight = default(0.7);   // Latency is slightly more prioritized
        double cpuWeight = default(0.3);       // CPU load still has impact

        // Simulated latencies to MEC hosts (moderate, fixed values)
        volatile double latencyHost1 @unit(ms) = default(10ms);
        volatile double latencyHost2 @unit(ms) = default(20ms);

        // Optional weights for throughput and queue metrics (unused in moderate scenario)
        double throughputWeight = default(0.0); 
        double queueLenWeight = default(0.0);

        int mecHostIndex = default(0);
        object mecHostList = default([]);
        object mecApplicationPackageList = default([]);

        double onboardingTime @unit(s) = default(50ms);
        double instantiationTime @unit(s) = default(50ms);
        double terminationTime @unit(s) = default(50ms);

    gates:
        output toUALCMP;     // Outgoing messages to UALCMP
        input fromUALCMP;    // Incoming requests from UALCMP
}
