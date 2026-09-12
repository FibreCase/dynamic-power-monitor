/*
 * ota_update.h — start an over-the-air firmware update.
 *
 * Downloads the new application .bin over HTTP (from the host, whose address
 * comes from config.h) into the passive OTA slot, validates it, switches the
 * boot pointer to it, and reboots. On any failure the device keeps running the
 * current firmware. See ota_update.c for the flow.
 */
#pragma once

/* Spawn the OTA update task and begin the download. Non-blocking: returns
 * immediately, the update runs on its own task and reboots on success. */
void ota_update_start(void);
