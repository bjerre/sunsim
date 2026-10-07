#include "sunsim/core/core.h"

namespace sunsim::core {

void wireCore(ev::EventBus &bus, CoreModules &m)
{
    using namespace ev;

    // Clock
    bus.on<Tick>([&](const Tick &e) {
        m.mode.onTick(e);
        m.registry.onTick(e);
        m.program.onTick(e);
        m.status.onTick(e);
    });
    bus.on<ProgramTick>([&](const ProgramTick &e) { m.program.onProgramTick(e); });

    // Settings
    bus.on<ConfigChanged>([&](const ConfigChanged &e) {
        m.program.onConfig(e);
        m.status.onConfig(e);
    });
    bus.on<RolesLoaded>([&](const RolesLoaded &e) { m.registry.onRolesLoaded(e); });

    // From the radio
    bus.on<NetworkUp>([&](const NetworkUp &) { m.program.onRefresh(RefreshLights{}); });
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

    // Mode
    bus.on<ModeChanged>([&](const ModeChanged &e) {
        m.output.onModeChanged(e);
        m.status.onModeChanged(e);
    });
    bus.on<RedAlertPulse>([&](const RedAlertPulse &e) { m.output.onRedAlertPulse(e); });

    // Program -> output
    bus.on<RefreshLights>([&](const RefreshLights &e) { m.program.onRefresh(e); });
    bus.on<LightTarget>([&](const LightTarget &e) { m.output.onTarget(e); });
}

} // namespace sunsim::core
