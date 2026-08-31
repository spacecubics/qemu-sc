#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/ssi/ssi.h"
#include "qemu/fifo32.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "hw/ssi/sc_spi.h"

/* Register Offsets */
#define SC_SPI_VER       0x000
#define SC_SPI_IPCFG     0x004
#define SC_SPI_RST       0x010
#define SC_SPI_SPAD      0x01C
#define SC_SPI_ISR       0x020
#define SC_SPI_IER       0x024
#define SC_SPI_TCR       0x100
#define SC_SPI_TFR       0x104
#define SC_SPI_OMR       0x108
#define SC_SPI_TXDATA(n) (0x200 + 0x4 * (n))
#define SC_SPI_RXDATA(n) (0x300 + 0x4 * (n))

#define SC_SPI_INT_COMP   BIT(0)

static void sc_spi_update_irq(ScSPIState *s)
{
    qemu_set_irq(s->irq, !!(s->int_status & s->int_enable));
}

static void sc_spi_reset_registers(ScSPIState *s)
{
    s->ip_reset = BIT(0);
    s->scratch_pad = 0;
    s->int_enable = 0;
    s->int_status = 0;
    s->frame_size = 1;
    s->cs_ext = false;
    s->cs_select = 0;
    s->spi_status = false;
    s->cs_hold = 1;
    s->cs_setup = 1;
    s->clk_phase = true;
    s->clk_polarity = true;
    s->clk_div = 1;
    s->start_mode = false;
    s->byte_order = false;

    fifo32_reset(&s->tx_fifo);
    fifo32_reset(&s->rx_fifo);
}

static void sc_spi_reset(DeviceState *d)
{
    ScSPIState *s = SC_SPI(d);
    unsigned int i;

    s->ip_version = BIT(16);
    sc_spi_reset_registers(s);
    s->cs_active = false;

    /* SYSRSTB resets the SPI engine as well as its register interface. */
    for (i = 0; i < s->num_cs; i++) {
        qemu_set_irq(s->cs_lines[i], 1);
    }
    sc_spi_update_irq(s);
}

/*
 * Map a zero-based position in the transmitted bit stream to TXDATA/RXDATA.
 * transfer_bit zero is the first bit sent or received in a transfer.
 */
static unsigned int sc_spi_data_register_bit(const ScSPIState *s,
                                             unsigned int transfer_bit)
{
    if (s->byte_order) {
        return s->frame_size - 1 - transfer_bit;
    }

    return (transfer_bit / 8) * 8 + 7 - (transfer_bit % 8);
}

/*
 * Pack TXDATA into a right-aligned value in serial order. Its most
 * significant active bit is the first bit transmitted on the SPI bus.
 */
static uint32_t sc_spi_pack_txdata(const ScSPIState *s, uint32_t txdata)
{
    uint32_t frame = 0;
    unsigned int transfer_bit;

    for (transfer_bit = 0; transfer_bit < s->frame_size; transfer_bit++) {
        unsigned int frame_bit = s->frame_size - 1 - transfer_bit;
        unsigned int register_bit =
            sc_spi_data_register_bit(s, transfer_bit);

        frame = deposit32(frame, frame_bit, 1,
                          extract32(txdata, register_bit, 1));
    }

    return frame;
}

/* Convert a right-aligned value in serial order back to RXDATA layout. */
static uint32_t sc_spi_unpack_rxdata(const ScSPIState *s, uint32_t frame)
{
    uint32_t rxdata = 0;
    unsigned int transfer_bit;

    for (transfer_bit = 0; transfer_bit < s->frame_size; transfer_bit++) {
        unsigned int frame_bit = s->frame_size - 1 - transfer_bit;
        unsigned int register_bit =
            sc_spi_data_register_bit(s, transfer_bit);

        rxdata = deposit32(rxdata, register_bit, 1,
                           extract32(frame, frame_bit, 1));
    }

    return rxdata;
}

static uint32_t sc_spi_transfer_frame(ScSPIState *s, uint32_t tx_frame)
{
    uint32_t rx_frame = 0;
    unsigned int remaining_bits;

    if (s->frame_size % 8) {
        return ssi_transfer(s->spi, tx_frame);
    }

    /* QEMU's common SPI flash models consume one byte per SSI transfer. */
    for (remaining_bits = s->frame_size; remaining_bits > 0;
         remaining_bits -= 8) {
        unsigned int byte_shift = remaining_bits - 8;
        uint8_t tx_byte = extract32(tx_frame, byte_shift, 8);
        uint8_t rx_byte = ssi_transfer(s->spi, tx_byte);

        rx_frame = deposit32(rx_frame, byte_shift, 8, rx_byte);
    }

    return rx_frame;
}

