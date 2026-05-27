#include <string.h>
#include <stdlib.h>
#include "brain.h"
#include "board.h"

#if defined(BRAIN_HEARTBEAT) && BRAIN_HEARTBEAT
extern void brain_heartbeat_init(void);
extern void brain_heartbeat_tick(void);
#endif

#define PROBE_INTERVAL_MS  3000u

extern awbus_port_t *brain_bus_port_init(void);

static const arch_api_t *A;
static struct konsole        g_ks;
static struct kon_line_state g_line;
static awbus_t               g_bus;
static uint8_t               s_online[AWBUS_COMPANION_COUNT + 1]; /* 1-indexed */
static uint32_t              s_last_probe_ms;

/* ---- keyboard ring ---- */
#define KBD_BUF 32
static uint8_t s_kbuf[KBD_BUF];
static int     s_khead = 0, s_ktail = 0;
static void kbd_push(uint8_t c) {
    int next = (s_khead + 1) % KBD_BUF;
    if (next != s_ktail) { s_kbuf[s_khead] = c; s_khead = next; }
}

/* ---- konsole I/O bridge ---- */
struct io_ctx { const arch_api_t *A; int uart; };
static struct io_ctx g_ioctx;

static size_t io_read_avail(void *ctx) { (void)ctx; return 1024; }
static size_t io_read(void *ctx, uint8_t *buf, size_t len) {
    struct io_ctx *c = (struct io_ctx *)ctx;
    size_t n = 0;
    while (n < len && s_khead != s_ktail) {
        buf[n++] = s_kbuf[s_ktail];
        s_ktail  = (s_ktail + 1) % KBD_BUF;
    }
    if (n < len) {
        int r = c->A->uart_read(c->uart, buf + n, len - n);
        if (r > 0) n += (size_t)r;
    }
    return n;
}
static size_t io_write(void *ctx, const uint8_t *buf, size_t len) {
    struct io_ctx *c = (struct io_ctx *)ctx;
    int n = c->A->uart_write(c->uart, buf, len);
    return (n > 0) ? (size_t)n : 0;
}
static uint32_t io_millis(void *ctx) {
    return ((struct io_ctx *)ctx)->A->millis();
}

/* ---- companion presence ---- */

/*
 * Send `cmd` to `id`, busy-wait up to 50 ms for a reply with `expect_cmd`.
 * rx_out receives the companion frame; pass NULL to discard.
 * Returns AWBUS_OK, AWBUS_ERR_TIMEOUT, or AWBUS_ERR_CRC.
 */
static int ping_pong(uint8_t id, uint8_t cmd, uint8_t expect_cmd,
                     awbus_frame_t *rx_out) {
    int rc = awbus_send(&g_bus, id, cmd, NULL, 0, NULL);
    if (rc != AWBUS_OK) return rc;

    uint32_t t0 = A->millis();
    while (!g_bus.port->ready(g_bus.port->ctx, id))
        if (A->millis() - t0 >= 50u) return AWBUS_ERR_TIMEOUT;

    awbus_frame_t local = {0};
    awbus_frame_t *rx   = rx_out ? rx_out : &local;
    rc = awbus_poll(&g_bus, id, rx);
    return (rc == AWBUS_OK && rx->cmd == expect_cmd) ? AWBUS_OK : AWBUS_ERR_CRC;
}

static void probe_companions(void) {
    for (int i = 1; i <= AWBUS_COMPANION_COUNT; i++) {
        int now = (ping_pong((uint8_t)i, AWBUS_CMD_PING, AWBUS_CMD_PONG, NULL) == AWBUS_OK);
        if (now != (int)s_online[i])
            kon_printf(&g_ks, "[bus] companion %d %s\r\n", i, now ? "online" : "offline");
        s_online[i] = (uint8_t)now;
    }
    s_last_probe_ms = A->millis();
}

/* ---- konsole commands ---- */

static int cmd_sys(struct konsole *ks, int argc, char **argv) {
    (void)argc; (void)argv;
    kon_printf(ks, "board  : %s\r\n", NEU_BOARD_NAME);
    kon_printf(ks, "uptime : %u ms\r\n", (unsigned)A->millis());
    kon_printf(ks, "spi    : %lu Hz  frame %u B\r\n",
               (unsigned long)AWBUS_SPI_HZ, (unsigned)AWBUS_FRAME_SIZE);
    for (int i = 1; i <= AWBUS_COMPANION_COUNT; i++)
        kon_printf(ks, "  comp-%d : %s\r\n", i, s_online[i] ? "online" : "offline");
    return 0;
}

