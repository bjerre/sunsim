#pragma once

#include "sunsim/ev/events.h"

namespace sunsim::platform {

// Everything sunsim persists, in NVS namespace "sunsim", one key per field
// so the format survives firmware changes (a new field just reads as its
// default). Keys are unchanged from the pre-event firmware:
//   roles     "<role>.ext" / ".short" / ".ep"   (a role is assigned iff .ext exists)
//   program   "act.start" "act.end" "act.fade" "act.days" "work.start"
//             "work.end" "sun.shift" "sun.amber"  (hours stored as minutes)
//   intensity "<role>.peak"
// Wi-Fi credentials are kept separately (wifi_time_sync.h).
class SettingsStore {
public:
    // `defaults`: the program settings before anything was saved (location
    // comes from menuconfig).
    SettingsStore(ev::Post post, ev::ProgramConfig defaults);

    // Posts RolesLoaded and ConfigChanged with what's stored.
    void loadAll();

    void onSetConfig(const ev::SetConfig &e);
    void onSetIntensity(const ev::SetIntensity &e);
    void onPersistRoles(const ev::PersistRoles &e);
    void onFactoryReset();

private:
    void publish();

    ev::Post          post_;
    ev::ProgramConfig config_;
    ev::Peaks         peaks_ = ev::kDefaultPeaks;
};

} // namespace sunsim::platform
