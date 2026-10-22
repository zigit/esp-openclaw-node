set(SRCS
    "tailscale_manager.c"
    "src/room_dns_hook.c"
)

set(REQUIRES
    microlink
    esp_netif
    nvs_flash
)   # esp-openclaw-node adaptation: clawkie's wifi_manager / clawkie_protocol /
    # audio_pipeline / status_led deps are gone — the room node owns its own
    # station and transport, and the WG voice lane is intentionally absent.

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "include"
    REQUIRES ${REQUIRES}
    PRIV_REQUIRES esp_timer
)