/*
 * Fighting Game Controller - Sender
 * Hardware:  Seeed XIAO nRF52840
 * Protocol:  Nordic Enhanced ShockBurst (ESB) @ 2 Mbps
 * Latency:   ~1-2 ms end-to-end
 *
 * Pin map (XIAO nRF52840 silk → nRF52840 port):
 *   D0  P0.02  → UP
 *   D1  P0.03  → DOWN
 *   D2  P0.28  → LEFT
 *   D3  P0.29  → RIGHT
 *   D4  P0.04  → LP  (Light Punch)
 *   D5  P0.05  → MP  (Medium Punch)
 *   D6  P1.11  → HP  (Hard Punch)
 *   D7  P1.12  → LK  (Light Kick)
 *   D8  P1.13  → MK  (Medium Kick)
 *   D9  P1.14  → HK  (Hard Kick)
 *   D10 P1.15  → START
 *
 * All buttons are active-LOW (switch connects pin to GND).
 * Internal pull-ups are enabled by this firmware.
 *
 * SOCD mode is selected by holding buttons at power-on:
 *   Nothing held  → Neutral   (L+R = none, U+D = none)
 *   LP held       → Last Win  (most recent direction wins)
 *   MP held       → 2nd Win   (Hitbox standard: second press wins)
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <esb.h>
#include <string.h>

LOG_MODULE_REGISTER(controller, LOG_LEVEL_INF);

/* ── Packet (4 bytes, shared with dongle) ────────────────────────────────── */

typedef struct __packed {
    uint16_t buttons;  /* bit N = action button N pressed (active-high in packet) */
    uint8_t  dpad;     /* bits 3:0 = U/D/L/R after SOCD cleaning                 */
    uint8_t  crc;      /* CRC-8/MAXIM over bytes 0–2                              */
} gamepad_packet_t;

#define DPAD_UP    BIT(0)
#define DPAD_DOWN  BIT(1)
#define DPAD_LEFT  BIT(2)
#define DPAD_RIGHT BIT(3)

/* Action button bit positions in packet.buttons */
enum btn_idx {
    BTN_LP = 0, BTN_MP, BTN_HP,
    BTN_LK,     BTN_MK, BTN_HK,
    BTN_START,
    BTN_COUNT
};

/* ── ESB address (must match dongle exactly) ─────────────────────────────── */

static uint8_t esb_base_addr[4]  = {0x5A, 0x6B, 0x7C, 0x8D};
static uint8_t esb_addr_prefix[] = {0xF0};

/* ── GPIO specs from device tree ─────────────────────────────────────────── */

static const struct gpio_dt_spec dpad_pins[] = {
    GPIO_DT_SPEC_GET(DT_ALIAS(dpad_up),    gpios),  /* DPAD_UP    */
    GPIO_DT_SPEC_GET(DT_ALIAS(dpad_down),  gpios),  /* DPAD_DOWN  */
    GPIO_DT_SPEC_GET(DT_ALIAS(dpad_left),  gpios),  /* DPAD_LEFT  */
    GPIO_DT_SPEC_GET(DT_ALIAS(dpad_right), gpios),  /* DPAD_RIGHT */
};

static const struct gpio_dt_spec btn_pins[] = {
    GPIO_DT_SPEC_GET(DT_ALIAS(btn_lp),    gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(btn_mp),    gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(btn_hp),    gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(btn_lk),    gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(btn_mk),    gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(btn_hk),    gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(btn_start), gpios),
};

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

/* ── SOCD cleaning ───────────────────────────────────────────────────────── */

typedef enum {
    SOCD_NEUTRAL     = 0, /* L+R = none, U+D = none (tournament default) */
    SOCD_LAST_WIN    = 1, /* most recently pressed direction takes priority */
    SOCD_SECOND_WIN  = 2, /* second press wins (Hitbox standard)           */
    SOCD_MODE_COUNT
} socd_mode_t;

static socd_mode_t socd_mode = SOCD_NEUTRAL;
static uint8_t     prev_raw_dpad = 0;

