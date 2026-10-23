/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/* Internal-RAM fragmentation census: caps totals, block counts, IDF
 * per-region info, and (with CONFIG_FREERTOS_USE_TRACE_FACILITY) task
 * stack high-water inventory. Diagnostic; call at talk lifecycle stages
 * and diff the output. */
void room_heap_census_log(const char *stage);

/* Lightweight one-liner for timers: internal8 free/largest + block counts
 * in a single ESP_LOGW. Safe from esp_timer callbacks (heap reads only). */
void room_heap_census_tick(void);

/* Per-task CPU-share probe (top-3 consumers as % of the window). Names the
 * task starving taskLVGL at idle — the call-gated room_talk_cpu cannot see
 * an idle hog. Requires CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS (pinned in
 * sdkconfig.defaults); a no-op stub otherwise. Safe from esp_timer context
 * (static buffers, no allocation). */
void room_cpu_probe_tick(void);