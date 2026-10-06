file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")

idf_component_register(SRC_DIRS "."
                       PRIV_INCLUDE_DIRS "." "${CMAKE_CURRENT_BINARY_DIR}/generated"
                       PRIV_REQUIRES console driver esp_lcd nvs_flash esp_wifi esp_netif esp_timer lwip)

set(role_manifest_json "${CMAKE_CURRENT_SOURCE_DIR}/role_manifest.json")
set(role_manifest_hdr "${CMAKE_CURRENT_BINARY_DIR}/generated/role_manifest.h")

add_custom_command(
    OUTPUT "${role_manifest_hdr}"
    COMMAND "${PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/gen_role_manifest.py"
            "${role_manifest_json}" "${role_manifest_hdr}"
    DEPENDS "${role_manifest_json}" "${CMAKE_CURRENT_SOURCE_DIR}/gen_role_manifest.py"
    COMMENT "Generating role_manifest.h from role_manifest.json"
    VERBATIM)
add_custom_target(generate_role_manifest DEPENDS "${role_manifest_hdr}")
add_dependencies(${COMPONENT_LIB} generate_role_manifest)
