#!/usr/bin/env bash
# `cargo build` for the board, with ESP-IDF's environment loaded as
# ../../sunsim loads it (eim's activate script). Arguments go to cargo.
set -euo pipefail
IDF=${SUNSIM_IDF:-$HOME/.espressif/v6.1/esp-idf}
ACTIVATE=$HOME/.espressif/tools/activate_idf_$(basename "$(dirname "$IDF")").sh
while IFS= read -r line; do
    case $line in
    PATH=*) PATH=${line#PATH=}:$PATH ;;
    SYSTEM_PATH=*) ;;
    *=*) export "${line?}" ;;
    esac
done < <(bash "$ACTIVATE" -e)
# esp-idf-sys wants the idf.py on PATH to be $IDF_PATH's, not eim's shim.
export PATH=$HOME/.cargo/bin:$IDF_PATH/tools:$PATH
cd "$(dirname "${BASH_SOURCE[0]}")"
# Menuconfig's settings (src/config_names.rs), from the C++ build's sdkconfig,
# for any not set here: the same Wi-Fi, location and zone as the C++ firmware.
SDKCONFIG=../../sdkconfig
if [[ -f $SDKCONFIG ]]; then
    for name in $(grep -oE '"SUNSIM_[A-Z_]+"' src/config_names.rs | tr -d '"'); do
        [[ -n ${!name+set} ]] && continue
        line=$(grep -m1 "^CONFIG_$name=" "$SDKCONFIG" || true)
        [[ -n $line ]] || continue
        value=${line#*=}
        if [[ $value == \"*\" ]]; then # a Kconfig string: quoted, \" and \\ escaped
            value=${value:1:-1}
            value=${value//\\\"/\"}
            value=${value//\\\\/\\}
        fi
        export "$name=$value"
    done
fi
# The C++ firmware's partition table. IDF takes a relative path from the
# project esp-idf-sys generates under target/, so it gets an absolute one.
mkdir -p target
echo "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"$(realpath ../../partitions.csv)\"" > target/sdkconfig.partitions
export ESP_IDF_SDKCONFIG_DEFAULTS="$(realpath ../../sdkconfig.defaults);$PWD/sdkconfig.defaults;$PWD/target/sdkconfig.partitions"
exec cargo "${CARGO_COMMAND:-build}" "$@"
