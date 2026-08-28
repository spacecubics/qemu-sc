#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/gpio/sc_gpio.h"
#include "migration/vmstate.h"
#include "trace.h"

static void update_int_status(SCGPIOState *s)
{
    uint32_t port;

    s->int_status = 0;
    for (port = 0; port < s->num_ports; port++) {
        SCGPIOPortState *p = &s->ports[port];

        if (p->pending & p->int_en) {
            s->int_status |= BIT(port);
        }
    }

    qemu_set_irq(s->irq, !!(s->int_status & s->int_enable));
}

static void update_state(SCGPIOState *s)
{
    uint32_t port;
    uint32_t pin;

    if (s->ip_reset) {
        return;
    }

    for (port = 0; port < s->num_ports; port++) {
        SCGPIOPortState *p = &s->ports[port];
        for (pin = 0; pin < SC_GPIO_PINS; pin++) {
            bool pin_present = extract32(p->present, pin, 1);
            bool externally_driven = pin_present &&
                                     extract32(p->external_mask, pin, 1);
            bool output_enabled = pin_present &&
                                  extract32(p->dir, pin, 1);
            bool output = extract32(p->output, pin, 1);
            bool value;
            bool old_value;

            if (externally_driven) {
                value = extract32(p->external_input, pin, 1);
            } else if (output_enabled) {
                value = output;
            } else {
                value = false;
            }

            old_value = extract32(p->input, pin, 1);
            p->input = deposit32(p->input, pin, 1, value);

            /* Check if interrupt condition is fulfilled */
            if (pin_present && value != old_value &&
                ((value && extract32(p->rise_en, pin, 1)) ||
                 (!value && extract32(p->fall_en, pin, 1)))) {
                p->pending = deposit32(p->pending, pin, 1, 1);
            }

            if (extract32(p->drive_mask, pin, 1) != output_enabled ||
                (output_enabled &&
                 extract32(p->driven, pin, 1) != output)) {
                p->drive_mask = deposit32(p->drive_mask, pin, 1,
                                           output_enabled);
                p->driven = deposit32(p->driven, pin, 1, output);
                qemu_set_irq(s->output[port][pin],
                             output_enabled ? output : -1);
            }
        }
    }

    update_int_status(s);
}

static uint64_t sc_gpio_read(void *opaque, hwaddr offset, unsigned int size)
{
    SCGPIOState *s = SC_GPIO(opaque);
    uint32_t r = 0;

    if (offset >= SC_GPIO_PORT_REG_BASE) {
        uint32_t relative = offset - SC_GPIO_PORT_REG_BASE;
        uint32_t port = relative / SC_GPIO_PORT_REG_STRIDE;
        uint32_t reg = relative % SC_GPIO_PORT_REG_STRIDE;
        SCGPIOPortState *p;

        if (port >= s->num_ports) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: bad read offset 0x%" HWADDR_PRIx "\n",
                          __func__, offset);
            return 0;
        }

        p = &s->ports[port];

        switch (reg) {
        case SC_GPIO_PORT_REG_INPUT:
            r = p->input & p->present;
            break;
        case SC_GPIO_PORT_REG_PRESENT:
            r = p->present;
            break;
        case SC_GPIO_PORT_REG_OUTPUT:
            r = p->output & p->present;
            break;
        case SC_GPIO_PORT_REG_DIR:
            r = p->dir & p->present;
            break;
        case SC_GPIO_PORT_REG_FALL_EN:
            r = p->fall_en & p->present;
            break;
        case SC_GPIO_PORT_REG_RISE_EN:
            r = p->rise_en & p->present;
            break;
        case SC_GPIO_PORT_REG_INT_EN:
            r = p->int_en & p->present;
            break;
        case SC_GPIO_PORT_REG_PENDING:
            r = p->pending & p->present;
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: bad read offset 0x%" HWADDR_PRIx "\n",
                          __func__, offset);
        }
    } else {
        switch (offset) {
        case SC_GPIO_REG_IP_VERSION:
            r = s->ip_version;
            break;
        case SC_GPIO_REG_IP_CONFIG:
            r = s->ip_config;
            break;
        case SC_GPIO_REG_IP_RST:
            r = s->ip_reset;
            break;
        case SC_GPIO_REG_SCRATCH_PAD:
            r = s->scratch_pad;
            break;
        case SC_GPIO_REG_INT_ENABLE:
            r = s->int_enable;
            break;
        case SC_GPIO_REG_INT_STATUS:
            r = s->int_status;
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: bad read offset 0x%" HWADDR_PRIx "\n",
                          __func__, offset);
        }
    }

    return (uint64_t)r;
}

