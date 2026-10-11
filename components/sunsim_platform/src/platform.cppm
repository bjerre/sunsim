// sunsim::platform - everything that touches ESP-IDF, as one module.
// Importing sunsim.platform gets the parts the app wires up; the internal
// partitions (bulb_endpoint, fast_day_clock, format, wifi) are
// used only inside the module.
export module sunsim.platform;

export import :clock;
export import :console;
export import :internet_sync;
export import :settings_store;
export import :status_display;
export import :status_slot;
export import :zigbee_adapter;
