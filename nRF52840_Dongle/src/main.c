/*
 * Fighting Game Dongle - Receiver
 * Hardware:  Seeed XIAO nRF52840  OR  Nordic nRF52840 Dongle (PCA10059)
 * Protocol:  Nordic Enhanced ShockBurst (ESB) @ 2 Mbps receive
 * USB:       HID Gamepad @ 1000 Hz (1 ms poll interval)
 *
 * The dongle plugs into the PC via USB-C (XIAO) or USB-A (PCA10059).
 * It appears as a standard USB HID Gamepad — no drivers needed.
 *
 * HID report layout (4 bytes):
 *   [0]     buttons  7:0   (LP/MP/HP/LK/MK/HK/START + 1 spare)
 *   [1]     buttons 15:8   (reserved / future)
 *   [2][3:0] hat switch    (0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW, 8=none)
 *   [2][7:4] padding
 *   [3]     padding
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/usb_hid.h>
#include <zephyr/logging/log.h>
#include <esb.h>
#include <string.h>

LOG_MODULE_REGISTER(dongle, LOG_LEVEL_INF);

/* ── Shared packet definition (must match controller exactly) ────────────── */

typedef struct __packed {
    uint16_t buttons;
    uint8_t  dpad;
    uint8_t  crc;
} gamepad_packet_t;

#define DPAD_UP    BIT(0)
#define DPAD_DOWN  BIT(1)
#define DPAD_LEFT  BIT(2)
#define DPAD_RIGHT BIT(3)

/* ── ESB address (must match controller exactly) ─────────────────────────── */

static uint8_t esb_base_addr[4]  = {0x5A, 0x6B, 0x7C, 0x8D};
static uint8_t esb_addr_prefix[] = {0xF0};

/* ── CRC-8/MAXIM (poly 0x31, reflected) ─────────────────────────────────── */

static uint8_t crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x01) ? ((crc >> 1) ^ 0x8C) : (crc >> 1);
    }
    return crc;
}

/* ── USB HID descriptor (Gamepad, 4-byte report, 1 ms bInterval) ─────────── */
/*
 * Report format:
 *   16 buttons (2 bytes)
 *    4-bit hat switch (0–7 = direction, 8 = neutral)
 *    4-bit padding
 *    8-bit padding
 */
static const uint8_t hid_report_desc[] = {
    /* Usage Page: Generic Desktop */
    0x05, 0x01,
    /* Usage: Gamepad */
    0x09, 0x05,
    /* Collection: Application */
    0xA1, 0x01,

        /* ── 16 buttons ── */
        0x05, 0x09,        /* Usage Page: Button */
        0x19, 0x01,        /* Usage Minimum: Button 1 */
        0x29, 0x10,        /* Usage Maximum: Button 16 */
        0x15, 0x00,        /* Logical Minimum: 0 */
        0x25, 0x01,        /* Logical Maximum: 1 */
        0x75, 0x01,        /* Report Size: 1 */
        0x95, 0x10,        /* Report Count: 16 */
        0x81, 0x02,        /* Input: Data, Variable, Absolute */

        /* ── Hat switch (D-pad) ── */
        0x05, 0x01,        /* Usage Page: Generic Desktop */
        0x09, 0x39,        /* Usage: Hat Switch */
        0x15, 0x00,        /* Logical Minimum: 0 */
        0x25, 0x07,        /* Logical Maximum: 7 */
        0x35, 0x00,        /* Physical Minimum: 0 */
        0x46, 0x3B, 0x01,  /* Physical Maximum: 315 */
        0x65, 0x14,        /* Unit: Eng Rotation, degrees */
        0x75, 0x04,        /* Report Size: 4 */
        0x95, 0x01,        /* Report Count: 1 */
        0x81, 0x42,        /* Input: Data, Variable, Absolute, Null state */

        /* ── 4-bit + 8-bit padding ── */
        0x75, 0x04,        /* Report Size: 4 */
        0x95, 0x01,        /* Report Count: 1 */
        0x81, 0x03,        /* Input: Constant, Variable, Absolute */
        0x75, 0x08,        /* Report Size: 8 */
        0x95, 0x01,        /* Report Count: 1 */
        0x81, 0x03,        /* Input: Constant, Variable, Absolute */

    /* End Collection */
    0xC0,
};

/*
 * dpad bitmask → HID hat value
 * Hat: 0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW, 8=neutral
 *
 * Index = dpad nibble (bits: RLDU)
 */
static const uint8_t dpad_to_hat[16] = {
    8,  /* 0000: none       */
    0,  /* 0001: U          → N  */
    4,  /* 0010: D          → S  */
    8,  /* 0011: U+D        → neutral (SOCD cleaned upstream) */
    6,  /* 0100: L          → W  */
    7,  /* 0101: U+L        → NW */
    5,  /* 0110: D+L        → SW */
    8,  /* 0111: impossible */
    2,  /* 1000: R          → E  */
    1,  /* 1001: U+R        → NE */
    3,  /* 1010: D+R        → SE */
    8,  /* 1011: impossible */
    8,  /* 1100: L+R        → neutral (SOCD cleaned upstream) */
    8,  /* 1101: impossible */
    8,  /* 1110: impossible */
    8,  /* 1111: impossible */
};

/* ── HID report buffer (4 bytes) ─────────────────────────────────────────── */

