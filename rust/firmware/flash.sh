#!/usr/bin/env bash
# Flashes the Rust firmware's app, and only that: into ota_0 (0x20000, as
# ../../partitions.csv has it), over whatever the C++ firmware left there.
# The bootloader, the partition table and NVS - the settings, the roles,
# the Zigbee network - stay as they are, so `../../sunsim flash` puts the
# C++ firmware back with everything intact. Builds first, and saves NVS to
# backups/nvs-<time>.bin before writing anything (it won't flash without
# one; `esptool write-flash 0x9000 <file>` restores it); then the serial
# monitor (Ctrl+] quits). SUNSIM_PORT as for ../../sunsim.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
./build.sh --release
IDF=${SUNSIM_IDF:-$HOME/.espressif/v6.1/esp-idf}
while IFS= read -r line; do
    case $line in
    PATH=*) PATH=${line#PATH=}:$PATH ;;
    SYSTEM_PATH=*) ;;
    *=*) export "${line?}" ;;
    esac
done < <(bash "$HOME/.espressif/tools/activate_idf_$(basename "$(dirname "$IDF")").sh" -e)
export PATH
ports=(/dev/ttyACM*)
PORT=${SUNSIM_PORT:-${ports[0]}}
[[ -e $PORT ]] || { echo "flash.sh: no board found (/dev/ttyACM*); plug it in or set SUNSIM_PORT" >&2; exit 1; }
ELF=target/riscv32imac-esp-espidf/release/sunsim-firmware
# NVS: 80 KB at 0x9000, as ../../partitions.csv has it.
mkdir -p backups
BACKUP=backups/nvs-$(date +%Y%m%d-%H%M%S).bin
esptool --chip esp32c6 -p "$PORT" read-flash 0x9000 0x14000 "$BACKUP"
[[ $(stat -c %s "$BACKUP") -eq $((0x14000)) ]] || { echo "flash.sh: the NVS backup is incomplete; not flashing" >&2; exit 1; }
echo "NVS saved to $BACKUP"
esptool --chip esp32c6 elf2image --flash-size 16MB -o "$ELF.bin" "$ELF"
esptool --chip esp32c6 -p "$PORT" write-flash 0x20000 "$ELF.bin"
exec python -m esp_idf_monitor -p "$PORT" "$ELF"
