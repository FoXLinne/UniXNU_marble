// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test: GeniSpi.c against a register model of a GENI serial engine in FIFO mode. The model is derived from the
// documented register behaviour; it is not hardware.
#include "../GeniSpi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAIL(...) do { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); exit(1); } while (0)
#define CHECK(c) do { if (!(c)) FAIL("%s", #c); } while (0)

#define DEPTH 16
struct sim {
    uint32_t reg[0x1000 / 4];
    uint32_t wrap_ver, hw_param0, fw_rev, if_disable;
    // DMA mode
    uint32_t dma_tx_len, dma_rx_len, dma_tx_stat, dma_rx_stat; int dma_tx_set, dma_rx_set;
    unsigned fsm_rst;
    uint32_t txq[DEPTH + 8]; unsigned txn;
    uint32_t rxq[4096]; unsigned rxh, rxt;     // RX words
    int active, tx_ops, rx_ops;
    size_t tx_bytes_left, rx_bytes_left, tx_total, rx_total;
    uint32_t cur_word; unsigned cur_bytes;     // RX word being assembled
    unsigned last_valid;
    uint32_t irq;
    int stuck, inject_error;
    unsigned long writes_cancel, writes_abort;
    uint8_t seen_tx[8192]; size_t seen_n;
    uint32_t last_cmd0;
    int max_txn;
};

static uint8_t slave(uint8_t in, size_t i) { return (uint8_t)((in ^ 0x5a) + (uint8_t)i); }

static void finish_rx_word(struct sim *m) { m->rxq[m->rxt++ % 4096] = m->cur_word; m->cur_word = 0; m->cur_bytes = 0; }

static void tick(struct sim *m) {
    if (!m->active || m->stuck) return;
    if (m->inject_error) { m->irq |= GENI_M_CMD_OVERRUN; return; }
    // The slave shifts up to 8 bytes per poll.
    for (int k = 0; k < 8; k++) {
        size_t pos = m->tx_total ? m->tx_total - m->tx_bytes_left : m->rx_total - m->rx_bytes_left;
        uint8_t in = 0;
        if (m->tx_ops) {
            if (m->tx_bytes_left == 0) break;
            if (m->txn == 0) break;                       // underrun: wait for firmware
            in = (uint8_t)(m->txq[0] >> 0);
            // pop one byte from the packed word
            uint8_t b = (uint8_t)(m->txq[0] & 0xff);
            in = b;
            m->txq[0] >>= 8;
            m->tx_bytes_left--;
            m->seen_tx[m->seen_n++] = b;
            static unsigned used;                         // bytes consumed from txq[0]
            used = (used + 1) % 4;
            if (used == 0 || m->tx_bytes_left == 0) {
                for (unsigned i = 1; i < m->txn; i++) m->txq[i - 1] = m->txq[i];
                m->txn--;
                used = 0;
            }
        } else {
            if (m->rx_bytes_left == 0) break;
        }
        if (m->rx_ops) {
            if (m->rx_bytes_left == 0) break;
            uint8_t out = slave(in, pos);
            m->cur_word |= (uint32_t)out << (8 * m->cur_bytes);
            m->cur_bytes++;
            m->rx_bytes_left--;
            if (m->cur_bytes == 4) finish_rx_word(m);
            if (m->rx_bytes_left == 0 && m->cur_bytes) { m->last_valid = m->cur_bytes; finish_rx_word(m); m->irq |= GENI_M_RX_FIFO_LAST; }
            else if (m->rx_bytes_left == 0) { m->last_valid = 0; m->irq |= GENI_M_RX_FIFO_LAST; }
        }
    }
    unsigned rxwords = m->rxt - m->rxh;
    if (rxwords >= m->reg[SE_GENI_RX_WATERMARK_REG / 4] && m->reg[SE_GENI_RX_WATERMARK_REG / 4]) m->irq |= GENI_M_RX_FIFO_WATERMARK;
    if (m->tx_ops && m->reg[SE_GENI_TX_WATERMARK_REG / 4] && m->txn <= m->reg[SE_GENI_TX_WATERMARK_REG / 4])
        m->irq |= GENI_M_TX_FIFO_WATERMARK;
    if ((!m->tx_ops || m->tx_bytes_left == 0) && (!m->rx_ops || m->rx_bytes_left == 0)) {
        m->irq |= GENI_M_CMD_DONE;
        m->active = 0;
    }
}