static uint8_t              hid_report[4];
static const struct device *hid_dev;

static void build_hid_report(const gamepad_packet_t *pkt)
{
    hid_report[0] =  pkt->buttons & 0xFF;
    hid_report[1] = (pkt->buttons >> 8) & 0xFF;
    hid_report[2] =  dpad_to_hat[pkt->dpad & 0x0F] & 0x0F;
    hid_report[3] =  0;
}

/* ── ESB receive state ───────────────────────────────────────────────────── */

static K_MUTEX_DEFINE(pkt_mutex);
static gamepad_packet_t latest_pkt;
static volatile bool    rx_pending = false;

static void esb_event_handler(struct esb_evt const *event)
{
    if (event->evt_id != ESB_EVENT_RX_RECEIVED) return;

    struct esb_payload rx;
    while (esb_read_rx_payload(&rx) == 0) {
        if (rx.length < sizeof(gamepad_packet_t)) continue;

        gamepad_packet_t pkt;
        memcpy(&pkt, rx.data, sizeof(pkt));

        uint8_t expected = crc8((const uint8_t *)&pkt,
                                offsetof(gamepad_packet_t, crc));
        if (pkt.crc != expected) {
            LOG_DBG("CRC mismatch – dropped");
            continue;
        }

        k_mutex_lock(&pkt_mutex, K_NO_WAIT);
        latest_pkt = pkt;
        rx_pending = true;
        k_mutex_unlock(&pkt_mutex);
    }
}

static int esb_dongle_init(void)
{
    struct esb_config cfg = ESB_DEFAULT_CONFIG;
    cfg.protocol          = ESB_PROTOCOL_ESB_DPL;
    cfg.mode              = ESB_MODE_PRX;
    cfg.bitrate           = ESB_BITRATE_2MBPS;
    cfg.crc               = ESB_CRC_16BIT;
    cfg.event_handler     = esb_event_handler;
    cfg.selective_auto_ack = false;

    int err = esb_init(&cfg);
    if (err) return err;

    err = esb_set_base_address_0(esb_base_addr);
    if (err) return err;

    return esb_set_prefixes(esb_addr_prefix, ARRAY_SIZE(esb_addr_prefix));
}

/* ── USB HID send thread (runs at ~2 kHz, sends USB report every 1 ms) ───── */

#define HID_THREAD_STACK 1024
#define HID_THREAD_PRIO  5
#define WATCHDOG_MS      300  /* release all inputs if controller drops out */

static void hid_send_thread(void *a, void *b, void *c)
{
    static const gamepad_packet_t safe_pkt = {0};  /* all released */
    uint32_t last_rx_ms   = k_uptime_get_32();
    uint32_t last_send_ms = k_uptime_get_32();
    bool     watchdog_active = false;

    while (1) {
        uint32_t now = k_uptime_get_32();
        gamepad_packet_t pkt;
        bool send = false;

        k_mutex_lock(&pkt_mutex, K_NO_WAIT);
        if (rx_pending) {
            pkt        = latest_pkt;
            rx_pending = false;
            send       = true;
            last_rx_ms = now;
            watchdog_active = false;
        }
        k_mutex_unlock(&pkt_mutex);

        /* Watchdog: release all inputs after WATCHDOG_MS with no packet */
        if (!watchdog_active && (now - last_rx_ms) > WATCHDOG_MS) {
            pkt = safe_pkt;
            send = true;
            watchdog_active = true;
            LOG_WRN("Controller lost – inputs released");
        }

        /* Always send at least once per ms to maintain 1000 Hz USB polling */
        if (!send && (now - last_send_ms) >= 1) {
            k_mutex_lock(&pkt_mutex, K_NO_WAIT);
            pkt = latest_pkt;
            k_mutex_unlock(&pkt_mutex);
            send = true;
        }

        if (send) {
            build_hid_report(&pkt);
            hid_int_ep_write(hid_dev, hid_report, sizeof(hid_report), NULL);
            last_send_ms = now;
        }

        k_usleep(500);  /* 500 µs → 2 kHz check rate */
    }
}

K_THREAD_DEFINE(hid_thread_id, HID_THREAD_STACK,
                hid_send_thread, NULL, NULL, NULL,
                HID_THREAD_PRIO, 0, 0);

/* ── USB HID callbacks (no output reports needed) ────────────────────────── */

static const struct hid_ops hid_callbacks = { 0 };

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    LOG_INF("Fighting Gamepad Dongle starting");

    hid_dev = device_get_binding("HID_0");
    if (!hid_dev) {
        LOG_ERR("HID device not found");
        return -1;
    }

    usb_hid_register_device(hid_dev,
                             hid_report_desc,
                             sizeof(hid_report_desc),
                             &hid_callbacks);

    usb_hid_init(hid_dev);

    if (usb_enable(NULL) != 0) {
        LOG_ERR("USB enable failed");
        return -1;
    }

    if (esb_dongle_init() != 0) {
        LOG_ERR("ESB init failed");
        return -1;
    }

    esb_start_rx();

    LOG_INF("ESB listening, USB HID @ 1000 Hz");

    /* main thread is idle — work is done in hid_send_thread + ESB callbacks */
    while (1) {
        k_sleep(K_FOREVER);
    }

    return 0;
}