static void sc_gpio_write(void *opaque, hwaddr offset,
                              uint64_t value, unsigned int size)
{
    SCGPIOState *s = SC_GPIO(opaque);

    if (offset >= SC_GPIO_PORT_REG_BASE) {
        uint32_t relative = offset - SC_GPIO_PORT_REG_BASE;
        uint32_t port = relative / SC_GPIO_PORT_REG_STRIDE;
        uint32_t reg = relative % SC_GPIO_PORT_REG_STRIDE;
        SCGPIOPortState *p;
        uint32_t masked_value;

        if (port >= s->num_ports) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: bad write offset 0x%" HWADDR_PRIx "\n",
                          __func__, offset);
            return;
        }

        p = &s->ports[port];
        masked_value = (uint32_t)value & p->present;

        switch (reg) {
        case SC_GPIO_PORT_REG_DIR:
            p->dir = masked_value;
            break;
        case SC_GPIO_PORT_REG_OUTPUT:
            p->output = masked_value;
            break;
        case SC_GPIO_PORT_REG_FALL_EN:
            p->fall_en = masked_value;
            break;
        case SC_GPIO_PORT_REG_RISE_EN:
            p->rise_en = masked_value;
            break;
        case SC_GPIO_PORT_REG_INT_EN:
            p->int_en = masked_value;
            break;
        case SC_GPIO_PORT_REG_PENDING:
            p->pending &= ~masked_value;
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: bad write offset 0x%" HWADDR_PRIx "\n",
                          __func__, offset);
        }
    } else {
        switch (offset) {
        case SC_GPIO_REG_IP_RST:
            s->ip_reset = (uint32_t)value;
            break;
        case SC_GPIO_REG_SCRATCH_PAD:
            s->scratch_pad = (uint32_t)value;
            break;
        case SC_GPIO_REG_INT_ENABLE:
            s->int_enable = deposit32(0, 0, s->num_ports, (uint32_t)value);
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: bad write offset 0x%" HWADDR_PRIx "\n",
                          __func__, offset);
        }
    }

    update_state(s);
}