// DMA mode: when the command and every requested direction's LEN register have been written, the slave exchanges the bytes
// between the buffers the pointer registers name (the test uses host addresses as bus addresses).
static void dma_maybe_run(struct sim *m) {
    if (!m->active || !(m->reg[SE_GENI_DMA_MODE_EN / 4] & 1)) return;
    if ((m->tx_ops && !m->dma_tx_set) || (m->rx_ops && !m->dma_rx_set)) return;
    if (m->stuck) return;
    if (m->inject_error) { if (m->tx_ops) m->dma_tx_stat |= 4; if (m->rx_ops) m->dma_rx_stat |= 4; return; }
    uint8_t *txp = (uint8_t *)(uintptr_t)(((uint64_t)m->reg[SE_DMA_TX_PTR_H / 4] << 32) | m->reg[SE_DMA_TX_PTR_L / 4]);
    uint8_t *rxp = (uint8_t *)(uintptr_t)(((uint64_t)m->reg[SE_DMA_RX_PTR_H / 4] << 32) | m->reg[SE_DMA_RX_PTR_L / 4]);
    size_t n = m->tx_ops ? m->dma_tx_len : m->dma_rx_len;
    CHECK(!m->tx_ops || !m->rx_ops || m->dma_tx_len == m->dma_rx_len);
    CHECK(!m->tx_ops || m->reg[SE_DMA_TX_ATTR / 4] == 1);
    for (size_t i = 0; i < n; i++) {
        uint8_t in = m->tx_ops ? txp[i] : 0;
        if (m->tx_ops) m->seen_tx[m->seen_n++] = in;
        if (m->rx_ops) rxp[i] = slave(in, i);
    }
    if (m->tx_ops) m->dma_tx_stat |= 1;
    if (m->rx_ops) m->dma_rx_stat |= 1;
    m->active = 0;
}

static uint32_t se_rd(void *ctx, uint32_t r) {
    struct sim *m = ctx;
    switch (r) {
    case GENI_FW_REVISION_RO: return m->fw_rev;
    case GENI_IF_DISABLE_RO: return m->if_disable;
    case SE_DMA_TX_IRQ_STAT: return m->dma_tx_stat;
    case SE_DMA_RX_IRQ_STAT: return m->dma_rx_stat;
    case SE_HW_PARAM_0: return m->hw_param0;
    case SE_GENI_M_IRQ_STATUS: tick(m); return m->irq;
    case SE_GENI_RX_FIFO_STATUS: {
        unsigned words = m->rxt - m->rxh;
        return words | ((m->irq & GENI_M_RX_FIFO_LAST) && words <= 1 ? GENI_RX_LAST | (m->last_valid << GENI_RX_LAST_VALID_SHIFT) : 0);
    }
    case SE_GENI_RX_FIFOn: return m->rxh < m->rxt ? m->rxq[m->rxh++ % 4096] : 0xdeadbeef;
    default: return m->reg[r / 4];
    }
}

