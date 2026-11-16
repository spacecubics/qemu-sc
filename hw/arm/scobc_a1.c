/*
 * Space Cubics SCOBC A1 board
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/arm/armv7m.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "hw/char/xilinx_uartlite.h"
#include "hw/misc/unimp.h"

static void scobc_a1_init(MachineState *machine)
{
    DeviceState *uart;
    DeviceState *armv7m;
    Clock *cpuclk;

    /* Only direct boot into HRMEM is currently supported */
    memory_region_add_subregion(get_system_memory(), 0x00000000, machine->ram);

    cpuclk = clock_new(OBJECT(machine), "cpuclk");
    clock_set_hz(cpuclk, 48000000);

    armv7m = qdev_new(TYPE_ARMV7M);
    object_property_add_child(OBJECT(machine), "armv7m", OBJECT(armv7m));
    qdev_prop_set_string(armv7m, "cpu-type", machine->cpu_type);

    qdev_prop_set_uint32(armv7m, "num-irq", 32);
    qdev_prop_set_uint8(armv7m, "num-prio-bits", 3);
    object_property_set_link(OBJECT(armv7m), "memory",
                             OBJECT(get_system_memory()), &error_fatal);
    qdev_connect_clock_in(armv7m, "cpuclk", cpuclk);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(armv7m), &error_fatal);

    uart = qdev_new(TYPE_XILINX_UARTLITE);
    qdev_prop_set_enum(uart, "endianness", ENDIAN_MODE_LITTLE);
    qdev_prop_set_chr(uart, "chardev", serial_hd(0));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(uart), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(uart), 0, 0x4f010000);
    sysbus_connect_irq(SYS_BUS_DEVICE(uart), 0,
                       qdev_get_gpio_in(armv7m, 0));

    /* System peripherals */
    create_unimplemented_device("scobc-a1.qspi-config", 0x40000000, 0x10000);
    create_unimplemented_device("scobc-a1.qspi-data", 0x40100000, 0x10000);
    create_unimplemented_device("scobc-a1.qspi-fram", 0x40200000, 0x10000);
    create_unimplemented_device("scobc-a1.can", 0x40400000, 0x10000);
    create_unimplemented_device("scobc-a1.hrmem-regs", 0x40500000, 0x10000);
    create_unimplemented_device("scobc-a1.sysreg", 0x4f000000, 0x10000);
    create_unimplemented_device("scobc-a1.i2c0", 0x4f030000, 0x10000);
    create_unimplemented_device("scobc-a1.sysmon", 0x4f040000, 0x10000);
    create_unimplemented_device("scobc-a1.timer", 0x4f050000, 0x10000);

    /* Mission AXI bus peripherals (as defined in standard FPGA image) */
    create_unimplemented_device("scobc-a1.uartlite1", 0x50000000, 0x10000);
    create_unimplemented_device("scobc-a1.uartlite2", 0x50010000, 0x10000);
    create_unimplemented_device("scobc-a1.uartlite3", 0x50020000, 0x10000);
    create_unimplemented_device("scobc-a1.uartlite4", 0x50030000, 0X10000);
    create_unimplemented_device("scobc-a1.uartlite5", 0x50040000, 0x10000);
    create_unimplemented_device("scobc-a1.uartlite6", 0x50050000, 0x10000);
    create_unimplemented_device("scobc-a1.i2c1", 0x50060000, 0x10000);
    create_unimplemented_device("scobc-a1.i2c2", 0x50070000, 0x10000);
    create_unimplemented_device("scobc-a1.spi", 0x50080000, 0x10000);
    create_unimplemented_device("scobc-a1.gpio", 0x50090000, 0x10000);

    armv7m_load_kernel(ARMV7M(armv7m)->cpu, machine->kernel_filename,
                      0x00000000, machine->ram_size);
}

static void scobc_a1_machine_init(MachineClass *mc)
{
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-m3"),
        NULL
    };

    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-m3");
    mc->valid_cpu_types = valid_cpu_types;
    mc->desc = "SCOBC A1";
    mc->init = scobc_a1_init;
    mc->max_cpus = 1;
    mc->default_ram_size = 4 * MiB;
    mc->default_ram_id = "scobc-a1.hrmem";
    mc->no_serial = false;
    mc->no_parallel = true;
    mc->no_floppy = true;
    mc->no_cdrom = true;
}

DEFINE_MACHINE_ARM("scobc-a1", scobc_a1_machine_init)
