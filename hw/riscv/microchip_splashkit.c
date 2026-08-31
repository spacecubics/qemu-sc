#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "hw/block/flash.h"
#include "hw/gpio/sc_gpio.h"
#include "hw/ssi/sc_spi.h"
#include "hw/char/microchip_coreuart.h"
#include "hw/intc/riscv_aclint.h"
#include "hw/intc/sifive_plic.h"
#include "hw/riscv/boot.h"
#include "hw/riscv/riscv_hart.h"
#include "chardev/char-fe.h"
#include "chardev/char.h"
#include "qobject/qlist.h"
#include "target/riscv/cpu.h"
#include "system/system.h"
#include "hw/ssi/ssi.h"

enum {
    MICROCHIP_SPLASHKIT_ROM,
    MICROCHIP_SPLASHKIT_PLIC,
    MICROCHIP_SPLASHKIT_UART,
    MICROCHIP_SPLASHKIT_MTIMECMP,
    MICROCHIP_SPLASHKIT_MTIME,
    MICROCHIP_SPLASHKIT_TCM,
    SPACECUBICS_GPIO,
    SPACECUBICS_SPI,
};

static const MemMapEntry microchip_splashkit_memmap[] = {
    [MICROCHIP_SPLASHKIT_ROM] =      { 0x00001000, 0x00001000 },
    [MICROCHIP_SPLASHKIT_PLIC] =     { 0x10000000, 0x04000000 },
    [MICROCHIP_SPLASHKIT_UART] =     { 0x11000000, 0x00001000 },
    [MICROCHIP_SPLASHKIT_MTIMECMP] = { 0x12004000, 0x00000008 },
    [MICROCHIP_SPLASHKIT_MTIME] =    { 0x1200bff8, 0x00000008 },
    [MICROCHIP_SPLASHKIT_TCM] =      { 0x20000000, 0x00040000 },
    [SPACECUBICS_GPIO] =             { 0x80010000, 0x00001000 },
    [SPACECUBICS_SPI] =             { 0x80040000, 0x00001000 },
};

#define MICROCHIP_SPLASHKIT_TIMEBASE_FREQ 25000000
#define MICROCHIP_SPLASHKIT_PLIC_SOURCES  32
#define MICROCHIP_SPLASHKIT_GPIO_IRQ      1
#define MICROCHIP_SPLASHKIT_SPI_IRQ      4
/* The DTS PLIC window overlaps the timer; only the register aperture is MMIO. */
#define MICROCHIP_SPLASHKIT_PLIC_APERTURE_SIZE 0x00201000
#define MICROCHIP_SPLASHKIT_MTIMER_SIZE 0x00008000
#define TYPE_MICROCHIP_SPLASHKIT_MACHINE MACHINE_TYPE_NAME("microchip-splashkit")
OBJECT_DECLARE_SIMPLE_TYPE(MicrochipSplashKitState,
                           MICROCHIP_SPLASHKIT_MACHINE)

typedef struct MicrochipSplashKitState {
    MachineState parent_obj;
    RISCVHartArrayState cpus;
    MemoryRegion rom;
    DeviceState *plic;
} MicrochipSplashKitState;

