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