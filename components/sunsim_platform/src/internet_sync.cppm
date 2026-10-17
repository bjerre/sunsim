// The class only; its definitions are in internet_sync.cpp, a module
// implementation unit: compiled into the interface, std::jthread and the
// semaphores crash GCC 15 (which streams everything in a .cppm into the
// module's interface, used or not).
export module sunsim.platform:internet_sync;

import std.compat;
import sunsim.ev;

export namespace sunsim::platform {

// Everything that needs the internet, batched into short Wi-Fi sessions
// (Wi-Fi shares the 2.4 GHz radio with Zigbee). Each job has a cadence:
//
//   clock        SNTP                                      daily
//   forecast     Open-Meteo hourly cloud cover, today and  daily
//                tomorrow -> CloudCoverUpdated
//   conditions   Open-Meteo's weather now -> ConditionsUpdated   every 15 min
//   alerts       NWS's active alerts for the location -> AlertsUpdated   every 15 min
//
// Daily: once a day in the sync window (ProgramConfig, `config syncwindow`,
// default 04:00-05:00), retrying every 10 minutes within it on failure.
// Every job also runs at boot (start() waits for it, before the Zigbee radio
// starts) and when asked (SyncRequested: console `sync`, switching fast-day
// mode). A session runs whichever jobs are due, in the order above (HTTPS
// needs the clock); while the clock has never been set, every 10 minutes
// regardless: the lights stay off without it. Everything runs on its own
// task (TLS needs the stack); the replies are read as they stream in
// (sunsim.weather), whatever their size.
class InternetSync {
public:
    // The location is menuconfig's: the weather is for it.
    InternetSync(ev::Post post, double latitude_deg, double longitude_deg);

    ~InternetSync();

    InternetSync(const InternetSync &)            = delete;
    InternetSync &operator=(const InternetSync &) = delete;

    // Starts the task and waits for its first sync (the boot sync).
    void start();

    void on(const ev::ConfigChanged &e);
    void on(const ev::SyncRequested &e);

private:
    struct Daily {};
    struct Every {
        std::chrono::minutes period;
    };
    using Cadence = std::variant<Daily, Every>;

    // A job's result: done, or why not.
    using Outcome = std::expected<void, std::string>;

    struct Job {
        const char *name;
        Outcome (InternetSync::*run)();
        Cadence     when;
    };
    struct JobState {
        std::optional<std::chrono::sys_seconds> last_try;
        std::optional<std::chrono::sys_seconds> last_ok;
    };
    static constexpr std::size_t kJobCount = 4;
    static const std::array<Job, kJobCount> kJobs;

    void run(bool everything);
    Outcome syncClock();
    Outcome fetchCloudCover();
    Outcome fetchConditions();
    Outcome fetchAlerts();
    [[nodiscard]] bool due(const Job &job, const JobState &state, std::chrono::sys_seconds now) const;
    void loop(std::stop_token stop);

    ev::Post          post_;
    double            latitude_deg_;
    double            longitude_deg_;
    // Set from the app task, read by the sync task: minutes after midnight.
    std::atomic<uint16_t> window_start_{4 * 60};
    std::atomic<uint16_t> window_end_{5 * 60};

    std::array<JobState, kJobCount> state_{}; // the sync task's own, by kJobs index

    struct Task;                 // the thread and what it waits on
    std::unique_ptr<Task> task_; // last: the thread uses everything above
};

} // namespace sunsim::platform