static int cmd_bus(struct konsole *ks, int argc, char **argv) {
    if (argc < 2) {
        kon_printf(ks, "usage: bus <ping|status|reset|scan|probe> [id]\r\n");
        return -1;
    }

    if (strcmp(argv[1], "probe") == 0) {
        probe_companions();
        return 0;
    }

    if (strcmp(argv[1], "reset") == 0) {
        kon_printf(ks, "bus: resetting all companions...\r\n");
        awbus_reset_all(&g_bus);
        A->delay_ms(500);
        probe_companions();
        return 0;
    }

    if (strcmp(argv[1], "scan") == 0) {
        for (int i = 1; i <= AWBUS_COMPANION_COUNT; i++) {
            int rdy = g_bus.port->ready(g_bus.port->ctx, (uint8_t)i);
            kon_printf(ks, "  comp-%d : %-7s  ready=%s\r\n",
                       i, s_online[i] ? "online" : "offline", rdy ? "yes" : "no");
        }
        return 0;
    }

    if (argc < 3) {
        kon_printf(ks, "usage: bus %s <id>\r\n", argv[1]);
        return -1;
    }

    uint8_t id = (uint8_t)atoi(argv[2]);
    if (id < 1 || id > AWBUS_COMPANION_COUNT) {
        kon_printf(ks, "bus: invalid id %d (1..%d)\r\n", id, AWBUS_COMPANION_COUNT);
        return -1;
    }

    if (strcmp(argv[1], "ping") == 0) {
        int rc = ping_pong(id, AWBUS_CMD_PING, AWBUS_CMD_PONG, NULL);
        if (rc == AWBUS_OK) { kon_printf(ks, "PONG from comp-%d\r\n", id); s_online[id] = 1; }
        else                { kon_printf(ks, "no response from comp-%d (err %d)\r\n", id, rc); s_online[id] = 0; }
        return rc == AWBUS_OK ? 0 : -1;
    }

    if (strcmp(argv[1], "status") == 0) {
        awbus_frame_t rx = {0};
        int rc = ping_pong(id, AWBUS_CMD_STATUS_REQ, AWBUS_CMD_STATUS_RSP, &rx);
        if (rc == AWBUS_OK) {
            uint32_t up = 0;
            if (rx.payload_len >= 5)
                up = (uint32_t)rx.payload[1]
                   | ((uint32_t)rx.payload[2] <<  8)
                   | ((uint32_t)rx.payload[3] << 16)
                   | ((uint32_t)rx.payload[4] << 24);
            kon_printf(ks, "comp-%d  node_id=%d  uptime=%u ms\r\n",
                       id, rx.payload[0], (unsigned)up);
        } else {
            kon_printf(ks, "comp-%d error %d\r\n", id, rc);
        }
        return rc == AWBUS_OK ? 0 : -1;
    }

    kon_printf(ks, "bus: unknown subcommand '%s'\r\n", argv[1]);
    return -1;
}

/* ---- riginfo command ---- */

static int cmd_riginfo(struct konsole *ks, int argc, char **argv) {
    (void)argc; (void)argv;

    kon_printf(ks, "rig      : %s  rev %s\r\n", AWBUS_RIG_NAME, AWBUS_RIG_REVISION);
    kon_printf(ks, "brain    : %s\r\n", NEU_BOARD_NAME);
    kon_printf(ks, "spi      : %lu Hz  frame %u B\r\n",
               (unsigned long)AWBUS_SPI_HZ, (unsigned)AWBUS_FRAME_SIZE);
    kon_printf(ks, "\r\n");
    kon_printf(ks, "  %-4s  %-7s  %-24s  %-7s  %s\r\n",
               "node", "status", "board", "fw-rev", "uptime");
    kon_printf(ks, "  %-4s  %-7s  %-24s  %-7s  %s\r\n",
               "────", "───────", "────────────────────────", "───────", "──────");

    for (int i = 1; i <= AWBUS_COMPANION_COUNT; i++) {
        if (!s_online[i]) {
            kon_printf(ks, "  %-4d  offline\r\n", i);
            continue;
        }
        awbus_frame_t rx = {0};
        int rc = ping_pong((uint8_t)i, AWBUS_CMD_INFO_REQ, AWBUS_CMD_INFO_RSP, &rx);
        if (rc != AWBUS_OK || rx.payload_len < 5) {
            kon_printf(ks, "  %-4d  err %-3d\r\n", i, rc);
            s_online[i] = 0;
            continue;
        }
        uint32_t up = (uint32_t)rx.payload[1]
                    | ((uint32_t)rx.payload[2] <<  8)
                    | ((uint32_t)rx.payload[3] << 16)
                    | ((uint32_t)rx.payload[4] << 24);
        char board[17] = {0};
        char rev[9]    = {0};
        if (rx.payload_len >= 21) memcpy(board, rx.payload + 5,  16);
        if (rx.payload_len >= 29) memcpy(rev,   rx.payload + 21,  8);
        kon_printf(ks, "  %-4d  online   %-24s  %-7s  %u ms\r\n",
                   i, board, rev, (unsigned)up);
    }
    return 0;
}

