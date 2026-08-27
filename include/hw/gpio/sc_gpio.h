#ifndef SC_GPIO_H
#define SC_GPIO_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_SC_GPIO "sc_soc.gpio"
typedef struct SCGPIOState SCGPIOState;
DECLARE_INSTANCE_CHECKER(SCGPIOState, SC_GPIO,
                         TYPE_SC_GPIO)

#define SC_GPIO_PINS 32
#define SC_GPIO_MAX_PORTS 32

#define SC_GPIO_REG_IP_VERSION  0x000
#define SC_GPIO_REG_IP_CONFIG   0x004
#define SC_GPIO_REG_IP_RST      0x010
#define SC_GPIO_REG_SCRATCH_PAD 0x01C
#define SC_GPIO_REG_INT_STATUS  0x020
#define SC_GPIO_REG_INT_ENABLE  0x024

#define SC_GPIO_NPORTS_MASK 0x3F
#define SC_GPIO_SW_RESET    (1U << 0)

#define SC_GPIO_VER_MAJOR_MASK 0xFF000000
#define SC_GPIO_VER_MINOR_MASK 0x00FF0000
#define SC_GPIO_VER_PATCH_MASK 0x0000FFFF

#define SC_GPIO_PORT_REG_BASE    0x100
#define SC_GPIO_PORT_REG_STRIDE  0x20
#define SC_GPIO_PORT_REG_OUTPUT  0x00
#define SC_GPIO_PORT_REG_INPUT   0x04
#define SC_GPIO_PORT_REG_DIR     0x08
#define SC_GPIO_PORT_REG_PRESENT 0x0C
#define SC_GPIO_PORT_REG_PENDING 0x10
#define SC_GPIO_PORT_REG_INT_EN  0x14
#define SC_GPIO_PORT_REG_RISE_EN 0x18
#define SC_GPIO_PORT_REG_FALL_EN 0x1C

#define SC_GPIO_SIZE (SC_GPIO_PORT_REG_BASE + \
                      SC_GPIO_MAX_PORTS * SC_GPIO_PORT_REG_STRIDE)

typedef struct SCGPIOPortState {
    uint32_t output;
    uint32_t input;
    uint32_t dir;
    uint32_t external_input;
    uint32_t external_mask;
    uint32_t driven;
    uint32_t drive_mask;
    uint32_t present;
    uint32_t pending;
    uint32_t int_en;
    uint32_t rise_en;
    uint32_t fall_en;
} SCGPIOPortState;

struct SCGPIOState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;

    qemu_irq irq;
    qemu_irq output[SC_GPIO_MAX_PORTS][SC_GPIO_PINS];

    uint32_t ip_version;
    uint32_t ip_config;
    uint32_t ip_reset;
    uint32_t scratch_pad;
    uint32_t int_enable;
    SCGPIOPortState ports[SC_GPIO_MAX_PORTS];

    /* config */
    uint32_t num_ports;
};

#endif /* SC_GPIO_H */