static void se_wr(void *ctx, uint32_t r, uint32_t v) {
    struct sim *m = ctx;
    switch (r) {
    case SE_GENI_M_IRQ_CLEAR: m->irq &= ~v; return;
    case SE_DMA_TX_IRQ_CLR: m->dma_tx_stat &= ~v; return;
    case SE_DMA_RX_IRQ_CLR: m->dma_rx_stat &= ~v; return;
    case SE_DMA_TX_FSM_RST: case SE_DMA_RX_FSM_RST: m->fsm_rst++; return;
    case SE_DMA_TX_LEN: m->dma_tx_len = v; m->dma_tx_set = 1; m->reg[r / 4] = v; dma_maybe_run(m); return;
    case SE_DMA_RX_LEN: m->dma_rx_len = v; m->dma_rx_set = 1; m->reg[r / 4] = v; dma_maybe_run(m); return;
    case SE_GENI_TX_FIFOn:
        if (m->txn >= DEPTH) { m->irq |= GENI_M_TX_FIFO_WR_ERR; return; }
        m->txq[m->txn++] = v;
        if ((int)m->txn > m->max_txn) m->max_txn = (int)m->txn;
        return;
    case SE_GENI_M_CMD0: {
        m->last_cmd0 = v;
        uint32_t op = v >> GENI_M_OPCODE_SHIFT;
        CHECK(op == 1 || op == 2 || op == GENI_M_CMD_TX_RX);
        m->tx_ops = (op == 1 || op == GENI_M_CMD_TX_RX);
        m->rx_ops = (op == 2 || op == GENI_M_CMD_TX_RX);
        m->tx_total = m->tx_bytes_left = m->tx_ops ? m->reg[SE_SPI_TX_TRANS_LEN / 4] : 0;
        m->rx_total = m->rx_bytes_left = m->rx_ops ? m->reg[SE_SPI_RX_TRANS_LEN / 4] : 0;
        m->active = 1; m->cur_word = 0; m->cur_bytes = 0; m->rxh = m->rxt = 0; m->txn = 0; m->seen_n = 0;
        m->dma_tx_set = m->dma_rx_set = 0;
        return;
    }
    case SE_GENI_M_CMD_CTRL_REG:
        if (v & GENI_M_CMD_CANCEL) { m->writes_cancel++; if (!m->stuck) m->irq |= GENI_M_CMD_CANCEL_DONE; }
        if (v & GENI_M_CMD_ABORT) { m->writes_abort++; m->irq |= GENI_M_CMD_ABORT_DONE; m->active = 0; }
        return;
    }
    m->reg[r / 4] = v;
}

static uint32_t wr_rd(void *ctx, uint32_t r) { struct sim *m = ctx; CHECK(r == QUPV3_HW_VER_REG); return m->wrap_ver; }
static void wr_wr(void *ctx, uint32_t r, uint32_t v) { (void)ctx; (void)r; (void)v; FAIL("wrapper write"); }

static void make(struct sim *m, struct geni_spi *s) {
    memset(m, 0, sizeof *m);
    m->wrap_ver = 0x40010000u;                               // major 4: 8-bit FIFO depth field
    m->hw_param0 = (32u << 24) | ((uint32_t)DEPTH << 16);
    m->fw_rev = GENI_PROTO_SPI << GENI_FW_PROTO_SHIFT;
    memset(s, 0, sizeof *s);
    s->se = (struct geni_io){m, se_rd, se_wr, 0};
    s->wrapper = (struct geni_io){m, wr_rd, wr_wr, 0};
    s->poll_limit = 2000;
}

static struct { unsigned clean, clean_inval, inval; } cache;
static void clean(void *c, void *v, size_t l) { (void)c; (void)v; (void)l; cache.clean++; }
static void clean_inval(void *c, void *v, size_t l) { (void)c; (void)v; (void)l; cache.clean_inval++; }
static void inval(void *c, void *v, size_t l) { (void)c; (void)v; (void)l; cache.inval++; }

static void fill(uint8_t *b, size_t n, unsigned seed) { for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(seed + i * 7 + (i >> 3)); }