/* ---- bench command ---- */

/* Parse "1", "1,2,3", or "all" into an array of node IDs.
 * "all" expands to all currently-online companions. */
static int parse_node_list(const char *s, uint8_t *out, int max) {
    int n = 0;
    if (!s || strcmp(s, "all") == 0) {
        for (int i = 1; i <= AWBUS_COMPANION_COUNT && n < max; i++)
            if (s_online[i]) out[n++] = (uint8_t)i;
        return n;
    }
    char buf[32];
    strncpy(buf, s, sizeof buf - 1);
    buf[sizeof buf - 1] = '\0';
    char *tok = strtok(buf, ",");
    while (tok && n < max) {
        int id = atoi(tok);
        if (id >= 1 && id <= AWBUS_COMPANION_COUNT)
            out[n++] = (uint8_t)id;
        tok = strtok(NULL, ",");
    }
    return n;
}

/*
 * bench <tx|rx|both> [nodes] [count=100]
 *
 *   tx  : blast AWBUS_CMD_NULL frames (1 SPI txn/frame, companion discards)
 *          → measures raw SPI bus throughput limited only by clock and frame size
 *   rx  : STATUS_REQ → STATUS_RSP exchange (2 SPI txns/exchange)
 *          → tests companion firmware response path with a small payload
 *   both: PING → PONG exchange (2 SPI txns/exchange, zero payload)
 *          → measures round-trip latency including companion scheduler
 *
 *   nodes: "1"  "2,3"  "all"  — default: all online
 *   count: iterations per node — default: 100  max: 10000
 *
 * The benchmark is synchronous and blocks the konsole for its duration.
 */
static int cmd_bench(struct konsole *ks, int argc, char **argv) {
    if (argc < 2) {
        kon_printf(ks,
            "usage: bench <tx|rx|both> [nodes] [count]\r\n"
            "  tx  : raw frame rate   (1 SPI txn each, companion ignores)\r\n"
            "  rx  : STATUS_REQ->RSP  (2 SPI txns, tests response path)\r\n"
            "  both: PING->PONG RTT   (2 SPI txns, full validated exchange)\r\n"
            "  nodes: 1  1,2,3  all   count: default 100\r\n");
        return -1;
    }

    int is_tx   = (strcmp(argv[1], "tx")   == 0);
    int is_rx   = (strcmp(argv[1], "rx")   == 0);
    int is_both = (strcmp(argv[1], "both") == 0);
    if (!is_tx && !is_rx && !is_both) {
        kon_printf(ks, "bench: action must be tx, rx, or both\r\n");
        return -1;
    }

    const char *nodes_str = (argc >= 3) ? argv[2] : "all";
    int count = (argc >= 4) ? atoi(argv[3]) : 100;
    if (count < 1 || count > 10000) count = 100;

    uint8_t nodes[AWBUS_COMPANION_COUNT];
    int n_nodes = parse_node_list(nodes_str, nodes, AWBUS_COMPANION_COUNT);
    if (n_nodes == 0) {
        kon_printf(ks, "bench: no nodes selected — run 'bus probe' first\r\n");
        return -1;
    }

    kon_printf(ks, "\r\nbench: %s  nodes=", argv[1]);
    for (int i = 0; i < n_nodes; i++) kon_printf(ks, "%s%d", i ? "," : "", nodes[i]);
    kon_printf(ks, "  count=%d  spi=%lu Hz\r\n\r\n", count, (unsigned long)AWBUS_SPI_HZ);

    int ok[AWBUS_COMPANION_COUNT + 1]  = {0};
    int err[AWBUS_COMPANION_COUNT + 1] = {0};

    uint32_t t_start = A->millis();

    for (int iter = 0; iter < count; iter++) {
        for (int ni = 0; ni < n_nodes; ni++) {
            uint8_t node = nodes[ni];
            int rc;
            if (is_tx)
                rc = awbus_send(&g_bus, node, AWBUS_CMD_NULL, NULL, 0, NULL);
            else if (is_rx)
                rc = ping_pong(node, AWBUS_CMD_STATUS_REQ, AWBUS_CMD_STATUS_RSP, NULL);
            else
                rc = ping_pong(node, AWBUS_CMD_PING, AWBUS_CMD_PONG, NULL);

            if (rc == AWBUS_OK) ok[node]++;
            else                err[node]++;
        }
    }

    uint32_t elapsed = A->millis() - t_start;

    int total_ok = 0, total_err = 0;
    kon_printf(ks, "  %-8s  %6s  %6s\r\n", "node",     "ok",     "err");
    kon_printf(ks, "  %-8s  %6s  %6s\r\n", "────────", "──────", "──────");
    for (int ni = 0; ni < n_nodes; ni++) {
        uint8_t node = nodes[ni];
        kon_printf(ks, "  comp-%-3d  %6d  %6d\r\n", node, ok[node], err[node]);
        total_ok  += ok[node];
        total_err += err[node];
    }
    kon_printf(ks, "  %-8s  %6s  %6s\r\n", "────────", "──────", "──────");

    uint32_t fps  = elapsed > 0 ? (uint32_t)(total_ok  * 1000u / elapsed) : 0;
    uint32_t kbps = fps * (uint32_t)AWBUS_FRAME_SIZE / 1024u;
    /* tx = 1 SPI txn/frame; rx/both = 2 SPI txns/exchange */
    uint32_t spi_tps = fps * (uint32_t)(is_tx ? 1 : 2);

    kon_printf(ks, "\r\n  elapsed  : %u ms\r\n", (unsigned)elapsed);
    kon_printf(ks, "  result   : %d ok  %d err\r\n", total_ok, total_err);
    kon_printf(ks, "  rate     : %u %s/s\r\n",
               (unsigned)fps, is_tx ? "frames" : "rtt");
    kon_printf(ks, "  app BW   : %u kB/s  |  ~%u SPI txns/s\r\n",
               (unsigned)kbps, (unsigned)spi_tps);

    return (total_err == 0) ? 0 : -1;
}

