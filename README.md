# openevse-16mb-migrator

In-system migration of a 16 MB ESP32 from the OpenEVSE **4 MB** layout
(`min_spiffs`) to the **16 MB** layout (`openevse_16mb`), leaving a working
OpenEVSE-16 MB install. This is the commit step
[PR #1136](https://github.com/OpenEVSE/openevse_esp32_firmware/pull/1136) needs
but its Arduino firmware can't perform — the prebuilt Arduino `spi_flash` lib
bakes in `DANGEROUS_WRITE_ABORTS`. It's a tiny ESP-IDF app built with
`CONFIG_SPI_FLASH_DANGEROUS_WRITE_ALLOWED=y`, so the modern `esp_flash` driver
writes the protected `0x1000`/`0x8000` regions correctly (no ROM funcs).

## What it does

Pre-staged before it runs (field: downloaded + staged by OpenEVSE-4 MB; bench:
placed by esptool): the real OpenEVSE 16 MB app at ota_1 = `0x650000`. The 16 MB
bootloader + partition table are embedded (`src/openevse_bl.h` / `openevse_pt.h`).

Commit order keeps the device always old-OpenEVSE-or-new, never neither:
1. refuse to proceed unless a valid app image (magic `0xE9`) is staged at `0x650000`
2. write + read-back-verify the OpenEVSE bootloader → `0x1000`
3. write + read-back-verify the `openevse_16mb` partition table → `0x8000`
4. point otadata → ota_1 (`esp_rom_crc32_le` for the OTA-select CRC)
5. reboot → the real OpenEVSE 16 MB boots from `0x650000`

`nvs` @`0x9000` and `otadata` @`0xe000` are at identical offsets in both layouts;
`nvs` is never touched, so WiFi creds survive.

## Result — HW-validated (16 MB ESP32-D0WD-V3, IDF 5.5)

Migrator log:
```
staged 16MB app present @0x650000 (image magic 0xE9). committing ...
    bootloader @0x1000 OK (25024 B written + verified)
    partition table @0x8000 OK (3072 B written + verified)
    otadata OK (seq=2, crc=0x55f63774)
COMMIT COMPLETE. rebooting into OpenEVSE 16MB (ota_1).
```
…then the OpenEVSE 16 MB app boots (LittleFS init + `$GV` RAPI poll = main loop).

Deterministic read-back of the migrated chip:
```
bootloader @0x1000  ==  OpenEVSE 16MB bootloader.bin : True
otadata    @0xe000  ota_seq = 2  ->  boots ota_1
partition table @0x8000 == openevse_16mb (app1/ota_1 @0x650000, etc.)
```

## Bench setup

```sh
# 1. build the real 16 MB OpenEVSE firmware (staged app + embedded bootloader/PT)
( cd <openevse_esp32_firmware> && pio run -e openevse_wifi_v1_16mb )

# 2. regenerate the embedded bootloader/PT headers from that build
python3 scripts/gen_embeds.py <openevse_esp32_firmware>/.pio/build/openevse_wifi_v1_16mb

# 3. build the migrator
pio run

# 4. flash migrator + stage the OpenEVSE app in one shot, then reset
esptool --chip esp32 -p <PORT> write_flash --flash_size detect \
  0x1000   .pio/build/esp32dev/bootloader.bin \
  0x8000   .pio/build/esp32dev/partitions.bin \
  0x10000  .pio/build/esp32dev/firmware.bin \
  0x650000 <openevse_esp32_firmware>/.pio/build/openevse_wifi_v1_16mb/firmware.bin
```

## Safety / field notes

- The only dangerous write is the ~25 KB bootloader (`0x1000`); it is written +
  verified before the single partition-table sector flip, so the point of no
  return is one sector. `nvs` (creds) and the staged app are never at risk.
- Deepest "working install" check (WiFi AP, web UI, EVSE control) needs a real
  OpenEVSE unit — the bench proves the migrated layout + that OpenEVSE boots and
  runs its main loop, not the full app surface.
- **Front-half still TODO for a field OTA:** OpenEVSE-4 MB downloads the 16 MB
  bootloader/PT/app, stages them (app → `0x650000`, in the free upper flash a 4 MB
  layout doesn't reach), OTAs this migrator into a slot, and reboots into it —
  reusing PR #1136's existing download/verify/stage pipeline.
