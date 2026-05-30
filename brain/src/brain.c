#include <string.h>
#include <stdlib.h>
#include "brain.h"
#include "board.h"
#include "aw_wifi/wifi_proto.h"

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

/* ---- test-sniff helpers ---- */

static uint16_t rd16le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8u);
}
static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8u)
         | ((uint32_t)p[2] << 16u)
         | ((uint32_t)p[3] << 24u);
}
static uint64_t rd64le(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (i * 8);
    return v;
}

/* One line of a hex dump: offset, 16 hex bytes (8+8), ASCII sidebar. */
static void hexdump_line(struct konsole *ks, uint16_t off,
                         const uint8_t *b, int n) {
    kon_printf(ks, "    %04x: ", (unsigned)off);
    for (int i = 0; i < 16; i++) {
        if (i < n) kon_printf(ks, "%02x ", (unsigned)b[i]);
        else       kon_printf(ks, "   ");
        if (i == 7) kon_printf(ks, " ");
    }
    kon_printf(ks, " |");
    for (int i = 0; i < n; i++) {
        uint8_t c = b[i];
        kon_printf(ks, "%c", (c >= 0x20 && c < 0x7f) ? (char)c : '.');
    }
    kon_printf(ks, "|\r\n");
}

static const char *frame_type_str(uint8_t fc0) {
    uint8_t t = (fc0 >> 2) & 0x03u;
    uint8_t s = (fc0 >> 4) & 0x0Fu;
    if (t == 0) {
        switch (s) {
        case 0:  return "ASSOC_REQ";  case 1: return "ASSOC_RSP";
        case 2:  return "REASSOC_REQ"; case 3: return "REASSOC_RSP";
        case 4:  return "PROBE_REQ";  case 5: return "PROBE_RSP";
        case 8:  return "BEACON";     case 10: return "DISASSOC";
        case 11: return "AUTH";       case 12: return "DEAUTH";
        case 13: return "ACTION";     default: return "MGMT";
        }
    }
    if (t == 1) return "CTRL";
    if (t == 2) return "DATA";
    return "UNKN";
}