static void sc_spi_flush_txfifo(ScSPIState *s)
{
    /*
     * txdata:   value written by the guest to TXDATA
     * tx_frame: bits arranged in physical transmission order
     * rx_frame: bits returned in physical reception order
     * rxdata:   received bits arranged in RXDATA register format
     */
    uint32_t txdata;
    uint32_t tx_frame;
    uint32_t rx_frame;
    uint32_t rxdata;

    while (!fifo32_is_empty(&s->tx_fifo)) {
        txdata = fifo32_pop(&s->tx_fifo);
        tx_frame = sc_spi_pack_txdata(s, txdata);
        rx_frame = sc_spi_transfer_frame(s, tx_frame);
        rxdata = sc_spi_unpack_rxdata(s, rx_frame);

        if (fifo32_is_full(&s->rx_fifo)) {
            fifo32_reset(&s->rx_fifo);
        }
        fifo32_push(&s->rx_fifo, rxdata);
    }
}

static uint64_t sc_spi_read(void *opaque, hwaddr addr, unsigned int size)
{
    ScSPIState *s = opaque;
    uint32_t r = 0;


    switch (addr) {
    case SC_SPI_VER:
        r = s->ip_version;
        break;
    case SC_SPI_IPCFG:
        r = deposit32(r, 0, 5, s->num_buf);
        r = deposit32(r, 8, 3, s->num_cs);
        break;
    case SC_SPI_RST:
        r = s->ip_reset;
        break;
    case SC_SPI_SPAD:
        r = s->scratch_pad;
        break;
    case SC_SPI_TCR:
        r = (uint32_t)s->spi_status;
        r = deposit32(r, 8, 5, s->cs_select);
        r = deposit32(r, 15, 1, s->cs_ext);
        r = deposit32(r, 16, 5, s->frame_size - 1);
        break;
    case SC_SPI_TFR:
        r = deposit32(r, 0, 8, s->clk_div);
        r = deposit32(r, 8, 1, s->clk_polarity);
        r = deposit32(r, 9, 1, s->clk_phase);
        r = deposit32(r, 16, 4, s->cs_setup);
        r = deposit32(r, 20, 4, s->cs_hold);
        break;
    case SC_SPI_OMR:
        r = (uint32_t)s->start_mode;
        r = deposit32(r, 8, 1, s->byte_order);
        break;
    case SC_SPI_IER:
        r = s->int_enable;
        break;
    case SC_SPI_ISR:
        r = s->int_status;
        break;
    case SC_SPI_RXDATA(0):
        if (fifo32_is_empty(&s->rx_fifo)) {
            return 0;
        }
        r = fifo32_pop(&s->rx_fifo);
        break;
    default:
        break;
    }

    sc_spi_update_irq(s);

    return r;
}

static void sc_spi_write(void *opaque, hwaddr addr,
                              uint64_t val64, unsigned int size)
{
    ScSPIState *s = opaque; uint32_t value = val64;

    /* While IPRST is set, only its own register can release the reset. */
    if (s->ip_reset && addr != SC_SPI_RST) {
        sc_spi_update_irq(s);
        return;
    }

    switch (addr) {
        case SC_SPI_SPAD:
            s->scratch_pad = value;
            break;
        case SC_SPI_RST:
            if (value & BIT(0)) {
                /* IPRST resets the register block, not the SPI engine. */
                sc_spi_reset_registers(s);
            } else {
                s->ip_reset = false;
            }
            break;
        case SC_SPI_TCR:
            s->spi_status = extract32(value, 0, 1);
            s->cs_select = extract32(value, 8, 5);
            s->cs_ext = extract32(value, 15, 1);
            s->frame_size = extract32(value, 16, 5) + 1;
            if (s->spi_status) {
                if (s->cs_select < s->num_cs) {
                    qemu_set_irq(s->cs_lines[s->cs_select], 0);
                    s->active_cs = s->cs_select;
                    s->cs_active = true;
                }

                sc_spi_flush_txfifo(s);

                if (!s->cs_ext && s->cs_select < s->num_cs) {
                    qemu_set_irq(s->cs_lines[s->cs_select], 1);
                    s->cs_active = false;
                }

                s->spi_status = false;
                s->int_status |= SC_SPI_INT_COMP;
            }
            break;
        case SC_SPI_TFR:
            s->clk_div = extract32(value, 0, 8);
            s->clk_polarity = extract32(value, 8, 1);
            s->clk_phase = extract32(value, 9, 1);
            s->cs_setup = extract32(value, 16, 4);
            s->cs_hold = extract32(value, 20, 4);
            break;
        case SC_SPI_OMR:
            s->start_mode = extract32(value, 0, 1);
            s->byte_order = extract32(value, 8, 1);
            break;
        case SC_SPI_IER:
            s->int_enable = value;
            break;
        case SC_SPI_ISR:
            s->int_status &= ~value;
            break;
        case SC_SPI_TXDATA(0):
            if (fifo32_is_full(&s->tx_fifo)) {
                fifo32_reset(&s->tx_fifo);
            }
            fifo32_push(&s->tx_fifo, value);
            break;
        default:
            break;
    }

    sc_spi_update_irq(s);
}

