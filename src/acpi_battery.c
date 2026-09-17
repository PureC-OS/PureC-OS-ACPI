#include <acpi/acpi.h>

#include "kernel/diagnostics/klog.h"
#include "lib/string.h"

#include <uacpi/namespace.h>
#include <uacpi/status.h>
#include <uacpi/types.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

static bool pkg_u64(uacpi_object_array *arr, uint32_t idx, uint64_t *out) {
    if (!arr || idx >= arr->count || !out)
        return false;
    uacpi_object *o = arr->objects[idx];
    if (!o || !uacpi_object_is(o, UACPI_OBJECT_INTEGER))
        return false;
    return uacpi_object_get_integer(o, out) == UACPI_STATUS_OK;
}

static bool pkg_str(uacpi_object_array *arr, uint32_t idx, char *dst,
                    uint32_t cap) {
    if (!arr || idx >= arr->count || !dst || cap == 0)
        return false;
    uacpi_object *o = arr->objects[idx];
    if (!o || !uacpi_object_is(o, UACPI_OBJECT_STRING))
        return false;
    uacpi_data_view v;
    if (uacpi_object_get_string(o, &v) != UACPI_STATUS_OK)
        return false;
    if (!v.text || v.length == 0)
        return false;
    uint32_t n = (uint32_t)v.length;
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, v.text, n);
    dst[n] = '\0';
    return true;
}

struct bat_find_ctx {
    uacpi_namespace_node *hid_match;
    uacpi_namespace_node *any_bif;
};

static uacpi_iteration_decision bat_find_cb(void *ctx,
                                            uacpi_namespace_node *node,
                                            uacpi_u32 depth) {
    (void)depth;
    struct bat_find_ctx *c = (struct bat_find_ctx *)ctx;
    uacpi_id_string *hid = NULL;
    if (uacpi_eval_hid(node, &hid) == UACPI_STATUS_OK) {
        if (hid->value &&
            __builtin_strcmp(hid->value, "PNP0C0A") == 0) {
            uacpi_free_id_string(hid);
            c->hid_match = node;
            return UACPI_ITERATION_DECISION_BREAK;
        }
        uacpi_free_id_string(hid);
    }
    if (!c->any_bif) {
        uacpi_object *pkg = NULL;
        if (uacpi_eval_simple_package(node, "_BIF", &pkg) ==
                UACPI_STATUS_OK ||
            uacpi_eval_simple_package(node, "_BIX", &pkg) ==
                UACPI_STATUS_OK) {
            uacpi_object_unref(pkg);
            c->any_bif = node;
        }
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static uacpi_namespace_node *battery_find_node(void) {
    struct bat_find_ctx ctx = {0};
    uacpi_status st = uacpi_namespace_for_each_child(
        uacpi_namespace_root(), bat_find_cb, NULL, UACPI_OBJECT_DEVICE_BIT,
        UACPI_MAX_DEPTH_ANY, &ctx);
    if (st != UACPI_STATUS_OK)
        return NULL;
    return ctx.hid_match ? ctx.hid_match : ctx.any_bif;
}

bool acpi_battery_present_in_namespace(void) {
    if (!acpi_uacpi_full_ready())
        return false;
    return battery_find_node() != NULL;
}

bool acpi_battery_refresh(struct acpi_battery_live *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!out || !acpi_uacpi_full_ready())
        return false;

    uacpi_namespace_node *bat = battery_find_node();
    if (!bat)
        return false;

bool acpi_battery_refresh(struct acpi_battery_live *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!out || !acpi_uacpi_full_ready())
        return false;
    for (int pass = 0; pass < 8; pass++) {
        uacpi_namespace_node *bat = battery_find_node_pass(pass);
        if (!bat) {
            if (pass == 0)
                return false;
            break;
        }

        uint64_t sta = 0;
        bool have_sta = uacpi_eval_simple_integer(bat, "_STA", &sta) ==
                        UACPI_STATUS_OK;
        if (have_info_sta_absent(have_sta_ok, sta))
            continue;

        // This battery object reports present — evaluate it.
        return battery_read_live(bat, out);
    }
    return false;
}

    uint64_t unit = 0, design = 0, full = 0, voltage = 0;
    bool have_info = false;
    uacpi_object *pkg = NULL;
    if (uacpi_eval_simple_package(bat, "_BIF", &pkg) == UACPI_STATUS_OK) {
        uacpi_object_array arr;
        if (uacpi_object_get_package(pkg, &arr) == UACPI_STATUS_OK &&
            arr.count >= 13) {
            have_info = pkg_u64(&arr, 0, &unit) &&
                        pkg_u64(&arr, 1, &design) &&
                        pkg_u64(&arr, 2, &full) &&
                        pkg_u64(&arr, 4, &voltage);
            pkg_str(&arr, 9, out->model, sizeof(out->model));
        }
        uacpi_object_unref(pkg);
        pkg = NULL;
    }
    if (!have_info &&
        uacpi_eval_simple_package(bat, "_BIX", &pkg) == UACPI_STATUS_OK) {
        uacpi_object_array arr;
        if (uacpi_object_get_package(pkg, &arr) == UACPI_STATUS_OK &&
            arr.count >= 17) {
            have_info = pkg_u64(&arr, 1, &unit) &&
                        pkg_u64(&arr, 2, &design) &&
                        pkg_u64(&arr, 3, &full) &&
                        pkg_u64(&arr, 5, &voltage);
            pkg_str(&arr, 16, out->model, sizeof(out->model));
        }
        uacpi_object_unref(pkg);
        pkg = NULL;
    }
    if (!have_info || full == 0)
        return false;

    uint64_t state = 0, rate = 0, remaining = 0, present_volt = 0;
    if (uacpi_eval_simple_package(bat, "_BST", &pkg) != UACPI_STATUS_OK)
        return false;
    bool ok = false;
    uacpi_object_array bst;
    if (uacpi_object_get_package(pkg, &bst) == UACPI_STATUS_OK &&
        bst.count >= 4) {
        ok = pkg_u64(&bst, 0, &state) && pkg_u64(&bst, 1, &rate) &&
             pkg_u64(&bst, 2, &remaining) && pkg_u64(&bst, 3, &present_volt);
    }
    uacpi_object_unref(pkg);
    if (!ok)
        return false;

    uint64_t pct = (remaining * 100) / full;
    if (pct > 100)
        pct = 100;
    out->percent = (uint32_t)pct;
    out->charging = (state & 0x02) ? 1 : 0;
    out->voltage_mv = (uint32_t)present_volt;
    if (unit != 0) {
        out->current_ma = (uint32_t)rate;
        if (rate > 0 && remaining > 0)
            out->remaining_min = (uint32_t)((remaining * 60) / rate);
    } else {
        out->current_ma = 0;
        if (rate > 0 && remaining > 0)
            out->remaining_min = (uint32_t)((remaining * 60) / rate);
    }
    (void)design;
    (void)voltage;
    out->valid = true;
    klogf(KLOG_INFO, "acpi: battery %u%% %s %umV%s%s", out->percent,
          out->charging ? "charging" : "discharging", out->voltage_mv,
          out->model[0] ? " " : "", out->model);
    return true;
}
