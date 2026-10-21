/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * room_heap_census — internal-RAM fragmentation census for the room node.
 *
 * Evidence (2026-10-21 logs): call open/close cycles shatter internal SRAM
 * into <=3KB shards (free 11-17KB, largest 1.3-3.3KB) until a call's own
 * allocations abort the transports (ECONNABORTED with largest=1600) or the
 * audio pipeline cannot reopen (GMF task buffer create fail). This census
 * prints, per stage:
 *   - free/largest per caps class (internal 8bit, internal total, SPIRAM)
 *   - heap_caps_get_info block counts: allocated/free blocks tell SHARDING
 *   - IDF's own per-region heap info summary
 *   - task stack high-water marks (ascending) when trace facility is on
 * Call it at talk lifecycle stages; diffing two stages allocates the cost.
 */

#include "room_heap_census.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#if CONFIG_FREERTOS_USE_TRACE_FACILITY
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#endif

#if CONFIG_HEAP_TASK_TRACKING
#include "esp_heap_task_info.h"
#include <stdlib.h>

/* Persistent totals array: the API keeps its order across calls when the
 * array is left populated, so consecutive census stages diff cleanly. */
#define CENSUS_MAX_TASK_TOTALS 64
static heap_task_totals_t s_task_totals[CENSUS_MAX_TASK_TOTALS];
static size_t s_num_totals;

static void census_per_task(const char *stage, const char *tag)
{
    heap_task_info_params_t params = {0};
    /* Partition 0 counts INTERNAL-only allocations (region caps & INTERNAL
     * mask == INTERNAL). SPIRAM regions have no INTERNAL bit -> excluded. */
    params.caps[0] = MALLOC_CAP_INTERNAL;
    params.mask[0] = MALLOC_CAP_INTERNAL;
    params.totals = s_task_totals;
    params.num_totals = &s_num_totals;
    params.max_totals = CENSUS_MAX_TASK_TOTALS;
    /* blocks=NULL: totals only; scanning per-block details is too heavy
     * for the teardown path. */
    (void)heap_caps_get_per_task_info(&params);

    /* Sort descending by held bytes: biggest holders print first. */
    for (size_t a = 1; a < s_num_totals; a++) {
        heap_task_totals_t key = s_task_totals[a];
        size_t b = a;
        while (b > 0 && s_task_totals[b - 1].size[0] < key.size[0]) {
            s_task_totals[b] = s_task_totals[b - 1];
            b--;
        }
        s_task_totals[b] = key;
    }
    size_t shown = 0;
    uint32_t others_size = 0, others_count = 0;
    for (size_t i = 0; i < s_num_totals; i++) {
        if (s_task_totals[i].size[0] == 0) continue;
        if (shown < 16) {
            const char *name = pcTaskGetName(s_task_totals[i].task);
            if (name == NULL) name = "(deleted-task)";
            ESP_LOGW(tag, "[%s] held %-16s bytes=%u blocks=%u",
                     stage, name,
                     (unsigned)s_task_totals[i].size[0],
                     (unsigned)s_task_totals[i].count[0]);
            shown++;
        } else {
            others_size += s_task_totals[i].size[0];
            others_count += s_task_totals[i].count[0];
        }
    }
    if (others_count > 0) {
        ESP_LOGW(tag, "[%s] held <others> bytes=%u blocks=%u "
                 "(truncated from %u)", stage,
                 (unsigned)others_size, (unsigned)others_count,
                 (unsigned)s_num_totals);
    }
}
#else
static void census_per_task(const char *stage, const char *tag)
{
    (void)stage; (void)tag;
}
#endif /* CONFIG_HEAP_TASK_TRACKING */

#define HEAP_CENSUS_TAG "room_heap"

#if CONFIG_FREERTOS_USE_TRACE_FACILITY
/* Static inventory buffer, never allocate: the census runs at the most
 * fragmented moments (teardown under heap pressure) — a calloc for the
 * task array fails exactly then and silently prints nothing (three
 * captures lost their inventory this way). Bound ~40 tasks x status. */
#define CENSUS_MAX_TASK_STAT 40
static TaskStatus_t s_task_stat[CENSUS_MAX_TASK_STAT];
#endif

