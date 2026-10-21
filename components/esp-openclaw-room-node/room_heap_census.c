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

#if FREERTOS_USE_TRACE_FACILITY
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#endif

#define HEAP_CENSUS_TAG "room_heap"

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

    /* IDF's own per-region lines (heap start, size, watermark). Heavy: gated
     * behind OPENCLAW_ROOM_HEAP_CENSUS_REGION_DUMP — its multi-line ESP_LOGW
     * burst on the timer-adjacent teardown path contributed to a Tmr Svc
     * stack overflow under heap pressure (2026-10-21, decoded backtrace). */
    HEAP_CENSUS_REGION_DUMP();

#if FREERTOS_USE_TRACE_FACILITY
    UBaseType_t count = uxTaskGetNumberOfTasks();
    if (count == 0) return;
    TaskStatus_t *tasks = calloc(count + 8, sizeof(TaskStatus_t));
    if (tasks == NULL) return;
    UBaseType_t n = uxTaskGetSystemState(tasks, count + 8, NULL);
    if (n != 0) {
        /* Ascending by stack HWM: the at-risk tasks print first. */
        for (UBaseType_t a = 1; a < n; a++) {
            TaskStatus_t key = tasks[a];
            UBaseType_t b = a;
            while (b > 0 && tasks[b - 1].usStackHighWaterMark >
                   key.usStackHighWaterMark) {
                tasks[b] = tasks[b - 1];
                b--;
            }
            tasks[b] = key;
        }
        for (UBaseType_t i = 0; i < n; i++) {
            ESP_LOGW(HEAP_CENSUS_TAG, "[%s] task %-16s state=%d prio=%u "
                     "stack_hwm=%u cpu=%d",
                     stage, tasks[i].pcTaskName,
                     (int)tasks[i].eCurrentState,
                     (unsigned)tasks[i].uxCurrentPriority,
                     (unsigned)tasks[i].usStackHighWaterMark,
                     (int)tasks[i].xCoreID);
        }
    }
    free(tasks);
#endif
}