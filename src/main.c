// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andrew Rankin

/*
 * openevse-16mb-migrator
 * ----------------------
 * In-system migration of a 16MB ESP32 from the OpenEVSE 4MB layout (min_spiffs)
 * to the 16MB layout (openevse_16mb), leaving a WORKING OpenEVSE-16MB install.
 * Runs as a tiny ESP-IDF app (CONFIG_SPI_FLASH_DANGEROUS_WRITE_ALLOWED) -- the
 * protected-region commit PR #1136 needs but its Arduino firmware can't perform.
 *
 * Pre-staged before this runs (in the field: downloaded + staged by OpenEVSE-4MB;
 * on the bench: placed by esptool): the real OpenEVSE 16MB app at ota_1 = 0x650000.
 * The 16MB bootloader and partition table are embedded here (small).
 *
 * Commit order is chosen so the device is always old-OpenEVSE-or-new, never
 * neither: verify the staged app first; write+verify the bootloader; write+verify
 * the partition table; point otadata at ota_1; reboot. nvs @0x9000 is never
 * touched, so WiFi creds survive.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_flash.h"
#include "esp_rom_crc.h"
#include "openevse_bl.h"   /* openevse_bl[],  openevse_bl_len  -- 16MB bootloader */
#include "openevse_pt.h"   /* openevse_pt[],  openevse_pt_len  -- 16MB partition table */

static const char *TAG = "migrator";

#define BL_OFFSET    0x1000u    /* bootloader */
#define BL_REGION    0x7000u    /* 0x1000..0x8000 is reserved for the bootloader */
#define PT_OFFSET    0x8000u    /* partition table */
#define PT_SECTOR    0x1000u
#define OTADATA_OFF  0xe000u
#define OTADATA_SIZE 0x2000u
#define APP_STAGED   0x650000u  /* new ota_1: where the 16MB OpenEVSE app is staged */

static uint8_t verbuf[0x1000];

static bool write_verify(uint32_t off, const uint8_t *data, uint32_t len,
                         uint32_t erase_len, const char *what)
{
    ESP_LOGW(TAG, ">>> %s: erase 0x%lx (%lu B) then write %lu B @0x%lx", what,
             (unsigned long)off, (unsigned long)erase_len,
             (unsigned long)len, (unsigned long)off);
    esp_err_t e = esp_flash_erase_region(esp_flash_default_chip, off, erase_len);
    if (e != ESP_OK) { ESP_LOGE(TAG, "    erase -> %s", esp_err_to_name(e)); return false; }
    e = esp_flash_write(esp_flash_default_chip, data, off, len);
    if (e != ESP_OK) { ESP_LOGE(TAG, "    write -> %s", esp_err_to_name(e)); return false; }

    for (uint32_t done = 0; done < len; ) {
        uint32_t n = len - done; if (n > sizeof(verbuf)) n = sizeof(verbuf);
        if (esp_flash_read(esp_flash_default_chip, verbuf, off + done, n) != ESP_OK) {
            ESP_LOGE(TAG, "    verify read failed"); return false;
        }
        if (memcmp(verbuf, data + done, n) != 0) {
            ESP_LOGE(TAG, "    verify MISMATCH at +0x%lx", (unsigned long)done); return false;
        }
        done += n;
    }
    ESP_LOGI(TAG, "    %s OK (%lu B written + verified)", what, (unsigned long)len);
    return true;
}

/* Point otadata at ota_1 (where the staged app lives) with a bootloader-valid CRC. */
static bool select_ota1(void)
{
    struct __attribute__((packed)) {
        uint32_t ota_seq;
        uint8_t  seq_label[20];
        uint32_t ota_state;
        uint32_t crc;
    } e;
    memset(&e, 0xFF, sizeof(e));
    e.ota_seq   = 2;            /* (2 - 1) % 2 == 1  -> boot ota_1 */
    e.ota_state = 0xFFFFFFFF;   /* UNDEFINED; ignored unless rollback is enabled */
    e.crc       = esp_rom_crc32_le(UINT32_MAX, (const uint8_t *)&e.ota_seq, sizeof(e.ota_seq));

    ESP_LOGW(TAG, ">>> otadata: erase 0x%x then write ota_select(seq=2 -> ota_1)", OTADATA_OFF);
    if (esp_flash_erase_region(esp_flash_default_chip, OTADATA_OFF, OTADATA_SIZE) != ESP_OK) return false;
    if (esp_flash_write(esp_flash_default_chip, &e, OTADATA_OFF, sizeof(e)) != ESP_OK) return false;
    ESP_LOGI(TAG, "    otadata OK (seq=2, crc=0x%08lx)", (unsigned long)e.crc);
    return true;
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGI(TAG, "======= OpenEVSE 4MB -> 16MB in-system migrator =======");
    uint32_t fsz = 0; esp_flash_get_size(esp_flash_default_chip, &fsz);
    ESP_LOGI(TAG, "flash %lu MB | staged app @0x%x | bootloader %u B | PT %u B",
             (unsigned long)(fsz / 1048576), APP_STAGED, openevse_bl_len, openevse_pt_len);

    /* 1) refuse to touch anything unless the new app is actually staged */
    uint8_t magic = 0;
    esp_flash_read(esp_flash_default_chip, &magic, APP_STAGED, 1);
    if (magic != 0xE9) {
        ESP_LOGE(TAG, "no app image @0x%x (magic=0x%02x) -- not staged. ABORT, nothing written.",
                 APP_STAGED, magic);
        while (1) { vTaskDelay(pdMS_TO_TICKS(5000)); }
    }
    ESP_LOGI(TAG, "staged 16MB app present @0x%x (image magic 0xE9). committing ...", APP_STAGED);

    /* 2) bootloader, 3) partition table, 4) otadata -- in that order */
    if (!write_verify(BL_OFFSET, openevse_bl, openevse_bl_len, BL_REGION, "bootloader @0x1000")) goto fail;
    if (!write_verify(PT_OFFSET, openevse_pt, openevse_pt_len, PT_SECTOR, "partition table @0x8000")) goto fail;
    if (!select_ota1()) goto fail;

    ESP_LOGI(TAG, "*******************************************************");
    ESP_LOGI(TAG, "COMMIT COMPLETE. rebooting into OpenEVSE 16MB (ota_1).");
    ESP_LOGI(TAG, "watch for the OpenEVSE / Arduino boot banner below.");
    ESP_LOGI(TAG, "*******************************************************");
    vTaskDelay(pdMS_TO_TICKS(2500));
    esp_restart();

fail:
    ESP_LOGE(TAG, "commit FAILED. If the bootloader write finished, a re-run is safe;");
    ESP_LOGE(TAG, "otherwise reflash over USB (bench board -- recover with esptool).");
    while (1) { vTaskDelay(pdMS_TO_TICKS(5000)); }
}