static void microchip_splashkit_machine_init(MachineState *machine)
{
    MicrochipSplashKitState *s = MICROCHIP_SPLASHKIT_MACHINE(machine);
    MemoryRegion *system_memory = get_system_memory();
    const MemMapEntry *memmap = microchip_splashkit_memmap;
    DeviceState *uart;
    DeviceState *gpio;
    DeviceState *spi;
    DeviceState *flash_dev;
    QList *gpio_present;
    QList *gpio_config;
    RISCVBootInfo boot_info;
    hwaddr kernel_entry = memmap[MICROCHIP_SPLASHKIT_TCM].base;

    if (machine->ram_size != memmap[MICROCHIP_SPLASHKIT_TCM].size) {
        error_report("Invalid RAM size, should be 256 KiB");
        exit(EXIT_FAILURE);
    }

    sysbus_realize(SYS_BUS_DEVICE(&s->cpus), &error_fatal);

    memory_region_init_rom(&s->rom, NULL, "microchip.splashkit.rom",
                           memmap[MICROCHIP_SPLASHKIT_ROM].size, &error_fatal);
    memory_region_add_subregion(system_memory,
                                memmap[MICROCHIP_SPLASHKIT_ROM].base, &s->rom);
    memory_region_add_subregion(system_memory,
                                memmap[MICROCHIP_SPLASHKIT_TCM].base,
                                machine->ram);

    s->plic = sifive_plic_create(memmap[MICROCHIP_SPLASHKIT_PLIC].base,
                                 "M", 1, 0,
                                 MICROCHIP_SPLASHKIT_PLIC_SOURCES, 2, 0, 0x1000,
                                 0x2000, 0x80, 0x200000, 0x1000,
                                 MICROCHIP_SPLASHKIT_PLIC_APERTURE_SIZE);
    riscv_aclint_mtimer_create(memmap[MICROCHIP_SPLASHKIT_MTIMECMP].base,
                               MICROCHIP_SPLASHKIT_MTIMER_SIZE, 0, 1, 0,
                               memmap[MICROCHIP_SPLASHKIT_MTIME].base -
                               memmap[MICROCHIP_SPLASHKIT_MTIMECMP].base,
                               MICROCHIP_SPLASHKIT_TIMEBASE_FREQ, false);

    uart = qdev_new(TYPE_MICROCHIP_COREUART);
    qdev_prop_set_chr(uart, "chardev", serial_hd(0));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(uart), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(uart), 0,
                    memmap[MICROCHIP_SPLASHKIT_UART].base);

    spi = qdev_new(TYPE_SC_SPI);
    qdev_prop_set_uint32(spi, "num-cs", 1);
    qdev_prop_set_uint8(spi, "num-buf", 1);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(spi), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(spi), 0,
                       qdev_get_gpio_in(s->plic,
                                        MICROCHIP_SPLASHKIT_SPI_IRQ));
    sysbus_mmio_map(SYS_BUS_DEVICE(spi), 0, memmap[SPACECUBICS_SPI].base);

    /* Example flash device */
    flash_dev = qdev_new("w25q64");
    ssi_realize_and_unref(flash_dev, SC_SPI(spi)->spi, &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(spi), 1, qdev_get_gpio_in_named(flash_dev, SSI_GPIO_CS, 0));

    gpio = qdev_new(TYPE_SC_GPIO);
    qdev_prop_set_uint32(gpio, "num_ports", 3);
    gpio_present = qlist_new();
    qlist_append_int(gpio_present, 0x0000ffff);
    qlist_append_int(gpio_present, 0xffffffff);
    qlist_append_int(gpio_present, 0xffffffff);
    qdev_prop_set_array(gpio, "present", gpio_present);
    gpio_config = qlist_new();
    qlist_append_int(gpio_config, 0x00000000);
    qlist_append_int(gpio_config, 0xffffffff);
    qlist_append_int(gpio_config, 0xffffffff);
    qdev_prop_set_array(gpio, "dir-writable", gpio_config);
    gpio_config = qlist_new();
    qlist_append_int(gpio_config, 0x000000ff);
    qlist_append_int(gpio_config, 0x00000000);
    qlist_append_int(gpio_config, 0x00000000);
    qdev_prop_set_array(gpio, "dir-init", gpio_config);
    gpio_config = qlist_new();
    qlist_append_int(gpio_config, 0x00000000);
    qlist_append_int(gpio_config, 0x00000000);
    qlist_append_int(gpio_config, 0x00000000);
    qdev_prop_set_array(gpio, "out-init", gpio_config);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(gpio), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(gpio), 0,
                       qdev_get_gpio_in(s->plic,
                                        MICROCHIP_SPLASHKIT_GPIO_IRQ));
    sysbus_mmio_map(SYS_BUS_DEVICE(gpio), 0,
                    memmap[SPACECUBICS_GPIO].base);

    for (int pin = 0; pin < SC_GPIO_PINS; pin++) {
        qdev_connect_gpio_out(gpio, SC_GPIO_PINS + pin,
                              qdev_get_gpio_in(gpio,
                                               2 * SC_GPIO_PINS + pin));
        qdev_connect_gpio_out(gpio, 2 * SC_GPIO_PINS + pin,
                              qdev_get_gpio_in(gpio,
                                               SC_GPIO_PINS + pin));
    }

    riscv_boot_info_init(&boot_info, &s->cpus);
    if (machine->kernel_filename) {
        riscv_load_kernel(machine, &boot_info, kernel_entry, false, NULL);
        kernel_entry = boot_info.image_low_addr;
    } else if (machine->firmware) {
        riscv_load_firmware(machine->firmware, &kernel_entry, NULL);
    }
    riscv_setup_rom_reset_vec(machine, &s->cpus, kernel_entry,
                              memmap[MICROCHIP_SPLASHKIT_ROM].base,
                              memmap[MICROCHIP_SPLASHKIT_ROM].size,
                              kernel_entry, 0);
}

static void microchip_splashkit_machine_instance_init(Object *obj)
{
    MicrochipSplashKitState *s = MICROCHIP_SPLASHKIT_MACHINE(obj);

    object_initialize_child(obj, "cpus", &s->cpus, TYPE_RISCV_HART_ARRAY);
    qdev_prop_set_string(DEVICE(&s->cpus), "cpu-type", TYPE_RISCV_CPU_SIFIVE_E31);
    qdev_prop_set_uint32(DEVICE(&s->cpus), "num-harts", 1);
    qdev_prop_set_uint64(DEVICE(&s->cpus), "resetvec",
                          microchip_splashkit_memmap[
                              MICROCHIP_SPLASHKIT_ROM].base);
}

static void microchip_splashkit_machine_class_init(ObjectClass *oc,
                                                   const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Microchip Splash Kit";
    mc->init = microchip_splashkit_machine_init;
    mc->max_cpus = 1;
    mc->default_cpu_type = TYPE_RISCV_CPU_SIFIVE_E31;
    mc->default_ram_id = "microchip.splashkit.tcm";
    mc->default_ram_size =
        microchip_splashkit_memmap[MICROCHIP_SPLASHKIT_TCM].size;
}

static const TypeInfo microchip_splashkit_machine_type_info = {
    .name = TYPE_MICROCHIP_SPLASHKIT_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(MicrochipSplashKitState),
    .instance_init = microchip_splashkit_machine_instance_init,
    .class_init = microchip_splashkit_machine_class_init,
};

static void microchip_splashkit_register_types(void)
{
    type_register_static(&microchip_splashkit_machine_type_info);
}

type_init(microchip_splashkit_register_types)