static const MemoryRegionOps sc_spi_ops = {
    .read = sc_spi_read,
    .write = sc_spi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4
    }
};

static int sc_spi_post_load(void *opaque, int version_id)
{
    ScSPIState *s = opaque;
    unsigned int i;

    for (i = 0; i < s->num_cs; i++) {
        qemu_set_irq(s->cs_lines[i], 1);
    }
    if (s->cs_active && s->active_cs < s->num_cs) {
        qemu_set_irq(s->cs_lines[s->active_cs], 0);
    }
    sc_spi_update_irq(s);

    return 0;
}

static const VMStateDescription vmstate_sc_spi = {
    .name = TYPE_SC_SPI,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = sc_spi_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ip_reset, ScSPIState),
        VMSTATE_UINT32(scratch_pad, ScSPIState),
        VMSTATE_UINT32(int_enable, ScSPIState),
        VMSTATE_UINT32(int_status, ScSPIState),
        VMSTATE_UINT16(frame_size, ScSPIState),
        VMSTATE_BOOL(cs_ext, ScSPIState),
        VMSTATE_UINT8(cs_select, ScSPIState),
        VMSTATE_BOOL(spi_status, ScSPIState),
        VMSTATE_BOOL(cs_active, ScSPIState),
        VMSTATE_UINT8(active_cs, ScSPIState),
        VMSTATE_UINT8(cs_hold, ScSPIState),
        VMSTATE_UINT8(cs_setup, ScSPIState),
        VMSTATE_BOOL(clk_phase, ScSPIState),
        VMSTATE_BOOL(clk_polarity, ScSPIState),
        VMSTATE_UINT8(clk_div, ScSPIState),
        VMSTATE_BOOL(start_mode, ScSPIState),
        VMSTATE_BOOL(byte_order, ScSPIState),
        VMSTATE_FIFO32(tx_fifo, ScSPIState),
        VMSTATE_FIFO32(rx_fifo, ScSPIState),
        VMSTATE_END_OF_LIST()
    }
};

static void sc_spi_realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    ScSPIState *s = SC_SPI(dev);
    int i;

    s->spi = ssi_create_bus(dev, "spi");
    sysbus_init_irq(sbd, &s->irq);

    s->cs_lines = g_new0(qemu_irq, s->num_cs);
    for (i = 0; i < s->num_cs; i++) {
        sysbus_init_irq(sbd, &s->cs_lines[i]);
    }

    memory_region_init_io(&s->mmio, OBJECT(s), &sc_spi_ops, s,
                          TYPE_SC_SPI, 0x1000);
    sysbus_init_mmio(sbd, &s->mmio);

    fifo32_create(&s->tx_fifo, 1);
    fifo32_create(&s->rx_fifo, 1);
}

static const Property sc_spi_properties[] = {
    DEFINE_PROP_UINT32("num-cs", ScSPIState, num_cs, 1),
    DEFINE_PROP_UINT8("num-buf", ScSPIState, num_buf, 1),
};

static void sc_spi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, sc_spi_properties);
    device_class_set_legacy_reset(dc, sc_spi_reset);
    dc->vmsd = &vmstate_sc_spi;
    dc->realize = sc_spi_realize;
}

static const TypeInfo sc_spi_info = {
    .name           = TYPE_SC_SPI,
    .parent         = TYPE_SYS_BUS_DEVICE,
    .instance_size  = sizeof(ScSPIState),
    .class_init     = sc_spi_class_init,
};

static void sc_spi_register_types(void)
{
    type_register_static(&sc_spi_info);
}

type_init(sc_spi_register_types)