int main(void) {
    struct sim m; struct geni_spi s;
    // protocol checks
    make(&m, &s); m.fw_rev = 2u << GENI_FW_PROTO_SHIFT; CHECK(geni_spi_init(&s) == GENI_ERR_PROTO);
    make(&m, &s); m.if_disable = 1; CHECK(geni_spi_init(&s) == GENI_ERR_NOTSUP);
    make(&m, &s); m.if_disable = 1; s.allow_fifo_disabled = 1; CHECK(geni_spi_init(&s) == 0 && s.info.fifo_disabled);
    make(&m, &s); m.hw_param0 = (16u << 24) | (16u << 16); CHECK(geni_spi_init(&s) == GENI_ERR_NODEV);
    make(&m, &s); CHECK(geni_spi_init(&s) == 0);
    CHECK(s.info.tx_fifo_depth == DEPTH && s.info.fifo_width_bits == 32 && s.oversampling == 1);
    CHECK(m.reg[SE_GENI_RX_WATERMARK_REG / 4] == DEPTH - 3 && m.reg[SE_GENI_RX_RFR_WATERMARK_REG / 4] == DEPTH - 2);
    CHECK(m.reg[SE_GENI_DMA_MODE_EN / 4] == 0 && (m.reg[SE_GENI_CGC_CTRL / 4] & 0x7f) == 0x7f);
    make(&m, &s); s.poll_only = 1; CHECK(geni_spi_init(&s) == 0 && m.reg[SE_IRQ_EN / 4] == 0);
    // old QUP version: 6-bit depth field, oversampling stays 1 except 1.0
    make(&m, &s); m.wrap_ver = 0x10000000u; m.hw_param0 = (32u << 24) | (0x3fu << 16) | (0xc0u << 16); CHECK(geni_spi_init(&s) == 0);
    CHECK(s.info.tx_fifo_depth == 0x3f && s.oversampling == 2);

    make(&m, &s); CHECK(geni_spi_init(&s) == 0);
    // clock: 100 MHz source, 19.2 MHz -> divider 6
    CHECK(geni_spi_set_clock(&s, 19200000, 100000000, 0) == 0);
    CHECK(m.reg[GENI_SER_M_CLK_CFG / 4] == ((6u << 4) | 1) && m.reg[SE_GENI_CLK_SEL / 4] == 0);
    CHECK(geni_spi_set_clock(&s, 1, 100000000, 0) == GENI_ERR_PARAM);
    CHECK(geni_spi_set_clock(&s, 1000000, 100000000, 8) == GENI_ERR_PARAM);
    CHECK(geni_spi_set_clock(&s, 19200000, 100000000, 0) == 0);
    // mode 0, cs 0 then mode 3, cs 1
    CHECK(geni_spi_setup(&s, 0, 0) == 0);
    CHECK(m.reg[SE_SPI_CPHA / 4] == 0 && m.reg[SE_SPI_CPOL / 4] == 0);
    CHECK(geni_spi_setup(&s, GENI_SPI_CPHA | GENI_SPI_CPOL | GENI_SPI_CS_HIGH, 1) == 0);
    CHECK(m.reg[SE_SPI_CPHA / 4] == 1 && m.reg[SE_SPI_CPOL / 4] == 4 && m.reg[SE_SPI_DEMUX_SEL / 4] == 1 && m.reg[SE_SPI_DEMUX_OUTPUT_INV / 4] == 2);
    CHECK(geni_spi_setup(&s, 0, 0) == 0);
    CHECK(m.reg[SE_SPI_CPHA / 4] == 0 && m.reg[SE_SPI_CPOL / 4] == 0 && m.reg[SE_SPI_DEMUX_OUTPUT_INV / 4] == 0);
    CHECK(geni_spi_setup(&s, 0, 16) == GENI_ERR_PARAM);

    static uint8_t tx[8192], rx[8192];
    size_t lens[] = {1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 60, 63, 64, 65, 67, 100, 256, 1000, 5417 + 8};
    for (size_t li = 0; li < sizeof lens / sizeof *lens; li++) {
        size_t n = lens[li];
        fill(tx, n, (unsigned)li); memset(rx, 0, sizeof rx);
        int r = geni_spi_transfer(&s, tx, rx, n, 0);
        if (r) FAIL("full duplex len %zu -> %d", n, r);
        CHECK(m.last_cmd0 >> GENI_M_OPCODE_SHIFT == GENI_M_CMD_TX_RX);
        CHECK((m.last_cmd0 & GENI_M_PARAMS_MASK) == 0);
        CHECK(m.seen_n == n && memcmp(m.seen_tx, tx, n) == 0);
        for (size_t i = 0; i < n; i++) if (rx[i] != slave(tx[i], i)) FAIL("rx mismatch len %zu at %zu", n, i);
        CHECK(m.max_txn <= DEPTH);
        CHECK(m.reg[SE_SPI_TX_TRANS_LEN / 4] == n && m.reg[SE_SPI_RX_TRANS_LEN / 4] == n);
    }
    // keep_cs sets FRAGMENTATION
    fill(tx, 4, 9); CHECK(geni_spi_transfer(&s, tx, rx, 4, 1) == 0);
    CHECK((m.last_cmd0 & GENI_M_PARAM_FRAGMENTATION) != 0);
    // TX only
    fill(tx, 129, 3); CHECK(geni_spi_transfer(&s, tx, 0, 129, 0) == 0);
    CHECK(m.last_cmd0 >> GENI_M_OPCODE_SHIFT == 1 && m.seen_n == 129 && memcmp(m.seen_tx, tx, 129) == 0);
    // RX only (slave answers with slave(0, i))
    memset(rx, 0, 70); CHECK(geni_spi_transfer(&s, 0, rx, 70, 0) == 0);
    CHECK(m.last_cmd0 >> GENI_M_OPCODE_SHIFT == 2);
    for (size_t i = 0; i < 70; i++) CHECK(rx[i] == slave(0, i));
    // packing registers for 8-bit words, MSB first, four per FIFO word
    CHECK(m.reg[SE_GENI_TX_PACKING_CFG0 / 4] == 522494u && m.reg[SE_GENI_TX_PACKING_CFG1 / 4] == 1048318u);
    CHECK(m.reg[SE_GENI_RX_PACKING_CFG0 / 4] == 522494u && m.reg[SE_GENI_RX_PACKING_CFG1 / 4] == 1048318u);
    CHECK(m.reg[SE_SPI_WORD_LEN / 4] == 4 && m.reg[SE_GENI_BYTE_GRAN / 4] == 0);
    // argument errors
    CHECK(geni_spi_transfer(&s, tx, rx, 0, 0) == GENI_ERR_PARAM);
    CHECK(geni_spi_transfer(&s, 0, 0, 4, 0) == GENI_ERR_PARAM);
    // error IRQ
    m.inject_error = 1; CHECK(geni_spi_transfer(&s, tx, rx, 8, 0) == GENI_ERR_IO); m.inject_error = 0;
    // timeout -> cancel then abort
    m.stuck = 1; m.writes_cancel = m.writes_abort = 0;
    CHECK(geni_spi_transfer(&s, tx, rx, 8, 0) == GENI_ERR_TIMEOUT);
    CHECK(s.snap_valid);
    CHECK(m.writes_cancel == 1 && m.writes_abort == 1);
    m.stuck = 0;
    fill(tx, 12, 1); CHECK(geni_spi_transfer(&s, tx, rx, 12, 0) == 0);
    // SE DMA mode (FIFO interface disabled by the firmware): buffers, cache callbacks, errors and timeout
    {
        static uint8_t dtx[4096], drx[4096];
        make(&m, &s); m.if_disable = 1;
        CHECK(geni_spi_init(&s) == GENI_ERR_NOTSUP);                       // no DMA configured
        make(&m, &s); m.if_disable = 1; s.use_dma = 1;
        CHECK(geni_spi_init(&s) == GENI_ERR_PARAM);                        // buffers required
        make(&m, &s); m.if_disable = 1; s.use_dma = 1;
        s.dma = (struct geni_dma){0, clean, clean_inval, inval, dtx, drx, (uint64_t)(uintptr_t)dtx, (uint64_t)(uintptr_t)drx, sizeof dtx};
        CHECK(geni_spi_init(&s) == 0);
        CHECK(m.reg[SE_GENI_DMA_MODE_EN / 4] == 1);
        CHECK((m.reg[SE_GENI_M_IRQ_EN / 4] & (GENI_M_CMD_DONE | GENI_M_TX_FIFO_WATERMARK | GENI_M_RX_FIFO_WATERMARK | GENI_M_RX_FIFO_LAST)) == 0);
        CHECK(geni_spi_set_clock(&s, 19200000, 100000000, 0) == 0 && geni_spi_setup(&s, 0, 0) == 0);
        size_t dl[] = {1, 3, 4, 5, 63, 64, 100, 1000, 4096};
        for (size_t li = 0; li < sizeof dl / sizeof *dl; li++) {
            size_t n = dl[li];
            fill(tx, n, (unsigned)li + 40); memset(rx, 0, sizeof rx);
            int r = geni_spi_transfer(&s, tx, rx, n, li & 1);
            if (r) FAIL("dma full duplex len %zu -> %d", n, r);
            CHECK(m.last_cmd0 >> GENI_M_OPCODE_SHIFT == GENI_M_CMD_TX_RX);
            CHECK(!!(m.last_cmd0 & GENI_M_PARAM_FRAGMENTATION) == (int)(li & 1));
            CHECK(m.seen_n == n && memcmp(m.seen_tx, tx, n) == 0);
            for (size_t i = 0; i < n; i++) if (rx[i] != slave(tx[i], i)) FAIL("dma rx mismatch len %zu at %zu", n, i);
            CHECK(m.dma_tx_stat == 0 && m.dma_rx_stat == 0);               // status cleared after each transfer
        }
        CHECK(cache.clean == sizeof dl / sizeof *dl && cache.clean_inval == cache.clean && cache.inval == cache.clean);
        // TX only and RX only
        fill(tx, 33, 5); CHECK(geni_spi_transfer(&s, tx, 0, 33, 0) == 0 && m.last_cmd0 >> GENI_M_OPCODE_SHIFT == 1 && memcmp(m.seen_tx, tx, 33) == 0);
        memset(rx, 0, 20); CHECK(geni_spi_transfer(&s, 0, rx, 20, 0) == 0 && m.last_cmd0 >> GENI_M_OPCODE_SHIFT == 2);
        for (size_t i = 0; i < 20; i++) CHECK(rx[i] == slave(0, i));
        CHECK(geni_spi_transfer(&s, tx, rx, sizeof dtx + 1, 0) == GENI_ERR_PARAM);   // larger than the DMA buffer
        // errors: DMA bus error status, then a stuck engine (cancel/abort and FSM resets, snapshot kept)
        m.inject_error = 1; CHECK(geni_spi_transfer(&s, tx, rx, 8, 0) == GENI_ERR_IO); m.inject_error = 0;
        m.dma_tx_stat = m.dma_rx_stat = 0;
        m.stuck = 1; m.writes_cancel = m.writes_abort = 0; m.fsm_rst = 0; s.snap_valid = 0;
        CHECK(geni_spi_transfer(&s, tx, rx, 8, 0) == GENI_ERR_TIMEOUT && s.snap_valid);
        CHECK(m.writes_cancel == 1 && m.writes_abort == 1 && m.fsm_rst == 2);
        m.stuck = 0;
        fill(tx, 12, 1); CHECK(geni_spi_transfer(&s, tx, rx, 12, 0) == 0);
    }
    printf("PASS: GeniSpi init, clock, mode, packing, PIO and SE DMA transfers (%zu PIO lengths) against the register model\n", sizeof lens / sizeof *lens);
    return 0;
}
