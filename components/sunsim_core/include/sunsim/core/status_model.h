#pragma once

#include "sunsim/ev/events.h"

namespace sunsim::core {

// Gathers what the status display and console show into one snapshot,
// published as StatusChanged once a second (after each Tick's DevicesUpdated).
class StatusModel {
public:
    explicit StatusModel(ev::Post post);

    void onConfig(const ev::ConfigChanged &e);
    void onModeChanged(const ev::ModeChanged &e);
    void onDevices(const ev::DevicesUpdated &e);
    void onTick(const ev::Tick &e);

private:
    void publish();

    ev::Post           post_;
    ev::StatusSnapshot snap_{};
};

} // namespace sunsim::core
