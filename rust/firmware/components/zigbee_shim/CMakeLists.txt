# esp-zigbee-lib for the Rust firmware: the dependency (idf_component.yml),
# and what bindgen can't see in its headers (function-like macros), as
# functions.
idf_component_register(SRCS "zigbee_shim.c" INCLUDE_DIRS ".")
