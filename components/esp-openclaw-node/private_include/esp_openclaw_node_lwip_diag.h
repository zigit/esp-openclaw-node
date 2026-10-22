/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/* Component-internal lwIP fault snapshot. Reads lwip_stats only;
 * never mutates network state. Requires CONFIG_LWIP_STATS=y to have
 * content; compiles to a no-op otherwise. Call at transport failure
 * observation points (DISCONNECTED / ERROR with a captured TLS error). */
void esp_openclaw_node_lwip_diag_dump(const char *reason);