// sunsim::core - all of the pure application logic, as one C++ module: a
// partition per class (and for the math), all re-exported here.
export module sunsim.core;

import std.compat;
import sunsim.ev;

export import :day_info;
export import :daylight;
export import :daylight_program;
export import :device_registry;
export import :light_output;
export import :mode_controller;
export import :off_program;
export import :program;
export import :program_host;
export import :red_alert_program;
export import :solar;
export import :status_model;

export namespace sunsim::core {

// All of the pure application logic. Platform code (radio, storage, clock,
// console, display) talks to it only through events on the bus.
struct CoreModules {
    CoreModules(const ev::Post &post, DeviceRegistry::Manifest manifest)
        : mode(post), registry(post, manifest), programs(post), output(post), status(post)
    {
    }

    ModeController  mode;
    DeviceRegistry  registry;
    ProgramHost     programs;
    LightOutput     output;
    StatusModel     status;
};

// Every edge between core modules, in one place. Platform edges are wired
// separately (main/main.cpp); tests wire only this.
void wireCore(ev::EventBus &bus, CoreModules &m);

} // namespace sunsim::core

namespace sunsim::core {

void wireCore(ev::EventBus &bus, CoreModules &m)
{
    using namespace ev;

    // Clock
    bus.on<Tick>([&](const Tick &e) {
        m.mode.onTick(e);
        m.registry.onTick(e);
        m.programs.onTick(e);
        m.status.onTick(e);
    });
    bus.on<ProgramTick>([&](const ProgramTick &e) { m.programs.onProgramTick(e); });

    // Settings
    bus.on<ConfigChanged>([&](const ConfigChanged &e) {
        m.programs.onConfig(e);
        m.status.onConfig(e);
    });
    bus.on<RolesLoaded>([&](const RolesLoaded &e) { m.registry.onRolesLoaded(e); });

    // From the radio
    bus.on<NetworkUp>([&](const NetworkUp &e) {
        m.registry.onNetworkUp(e);
        m.programs.onRefresh(RefreshLights{});
    });
    bus.on<DeviceAnnounced>([&](const DeviceAnnounced &e) { m.registry.onAnnounced(e); });
    bus.on<EndpointFound>([&](const EndpointFound &e) { m.registry.onEndpointFound(e); });
    bus.on<BindResult>([&](const BindResult &e) { m.registry.onBindResult(e); });
    bus.on<PingResult>([&](const PingResult &e) { m.registry.onPingResult(e); });
    bus.on<AckReceived>([&](const AckReceived &e) { m.registry.onAck(e); });
    bus.on<ButtonPressed>([&](const ButtonPressed &e) {
        m.registry.onButtonPressed(e);
        m.mode.onPress(e);
    });

    // Console requests handled in core
    bus.on<PairRequested>([&](const PairRequested &e) { m.registry.onPairRequested(e); });
    bus.on<PairCancelled>([&](const PairCancelled &e) { m.registry.onPairCancelled(e); });
    bus.on<UnpairRequested>([&](const UnpairRequested &e) { m.registry.onUnpairRequested(e); });

    // Registry
    bus.on<RoleAssigned>([&](const RoleAssigned &e) { m.output.onRoleAssigned(e); });
    bus.on<RoleCleared>([&](const RoleCleared &e) { m.output.onRoleCleared(e); });
    bus.on<ReachabilityChanged>([&](const ReachabilityChanged &e) { m.output.onReachability(e); });
    bus.on<DevicesUpdated>([&](const DevicesUpdated &e) { m.status.onDevices(e); });

    // Mode -> programs
    bus.on<ModeChanged>([&](const ModeChanged &e) {
        m.programs.onModeChanged(e);
        m.status.onModeChanged(e);
    });
    bus.on<RefreshLights>([&](const RefreshLights &e) { m.programs.onRefresh(e); });

    // Programs -> output
    bus.on<LightTarget>([&](const LightTarget &e) { m.output.onTarget(e); });
}

} // namespace sunsim::core
