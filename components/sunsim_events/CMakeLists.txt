# sunsim::ev - the event vocabulary shared by every layer: value types,
# event payloads, and the EventBus. Header-only; no ESP-IDF dependencies
# beyond logging, so it also builds on the host (see test/host).
idf_component_register(INCLUDE_DIRS "include"
                       REQUIRES log eventpp)
