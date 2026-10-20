#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "microlink.h"   /* microlink_t for the handle accessor below */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * tailscale_manager — microlink wrapper adapted for esp-openclaw-node
 * (room node).
 *
 * Owns the microlink lifecycle (wait-for-WiFi → init → start → state log)
 * so examples keep one call: tailscale_manager_start(). WiFi stays owned
 * by wifi_manager (microlink waits in ML_STATE_WIFI_WAIT by design —
 * verified 2026-10-17: it never touches esp_wifi itself).
 *
 * Flag-gated by CONFIG_TS_ENABLE (default n): the AMOLED example does not
 * require this component, so microlink never links into its binary.
 *
 * Node identity continuity: round board registered as esp32-9b9198
 * (100.108.153.58) during the 2026-10-17 spike; its machine keys live in
 * NVS ns "microlink" and survive reflash (NVS partition identical at
 * 0x9000/24KB in both partition tables). device_name left at the MAC
 * default so the tailnet hostname — and therefore the VPN IP — stays
 * stable for the daemon's lane registry.
 *
 * Auth key: passes through CONFIG_TS_AUTH_KEY. NVS-stored key (from the
 * spike's config server) takes precedence inside microlink when present;
 * the overlay value covers fresh-NVS / key-expiry re-registration.
 */

/**
 * @brief Start the tailscale manager (async).
 *
 * Spawns a task that waits for wifi_manager_is_connected(), initializes
 * microlink with the TS_* Kconfig/overlay settings, and logs state
 * transitions (transitions + 1-min heartbeat only — serial discipline:
 * log level is a performance parameter on USB-CDC boards).
 *
 * Returns ESP_OK immediately when CONFIG_TS_ENABLE is n (no-op, AMOLED-safe).
 * @return ESP_OK on success or intentional no-op
 */
esp_err_t tailscale_manager_start(void);

/** True when microlink reports ML_STATE_CONNECTED. */
bool tailscale_manager_is_connected(void);

/** Our tailnet VPN IP (host byte order), 0 when unknown/disabled. */
uint32_t tailscale_manager_vpn_ip(void);

/** Current microlink state (raw; -1 when the manager is disabled). */
int tailscale_manager_state(void);

/** Opaque microlink handle for socket creation (NULL when not inited). */
microlink_t *tailscale_manager_handle(void);

#ifdef __cplusplus
}
#endif