static const MemoryRegionOps gpio_ops = {
    .read =  sc_gpio_read,
    .write = sc_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void sc_gpio_set(void *opaque, int line, int value)
{
    SCGPIOState *s = SC_GPIO(opaque);

    uint32_t port = line / SC_GPIO_PINS;
    uint32_t pin = line % SC_GPIO_PINS;

    assert(port < s->num_ports);

    s->ports[port].external_mask = deposit32(s->ports[port].external_mask,
                                              pin, 1, value >= 0);
    if (value >= 0) {
        s->ports[port].external_input =
            deposit32(s->ports[port].external_input, pin, 1, value != 0);
    }

    update_state(s);
}

static void sc_gpio_reset(DeviceState *dev)
{
    SCGPIOState *s = SC_GPIO(dev);

    s->ip_version = BIT(24);
    s->ip_config = s->num_ports;
    s->ip_reset = BIT(0);
    s->scratch_pad = 0;
    s->int_enable = 0;
    s->int_status = 0;
    qemu_set_irq(s->irq, 0);
    for (uint32_t i = 0; i < SC_GPIO_MAX_PORTS; i++) {
        s->ports[i].output = 0;
        s->ports[i].input = 0;
        s->ports[i].external_input = 0;
        s->ports[i].external_mask = 0;
        s->ports[i].driven = 0;
        s->ports[i].drive_mask = 0;
        s->ports[i].present = i < s->num_ports ?
                              (s->num_present ? s->present[i] : UINT32_MAX) :
                              0;
        s->ports[i].pending = 0;
        s->ports[i].int_en = 0;
        s->ports[i].rise_en = 0;
        s->ports[i].fall_en = 0;

        if (i < s->num_ports) {
            for (uint32_t pin = 0; pin < SC_GPIO_PINS; pin++) {
                qemu_set_irq(s->output[i][pin], -1);
            }
        }
    }
}

static const VMStateDescription vmstate_sc_gpio_port = {
    .name = TYPE_SC_GPIO "/port",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(output,  SCGPIOPortState),
        VMSTATE_UINT32(input,   SCGPIOPortState),
        VMSTATE_UINT32(dir,     SCGPIOPortState),
        VMSTATE_UINT32(external_input, SCGPIOPortState),
        VMSTATE_UINT32(external_mask,  SCGPIOPortState),
        VMSTATE_UINT32(driven,         SCGPIOPortState),
        VMSTATE_UINT32(drive_mask,     SCGPIOPortState),
        VMSTATE_UINT32(present, SCGPIOPortState),
        VMSTATE_UINT32(pending, SCGPIOPortState),
        VMSTATE_UINT32(int_en,  SCGPIOPortState),
        VMSTATE_UINT32(rise_en, SCGPIOPortState),
        VMSTATE_UINT32(fall_en, SCGPIOPortState),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sc_gpio = {
    .name = TYPE_SC_GPIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ip_version,  SCGPIOState),
        VMSTATE_UINT32(ip_config,   SCGPIOState),
        VMSTATE_UINT32(ip_reset,   SCGPIOState),
        VMSTATE_UINT32(scratch_pad, SCGPIOState),
        VMSTATE_UINT32(int_enable,  SCGPIOState),
        VMSTATE_UINT32(int_status, SCGPIOState),
        VMSTATE_STRUCT_ARRAY(ports, SCGPIOState, SC_GPIO_MAX_PORTS,
                             1, vmstate_sc_gpio_port, SCGPIOPortState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property sc_gpio_properties[] = {
    DEFINE_PROP_UINT32("num_ports", SCGPIOState, num_ports, 1),
    DEFINE_PROP_ARRAY("present", SCGPIOState, num_present, present,
                      qdev_prop_uint32, uint32_t),
};

static void sc_gpio_realize(DeviceState *dev, Error **errp)
{
    SCGPIOState *s = SC_GPIO(dev);

    if (s->num_ports == 0 || s->num_ports > SC_GPIO_MAX_PORTS) {
        error_setg(errp, "num_ports must be between 1 and %u",
                   SC_GPIO_MAX_PORTS);
        return;
    }

    if (s->num_present && s->num_present != s->num_ports) {
        error_setg(errp, "present array must contain one mask per port");
        return;
    }

    memory_region_init_io(&s->mmio, OBJECT(dev), &gpio_ops, s,
            TYPE_SC_GPIO, SC_GPIO_SIZE);

    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    qdev_init_gpio_in(DEVICE(s), sc_gpio_set, s->num_ports * SC_GPIO_PINS);
    qdev_init_gpio_out(DEVICE(s), &s->output[0][0],
                       s->num_ports * SC_GPIO_PINS);
}

static void sc_gpio_finalize(Object *obj)
{
    SCGPIOState *s = SC_GPIO(obj);

    g_free(s->present);
}

static void sc_gpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, sc_gpio_properties);
    dc->vmsd = &vmstate_sc_gpio;
    dc->realize = sc_gpio_realize;
    device_class_set_legacy_reset(dc, sc_gpio_reset);
    dc->desc = "SC GPIO IP core";
}

static const TypeInfo sc_gpio_info = {
    .name = TYPE_SC_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SCGPIOState),
    .instance_finalize = sc_gpio_finalize,
    .class_init = sc_gpio_class_init
};

static void sc_gpio_register_types(void)
{
    type_register_static(&sc_gpio_info);
}

type_init(sc_gpio_register_types)