static uint8_t socd_clean(uint8_t raw)
{
    uint8_t out = raw;

    switch (socd_mode) {

    case SOCD_NEUTRAL:
        if ((raw & DPAD_LEFT) && (raw & DPAD_RIGHT))
            out &= ~(DPAD_LEFT | DPAD_RIGHT);
        if ((raw & DPAD_UP)   && (raw & DPAD_DOWN))
            out &= ~(DPAD_UP | DPAD_DOWN);
        break;

    case SOCD_LAST_WIN:
        if ((raw & DPAD_LEFT) && (raw & DPAD_RIGHT)) {
            /* Bits that are NEW this cycle (just pressed) */
            uint8_t new_h = (raw ^ prev_raw_dpad) & raw & (DPAD_LEFT | DPAD_RIGHT);
            if      (new_h & DPAD_LEFT)  out &= ~DPAD_RIGHT;
            else if (new_h & DPAD_RIGHT) out &= ~DPAD_LEFT;
            else                         out &= ~(DPAD_LEFT | DPAD_RIGHT);
        }
        if ((raw & DPAD_UP) && (raw & DPAD_DOWN)) {
            uint8_t new_v = (raw ^ prev_raw_dpad) & raw & (DPAD_UP | DPAD_DOWN);
            if      (new_v & DPAD_UP)   out &= ~DPAD_DOWN;
            else if (new_v & DPAD_DOWN) out &= ~DPAD_UP;
            else                        out &= ~(DPAD_UP | DPAD_DOWN);
        }
        break;

    case SOCD_SECOND_WIN:
        if ((raw & DPAD_LEFT) && (raw & DPAD_RIGHT)) {
            uint8_t prev_h = prev_raw_dpad & (DPAD_LEFT | DPAD_RIGHT);
            if      (prev_h == DPAD_LEFT)  out &= ~DPAD_LEFT;   /* left was first, right wins */
            else if (prev_h == DPAD_RIGHT) out &= ~DPAD_RIGHT;  /* right was first, left wins */
            else                           out &= ~(DPAD_LEFT | DPAD_RIGHT);
        }
        if ((raw & DPAD_UP) && (raw & DPAD_DOWN)) {
            uint8_t prev_v = prev_raw_dpad & (DPAD_UP | DPAD_DOWN);
            if      (prev_v == DPAD_UP)   out &= ~DPAD_UP;
            else if (prev_v == DPAD_DOWN) out &= ~DPAD_DOWN;
            else                          out &= ~(DPAD_UP | DPAD_DOWN);
        }
        break;

    default:
        break;
    }

    prev_raw_dpad = raw;
    return out;
}

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

/* ── Input reading ───────────────────────────────────────────────────────── */

static gamepad_packet_t read_inputs(void)
{
    gamepad_packet_t pkt = {0};

    for (int i = 0; i < BTN_COUNT; i++) {
        if (!gpio_pin_get_dt(&btn_pins[i]))   /* active-LOW */
            pkt.buttons |= BIT(i);
    }

    uint8_t raw = 0;
    if (!gpio_pin_get_dt(&dpad_pins[0])) raw |= DPAD_UP;
    if (!gpio_pin_get_dt(&dpad_pins[1])) raw |= DPAD_DOWN;
    if (!gpio_pin_get_dt(&dpad_pins[2])) raw |= DPAD_LEFT;
    if (!gpio_pin_get_dt(&dpad_pins[3])) raw |= DPAD_RIGHT;

    pkt.dpad = socd_clean(raw);
    pkt.crc  = crc8((const uint8_t *)&pkt, offsetof(gamepad_packet_t, crc));
    return pkt;
}

/* ── ESB ─────────────────────────────────────────────────────────────────── */

static volatile bool tx_done = true;

static void esb_event_handler(struct esb_evt const *event)
{
    switch (event->evt_id) {
    case ESB_EVENT_TX_SUCCESS:
        tx_done = true;
        gpio_pin_set_dt(&led, 1);
        break;
    case ESB_EVENT_TX_FAILED:
        tx_done = true;
        gpio_pin_set_dt(&led, 0);
        LOG_DBG("ESB TX failed");
        break;
    case ESB_EVENT_RX_RECEIVED:
        break;  /* ACKs handled by hardware */
    }
}

