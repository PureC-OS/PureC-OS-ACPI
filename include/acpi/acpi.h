#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
int acpi_init(void *rsdp_address, uint64_t hhdm_offset);
bool acpi_is_ready(void);
void acpi_dump_tables(void);
void *acpi_find_table(const char *signature);
typedef void (*acpi_table_visitor)(const char *sig, void *table, uint32_t len, void *ctx);
void acpi_for_each_table(const char *signature, acpi_table_visitor visitor, void *ctx);
#define ACPI_MADT_MAX_CPUS 16
struct acpi_madt_info {
    bool present;
    uint32_t lapic_base;
    uint32_t enabled_cpus;
    uint32_t total_cpus;
    uint32_t ioapic_count;
    uint32_t iso_count;
    uint32_t nmi_count;
    uint32_t lapic_nmi_count;
    uint32_t override_count;
    uint32_t ioapic_first_addr;
    uint8_t lapic_ids[ACPI_MADT_MAX_CPUS];
};
bool acpi_get_madt(struct acpi_madt_info *out);
bool acpi_get_sci_irq(uint16_t *out);
void uacpi_glue_set_rsdp(void *rsdp_virt, uint64_t hhdm);
int acpi_uacpi_early_init(void);
int acpi_uacpi_full_init(void);
bool acpi_uacpi_tables_ready(void);
bool acpi_uacpi_full_ready(void);
bool acpi_uacpi_find_table(const char *sig, void **out_ptr, uint32_t *out_len);
void acpi_uacpi_dump(void);
void acpi_uacpi_rescan_tables(void);
void acpi_scan_blob(const uint8_t *data, uint32_t len, const char *tname);
void acpi_ec_init(void);
bool acpi_ec_ready(void);
bool acpi_ec_read(uint8_t offset, uint8_t *out);
bool acpi_ec_write(uint8_t offset, uint8_t value);
struct acpi_battery_live {
    bool valid;
    bool present;
    uint32_t percent;
    uint32_t charging;
    uint32_t voltage_mv;
    uint32_t current_ma;
    uint32_t remaining_min;
    char model[16];
};
bool acpi_battery_refresh(struct acpi_battery_live *out);
bool acpi_battery_present_in_namespace(void);
bool acpi_get_slp_typ(uint16_t *slp_typa, uint16_t *slp_typb);
bool acpi_has_battery(void);
int acpi_battery_count(void);
const char *acpi_battery_name(void);
bool acpi_has_ac(void);
bool acpi_battery_has_bif(void);
bool acpi_battery_has_bst(void);
bool acpi_has_ec(void);

/* AML namespace inventory consumed by the kernel device manager. */
#define ACPI_NAMESPACE_ID_MAX 32
enum acpi_namespace_device_type {
    ACPI_NAMESPACE_DEVICE = 1,
    ACPI_NAMESPACE_PROCESSOR,
    ACPI_NAMESPACE_THERMAL_ZONE,
};
struct acpi_namespace_device {
    enum acpi_namespace_device_type type;
    char name[5];
    char hid[ACPI_NAMESPACE_ID_MAX];
    char uid[ACPI_NAMESPACE_ID_MAX];
    uint64_t address;
    uint32_t status;
    uint32_t irq;
    bool has_i2c;
    uint16_t i2c_address;
    uint32_t i2c_speed_hz;
    char i2c_controller[ACPI_NAMESPACE_ID_MAX];
    bool enabled;
};
typedef bool (*acpi_namespace_device_visitor)(
    const struct acpi_namespace_device *device, void *ctx);
bool acpi_for_each_namespace_device(acpi_namespace_device_visitor visitor,
                                    void *ctx);

#define ACPI_THERMAL_MAX_ZONES 8
#define ACPI_THERMAL_MAX_FANS 8
struct acpi_thermal_info {
    bool available;
    uint32_t zone_count;
    uint32_t fan_count;
    /* Tenths of a degree Celsius; INT32_MIN means firmware gave no value. */
    int32_t temperature_deci_c;
    /* Revolutions per minute; zero means unavailable or stopped. */
    uint32_t fan_rpm;
    char zone_name[ACPI_NAMESPACE_ID_MAX];
    char fan_name[ACPI_NAMESPACE_ID_MAX];
};
bool acpi_thermal_get(struct acpi_thermal_info *out);
void acpi_shutdown(void);
void acpi_reboot(void);
