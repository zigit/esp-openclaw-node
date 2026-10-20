/*
 * room_dns_hook.c — lwip_hook_netconn_external_resolve implementation.
 * See room_dns_hook.h for the design. Compiled with the tailscale_manager
 * component; the hook is wired by CONFIG_LWIP_HOOK_NETCONN_EXT_RESOLVE_CUSTOM.
 */
#include "room_dns_hook.h"

#include <string.h>
#include "lwip/arch.h"
#include "lwip/err.h"
#include "lwip/ip_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define ROOM_DNS_HOOK_MAX 4

typedef struct {
    char name[96];
    uint32_t host_order_ip;
} room_dns_entry_t;

static room_dns_entry_t s_entries[ROOM_DNS_HOOK_MAX];
static SemaphoreHandle_t s_lock;

static void ensure_lock(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
}

void room_dns_hook_set(const char *hostname, uint32_t host_order_ip)
{
    if (hostname == NULL || hostname[0] == '\0') {
        return;
    }
    ensure_lock();
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int slot = -1;
    for (int i = 0; i < ROOM_DNS_HOOK_MAX; i++) {
        if (s_entries[i].name[0] == '\0') {
            if (slot < 0) {
                slot = i; /* keep scanning for an exact match below */
            }
            continue;
        }
        if (strcmp(s_entries[i].name, hostname) == 0) {
            slot = i;
            break;
        }
    }
    if (slot >= 0) {
        if (host_order_ip != 0) {
            strncpy(s_entries[slot].name, hostname, sizeof(s_entries[slot].name) - 1);
            s_entries[slot].name[sizeof(s_entries[slot].name) - 1] = '\0';
        } else {
            s_entries[slot].name[0] = '\0'; /* clear */
        }
        s_entries[slot].host_order_ip = host_order_ip;
    }
    xSemaphoreGive(s_lock);
}

uint32_t room_dns_hook_get(const char *hostname)
{
    if (hostname == NULL) {
        return 0;
    }
    ensure_lock();
    if (s_lock == NULL || s_lock == NULL) {
        return 0;
    }
    uint32_t found = 0;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        for (int i = 0; i < ROOM_DNS_HOOK_MAX; i++) {
            if (s_entries[i].name[0] != '\0' &&
                strcmp(s_entries[i].name, hostname) == 0) {
                found = s_entries[i].host_order_ip;
                break;
            }
        }
        xSemaphoreGive(s_lock);
    }
    return found;
}

/* Hook signature fixed by lwip_default_hooks.h (CONFIG_LWIP_HOOK_NETCONN_EXT_RESOLVE_CUSTOM). */
int lwip_hook_netconn_external_resolve(const char *name, ip_addr_t *addr,
                                       u8_t addrtype, err_t *err)
{
    uint32_t ip = room_dns_hook_get(name);
    if (ip == 0) {
        if (err != NULL) {
            *err = ERR_VAL; /* not ours — let lwIP DNS try */
        }
        return 0;
    }
    if (addrtype == LWIP_DNS_ADDRTYPE_IPV6 || addrtype == LWIP_DNS_ADDRTYPE_IPV6_IPV4) {
        if (err != NULL) {
            *err = ERR_VAL;
        }
        return 0; /* records are A-only */
    }
    ip_addr_t result;
    ip_addr_set_zero_ip4(&result);
    ip4_addr_set_u32(ip_2_ip4(&result), htonl(ip));
    IP_SET_TYPE_VAL(result, IPADDR_TYPE_V4);
    *addr = result;
    if (err != NULL) {
        *err = ERR_OK;
    }
    return 1;
}