static int esb_controller_init(void)
{
    struct esb_config cfg = ESB_DEFAULT_CONFIG;
    cfg.protocol          = ESB_PROTOCOL_ESB_DPL;
    cfg.mode              = ESB_MODE_PTX;
    cfg.bitrate           = ESB_BITRATE_2MBPS;
    cfg.crc               = ESB_CRC_16BIT;
    cfg.tx_output_power   = ESB_TX_POWER_4DBM;
    cfg.retransmit_count  = 3;
    cfg.retransmit_delay  = 250;  /* µs */
    cfg.event_handler     = esb_event_handler;
    cfg.selective_auto_ack = false;

    int err = esb_init(&cfg);
    if (err) return err;

    err = esb_set_base_address_0(esb_base_addr);
    if (err) return err;

    return esb_set_prefixes(esb_addr_prefix, ARRAY_SIZE(esb_addr_prefix));
}

static void transmit(const gamepad_packet_t *pkt)
{
    struct esb_payload payload = {
        .pipe  = 0,
        .noack = false,
        .length = sizeof(gamepad_packet_t),
    };
    memcpy(payload.data, pkt, sizeof(gamepad_packet_t));

    esb_flush_tx();
    tx_done = false;
    if (esb_write_payload(&payload) != 0) {
        tx_done = true;
        LOG_ERR("esb_write_payload failed");
        return;
    }
    esb_start_tx();
}

/* ── Boot: detect SOCD mode from held buttons ────────────────────────────── */

static void detect_socd_mode(void)
{
    /* Hold at power-on: nothing=Neutral, LP=LastWin, MP=SecondWin */
    if      (!gpio_pin_get_dt(&btn_pins[BTN_LP])) socd_mode = SOCD_LAST_WIN;
    else if (!gpio_pin_get_dt(&btn_pins[BTN_MP])) socd_mode = SOCD_SECOND_WIN;
    else                                           socd_mode = SOCD_NEUTRAL;

    LOG_INF("SOCD mode: %d (%s)",
            socd_mode,
            socd_mode == SOCD_NEUTRAL    ? "Neutral" :
            socd_mode == SOCD_LAST_WIN   ? "Last Win" : "2nd Win");

    /* Flash LED: 1=Neutral, 2=LastWin, 3=SecondWin */
    for (int i = 0; i <= (int)socd_mode; i++) {
        gpio_pin_set_dt(&led, 1);
        k_sleep(K_MSEC(120));
        gpio_pin_set_dt(&led, 0);
        k_sleep(K_MSEC(120));
    }
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    LOG_INF("Fighting Gamepad Controller starting");

    gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);

    for (int i = 0; i < BTN_COUNT; i++)
        gpio_pin_configure_dt(&btn_pins[i], GPIO_INPUT | GPIO_PULL_UP);

    for (int i = 0; i < 4; i++)
        gpio_pin_configure_dt(&dpad_pins[i], GPIO_INPUT | GPIO_PULL_UP);

    /* Settle time for pins + SOCD mode detection */
    k_sleep(K_MSEC(50));
    detect_socd_mode();

    if (esb_controller_init() != 0) {
        LOG_ERR("ESB init failed – halting");
        while (1) k_sleep(K_FOREVER);
    }

    LOG_INF("ESB ready @ 2 Mbps");

    gamepad_packet_t prev = {.buttons = 0xFFFF, .dpad = 0xFF, .crc = 0};

    while (1) {
        gamepad_packet_t cur = read_inputs();

        /* Only transmit when state changes, or every 8 ms as a keepalive */
        bool changed = (cur.buttons != prev.buttons || cur.dpad != prev.dpad);

        if (changed && tx_done) {
            prev = cur;
            transmit(&cur);
        }

        /* 500 µs poll → 2000 Hz input sampling rate */
        k_sleep(K_USEC(500));
    }

    return 0;
}