/* Extract SSID IE (type=0) starting at ie_off. Returns SSID length, 0 if absent. */
static int extract_ssid(const uint8_t *fr, uint16_t flen, uint16_t ie_off,
                        char *out, int outmax) {
    while (ie_off + 2u <= flen) {
        uint8_t id  = fr[ie_off];
        uint8_t len = fr[ie_off + 1];
        if (ie_off + 2u + len > flen) break;
        if (id == 0) {
            int n = (len < outmax - 1) ? len : outmax - 1;
            for (int i = 0; i < n; i++) {
                uint8_t c = fr[ie_off + 2 + i];
                out[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
            }
            out[n] = '\0';
            return n;
        }
        ie_off += 2u + len;
    }
    return 0;
}

static void print_mac(struct konsole *ks, const uint8_t *m) {
    kon_printf(ks, "%02x:%02x:%02x:%02x:%02x:%02x",
               m[0], m[1], m[2], m[3], m[4], m[5]);
}

static void verbose_frame(struct konsole *ks, uint32_t seq, uint64_t ts_us, uint8_t ch,
                          int8_t rssi, uint8_t fc0, uint8_t rec_flags,
                          const uint8_t *fr, uint16_t flen, uint16_t orig_len) {
    if (flen < 2) return;

    uint8_t fc1     = fr[1];
    uint8_t ftype   = (fc0 >> 2) & 0x03u;
    uint8_t fsub    = (fc0 >> 4) & 0x0Fu;
    int is_eapol    = (rec_flags & WIFI_REC_F_EAPOL) != 0;
    int truncated   = (rec_flags & WIFI_REC_F_TRUNCATED) != 0;

    uint32_t ts_ms  = (uint32_t)(ts_us / 1000u);
    uint32_t ts_s   = ts_ms / 1000u;
    uint32_t ts_fms = ts_ms % 1000u;

    kon_printf(ks, "  [%u.%03u] seq=%u ch%-2u rssi=%-4d len=%u/%u  %s%s",
               (unsigned)ts_s, (unsigned)ts_fms,
               (unsigned)seq, (unsigned)ch, (int)rssi,
               (unsigned)flen, (unsigned)orig_len,
               is_eapol ? "EAPOL" : frame_type_str(fc0),
               truncated ? " TRUNC" : "");

    if (ftype == 0 && flen >= 24u) {
        uint16_t ie_off = (fsub == 8 || fsub == 5) ? 36u : 24u;
        char ssid[33] = {0};
        int ssid_len = extract_ssid(fr, flen, ie_off, ssid, (int)sizeof ssid);
        kon_printf(ks, "  bssid="); print_mac(ks, fr + 16);
        kon_printf(ks, "  src=");   print_mac(ks, fr + 10);
        if (ssid_len > 0)
            kon_printf(ks, "  ssid=\"%s\"", ssid);
        else if (fsub == 4)
            kon_printf(ks, "  ssid=<wildcard>");
    } else if (is_eapol && flen >= 16u) {
        (void)fc1;
        kon_printf(ks, "  src="); print_mac(ks, fr + 10);
        kon_printf(ks, "  dst="); print_mac(ks, fr +  4);
    }
    kon_printf(ks, "\r\n");

    uint16_t dump = flen > 64u ? 64u : flen;
    for (uint16_t off = 0; off < dump; off += 16u) {
        int n = (dump - off > 16u) ? 16 : (int)(dump - off);
        hexdump_line(ks, off, fr + off, n);
    }
    if (flen > 64u)
        kon_printf(ks, "    ... (%u bytes captured)\r\n", (unsigned)flen);
}

/* Parse "1,6,11", "1-13", or "all" into a channel array (values 1..13). */
static int parse_channels(const char *s, uint8_t *out, int max) {
    if (!s || strcmp(s, "all") == 0) {
        int n = 0;
        for (int i = 1; i <= 13 && n < max; i++) out[n++] = (uint8_t)i;
        return n;
    }
    const char *dash = strchr(s, '-');
    if (dash && dash != s) {
        int lo = atoi(s);
        int hi = atoi(dash + 1);
        if (lo < 1)  lo = 1;
        if (hi > 13) hi = 13;
        int n = 0;
        for (int i = lo; i <= hi && n < max; i++) out[n++] = (uint8_t)i;
        return n;
    }
    char buf[64];
    strncpy(buf, s, sizeof buf - 1);
    buf[sizeof buf - 1] = '\0';
    int n = 0;
    char *tok = strtok(buf, ",");
    while (tok && n < max) {
        int ch = atoi(tok);
        if (ch >= 1 && ch <= 13) out[n++] = (uint8_t)ch;
        tok = strtok(NULL, ",");
    }
    return n;
}

struct wifi_telem_snapshot {
    uint8_t valid;
    uint8_t version;
    uint8_t cur_ch;
    uint8_t running;
    uint8_t pending;
    uint8_t fill_idx;
    uint8_t last_err;
    uint32_t v[WIFI_TELEM_U32_COUNT];
};

struct sniff_stat {
    uint32_t blobs;
    uint32_t frames;
    uint32_t bytes;
    uint32_t telem_rx;

    uint32_t ready_high;
    uint32_t poll_attempts;
    uint32_t poll_ok;
    uint32_t poll_nodata;
    uint32_t poll_crc;
    uint32_t poll_magic;
    uint32_t poll_overflow;
    uint32_t poll_io;
    uint32_t poll_timeout;
    uint32_t poll_other;
    uint32_t rx_null;
    uint32_t unexpected_cmd;
    uint32_t unexpected_data;

    uint32_t bad_blob_short;
    uint32_t bad_blob_version;
    uint32_t bad_blob_hdr;
    uint32_t bad_blob_len;
    uint32_t bad_record_len;
    uint32_t blob_seq_gap;
    uint32_t frame_seq_gap;
    uint32_t truncated_frames;
    uint32_t eapol_frames;
    uint32_t action_send_err;

    uint32_t last_blob_seq;
    uint32_t last_frame_seq;
    uint8_t  have_blob_seq;
    uint8_t  have_frame_seq;
    uint8_t  cur_ch;

    struct wifi_telem_snapshot tel;
};

static void count_poll_rc(struct sniff_stat *st, int rc) {
    if (rc == AWBUS_OK) { st->poll_ok++; return; }
    if (rc == AWBUS_ERR_NODATA)   st->poll_nodata++;
    else if (rc == AWBUS_ERR_CRC) st->poll_crc++;
    else if (rc == AWBUS_ERR_MAGIC) st->poll_magic++;
    else if (rc == AWBUS_ERR_OVERFLOW) st->poll_overflow++;
    else if (rc == AWBUS_ERR_IO) st->poll_io++;
    else if (rc == AWBUS_ERR_TIMEOUT) st->poll_timeout++;
    else st->poll_other++;
}

static void process_telem(struct sniff_stat *st, const uint8_t *p, uint16_t len) {
    if (len < WIFI_TELEMETRY_SIZE || p[0] != WIFI_DATA_TELEMETRY) {
        st->unexpected_data++;
        return;
    }
    st->telem_rx++;
    st->tel.valid = 1;
    st->tel.version = p[1];
    st->tel.cur_ch = p[3];
    st->tel.running = p[4];
    st->tel.pending = p[5];
    st->tel.fill_idx = p[6];
    st->tel.last_err = p[7];
    for (unsigned i = 0; i < WIFI_TELEM_U32_COUNT; i++)
        st->tel.v[i] = rd32le(p + WIFI_TELEM_HDR_SIZE + i * 4u);
    st->cur_ch = st->tel.cur_ch;
}

static void process_blob(struct konsole *ks, struct sniff_stat *st,
                         const uint8_t *pay, uint16_t pay_len, int verbose) {
    if (pay_len < WIFI_BLOB_HDR_SIZE) { st->bad_blob_short++; return; }
    if (pay[0] != WIFI_DATA_SNIFF_BLOB) { st->unexpected_data++; return; }
    if (pay[1] != WIFI_PROTO_VERSION) st->bad_blob_version++;
    if (pay[6] != WIFI_BLOB_HDR_SIZE) { st->bad_blob_hdr++; return; }

    uint8_t  n_frames = pay[3];
    uint8_t  ch       = pay[4];
    uint32_t blob_seq = rd32le(pay + 8);
    uint32_t first_seq = rd32le(pay + 12);
    uint16_t records_len = rd16le(pay + 16);

    st->cur_ch = ch;
    if (WIFI_BLOB_HDR_SIZE + records_len > pay_len) {
        st->bad_blob_len++;
        return;
    }

    if (st->have_blob_seq && blob_seq != st->last_blob_seq + 1u)
        st->blob_seq_gap++;
    st->have_blob_seq = 1;
    st->last_blob_seq = blob_seq;

    st->blobs++;
    st->bytes += records_len;

    uint16_t off = WIFI_BLOB_HDR_SIZE;
    uint8_t parsed = 0;
    for (uint8_t f = 0; f < n_frames; f++) {
        if (off + WIFI_RECORD_HDR_SIZE > WIFI_BLOB_HDR_SIZE + records_len) {
            st->bad_record_len++;
            break;
        }
        const uint8_t *r = pay + off;
        uint64_t ts_us = rd64le(r + 0);
        uint32_t frame_seq = rd32le(r + 8);
        uint16_t flen = rd16le(r + 12);
        uint16_t orig_len = rd16le(r + 14);
        uint8_t  fch = r[16];
        int8_t   rssi = (int8_t)r[17];
        uint8_t  fc0 = r[18];
        uint8_t  flags = r[19];
        off += WIFI_RECORD_HDR_SIZE;
        if (off + flen > WIFI_BLOB_HDR_SIZE + records_len) {
            st->bad_record_len++;
            break;
        }

        if (st->have_frame_seq && frame_seq != st->last_frame_seq + 1u)
            st->frame_seq_gap++;
        st->have_frame_seq = 1;
        st->last_frame_seq = frame_seq;

        if (flags & WIFI_REC_F_TRUNCATED) st->truncated_frames++;
        if (flags & WIFI_REC_F_EAPOL) st->eapol_frames++;
        if (verbose)
            verbose_frame(ks, frame_seq, ts_us, fch, rssi, fc0, flags,
                          pay + off, flen, orig_len);

        off = (uint16_t)(off + flen);
        parsed++;
    }

    if (first_seq && parsed && !st->have_frame_seq) {
        (void)first_seq;
    }
    st->frames += parsed;
    if (parsed != n_frames) st->bad_record_len++;
}

/*
 * Return value:
 *   1 = sniff blob consumed
 *   2 = telemetry consumed
 *   0 = valid NULL / wake transaction only
 *  -1 = valid AWBUS frame but not a known sniff payload
 *
 * Important: with the ESP-IDF SPI-slave pipeline the companion usually has an
 * idle RX transaction queued before a spontaneous sniff blob exists.  READY may
 * therefore mean "send one NULL/wakeup transaction so the companion task can
 * reap it and queue the DATA_PUSH reply".  The brain must not keep hammering
 * polls in the same tight loop after receiving AWBUS_CMD_NULL, otherwise the
 * next SPI transaction can race before the companion has queued the blob.
 */
static int process_rx_frame(struct konsole *ks, uint8_t nd, struct sniff_stat *stats,
                            const awbus_frame_t *rx, int verbose) {
    struct sniff_stat *st = &stats[nd];
    if (rx->cmd == AWBUS_CMD_NULL) { st->rx_null++; return 0; }
    if (rx->cmd != AWBUS_CMD_DATA_PUSH) { st->unexpected_cmd++; return -1; }
    if (rx->payload_len < 1u) { st->unexpected_data++; return -1; }

    if (rx->payload[0] == WIFI_DATA_SNIFF_BLOB) {
        process_blob(ks, st, rx->payload, rx->payload_len, verbose);
        return 1;
    }
    if (rx->payload[0] == WIFI_DATA_TELEMETRY) {
        process_telem(st, rx->payload, rx->payload_len);
        return 2;
    }

    st->unexpected_data++;
    return -1;
}

static int drain_node(struct konsole *ks, uint8_t nd, struct sniff_stat *stats,
                      int verbose, int max_polls) {
    int got = 0;
    for (int i = 0; i < max_polls; i++) {
        if (!g_bus.port->ready(g_bus.port->ctx, nd)) break;
        stats[nd].ready_high++;
        stats[nd].poll_attempts++;
        awbus_frame_t rx = {0};
        int rc = awbus_poll(&g_bus, nd, &rx);
        count_poll_rc(&stats[nd], rc);
        if (rc != AWBUS_OK) break;
        int kind = process_rx_frame(ks, nd, stats, &rx, verbose);
        got++;

        /*
         * NULL is a wake/trigger transaction, not a delivered blob.  Give the
         * companion task one scheduler tick to reap that NULL and queue the
         * DATA_PUSH descriptor before we poll this node again.  This prevents
         * READY-level races that show up as crc=0/magic>>0 at the brain.
         */
        if (kind <= 0) {
            A->delay_ms(1);
            break;
        }
    }
    return got;
}

static void request_telem(struct konsole *ks, uint8_t nd, struct sniff_stat *stats,
                          int verbose) {
    uint8_t req[1] = { WIFI_ACTION_TELEMETRY_REQ };
    awbus_frame_t rx = {0};
    int rc = awbus_send(&g_bus, nd, AWBUS_CMD_ACTION, req, 1, &rx);
    if (rc == AWBUS_OK) {
        /* The ACTION transaction may clock out a staged DATA_PUSH; do not lose it. */
        process_rx_frame(ks, nd, stats, &rx, verbose);
    } else {
        stats[nd].action_send_err++;
        count_poll_rc(&stats[nd], rc);
        return;
    }

    uint32_t t0 = A->millis();
    while (A->millis() - t0 < 30u) {
        int before = (int)stats[nd].telem_rx;
        drain_node(ks, nd, stats, verbose, 4);
        if ((int)stats[nd].telem_rx > before) return;
        A->delay_ms(1);
    }
}

/*
 * test-sniff [-v] [channels] [hop_ms=200] [duration_s=10]
 *
 * Splits channels contiguously across online companions, time-syncs each one,
 * starts beacon/probe/EAPOL sniffing, drains blob batches, requests telemetry
 * once per second, and prints enough counters to localize losses.
 */
static int cmd_test_sniff(struct konsole *ks, int argc, char **argv) {
    int   verbose  = 0;
    char *ch_str   = NULL;
    char *hop_str  = NULL;
    char *dur_str  = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) { verbose = 1; continue; }
        if (!ch_str)  { ch_str  = argv[i]; continue; }
        if (!hop_str) { hop_str = argv[i]; continue; }
        if (!dur_str) { dur_str = argv[i]; continue; }
    }

    uint8_t all_ch[13];
    int n_all = parse_channels(ch_str, all_ch, 13);
    if (n_all == 0) {
        kon_printf(ks, "test-sniff: no valid channels in '%s'\r\n", ch_str ? ch_str : "?");
        return -1;
    }

    int hop_ms     = hop_str ? atoi(hop_str) : 200;
    int duration_s = dur_str ? atoi(dur_str) : 10;
    if (hop_ms     < 10)    hop_ms     = 200;
    if (duration_s < 1)     duration_s = 10;
    if (duration_s > 3600)  duration_s = 3600;

    uint8_t nodes[AWBUS_COMPANION_COUNT];
    int n_nodes = 0;
    for (int i = 1; i <= AWBUS_COMPANION_COUNT; i++)
        if (s_online[i]) nodes[n_nodes++] = (uint8_t)i;

    if (n_nodes == 0) {
        kon_printf(ks, "test-sniff: no companions online — run 'bus probe' first\r\n");
        return -1;
    }

    uint8_t comp_ch[AWBUS_COMPANION_COUNT][13];
    uint8_t comp_n[AWBUS_COMPANION_COUNT];
    {
        int base  = n_all / n_nodes;
        int extra = n_all % n_nodes;
        int off   = 0;
        for (int ni = 0; ni < n_nodes; ni++) {
            int cnt = base + (ni < extra ? 1 : 0);
            comp_n[ni] = (uint8_t)cnt;
            for (int ci = 0; ci < cnt; ci++)
                comp_ch[ni][ci] = all_ch[off + ci];
            off += cnt;
        }
    }

    kon_printf(ks, "\r\ntest-sniff: %d ch  hop=%dms  dur=%ds  comps=%d  blob=%uB snap=%uB\r\n",
               n_all, hop_ms, duration_s, n_nodes,
               (unsigned)AWBUS_MAX_PAYLOAD, (unsigned)WIFI_SNIFF_SNAPLEN);
    for (int ni = 0; ni < n_nodes; ni++) {
        kon_printf(ks, "  comp-%d: ", nodes[ni]);
        for (int ci = 0; ci < comp_n[ni]; ci++)
            kon_printf(ks, "%s%d", ci ? "," : "", comp_ch[ni][ci]);
        kon_printf(ks, "\r\n");
    }

    struct sniff_stat stats[AWBUS_COMPANION_COUNT + 1];
    memset(stats, 0, sizeof stats);

    /* --- TIMESYNC each companion --- */
    for (int ni = 0; ni < n_nodes; ni++) {
        uint8_t pay[9];
        pay[0] = WIFI_ACTION_TIMESYNC;
        uint64_t brain_us = (uint64_t)A->millis() * 1000ULL;
        for (int i = 0; i < 8; i++) pay[1 + i] = (uint8_t)(brain_us >> (i * 8));
        int rc = awbus_send(&g_bus, nodes[ni], AWBUS_CMD_ACTION, pay, 9, NULL);
        if (rc != AWBUS_OK) stats[nodes[ni]].action_send_err++;
        A->delay_ms(5);
    }

    /* --- SNIFF_START each companion --- */
    uint8_t filter = WIFI_FILTER_BEACON | WIFI_FILTER_PROBE_REQ
                   | WIFI_FILTER_PROBE_RSP | WIFI_FILTER_EAPOL;
    for (int ni = 0; ni < n_nodes; ni++) {
        uint8_t pay[18];
        int pi = 0;
        pay[pi++] = WIFI_ACTION_SNIFF_START;
        pay[pi++] = filter;
        pay[pi++] = comp_n[ni];
        for (int ci = 0; ci < comp_n[ni]; ci++) pay[pi++] = comp_ch[ni][ci];
        pay[pi++] = (uint8_t)((uint16_t)hop_ms & 0xFFu);
        pay[pi++] = (uint8_t)((uint16_t)hop_ms >> 8u);
        int rc = awbus_send(&g_bus, nodes[ni], AWBUS_CMD_ACTION, pay, (uint16_t)pi, NULL);
        if (rc != AWBUS_OK) stats[nodes[ni]].action_send_err++;
        A->delay_ms(5);
    }

    kon_printf(ks, "  running for %d s ...\r\n\r\n", duration_s);

    uint32_t t_start    = A->millis();
    uint32_t t_end      = t_start + (uint32_t)duration_s * 1000u;
    uint32_t t_last_log = t_start;

    while (A->millis() < t_end) {
        for (int ni = 0; ni < n_nodes; ni++)
            drain_node(ks, nodes[ni], stats, verbose, 6);

        uint32_t now = A->millis();
        if (now - t_last_log >= 1000u) {
            t_last_log = now;
            for (int ni = 0; ni < n_nodes; ni++)
                request_telem(ks, nodes[ni], stats, verbose);

            uint32_t elapsed = (now - t_start) / 1000u;
            kon_printf(ks, "  [%2us]", (unsigned)elapsed);
            for (int ni = 0; ni < n_nodes; ni++) {
                uint8_t nd = nodes[ni];
                struct wifi_telem_snapshot *t = &stats[nd].tel;
                uint32_t cap = t->valid ? t->v[WIFI_TELEM_U32_CAPTURED] : 0;
                uint32_t drp = t->valid ? t->v[WIFI_TELEM_U32_DROP_TOTAL] : 0;
                uint32_t ring = t->valid ? (t->v[WIFI_TELEM_U32_DROP_BLOB_FULL] +
                                            t->v[WIFI_TELEM_U32_DROP_RECORD_LIMIT] +
                                            t->v[WIFI_TELEM_U32_DROP_MUTEX_BUSY]) : 0;
                kon_printf(ks,
                    "  c%d ch%-2d rx=%-5u cap=%-5u drp=%-4u ring=%-3u spiE=%-2u gap=%-2u",
                    nd, stats[nd].cur_ch,
                    (unsigned)stats[nd].frames,
                    (unsigned)cap,
                    (unsigned)drp,
                    (unsigned)ring,
                    (unsigned)(stats[nd].poll_crc + stats[nd].poll_magic + stats[nd].poll_io + stats[nd].poll_overflow),
                    (unsigned)(stats[nd].blob_seq_gap + stats[nd].frame_seq_gap));
            }
            kon_printf(ks, "\r\n");
        }
        A->delay_ms(1);
    }

    /* Drain staged blobs before STOP so the STOP transaction does not discard a ready blob. */
    for (int ni = 0; ni < n_nodes; ni++)
        drain_node(ks, nodes[ni], stats, verbose, 16);

    /* --- SNIFF_STOP all companions --- */
    uint8_t stop_pay[1] = { WIFI_ACTION_SNIFF_STOP };
    for (int ni = 0; ni < n_nodes; ni++) {
        uint8_t nd = nodes[ni];
        awbus_frame_t rx = {0};
        int rc = awbus_send(&g_bus, nd, AWBUS_CMD_ACTION, stop_pay, 1, &rx);
        if (rc == AWBUS_OK) process_rx_frame(ks, nd, stats, &rx, verbose);
        else { stats[nd].action_send_err++; count_poll_rc(&stats[nd], rc); }

        uint32_t t0 = A->millis();
        while (A->millis() - t0 < 150u) {
            uint32_t before = stats[nd].telem_rx;
            drain_node(ks, nd, stats, verbose, 8);
            if (stats[nd].telem_rx > before) break;
            A->delay_ms(1);
        }
    }

    /* Final drain, including remaining sniff blobs sealed by STOP. */
    A->delay_ms(100);
    for (int ni = 0; ni < n_nodes; ni++)
        drain_node(ks, nodes[ni], stats, verbose, 32);
    for (int ni = 0; ni < n_nodes; ni++)
        request_telem(ks, nodes[ni], stats, verbose);

    /* --- Final summaries --- */
    kon_printf(ks, "\r\nCAPTURE PATH\r\n");
    kon_printf(ks, "  %-6s %-7s %-7s %-7s %-7s %-7s %-7s %-7s %-7s %-7s\r\n",
               "comp", "seenM", "seenD", "filter", "cap", "sent", "drop", "blobF", "pend", "pushF");
    kon_printf(ks, "  %-6s %-7s %-7s %-7s %-7s %-7s %-7s %-7s %-7s %-7s\r\n",
               "──────", "───────", "───────", "───────", "───────", "───────", "───────", "───────", "───────", "───────");
    uint32_t total_frames = 0, total_blobs = 0, total_drop = 0;
    for (int ni = 0; ni < n_nodes; ni++) {
        uint8_t nd = nodes[ni];
        struct wifi_telem_snapshot *t = &stats[nd].tel;
        uint32_t seenM = t->valid ? t->v[WIFI_TELEM_U32_SEEN_MGMT] : 0;
        uint32_t seenD = t->valid ? t->v[WIFI_TELEM_U32_SEEN_DATA] : 0;
        uint32_t filt  = t->valid ? t->v[WIFI_TELEM_U32_FILTERED] : 0;
        uint32_t cap   = t->valid ? t->v[WIFI_TELEM_U32_CAPTURED] : 0;
        uint32_t sent  = t->valid ? t->v[WIFI_TELEM_U32_SENT] : 0;
        uint32_t drop  = t->valid ? t->v[WIFI_TELEM_U32_DROP_TOTAL] : 0;
        uint32_t blobF = t->valid ? t->v[WIFI_TELEM_U32_DROP_BLOB_FULL] : 0;
        uint32_t pend  = t->valid ? t->v[WIFI_TELEM_U32_DROP_PENDING_BUSY] : 0;
        uint32_t pushF = t->valid ? t->v[WIFI_TELEM_U32_DROP_PUSH_FAIL] : 0;
        kon_printf(ks, "  comp-%-1d %-7u %-7u %-7u %-7u %-7u %-7u %-7u %-7u %-7u\r\n",
                   nd, (unsigned)seenM, (unsigned)seenD, (unsigned)filt,
                   (unsigned)cap, (unsigned)sent, (unsigned)drop,
                   (unsigned)blobF, (unsigned)pend, (unsigned)pushF);
        total_frames += stats[nd].frames;
        total_blobs  += stats[nd].blobs;
        total_drop   += drop;
    }

    kon_printf(ks, "\r\nFRAME MIX / HOPPER\r\n");
    kon_printf(ks, "  %-6s %-7s %-7s %-7s %-7s %-7s %-7s %-7s %-7s %-7s\r\n",
               "comp", "beacon", "preq", "prsp", "eapol", "trunc", "hops", "chOK", "chERR", "lastE");
    for (int ni = 0; ni < n_nodes; ni++) {
        uint8_t nd = nodes[ni];
        struct wifi_telem_snapshot *t = &stats[nd].tel;
        kon_printf(ks, "  comp-%-1d %-7u %-7u %-7u %-7u %-7u %-7u %-7u %-7u %-7d\r\n",
                   nd,
                   (unsigned)(t->valid ? t->v[WIFI_TELEM_U32_KEPT_BEACON] : 0),
                   (unsigned)(t->valid ? t->v[WIFI_TELEM_U32_KEPT_PROBE_REQ] : 0),
                   (unsigned)(t->valid ? t->v[WIFI_TELEM_U32_KEPT_PROBE_RSP] : 0),
                   (unsigned)(t->valid ? t->v[WIFI_TELEM_U32_KEPT_EAPOL] : 0),
                   (unsigned)stats[nd].truncated_frames,
                   (unsigned)(t->valid ? t->v[WIFI_TELEM_U32_HOP_COUNT] : 0),
                   (unsigned)(t->valid ? t->v[WIFI_TELEM_U32_CHANNEL_SET_OK] : 0),
                   (unsigned)(t->valid ? t->v[WIFI_TELEM_U32_CHANNEL_SET_ERR] : 0),
                   (int)(int8_t)(t->valid ? t->last_err : 0));
    }

    kon_printf(ks, "\r\nBLOB / SPI RECEIVE\r\n");
    kon_printf(ks, "  %-6s %-7s %-7s %-7s %-7s %-7s %-7s %-7s %-7s %-7s\r\n",
               "comp", "blobs", "frames", "bytes", "poll", "null", "crc", "magic", "seqGap", "badRec");
    for (int ni = 0; ni < n_nodes; ni++) {
        uint8_t nd = nodes[ni];
        kon_printf(ks, "  comp-%-1d %-7u %-7u %-7u %-7u %-7u %-7u %-7u %-7u %-7u\r\n",
                   nd, (unsigned)stats[nd].blobs, (unsigned)stats[nd].frames,
                   (unsigned)stats[nd].bytes, (unsigned)stats[nd].poll_attempts,
                   (unsigned)stats[nd].rx_null, (unsigned)stats[nd].poll_crc,
                   (unsigned)stats[nd].poll_magic,
                   (unsigned)(stats[nd].blob_seq_gap + stats[nd].frame_seq_gap),
                   (unsigned)(stats[nd].bad_blob_short + stats[nd].bad_blob_len + stats[nd].bad_record_len));
    }

    kon_printf(ks, "\r\nBUFFER HIGHS / FLUSH\r\n");
    kon_printf(ks, "  %-6s %-7s %-7s %-7s %-7s %-7s %-7s %-7s %-7s\r\n",
               "comp", "maxBlob", "maxBF", "maxFill", "maxFF", "flush", "pop", "timer", "empty");
    for (int ni = 0; ni < n_nodes; ni++) {
        uint8_t nd = nodes[ni];
        struct wifi_telem_snapshot *t = &stats[nd].tel;
        kon_printf(ks, "  comp-%-1d %-7u %-7u %-7u %-7u %-7u %-7u %-7u %-7u\r\n",
                   nd,
                   (unsigned)(t->valid ? t->v[WIFI_TELEM_U32_MAX_BLOB_BYTES] : 0),
                   (unsigned)(t->valid ? t->v[WIFI_TELEM_U32_MAX_BLOB_FRAMES] : 0),
                   (unsigned)(t->valid ? t->v[WIFI_TELEM_U32_MAX_FILL_BYTES] : 0),
                   (unsigned)(t->valid ? t->v[WIFI_TELEM_U32_MAX_FILL_FRAMES] : 0),
                   (unsigned)(t->valid ? t->v[WIFI_TELEM_U32_BLOB_FLUSHES] : 0),
                   (unsigned)(t->valid ? t->v[WIFI_TELEM_U32_BLOB_POPPED] : 0),
                   (unsigned)(t->valid ? t->v[WIFI_TELEM_U32_FLUSH_TIMER_FIRES] : 0),
                   (unsigned)(t->valid ? t->v[WIFI_TELEM_U32_FLUSH_EMPTY] : 0));
    }

    uint32_t elapsed_s = ((A->millis() - t_start) + 500u) / 1000u;
    if (elapsed_s > 0u) {
        kon_printf(ks, "\r\n  total frames=%u blobs=%u drops=%u  frames/s=%u blobs/s=%u\r\n",
                   (unsigned)total_frames, (unsigned)total_blobs, (unsigned)total_drop,
                   (unsigned)(total_frames / elapsed_s),
                   (unsigned)(total_blobs / elapsed_s));
    }

    return 0;
}

/* ---- command table ---- */

static const struct kon_cmd g_cmds[] = {
    { "sys",        "system info and companion status",          cmd_sys        },
    { "riginfo",    "rig identity, SPI speed, and companion fw", cmd_riginfo    },
    { "bus",        "bus <ping|status|reset|scan|probe> [id]",   cmd_bus        },
    { "bench",      "bench <tx|rx|both> [nodes] [count=100]",    cmd_bench      },
    { "test-sniff", "test-sniff [-v] [ch=all] [hop_ms=200] [dur_s=10]", cmd_test_sniff },
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