/* ---- command table ---- */

static const struct kon_cmd g_cmds[] = {
    { "sys",     "system info and companion status",          cmd_sys     },
    { "riginfo", "rig identity, SPI speed, and companion fw", cmd_riginfo },
    { "bus",     "bus <ping|status|reset|scan|probe> [id]",   cmd_bus     },
    { "bench",   "bench <tx|rx|both> [nodes] [count=100]",    cmd_bench   },
};

/* ---- Public API ---- */

awbus_t *brain_bus(void) { return &g_bus; }

int brain_init(const arch_api_t *arch) {
    A = arch;

    A->uart_init(NEU_UART_CONSOLE_IDX, NEU_UART_CONSOLE_BAUD);

    g_ioctx.A    = A;
    g_ioctx.uart = NEU_UART_CONSOLE_IDX;

    struct konsole_io io = {
        .read_avail = io_read_avail,
        .read       = io_read,
        .write      = io_write,
        .millis     = io_millis,
        .ctx        = &g_ioctx,
    };
    konsole_init_with_storage(&g_ks, &g_line, &io,
                              g_cmds, sizeof g_cmds / sizeof g_cmds[0],
                              "[brain] ", true);

    awbus_port_t *port = brain_bus_port_init();
    awbus_init(&g_bus, port, 0x00u);

#if defined(BRAIN_HEARTBEAT) && BRAIN_HEARTBEAT
    brain_heartbeat_init();
#endif

    kon_banner(&g_ks, "aetherward-rig  brain  prism");
    kon_printf(&g_ks, "fw: " __DATE__ " " __TIME__ "\r\n");
    kon_printf(&g_ks, "spi: %lu Hz  frame: %u B  companions: %d\r\n",
               (unsigned long)AWBUS_SPI_HZ, (unsigned)AWBUS_FRAME_SIZE,
               AWBUS_COMPANION_COUNT);

    probe_companions();
    return 0;
}

void brain_run(void) {
    if (A->millis() - s_last_probe_ms >= PROBE_INTERVAL_MS)
        probe_companions();

    awbus_frame_t rx = {0};
    for (int i = 1; i <= AWBUS_COMPANION_COUNT; i++) {
        if (!s_online[i]) continue;
        int rc = awbus_poll(&g_bus, (uint8_t)i, &rx);
        if (rc == AWBUS_OK) {
            kon_printf(&g_ks, "[bus] comp-%d  cmd=0x%02x  len=%d\r\n",
                       i, rx.cmd, rx.payload_len);
        } else if (rc != AWBUS_ERR_NODATA) {
            /* Unexpected error from an online companion */
            s_online[i] = 0;
            kon_printf(&g_ks, "[bus] comp-%d offline (err %d)\r\n", i, rc);
        }
    }

    konsole_poll(&g_ks);
#if defined(BRAIN_HEARTBEAT) && BRAIN_HEARTBEAT
    brain_heartbeat_tick();
#endif
    A->delay_ms(1);
}