void room_heap_census_tick(void)
{
    const uint32_t internal8 = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    size_t i8_free = heap_caps_get_free_size(internal8);
    size_t i8_largest = heap_caps_get_largest_free_block(internal8);
    multi_heap_info_t info = {0};
    heap_caps_get_info(&info, internal8);
    /* One line only: safe from esp_timer context, cheap (heap reads),
     * designed to be diffed across the call's decay curve. */
    ESP_LOGW(HEAP_CENSUS_TAG, "[tick] free=%u largest=%u alloc=%u freeblocks=%u",
             (unsigned)i8_free, (unsigned)i8_largest,
             (unsigned)info.allocated_blocks,
             (unsigned)info.free_blocks);
}

#if CONFIG_OPENCLAW_ROOM_HEAP_CENSUS_REGION_DUMP
#define HEAP_CENSUS_REGION_DUMP() heap_caps_print_heap_info(internal8)
#else
#define HEAP_CENSUS_REGION_DUMP() ((void)0)
#endif

void room_heap_census_log(const char *stage)
{
    const uint32_t internal8 = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    size_t i8_free = heap_caps_get_free_size(internal8);
    size_t i8_largest = heap_caps_get_largest_free_block(internal8);
    size_t i_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t s_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    ESP_LOGW(HEAP_CENSUS_TAG, "[%s] internal8 free=%u largest=%u | "
             "internal_total free=%u | psram free=%u",
             stage,
             (unsigned)i8_free, (unsigned)i8_largest,
             (unsigned)i_free, (unsigned)s_free);

    /* Block counts: many free blocks with a small largest = fragmentation.
     * One big free block = leak pressure instead. */
    multi_heap_info_t info = {0};
    heap_caps_get_info(&info, internal8);
    ESP_LOGW(HEAP_CENSUS_TAG, "[%s] internal8 blocks: alloc=%u free=%u "
             "total_free=%u min_free_ever=%u largest=%u",
             stage,
             (unsigned)info.allocated_blocks,
             (unsigned)info.free_blocks,
             (unsigned)info.total_free_bytes,
             (unsigned)info.minimum_free_bytes,
             (unsigned)info.largest_free_block);

    /* Per-task held-byte attribution (requires HEAP_TASK_TRACKING):
     * names the subsystem holding residual blocks after teardown —
     * esp_peer / GMF / esp-sr / our code. Persistent order = diffable. */
    census_per_task(stage, HEAP_CENSUS_TAG);

    /* IDF's own per-region lines (heap start, size, watermark). Heavy: gated
     * behind OPENCLAW_ROOM_HEAP_CENSUS_REGION_DUMP — its multi-line ESP_LOGW
     * burst on the timer-adjacent teardown path contributed to a Tmr Svc
     * stack overflow under heap pressure (2026-10-21, decoded backtrace). */
    HEAP_CENSUS_REGION_DUMP();

#if CONFIG_FREERTOS_USE_TRACE_FACILITY
    /* Static inventory buffer, never allocate: the census runs at the most
     * fragmented moments (teardown under heap pressure) — a calloc for the
     * task array fails exactly then and silently prints nothing (three
     * captures lost their inventory this way). Bound ~40 tasks. */
    UBaseType_t count = uxTaskGetNumberOfTasks();
    if (count == 0) return;
    UBaseType_t cap = count > CENSUS_MAX_TASK_STAT ? CENSUS_MAX_TASK_STAT
                                                   : count;
    UBaseType_t n = uxTaskGetSystemState(s_task_stat, cap, NULL);
    if (n != 0) {
        /* Ascending by stack HWM: the at-risk tasks print first. */
        for (UBaseType_t a = 1; a < n; a++) {
            TaskStatus_t key = s_task_stat[a];
            UBaseType_t b = a;
            while (b > 0 && s_task_stat[b - 1].usStackHighWaterMark >
                   key.usStackHighWaterMark) {
                s_task_stat[b] = s_task_stat[b - 1];
                b--;
            }
            s_task_stat[b] = key;
        }
        for (UBaseType_t i = 0; i < n; i++) {
            ESP_LOGW(HEAP_CENSUS_TAG, "[%s] task %-16s state=%d prio=%u "
                     "stack_hwm=%u",
                     stage, s_task_stat[i].pcTaskName,
                     (int)s_task_stat[i].eCurrentState,
                     (unsigned)s_task_stat[i].uxCurrentPriority,
                     (unsigned)s_task_stat[i].usStackHighWaterMark);
        }
    }
#endif /* CONFIG_FREERTOS_USE_TRACE_FACILITY */
}