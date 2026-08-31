#ifndef HW_SC_SPI_H
#define HW_SC_SPI_H

#include "hw/core/sysbus.h"

#define TYPE_SC_SPI "sc.spi"
#define SC_SPI(obj) OBJECT_CHECK(ScSPIState, (obj), TYPE_SC_SPI)

typedef struct ScSPIState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;

    uint32_t ip_version;
    uint32_t ip_reset;
    uint32_t scratch_pad;
    uint32_t int_enable;
    uint32_t int_status;
    /* Transaction Control Register */
    uint16_t frame_size;
    bool cs_ext;
    uint8_t cs_select;
    bool spi_status;
    bool cs_active;
    uint8_t active_cs;
    /* Transaction Format Register */
    uint8_t cs_hold;
    uint8_t cs_setup;
    bool clk_phase;
    bool clk_polarity;
    uint8_t clk_div;
    /* Operation Mode Register */
    bool start_mode;
    bool byte_order;
    
    uint32_t num_cs;
    uint8_t num_buf;
    qemu_irq *cs_lines;

    SSIBus *spi;

    uint32_t txdata;
    uint32_t rxdata;
} ScSPIState;

#endif /* HW_SC_SPI_H */
