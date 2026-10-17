# Wiring and startup only; the logic lives in components/ (sunsim_events,
# sunsim_core, sunsim_platform - see ARCHITECTURE.md).
idf_component_register(SRCS "main.cpp"
                       PRIV_REQUIRES sunsim_events sunsim_core sunsim_platform nvs_flash)
