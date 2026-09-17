#include <acpi/acpi.h>

#include "kernel/diagnostics/klog.h"
#include "lib/string.h"

#include <uacpi/namespace.h>
#include <uacpi/status.h>
#include <uacpi/types.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

#define ACPI_TEMP_INVALID ((int32_t)0x80000000)

struct sensor_nodes {
    uacpi_namespace_node *zones[ACPI_THERMAL_MAX_ZONES];
    uacpi_namespace_node *fans[ACPI_THERMAL_MAX_FANS];
    uint32_t zone_count;
    uint32_t fan_count;
    bool scanned;
};

static struct sensor_nodes g_sensors;

static void copy_id(char *dst, uint32_t cap, const char *src) {
    uint32_t i = 0;
    if (!dst || !cap) return;
    if (src) while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void node_name(uacpi_namespace_node *node, char *out, uint32_t cap) {
    uacpi_object_name name = uacpi_namespace_node_name(node);
    char tmp[5] = { name.text[0], name.text[1], name.text[2], name.text[3], '\0' };
    copy_id(out, cap, tmp);
}

static bool node_is_fan(uacpi_namespace_node *node) {
    uacpi_id_string *hid = NULL;
    bool fan = false;
    if (uacpi_eval_hid(node, &hid) == UACPI_STATUS_OK) {
        fan = hid->value && __builtin_strcmp(hid->value, "PNP0C0B") == 0;
        uacpi_free_id_string(hid);
    }
    return fan;
}

static uacpi_iteration_decision sensor_scan_cb(void *ctx,
                                                uacpi_namespace_node *node,
                                                uacpi_u32 depth) {
    (void)ctx; (void)depth;
    uacpi_object_type type;
    if (uacpi_namespace_node_type(node, &type) != UACPI_STATUS_OK)
        return UACPI_ITERATION_DECISION_CONTINUE;
    if (type == UACPI_OBJECT_THERMAL_ZONE &&
        g_sensors.zone_count < ACPI_THERMAL_MAX_ZONES)
        g_sensors.zones[g_sensors.zone_count++] = node;
    else if (type == UACPI_OBJECT_DEVICE && node_is_fan(node) &&
             g_sensors.fan_count < ACPI_THERMAL_MAX_FANS)
        g_sensors.fans[g_sensors.fan_count++] = node;
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static void sensors_scan_once(void) {
    if (g_sensors.scanned || !acpi_uacpi_full_ready()) return;
    memset(&g_sensors, 0, sizeof(g_sensors));
    (void)uacpi_namespace_for_each_child(uacpi_namespace_root(), sensor_scan_cb,
        NULL, UACPI_OBJECT_DEVICE_BIT | UACPI_OBJECT_THERMAL_ZONE_BIT,
        UACPI_MAX_DEPTH_ANY, NULL);
    g_sensors.scanned = true;
    klogf(KLOG_INFO, "acpi: sensors: thermal-zones=%u fans=%u",
          g_sensors.zone_count, g_sensors.fan_count);
}

bool acpi_thermal_get(struct acpi_thermal_info *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->temperature_deci_c = ACPI_TEMP_INVALID;
    if (!acpi_uacpi_full_ready()) return false;
    sensors_scan_once();
    out->zone_count = g_sensors.zone_count;
    out->fan_count = g_sensors.fan_count;

    for (uint32_t i = 0; i < g_sensors.zone_count; i++) {
        uint64_t kelvin_deci = 0;
        if (uacpi_eval_simple_integer(g_sensors.zones[i], "_TMP", &kelvin_deci)
                != UACPI_STATUS_OK || kelvin_deci > 0x7fffffffU)
            continue;
        /* ACPI _TMP is tenths Kelvin. Do not expose negative unsigned values. */
        out->temperature_deci_c = (int32_t)kelvin_deci - 2732;
        node_name(g_sensors.zones[i], out->zone_name, sizeof(out->zone_name));
        break;
    }
    for (uint32_t i = 0; i < g_sensors.fan_count; i++) {
        uacpi_object *pkg = NULL;
        if (uacpi_eval_simple_package(g_sensors.fans[i], "_FST", &pkg)
                != UACPI_STATUS_OK)
            continue;
        uacpi_object_array arr;
        bool ok = uacpi_object_get_package(pkg, &arr) == UACPI_STATUS_OK &&
                  arr.count >= 3 && arr.objects[2] &&
                  uacpi_object_is(arr.objects[2], UACPI_OBJECT_INTEGER);
        uint64_t rpm = 0;
        if (ok) ok = uacpi_object_get_integer(arr.objects[2], &rpm)
                        == UACPI_STATUS_OK && rpm <= UINT32_MAX;
        uacpi_object_unref(pkg);
        if (!ok) continue;
        out->fan_rpm = (uint32_t)rpm;
        node_name(g_sensors.fans[i], out->fan_name, sizeof(out->fan_name));
        break;
    }
    out->available = out->zone_count != 0 || out->fan_count != 0;
    return true;
}

struct namespace_walk_ctx { acpi_namespace_device_visitor visitor; void *ctx; bool any; };

static uacpi_iteration_decision namespace_walk_cb(void *opaque,
                                                   uacpi_namespace_node *node,
                                                   uacpi_u32 depth) {
    (void)depth;
    struct namespace_walk_ctx *walk = opaque;
    uacpi_object_type type;
    if (uacpi_namespace_node_type(node, &type) != UACPI_STATUS_OK)
        return UACPI_ITERATION_DECISION_CONTINUE;
    struct acpi_namespace_device dev;
    memset(&dev, 0, sizeof(dev));
    if (type == UACPI_OBJECT_DEVICE) dev.type = ACPI_NAMESPACE_DEVICE;
    else if (type == UACPI_OBJECT_PROCESSOR) dev.type = ACPI_NAMESPACE_PROCESSOR;
    else if (type == UACPI_OBJECT_THERMAL_ZONE) dev.type = ACPI_NAMESPACE_THERMAL_ZONE;
    else return UACPI_ITERATION_DECISION_CONTINUE;
    node_name(node, dev.name, sizeof(dev.name));
    uint32_t sta = 0x0F;
    if (uacpi_eval_sta(node, &sta) == UACPI_STATUS_OK) dev.status = sta;
    else dev.status = 0x0F;
    dev.enabled = (dev.status & 0x02) != 0;
    uacpi_id_string *id = NULL;
    if (uacpi_eval_hid(node, &id) == UACPI_STATUS_OK) {
        copy_id(dev.hid, sizeof(dev.hid), id->value); uacpi_free_id_string(id);
    }
    id = NULL;
    if (uacpi_eval_uid(node, &id) == UACPI_STATUS_OK) {
        copy_id(dev.uid, sizeof(dev.uid), id->value); uacpi_free_id_string(id);
    }
    (void)uacpi_eval_adr(node, &dev.address);
    walk->any = true;
    return walk->visitor(&dev, walk->ctx) ? UACPI_ITERATION_DECISION_CONTINUE
                                           : UACPI_ITERATION_DECISION_BREAK;
}

bool acpi_for_each_namespace_device(acpi_namespace_device_visitor visitor,
                                    void *ctx) {
    if (!visitor || !acpi_uacpi_full_ready()) return false;
    struct namespace_walk_ctx walk = { visitor, ctx, false };
    (void)uacpi_namespace_for_each_child(uacpi_namespace_root(), namespace_walk_cb,
        NULL, UACPI_OBJECT_DEVICE_BIT | UACPI_OBJECT_PROCESSOR_BIT |
        UACPI_OBJECT_THERMAL_ZONE_BIT, UACPI_MAX_DEPTH_ANY, &walk);
    return walk.any;
}
