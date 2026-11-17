/* ACPI namespace bring-up and the ACPI device drivers, on uACPI (arch/x86_64/uacpi, MIT).
 *
 * Bring-up (acpi_aml_init, boot CPU, before the scheduler): uacpi_initialize (tables, ACPI mode), namespace load,
 * _OSI policy (Linux's: every Windows string and the ACPI host features; "Linux"/"Darwin" unknown; boot options
 * acpi_osi=!<name> / acpi_osi=<name> / acpi_osi=! adjust it), \_PIC(1) in IOAPIC mode, the embedded controller's
 * address-space handler (so _REG runs before _INI), _STA/_INI, GPE finalization (every _Lxx/_Exx GPE enabled),
 * fixed power/sleep button events, then the device scan. Any failure resets uACPI and leaves the kernel in its
 * table-only mode (acpi.c's MADT/FADT/_S5 parsing keeps working): never a boot failure. Boot option acpi=off or
 * acpi.noaml skips the interpreter.
 *
 * Devices: power/sleep buttons (fixed and PNP0C0C/PNP0C0E), lid (PNP0C0D), AC adapters (ACPI0003), batteries
 * (PNP0C0A: _BIF/_BIX/_BST, notify 0x80 status / 0x81 information), thermal zones (_TMP, _CRT, _HOT, _PSV, _ACx with
 * _ALx fan power resources, _TZP), ACPI video backlight (_BCL/_BCM/_BQC, hotkeys 0x86-0x89), the embedded controller
 * (ECDT / PNP0C09, _Qxx queries, burst mode for multi-byte accesses, _GLK), PCI root bridges (_SEG/_BBN/_CRS) and
 * _PRT interrupt routing with PCI interrupt link devices (PNP0C0F: _CRS, else _PRS + _SRS). QEMU's VM generation ID
 * device (QEMUVGID) is handled as well: a change (GPE -> Notify 0x80) is logged and runs an optional \_SB.VGEN.NXEV
 * method, the hook the test SSDT uses to deliver simulated battery/AC/lid/thermal events (tests/test_x64_acpi.py).
 *
 * Every entry point runs with the big kernel lock held (acpi_os.c describes the contexts). */
#include <uacpi/uacpi.h>
#include <uacpi/event.h>
#include <uacpi/sleep.h>
#include <uacpi/utilities.h>
#include <uacpi/resources.h>
#include <uacpi/notify.h>
#include <uacpi/osi.h>
#include <uacpi/opregion.h>
#include <uacpi/tables.h>
#include <uacpi/acpi.h>
#include <uacpi/registers.h>
#include "native.h"
#include "power.h"
#include "power_devices.h"
#include "acpi_ec.h"
#include "boot_info.h"
#include "../../kernel/port.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

uint64_t acpi_table_aml_bytes(void);  /* acpi.c */
static bool ready;
static const char* failure;
static char text_buffer[24];
static void say(const char* s) { vga_print(s); }
static void say_number(uint64_t value) { number64(value, text_buffer); vga_print(text_buffer); }
static void say_hex(uint64_t value) {
    char t[19]; t[0] = '0'; t[1] = 'x'; unsigned d = 1; while (d < 16 && (value >> (4 * d))) d++;
    for (unsigned i = 0; i < d; i++) t[2 + i] = "0123456789ABCDEF"[(value >> (4 * (d - 1 - i))) & 15];
    t[2 + d] = 0; vga_print(t);
}
static void say_path(uacpi_namespace_node* node) {
    const uacpi_char* path = uacpi_namespace_node_generate_absolute_path(node);
    if (path) { vga_print(path); uacpi_free_absolute_path(path); } else vga_print("?");
}
bool acpi_aml_ready(void) { return ready; }

/* ---- evaluation helpers ---- */
static bool eval_int(uacpi_namespace_node* node, const char* path, uint64_t* out) {
    return uacpi_eval_simple_integer(node, path, out) == UACPI_STATUS_OK;
}
static bool eval_int_arg(uacpi_namespace_node* node, const char* path, uint64_t argument, uint64_t* out) {
    uacpi_object* arg = uacpi_object_create_integer(argument); if (!arg) return false;
    uacpi_object_array args = {&arg, 1};
    bool ok = out ? uacpi_eval_integer(node, path, &args, out) == UACPI_STATUS_OK : uacpi_execute(node, path, &args) == UACPI_STATUS_OK;
    uacpi_object_unref(arg); return ok;
}
static bool has(uacpi_namespace_node* node, const char* name) {
    uacpi_namespace_node* child; return uacpi_namespace_node_find(node, name, &child) == UACPI_STATUS_OK;
}
static bool present(uacpi_namespace_node* node) {
    uacpi_u32 sta = 0xF; if (uacpi_eval_sta(node, &sta) != UACPI_STATUS_OK) return true;   /* no _STA: present */
    return (sta & 1) != 0;
}
static uint64_t package_int(uacpi_object_array* array, unsigned index) {
    uint64_t value = 0; if (index < array->count) uacpi_object_get_integer(array->objects[index], &value); return value;
}
static void package_string(uacpi_object_array* array, unsigned index, char* out, unsigned capacity) {
    out[0] = 0; if (index >= array->count) return;
    uacpi_data_view view;
    if (uacpi_object_get_string_or_buffer(array->objects[index], &view) != UACPI_STATUS_OK) return;
    unsigned n = 0;
    for (; n + 1 < capacity && n < view.length && view.const_text[n]; n++) {
        char c = view.const_text[n]; out[n] = c >= 32 && c < 127 ? c : '?';
    }
    while (n && out[n - 1] == ' ') n--;
    out[n] = 0;
}

/* ---- _OSI policy (Linux drivers/acpi/osi.c) ---- */
static void osi_policy(void) {
    static const uacpi_host_interface features[] = {UACPI_HOST_INTERFACE_MODULE_DEVICE, UACPI_HOST_INTERFACE_PROCESSOR_DEVICE,
        UACPI_HOST_INTERFACE_3_0_THERMAL_MODEL, UACPI_HOST_INTERFACE_3_0_SCP_EXTENSIONS, UACPI_HOST_INTERFACE_PROCESSOR_AGGREGATOR_DEVICE};
    for (unsigned i = 0; i < sizeof(features) / sizeof(features[0]); i++) uacpi_enable_host_interface(features[i]);
    /* acpi_osi=<name> adds, acpi_osi=!<name> removes, acpi_osi=! removes every vendor string (as Linux). */
    const char* line = boot_info.cmdline;
    for (unsigned i = 0; line[i]; ) {
        while (line[i] == ' ') i++;
        unsigned start = i; while (line[i] && line[i] != ' ') i++;
        if (i - start > 9 && !strncmp(line + start, "acpi_osi=", 9)) {
            char name[64]; unsigned n = 0;
            for (unsigned j = start + 9; j < i && n + 1 < sizeof(name); j++) name[n++] = line[j] == '_' ? ' ' : line[j];
            name[n] = 0;
            if (!strcmp(name, "!")) uacpi_bulk_configure_interfaces(UACPI_INTERFACE_ACTION_DISABLE, UACPI_INTERFACE_KIND_VENDOR);
            else if (name[0] == '!') uacpi_uninstall_interface(name + 1);
            else if (name[0]) uacpi_install_interface(name, UACPI_INTERFACE_KIND_FEATURE);
            say("[64] ACPI: _OSI override "); say(name); say("\n");
        }
    }
}

/* ---- embedded controller (ACPI 6.5 chapter 12; byte protocol in acpi_ec.c) ---- */
struct ec { uacpi_namespace_node* node; struct ec_port port; uint16_t gpe; bool gpe_valid, global_lock, ready; uint64_t queries; };
static struct ec ec;
static uint8_t ec_in(uint16_t port) { return port_byte_in(port); }
static void ec_out(uint16_t port, uint8_t value) { port_byte_out(port, value); }
static const struct ec_io ec_ports = {ec_in, ec_out, native_clock_ns};
/* One EC transaction under the firmware global lock when _GLK asks for it; multi-byte accesses in burst mode
 * (the EC holds off SMIs/SCIs and the bytes are atomic), the burst always disabled again. */
static uacpi_status ec_transfer(uint8_t address, unsigned width, uint64_t* value, bool write) {
    if (!ec.ready) return UACPI_STATUS_NO_HANDLER;
    if (width < 1 || width > 8 || (unsigned)address + width > 256) return UACPI_STATUS_INVALID_ARGUMENT;
    uacpi_u32 sequence = 0; bool locked = false;
    if (ec.global_lock) { if (uacpi_acquire_global_lock(0xFFFF, &sequence) != UACPI_STATUS_OK) return UACPI_STATUS_HARDWARE_TIMEOUT; locked = true; }
    int error = ec_transfer_bytes(&ec.port, address, width, value, write);
    if (locked) uacpi_release_global_lock(sequence);
    return error == -22 ? UACPI_STATUS_INVALID_ARGUMENT : error ? UACPI_STATUS_HARDWARE_TIMEOUT : UACPI_STATUS_OK;
}
static uacpi_status ec_region(uacpi_region_op op, uacpi_handle data) {
    if (op == UACPI_REGION_OP_ATTACH || op == UACPI_REGION_OP_DETACH) return UACPI_STATUS_OK;
    if (op != UACPI_REGION_OP_READ && op != UACPI_REGION_OP_WRITE) return UACPI_STATUS_INVALID_ARGUMENT;
    uacpi_region_rw_data* rw = data;
    if (rw->offset > 255) return UACPI_STATUS_INVALID_ARGUMENT;
    return ec_transfer((uint8_t)rw->offset, rw->byte_width, &rw->value, op == UACPI_REGION_OP_WRITE);
}
/* SCI_EVT: query (QR_EC) until no event is left, running _Qxx for each value (worker context). */
static void ec_query_work(uacpi_handle context) {
    (void)context;
    for (unsigned round = 0; round < 32 && ec.ready && ec_event_pending(&ec.port); round++) {
        uint8_t value = 0;
        if (ec_query(&ec.port, &value) != 1) break;
        char method[5] = {'_', 'Q', "0123456789ABCDEF"[value >> 4], "0123456789ABCDEF"[value & 15], 0};
        ec.queries++;
        uacpi_status status = uacpi_execute_simple(ec.node, method);
        if (status != UACPI_STATUS_OK && status != UACPI_STATUS_NOT_FOUND) { say("[64] ACPI EC: "); say(method); say(" failed\n"); }
    }
}
static uacpi_interrupt_ret ec_gpe(uacpi_handle context, uacpi_namespace_node* gpe_device, uacpi_u16 index) {
    (void)context; (void)gpe_device; (void)index;
    if (ec_event_pending(&ec.port)) uacpi_kernel_schedule_work(UACPI_WORK_GPE_EXECUTION, ec_query_work, NULL);
    return UACPI_INTERRUPT_HANDLED | UACPI_GPE_REENABLE;
}
static uacpi_iteration_decision ec_resource(void* user, uacpi_resource* r) {
    unsigned* count = user; uint16_t port = 0;
    if (r->type == UACPI_RESOURCE_TYPE_IO) port = r->io.minimum;
    else if (r->type == UACPI_RESOURCE_TYPE_FIXED_IO) port = r->fixed_io.address;
    else return UACPI_ITERATION_DECISION_CONTINUE;
    if (*count == 0) ec.port.data = port; else if (*count == 1) ec.port.command = port;
    (*count)++;
    return *count >= 2 ? UACPI_ITERATION_DECISION_BREAK : UACPI_ITERATION_DECISION_CONTINUE;
}
static uacpi_iteration_decision ec_found(void* user, uacpi_namespace_node* node, uacpi_u32 depth) {
    (void)user; (void)depth;
    if (ec.node || !present(node)) return UACPI_ITERATION_DECISION_CONTINUE;
    unsigned count = 0;
    if (uacpi_for_each_device_resource(node, "_CRS", ec_resource, &count) != UACPI_STATUS_OK || count < 2) return UACPI_ITERATION_DECISION_CONTINUE;
    ec.node = node;
    return UACPI_ITERATION_DECISION_BREAK;
}
/* After namespace load, before _INI: the handler must exist when _REG(EmbeddedControl, 1) runs. */
static void ec_init(void) {
    uacpi_table table; struct acpi_ecdt* ecdt = NULL;
    if (uacpi_table_find_by_signature("ECDT", &table) == UACPI_STATUS_OK) ecdt = table.ptr;
    uacpi_find_devices("PNP0C09", ec_found, NULL);
    if (!ec.node && ecdt && ecdt->hdr.length > sizeof(*ecdt) && ecdt->ec_control.address_space_id == UACPI_ADDRESS_SPACE_SYSTEM_IO) {
        uacpi_namespace_node* node;
        if (uacpi_namespace_node_find(NULL, ecdt->ec_id, &node) == UACPI_STATUS_OK) {
            ec.node = node; ec.port.data = (uint16_t)ecdt->ec_data.address; ec.port.command = (uint16_t)ecdt->ec_control.address;
        }
    }
    if (ecdt) uacpi_table_unref(&table);
    if (!ec.node) return;
    uint64_t value;
    if (eval_int(ec.node, "_GPE", &value) && value < 0x100) { ec.gpe = (uint16_t)value; ec.gpe_valid = true; }
    else if (ecdt) { ec.gpe = ecdt->gpe_bit; ec.gpe_valid = true; }
    ec.global_lock = eval_int(ec.node, "_GLK", &value) && value;
    ec.port.io = &ec_ports; ec.ready = true;
    if (uacpi_install_address_space_handler(ec.node, UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER, ec_region, NULL) != UACPI_STATUS_OK) {
        ec.ready = false; say("[64] ACPI EC: address space handler failed\n"); return;
    }
    say("[64] ACPI EC "); say_path(ec.node); say(" data="); say_hex(ec.port.data); say(" command="); say_hex(ec.port.command);
    if (ec.gpe_valid) { say(" gpe="); say_hex(ec.gpe); }
    say(ec.global_lock ? " glk\n" : "\n");
}
static void ec_events_enable(void) {
    if (!ec.ready || !ec.gpe_valid) return;
    if (uacpi_install_gpe_handler(NULL, ec.gpe, UACPI_GPE_TRIGGERING_EDGE, ec_gpe, NULL) != UACPI_STATUS_OK ||
        uacpi_enable_gpe(NULL, ec.gpe) != UACPI_STATUS_OK) say("[64] ACPI EC: query GPE unavailable\n");
}

/* ---- devices ---- */
struct acpi_devices power_devices;
static uacpi_status device_notify(uacpi_handle context, uacpi_namespace_node* node, uacpi_u64 value);
static void watch(uacpi_namespace_node* node, unsigned kind, unsigned index) {
    uintptr_t context = (uintptr_t)kind << 8 | index;
    if (uacpi_install_notify_handler(node, device_notify, (uacpi_handle)context) != UACPI_STATUS_OK) {
        say("[64] ACPI: notify handler for "); say_path(node); say(" failed\n");
    }
}
/* Battery: _BIX (ACPI 4.0+) or _BIF, then _BST. Units: _BIF[0] 0 = mWh/mW, 1 = mAh/mA. */
void acpi_battery_refresh(unsigned index, bool information) {
    struct acpi_supply* s = &power_devices.supplies[index];
    if (!s->node) return;
    uacpi_u32 sta = 0x1F; uacpi_eval_sta(s->node, &sta);
    bool was = s->state.present; s->state.present = (sta & 0x10) != 0;
    if (!s->state.present) { s->state.status = SUPPLY_STATUS_UNKNOWN; s->state.updated_ns = native_clock_ns(); return; }
    if (information || !was || !s->state.info_valid) {
        uacpi_object* object = NULL; uacpi_object_array p;
        bool bix = uacpi_eval_simple_package(s->node, "_BIX", &object) == UACPI_STATUS_OK && uacpi_object_get_package(object, &p) == UACPI_STATUS_OK && p.count >= 16;
        if (!bix) { if (object) uacpi_object_unref(object); object = NULL; }
        if (!bix && (uacpi_eval_simple_package(s->node, "_BIF", &object) != UACPI_STATUS_OK || uacpi_object_get_package(object, &p) != UACPI_STATUS_OK || p.count < 9)) {
            if (object) uacpi_object_unref(object);
            s->state.info_valid = false;
        } else {
            unsigned o = bix ? 1 : 0;   /* _BIX has a revision field first */
            s->state.mah = package_int(&p, o + 0) == 1;
            s->state.design_capacity = (uint32_t)package_int(&p, o + 1); s->state.full_capacity = (uint32_t)package_int(&p, o + 2);
            s->state.technology = (uint32_t)package_int(&p, o + 3); s->state.design_voltage = (uint32_t)package_int(&p, o + 4);
            s->state.warning = (uint32_t)package_int(&p, o + 5); s->state.low = (uint32_t)package_int(&p, o + 6);
            s->state.cycle_count = bix ? (uint32_t)package_int(&p, 8) : 0;
            unsigned strings = bix ? 16 : 9;
            package_string(&p, strings, s->state.model, sizeof(s->state.model));
            package_string(&p, strings + 1, s->state.serial, sizeof(s->state.serial));
            package_string(&p, strings + 2, s->state.kind, sizeof(s->state.kind));
            package_string(&p, strings + 3, s->state.manufacturer, sizeof(s->state.manufacturer));
            s->state.info_valid = true;
            uacpi_object_unref(object);
        }
    }
    uacpi_object* object = NULL; uacpi_object_array p;
    if (uacpi_eval_simple_package(s->node, "_BST", &object) == UACPI_STATUS_OK && uacpi_object_get_package(object, &p) == UACPI_STATUS_OK && p.count >= 4) {
        uint64_t state = package_int(&p, 0);
        s->state.rate = (uint32_t)package_int(&p, 1); s->state.remaining = (uint32_t)package_int(&p, 2); s->state.voltage = (uint32_t)package_int(&p, 3);
        s->state.critical = (state & 4) != 0;
        s->state.status = state & 2 ? SUPPLY_STATUS_CHARGING : state & 1 ? SUPPLY_STATUS_DISCHARGING :
                          s->state.full_capacity && s->state.remaining != 0xFFFFFFFFU && s->state.remaining >= s->state.full_capacity ? SUPPLY_STATUS_FULL : SUPPLY_STATUS_NOT_CHARGING;
        s->state.state_valid = true;
    } else s->state.state_valid = false;
    if (object) uacpi_object_unref(object);
    s->state.updated_ns = native_clock_ns();
}
void acpi_ac_refresh(unsigned index) {
    struct acpi_supply* s = &power_devices.supplies[index]; uint64_t value;
    if (!s->node) return;
    s->state.present = true;
    if (eval_int(s->node, "_PSR", &value)) s->state.online = value != 0;
    s->state.updated_ns = native_clock_ns();
}
/* Thermal zone: temperatures in tenths of Kelvin as ACPI returns them; trip points re-read on 0x81. */
void acpi_thermal_refresh(unsigned index, bool trips) {
    struct acpi_zone* z = &power_devices.zones[index]; uint64_t value;
    if (!z->node) return;
    if (trips) {
        z->state.critical = eval_int(z->node, "_CRT", &value) ? (uint32_t)value : 0;
        z->state.hot = eval_int(z->node, "_HOT", &value) ? (uint32_t)value : 0;
        z->state.passive = eval_int(z->node, "_PSV", &value) ? (uint32_t)value : 0;
        z->state.tc1 = eval_int(z->node, "_TC1", &value) ? (uint32_t)value : 0;
        z->state.tc2 = eval_int(z->node, "_TC2", &value) ? (uint32_t)value : 0;
        z->state.tsp = eval_int(z->node, "_TSP", &value) ? (uint32_t)value : 0;
        z->state.polling = eval_int(z->node, "_TZP", &value) ? (uint32_t)value : 0;
        z->state.active_count = 0;
        for (unsigned i = 0; i < THERMAL_ACTIVE_MAX; i++) {
            char name[5] = {'_', 'A', 'C', (char)('0' + i), 0};
            if (!eval_int(z->node, name, &value)) break;
            z->state.active[i] = (uint32_t)value; z->state.active_count = i + 1;
        }
    }
    z->state.valid = eval_int(z->node, "_TMP", &value) && value > 0 && value < 0x10000;
    if (z->state.valid) z->state.temperature = (uint32_t)value;
    z->state.updated_ns = native_clock_ns();
}
/* Active cooling level n (0 = highest): turn on the power resources (fans) of _AL0.._ALn, off the rest. */
static void power_resource(uacpi_object* reference, bool on) {
    uacpi_namespace_node* node = NULL;
    if (uacpi_object_resolve_as_aml_namepath(reference, NULL, &node) != UACPI_STATUS_OK || !node) return;
    uacpi_object_type type; if (uacpi_namespace_node_type(node, &type) != UACPI_STATUS_OK) return;
    if (type == UACPI_OBJECT_POWER_RESOURCE) uacpi_execute_simple(node, on ? "_ON" : "_OFF");
    else if (type == UACPI_OBJECT_DEVICE) {   /* a fan device: its D0 power resources */
        uacpi_object* list = NULL; uacpi_object_array p;
        if (uacpi_eval_simple_package(node, "_PR0", &list) == UACPI_STATUS_OK && uacpi_object_get_package(list, &p) == UACPI_STATUS_OK)
            for (unsigned i = 0; i < p.count; i++) power_resource(p.objects[i], on);
        if (list) uacpi_object_unref(list);
        uacpi_execute_simple(node, on ? "_PS0" : "_PS3");
    }
}
void acpi_thermal_set_active(unsigned index, int level) {
    struct acpi_zone* z = &power_devices.zones[index];
    if (!z->node) return;
    for (unsigned i = 0; i < z->state.active_count; i++) {
        char name[5] = {'_', 'A', 'L', (char)('0' + i), 0};
        uacpi_object* list = NULL; uacpi_object_array p;
        if (uacpi_eval_simple_package(z->node, name, &list) == UACPI_STATUS_OK && uacpi_object_get_package(list, &p) == UACPI_STATUS_OK)
            for (unsigned j = 0; j < p.count; j++) power_resource(p.objects[j], level >= 0 && i >= (unsigned)level);
        if (list) uacpi_object_unref(list);
    }
    z->state.active_level = level;
}
/* Backlight: _BCL = {AC level, battery level, levels...}; brightness is an index into the sorted unique levels. */
static bool backlight_levels(struct acpi_backlight* b) {
    uacpi_object* object = NULL; uacpi_object_array p; b->count = 0;
    if (uacpi_eval_simple_package(b->node, "_BCL", &object) != UACPI_STATUS_OK || uacpi_object_get_package(object, &p) != UACPI_STATUS_OK) { if (object) uacpi_object_unref(object); return false; }
    for (unsigned i = 2; i < p.count && b->count < BACKLIGHT_LEVELS_MAX; i++) {
        uint32_t level = (uint32_t)package_int(&p, i); unsigned at = 0;
        while (at < b->count && b->levels[at] < level) at++;
        if (at < b->count && b->levels[at] == level) continue;
        for (unsigned j = b->count; j > at; j--) b->levels[j] = b->levels[j - 1];
        b->levels[at] = level; b->count++;
    }
    uacpi_object_unref(object);
    return b->count >= 2;
}
void acpi_backlight_refresh(unsigned index) {
    struct acpi_backlight* b = &power_devices.backlights[index]; uint64_t value;
    if (!b->node) return;
    if (eval_int(b->node, "_BQC", &value)) {
        unsigned best = 0;
        for (unsigned i = 0; i < b->count; i++) if (b->levels[i] <= value) best = i;
        b->current = best;
    }
}
int acpi_backlight_set(unsigned index, unsigned level) {
    struct acpi_backlight* b = &power_devices.backlights[index];
    if (!b->node || level >= b->count) return -22;
    if (!eval_int_arg(b->node, "_BCM", b->levels[level], NULL)) return -5;
    b->current = level; return 0;
}
bool acpi_lid_open(void) {
    uint64_t value = 1;
    if (power_devices.lid && eval_int(power_devices.lid, "_LID", &value)) power_devices.lid_open = value != 0;
    return power_devices.lid_open;
}
/* ---- processors: performance (_PSS/_PCT/_PPC) and idle (_CST) states for cpufreq.c ---- */
#define PROCESSORS_MAX 64
static uacpi_namespace_node* processors[PROCESSORS_MAX];
static unsigned processor_count;
unsigned acpi_processor_count(void) { return processor_count; }
static bool generic_register(uacpi_object* object, struct acpi_register* out) {   /* ResourceTemplate { Register (...) } */
    uacpi_data_view view;
    if (uacpi_object_get_buffer(object, &view) != UACPI_STATUS_OK || view.length < 15 || view.const_bytes[0] != 0x82) return false;
    const uint8_t* b = view.const_bytes;
    out->space = b[3]; out->bit_width = b[4]; out->bit_offset = b[5]; out->access = b[6];
    out->address = 0; for (unsigned i = 0; i < 8; i++) out->address |= (uint64_t)b[7 + i] << (8 * i);
    return true;
}
int acpi_processor_pss(unsigned cpu, struct acpi_pstate* out, unsigned capacity) {
    if (cpu >= processor_count) return -19;
    uacpi_object* object; uacpi_object_array states; unsigned count = 0;
    if (uacpi_eval_simple_package(processors[cpu], "_PSS", &object) != UACPI_STATUS_OK) return -61;
    if (uacpi_object_get_package(object, &states) == UACPI_STATUS_OK)
        for (unsigned i = 0; i < states.count && count < capacity; i++) {
            uacpi_object_array s;
            if (uacpi_object_get_package(states.objects[i], &s) != UACPI_STATUS_OK || s.count < 6) continue;
            out[count++] = (struct acpi_pstate){(uint32_t)package_int(&s, 0), (uint32_t)package_int(&s, 1), (uint32_t)package_int(&s, 2),
                                                (uint32_t)package_int(&s, 3), package_int(&s, 4), package_int(&s, 5)};
        }
    uacpi_object_unref(object);
    return (int)count;
}
int acpi_processor_pct(unsigned cpu, struct acpi_register* control, struct acpi_register* status) {
    if (cpu >= processor_count) return -19;
    uacpi_object* object; uacpi_object_array p; int error = -61;
    if (uacpi_eval_simple_package(processors[cpu], "_PCT", &object) != UACPI_STATUS_OK) return error;
    if (uacpi_object_get_package(object, &p) == UACPI_STATUS_OK && p.count >= 2 && generic_register(p.objects[0], control) && generic_register(p.objects[1], status)) error = 0;
    uacpi_object_unref(object);
    return error;
}
int acpi_processor_ppc(unsigned cpu) {
    uint64_t value = 0;
    if (cpu >= processor_count || !eval_int(processors[cpu], "_PPC", &value)) return 0;
    return value > 255 ? 255 : (int)value;
}
int acpi_processor_cst(unsigned cpu, struct acpi_cstate* out, unsigned capacity) {
    if (cpu >= processor_count) return -19;
    uacpi_object* object; uacpi_object_array p; unsigned count = 0;
    if (uacpi_eval_simple_package(processors[cpu], "_CST", &object) != UACPI_STATUS_OK) return -61;
    if (uacpi_object_get_package(object, &p) == UACPI_STATUS_OK)
        for (unsigned i = 1; i < p.count && count < capacity; i++) {   /* [0] = count */
            uacpi_object_array c;
            if (uacpi_object_get_package(p.objects[i], &c) != UACPI_STATUS_OK || c.count < 4 || !generic_register(c.objects[0], &out[count].reg)) continue;
            out[count].type = (uint32_t)package_int(&c, 1); out[count].latency_us = (uint32_t)package_int(&c, 2); out[count].power_mw = (uint32_t)package_int(&c, 3);
            count++;
        }
    uacpi_object_unref(object);
    return (int)count;
}

static uacpi_iteration_decision scan(void* user, uacpi_namespace_node* node, uacpi_u32 depth) {
    (void)user; (void)depth;
    uacpi_object_type type;
    if (uacpi_namespace_node_type(node, &type) != UACPI_STATUS_OK) return UACPI_ITERATION_DECISION_CONTINUE;
    if (type == UACPI_OBJECT_THERMAL_ZONE) {
        if (power_devices.zone_count < THERMAL_ZONES_MAX && has(node, "_TMP")) {
            unsigned i = power_devices.zone_count++;
            power_devices.zones[i].node = node; power_devices.zones[i].state.active_level = -1;
            acpi_thermal_refresh(i, true); watch(node, DEVICE_THERMAL, i);
            power_event(POWER_EV_THERMAL, i, 0x81, power_devices.zones[i].state.temperature);   /* first policy pass (fans, caps) */
            say("[64] ACPI thermal zone "); say_path(node); say(" temp="); say_number(power_devices.zones[i].state.temperature);
            say(" crit="); say_number(power_devices.zones[i].state.critical); say(" (0.1 K)\n");
        }
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    static const char* const processor[] = {"ACPI0007", NULL};
    if (type == UACPI_OBJECT_PROCESSOR || (type == UACPI_OBJECT_DEVICE && uacpi_device_matches_pnp_id(node, processor))) {
        if (processor_count < PROCESSORS_MAX && present(node)) {
            watch(node, DEVICE_PROCESSOR, processor_count); processors[processor_count++] = node;
            say("[64] ACPI processor "); say_path(node); say(has(node, "_PSS") ? " _PSS" : ""); say(has(node, "_CST") ? " _CST" : ""); say("\n");
        }
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
    if (type != UACPI_OBJECT_DEVICE) return UACPI_ITERATION_DECISION_CONTINUE;
    static const char* const power_button[] = {"PNP0C0C", NULL}, *const sleep_button[] = {"PNP0C0E", NULL}, *const lid[] = {"PNP0C0D", NULL};
    static const char* const ac[] = {"ACPI0003", NULL}, *const battery[] = {"PNP0C0A", NULL}, *const vgen[] = {"QEMUVGID", NULL};
    if (uacpi_device_matches_pnp_id(node, power_button)) { if (present(node)) { watch(node, DEVICE_POWER_BUTTON, 0); say("[64] ACPI power button "); say_path(node); say("\n"); } }
    else if (uacpi_device_matches_pnp_id(node, sleep_button)) { if (present(node)) { watch(node, DEVICE_SLEEP_BUTTON, 0); power_devices.sleep_button = true; say("[64] ACPI sleep button "); say_path(node); say("\n"); } }
    else if (uacpi_device_matches_pnp_id(node, lid)) {
        if (present(node) && !power_devices.lid) { power_devices.lid = node; power_devices.lid_open = true; acpi_lid_open(); watch(node, DEVICE_LID, 0);
            say("[64] ACPI lid "); say_path(node); say(power_devices.lid_open ? " open\n" : " closed\n"); }
    } else if (uacpi_device_matches_pnp_id(node, ac) || uacpi_device_matches_pnp_id(node, battery)) {
        bool is_battery = uacpi_device_matches_pnp_id(node, battery);
        if (present(node) && power_devices.supply_count < SUPPLIES_MAX) {
            unsigned i = power_devices.supply_count++; struct acpi_supply* s = &power_devices.supplies[i];
            s->node = node; s->state.battery = is_battery;
            unsigned ordinal = 0; for (unsigned j = 0; j < i; j++) if (power_devices.supplies[j].state.battery == is_battery) ordinal++;
            strcpy(s->state.name, is_battery ? "BAT" : "AC"); if (is_battery || ordinal) { char n[4]; uint_to_str(ordinal, n); strcat(s->state.name, n); }
            if (is_battery) acpi_battery_refresh(i, true); else acpi_ac_refresh(i);
            watch(node, is_battery ? DEVICE_BATTERY : DEVICE_AC, i);
            say("[64] ACPI "); say(is_battery ? "battery " : "AC adapter "); say(s->state.name); say(" "); say_path(node);
            if (is_battery) { say(s->state.present ? " present" : " absent"); say(" remaining="); say_number(s->state.remaining); say("/"); say_number(s->state.full_capacity); say(s->state.mah ? " mAh\n" : " mWh\n"); }
            else say(s->state.online ? " online\n" : " offline\n");
        }
    } else if (uacpi_device_matches_pnp_id(node, vgen)) {
        if (!power_devices.vgen) { power_devices.vgen = node; watch(node, DEVICE_VGEN, 0); say("[64] ACPI VM generation ID "); say_path(node); say("\n"); }
    }
    if (power_devices.backlight_count < BACKLIGHTS_MAX && has(node, "_BCL") && has(node, "_BCM")) {
        unsigned i = power_devices.backlight_count; struct acpi_backlight* b = &power_devices.backlights[i];
        b->node = node;
        if (backlight_levels(b)) {
            power_devices.backlight_count++; b->current = b->count - 1; acpi_backlight_refresh(i); watch(node, DEVICE_BACKLIGHT, i);
            say("[64] ACPI backlight "); say_path(node); say(" levels="); say_number(b->count); say(" current="); say_number(b->current); say("\n");
        } else b->node = NULL;
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}
static uacpi_status device_notify(uacpi_handle context, uacpi_namespace_node* node, uacpi_u64 value) {
    unsigned kind = (unsigned)((uintptr_t)context >> 8), index = (unsigned)((uintptr_t)context & 0xFF);
    (void)node;
    switch (kind) {
    case DEVICE_POWER_BUTTON: if (value == 0x80) power_event(POWER_EV_BUTTON_POWER, 0, 0x80, 0); break;
    case DEVICE_SLEEP_BUTTON: if (value == 0x80) power_event(POWER_EV_BUTTON_SLEEP, 0, 0x80, 0); break;
    case DEVICE_LID: if (value == 0x80) { bool open = acpi_lid_open(); power_event(POWER_EV_LID, 0, 0x80, open); } break;
    case DEVICE_AC: acpi_ac_refresh(index); power_event(POWER_EV_AC, index, (uint32_t)value, power_devices.supplies[index].state.online); break;
    case DEVICE_BATTERY: acpi_battery_refresh(index, value == 0x81); power_event(POWER_EV_BATTERY, index, (uint32_t)value, 0); break;
    case DEVICE_THERMAL: acpi_thermal_refresh(index, value == 0x81); power_event(POWER_EV_THERMAL, index, (uint32_t)value, power_devices.zones[index].state.temperature); break;
    case DEVICE_BACKLIGHT: power_event(POWER_EV_BACKLIGHT, index, (uint32_t)value, 0); break;
    case DEVICE_PROCESSOR:   /* 0x80: _PPC (performance limit) changed; 0x81: _CST changed */
        if (value == 0x80) cpufreq_limits_changed(index); else if (value == 0x81) cpufreq_cstates_changed(index);
        break;
    case DEVICE_VGEN:
        if (value == 0x80) { power_devices.vgen_changes++; say("[64] ACPI: VM generation ID changed (snapshot restore or clone)\n"); }
        break;
    }
    return UACPI_STATUS_OK;
}
static uacpi_interrupt_ret fixed_button(uacpi_handle context) {
    power_event((unsigned)(uintptr_t)context, 0, 0x80, 1);   /* interrupt context: queued for power_poll */
    return UACPI_INTERRUPT_HANDLED;
}
/* Every Notify() also reaches the root handler (ACPICA's "system notify"): the bus-level codes (bus/device check,
 * wake, eject request, ...) are logged; device-specific codes (0x80+) belong to the device handlers above. */
static uacpi_status system_notify(uacpi_handle context, uacpi_namespace_node* node, uacpi_u64 value) {
    (void)context;
    if (value < 0x80) { say("[64] ACPI notify "); say_hex(value); say(" on "); say_path(node); say("\n"); }
    return UACPI_STATUS_OK;
}

/* ---- PCI root bridges and _PRT routing ---- */
#define ROOTS_MAX 8
struct root_bridge { uacpi_namespace_node* node; uint16_t segment; uint8_t bus; };
static struct root_bridge roots[ROOTS_MAX];
static unsigned root_count;
static uacpi_iteration_decision root_window(void* user, uacpi_resource* r) {
    (void)user;
    uint64_t minimum = 0, length = 0; unsigned kind = 0xFF;
    switch (r->type) {
    case UACPI_RESOURCE_TYPE_ADDRESS16: minimum = r->address16.minimum; length = r->address16.address_length; kind = r->address16.common.type; break;
    case UACPI_RESOURCE_TYPE_ADDRESS32: minimum = r->address32.minimum; length = r->address32.address_length; kind = r->address32.common.type; break;
    case UACPI_RESOURCE_TYPE_ADDRESS64: minimum = r->address64.minimum; length = r->address64.address_length; kind = r->address64.common.type; break;
    default: return UACPI_ITERATION_DECISION_CONTINUE;
    }
    if (!length) return UACPI_ITERATION_DECISION_CONTINUE;
    say(kind == 0 ? " mem[" : kind == 1 ? " io[" : kind == 2 ? " bus[" : " ?["); say_hex(minimum); say("-"); say_hex(minimum + length - 1); say("]");
    return UACPI_ITERATION_DECISION_CONTINUE;
}
static uacpi_iteration_decision root_found(void* user, uacpi_namespace_node* node, uacpi_u32 depth) {
    (void)user; (void)depth;
    if (root_count == ROOTS_MAX || !present(node)) return UACPI_ITERATION_DECISION_CONTINUE;
    uint64_t value; struct root_bridge* r = &roots[root_count++];
    r->node = node; r->segment = eval_int(node, "_SEG", &value) ? (uint16_t)value : 0; r->bus = eval_int(node, "_BBN", &value) ? (uint8_t)value : 0;
    say("[64] ACPI PCI root "); say_path(node); say(" segment "); say_number(r->segment); say(" bus "); say_number(r->bus);
    uacpi_for_each_device_resource(node, "_CRS", root_window, NULL); say(has(node, "_PRT") ? " _PRT\n" : "\n");
    return UACPI_ITERATION_DECISION_NEXT_PEER;
}
struct child_search { uint64_t address; uacpi_namespace_node* found; };
static uacpi_iteration_decision child_by_address(void* user, uacpi_namespace_node* node, uacpi_u32 depth) {
    (void)depth; struct child_search* search = user; uint64_t address;
    if (uacpi_eval_adr(node, &address) == UACPI_STATUS_OK && address == search->address) { search->found = node; return UACPI_ITERATION_DECISION_BREAK; }
    return UACPI_ITERATION_DECISION_NEXT_PEER;
}
/* The PCI-to-PCI bridge whose secondary bus is `bus` (config space walk of the kernel's device table). */
static bool bridge_to(unsigned segment, unsigned bus, uint8_t* parent_bus, uint8_t* device, uint8_t* function) {
    if (segment) return false;
    pci_device_t* list = pci_get_devices(); int count = pci_device_count();
    for (int i = 0; i < count; i++) {
        if (list[i].class_code != 0x06 || (list[i].subclass != 0x04 && list[i].subclass != 0x09)) continue;
        uint32_t buses = pci_read32(list[i].bus, list[i].device, list[i].function, 0x18);
        if (((buses >> 8) & 0xFF) == bus) { *parent_bus = list[i].bus; *device = list[i].device; *function = list[i].function; return true; }
    }
    return false;
}
static uacpi_namespace_node* bus_node(unsigned segment, unsigned bus, unsigned depth) {
    for (unsigned i = 0; i < root_count; i++) if (roots[i].segment == segment && roots[i].bus == bus) return roots[i].node;
    uint8_t parent, device, function;
    if (depth > 8 || !bridge_to(segment, bus, &parent, &device, &function)) return NULL;
    uacpi_namespace_node* above = bus_node(segment, parent, depth + 1); if (!above) return NULL;
    struct child_search search = {(uint64_t)device << 16 | function, NULL};
    uacpi_namespace_for_each_child(above, child_by_address, NULL, UACPI_OBJECT_DEVICE_BIT, 1, &search);
    return search.found;
}
static bool link_irq(uacpi_namespace_node* link, unsigned index, uint32_t* gsi, bool* level, bool* low) {
    for (unsigned attempt = 0; attempt < 2; attempt++) {
        uacpi_resources* resources = NULL;
        if (uacpi_get_current_resources(link, &resources) == UACPI_STATUS_OK) {
            bool found = false;
            for (uacpi_resource* r = resources->entries; r->type != UACPI_RESOURCE_TYPE_END_TAG; r = UACPI_NEXT_RESOURCE(r)) {
                if (r->type == UACPI_RESOURCE_TYPE_IRQ && r->irq.num_irqs > index && r->irq.irqs[index]) {
                    *gsi = r->irq.irqs[index]; *level = r->irq.triggering == UACPI_TRIGGERING_LEVEL; *low = r->irq.polarity == UACPI_POLARITY_ACTIVE_LOW; found = true; break;
                }
                if (r->type == UACPI_RESOURCE_TYPE_EXTENDED_IRQ && r->extended_irq.num_irqs > index && r->extended_irq.irqs[index]) {
                    *gsi = r->extended_irq.irqs[index]; *level = r->extended_irq.triggering == UACPI_TRIGGERING_LEVEL;
                    *low = r->extended_irq.polarity == UACPI_POLARITY_ACTIVE_LOW; found = true; break;
                }
            }
            uacpi_free_resources(resources);
            if (found) return true;
        }
        /* Disabled link: program the first possible IRQ (_PRS -> _SRS), as Linux's acpi_pci_link_allocate. */
        uacpi_resources* possible = NULL;
        if (attempt || uacpi_get_possible_resources(link, &possible) != UACPI_STATUS_OK) return false;
        bool chosen = false;
        for (uacpi_resource* r = possible->entries; r->type != UACPI_RESOURCE_TYPE_END_TAG && !chosen; r = UACPI_NEXT_RESOURCE(r)) {
            if (r->type == UACPI_RESOURCE_TYPE_IRQ && r->irq.num_irqs) { r->irq.irqs[0] = r->irq.irqs[r->irq.num_irqs - 1]; r->irq.num_irqs = 1; chosen = true; }
            else if (r->type == UACPI_RESOURCE_TYPE_EXTENDED_IRQ && r->extended_irq.num_irqs) { r->extended_irq.irqs[0] = r->extended_irq.irqs[r->extended_irq.num_irqs - 1]; r->extended_irq.num_irqs = 1; chosen = true; }
        }
        uacpi_status status = chosen ? uacpi_set_resources(link, possible) : UACPI_STATUS_NOT_FOUND;
        uacpi_free_resources(possible);
        if (status != UACPI_STATUS_OK) return false;
    }
    return false;
}
int acpi_pci_route(unsigned segment, unsigned bus, unsigned device, unsigned pin, uint32_t* gsi, bool* level, bool* active_low) {
    if (!ready || pin > 3 || device > 31) return -19;
    for (unsigned depth = 0; depth < 8; depth++) {
        uacpi_namespace_node* node = bus_node(segment, bus, 0);
        uacpi_pci_routing_table* table = NULL;
        if (node && uacpi_get_pci_routing_table(node, &table) == UACPI_STATUS_OK) {
            int result = -19;
            for (uacpi_size i = 0; i < table->num_entries; i++) {
                uacpi_pci_routing_table_entry* e = &table->entries[i];
                if ((e->address >> 16) != device || e->pin != pin) continue;
                if (!e->source) { *gsi = e->index; *level = true; *active_low = true; result = 0; }   /* hard-wired GSI: PCI level/low */
                else result = link_irq(e->source, e->index, gsi, level, active_low) ? 0 : -19;
                break;
            }
            uacpi_free_pci_routing_table(table);
            return result;
        }
        /* No _PRT for this bus: swizzle through the bridge (PCI-to-PCI bridge spec 9.1). */
        uint8_t parent, bridge_device, bridge_function;
        if (!bridge_to(segment, bus, &parent, &bridge_device, &bridge_function)) return -19;
        pin = (pin + device) % 4; device = bridge_device; bus = parent;
    }
    return -19;
}

/* ---- GPE block devices (ACPI0006, ACPI 6.5 section 9.10): _CRS names the status+enable registers (I/O or memory,
 * half status, half enable) and the interrupt; installed before the GPEs are finalized so their _Exx/_Lxx methods
 * are enabled with the FADT blocks'. ---- */
void acpi_os_irq_hint(uint32_t irq, bool level, bool active_low);   /* acpi_os.c */
struct block_resources { uint64_t address; uint32_t length, irq; bool memory, io_found, irq_found, level, low; };
static uacpi_iteration_decision gpe_block_resource(void* user, uacpi_resource* r) {
    struct block_resources* b = user;
    if (!b->io_found && r->type == UACPI_RESOURCE_TYPE_IO) { b->address = r->io.minimum; b->length = r->io.length; b->io_found = true; }
    else if (!b->io_found && r->type == UACPI_RESOURCE_TYPE_FIXED_IO) { b->address = r->fixed_io.address; b->length = r->fixed_io.length; b->io_found = true; }
    else if (!b->io_found && r->type == UACPI_RESOURCE_TYPE_FIXED_MEMORY32) { b->address = r->fixed_memory32.address; b->length = r->fixed_memory32.length; b->memory = b->io_found = true; }
    else if (!b->irq_found && r->type == UACPI_RESOURCE_TYPE_IRQ && r->irq.num_irqs) {
        b->irq = r->irq.irqs[0]; b->level = r->irq.triggering == UACPI_TRIGGERING_LEVEL; b->low = r->irq.polarity == UACPI_POLARITY_ACTIVE_LOW; b->irq_found = true;
    } else if (!b->irq_found && r->type == UACPI_RESOURCE_TYPE_EXTENDED_IRQ && r->extended_irq.num_irqs) {
        b->irq = r->extended_irq.irqs[0]; b->level = r->extended_irq.triggering == UACPI_TRIGGERING_LEVEL;
        b->low = r->extended_irq.polarity == UACPI_POLARITY_ACTIVE_LOW; b->irq_found = true;
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}
static uacpi_iteration_decision gpe_block_found(void* user, uacpi_namespace_node* node, uacpi_u32 depth) {
    (void)user; (void)depth;
    if (!present(node)) return UACPI_ITERATION_DECISION_CONTINUE;
    struct block_resources b = {0};
    if (uacpi_for_each_device_resource(node, "_CRS", gpe_block_resource, &b) != UACPI_STATUS_OK || !b.io_found || b.length < 2 || b.length > 256) {
        say("[64] ACPI GPE block "); say_path(node); say(": no usable register resource\n"); return UACPI_ITERATION_DECISION_CONTINUE;
    }
    uint32_t irq = b.irq_found ? b.irq : 0;
    struct acpi_fadt* fadt = NULL; uacpi_table_fadt(&fadt);
    if (!b.irq_found && fadt) irq = fadt->sci_int;   /* no interrupt resource: the block shares the SCI */
    if (b.irq_found) acpi_os_irq_hint(irq, b.level, b.low);
    uacpi_status status = uacpi_install_gpe_block(node, b.address, b.memory ? UACPI_ADDRESS_SPACE_SYSTEM_MEMORY : UACPI_ADDRESS_SPACE_SYSTEM_IO,
                                                  (uacpi_u16)(b.length / 2), irq);
    say("[64] ACPI GPE block "); say_path(node); say(b.memory ? " memory " : " io "); say_hex(b.address); say(" registers "); say_number(b.length / 2);
    say(" irq "); say_number(irq); say(b.irq_found ? (b.level ? " level" : " edge") : " (SCI)");
    say(status == UACPI_STATUS_OK ? "\n" : " install failed\n");
    return UACPI_ITERATION_DECISION_CONTINUE;
}

/* ---- sleep states, power off, reset ---- */
static void wake_arm(unsigned state, bool arm);   /* wake devices, below */
bool acpi_sleep_state_supported(unsigned state) {
    if (!ready || state > 5) return false;
    char name[4] = {'_', 'S', (char)('0' + state), 0};
    return has(uacpi_namespace_root(), name);
}
int acpi_prepare_sleep(unsigned state) {
    if (!ready) return -19;
    eval_int_arg(uacpi_namespace_root(), "_TTS", state, NULL);   /* optional: transition to state */
    wake_arm(state, true);
    return uacpi_prepare_for_sleep_state((uacpi_sleep_state)state) == UACPI_STATUS_OK ? 0 : -5;
}
int acpi_enter_sleep(unsigned state) {
    if (!ready) return -19;
    uacpi_u64 control = 0, status_bits = 0;
    uacpi_read_register(UACPI_REGISTER_PM1_CNT, &control); uacpi_read_register(UACPI_REGISTER_PM1_STS, &status_bits);
    say("[64] ACPI: S"); say_number(state); say(" entry PM1_CNT="); say_hex(control); say(" PM1_STS="); say_hex(status_bits); say("\n");
    uacpi_status status = uacpi_enter_sleep_state((uacpi_sleep_state)state);
    return status == UACPI_STATUS_OK ? 0 : -5;
}
void acpi_leave_sleep(unsigned state) {
    if (!ready) return;
    uacpi_prepare_for_wake_from_sleep_state((uacpi_sleep_state)state);
    uacpi_wake_from_sleep_state((uacpi_sleep_state)state);
    wake_arm(state, false);
    eval_int_arg(uacpi_namespace_root(), "_TTS", 0, NULL);
}
void acpi_set_waking_vector(uint32_t real_mode, uint64_t protected_mode) { if (ready) uacpi_set_waking_vector(real_mode, protected_mode); }
void acpi_poweroff_aml(void) {
    if (!ready || !acpi_sleep_state_supported(5)) return;
    say("[64] ACPI S5 power-off through AML (_PTS/_S5)\n");
    if (acpi_prepare_sleep(5)) { say("[64] ACPI: _PTS(5) failed\n"); return; }
    uacpi_status status = uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S5);
    say("[64] ACPI: S5 entry returned "); say(uacpi_status_to_string(status)); say("\n");
}
void acpi_reboot_aml(void) {
    if (!ready) return;
    eval_int_arg(uacpi_namespace_root(), "_TTS", 5, NULL);
    struct acpi_fadt* fadt = NULL; uacpi_table_fadt(&fadt);
    if (!fadt || !(fadt->flags & ACPI_RESET_REG_SUP) || !fadt->reset_reg.address) return;   /* no reset register: acpi.c goes on */
    say("[64] ACPI reset via FADT reset register (uACPI)\n"); serial_drain();
    uacpi_status status = uacpi_reboot();
    say("[64] ACPI reset register did not reset: "); say(uacpi_status_to_string(status)); say("\n");
}
/* After the platform lost power (S3 wake, hibernation restore): the SCI line, GPE enables and the fixed
 * events are hardware state that the reset cleared. */
void acpi_events_resume(void) {
    if (!ready) return;
    uacpi_enter_acpi_mode();
    if (power_devices.fixed_power_button) uacpi_enable_fixed_event(UACPI_FIXED_EVENT_POWER_BUTTON);
    if (power_devices.fixed_sleep_button) uacpi_enable_fixed_event(UACPI_FIXED_EVENT_SLEEP_BUTTON);
}
uint64_t acpi_facs_hardware_signature(void) {
    uacpi_table table;
    if (uacpi_table_find_by_signature("FACS", &table) != UACPI_STATUS_OK) return 0;
    uint64_t signature = ((struct acpi_facs*)table.ptr)->hardware_signature; uacpi_table_unref(&table);
    return signature | 1ULL << 32;
}

/* ---- wake devices (_PRW, ACPI 7.3.13): their GPEs are set up for wake before the GPEs are finalised; buttons and the
 * lid wake the system by default (as on Linux), other devices are only listed. _DSW (or _PSW) arms them around a sleep. */
#define WAKE_DEVICES_MAX 32
static struct wake_device { uacpi_namespace_node* node; uacpi_namespace_node* gpe_device; uint16_t gpe; uint8_t deepest; bool enabled; } wake_devices[WAKE_DEVICES_MAX];
static unsigned wake_device_count;
static uacpi_iteration_decision wake_found(void* user, uacpi_namespace_node* node, uacpi_u32 depth) {
    (void)user; (void)depth;
    if (wake_device_count == WAKE_DEVICES_MAX || !has(node, "_PRW")) return UACPI_ITERATION_DECISION_CONTINUE;
    uacpi_object* object; uacpi_object_array prw;
    if (uacpi_eval_simple_package(node, "_PRW", &object) != UACPI_STATUS_OK) return UACPI_ITERATION_DECISION_CONTINUE;
    struct wake_device w = {node, NULL, 0, 0, false}; bool ok = false;
    if (uacpi_object_get_package(object, &prw) == UACPI_STATUS_OK && prw.count >= 2) {
        uint64_t value; uacpi_object_array info;
        if (uacpi_object_get_integer(prw.objects[0], &value) == UACPI_STATUS_OK) { w.gpe = (uint16_t)value; ok = value < 0x10000; }
        else if (uacpi_object_get_package(prw.objects[0], &info) == UACPI_STATUS_OK && info.count >= 2 &&
                 uacpi_object_resolve_as_aml_namepath(info.objects[0], node, &w.gpe_device) == UACPI_STATUS_OK) { w.gpe = (uint16_t)package_int(&info, 1); ok = true; }
        w.deepest = (uint8_t)package_int(&prw, 1);
    }
    uacpi_object_unref(object);
    if (!ok || uacpi_setup_gpe_for_wake(w.gpe_device, w.gpe, node) != UACPI_STATUS_OK) return UACPI_ITERATION_DECISION_CONTINUE;
    static const char* const default_wake[] = {"PNP0C0C", "PNP0C0E", "PNP0C0D", NULL};   /* power/sleep button, lid */
    if (uacpi_device_matches_pnp_id(node, default_wake) && uacpi_enable_gpe_for_wake(w.gpe_device, w.gpe) == UACPI_STATUS_OK) w.enabled = true;
    wake_devices[wake_device_count++] = w;
    say("[64] ACPI wake device "); say_path(node); say(" GPE "); say_hex(w.gpe); say(" S"); say_number(w.deepest); say(w.enabled ? " (enabled)\n" : "\n");
    return UACPI_ITERATION_DECISION_CONTINUE;
}
static void wake_runtime(void) {   /* button and lid GPEs are run-wake: also enabled while running (Linux acpi_button) */
    for (unsigned i = 0; i < wake_device_count; i++)
        if (wake_devices[i].enabled && uacpi_enable_gpe(wake_devices[i].gpe_device, wake_devices[i].gpe) != UACPI_STATUS_OK) {
            say("[64] ACPI: runtime GPE "); say_hex(wake_devices[i].gpe); say(" of "); say_path(wake_devices[i].node); say(" not enabled\n");
        }
}
static void wake_arm(unsigned state, bool arm) {   /* _DSW(enable, target state, 0), else the legacy _PSW(enable) */
    for (unsigned i = 0; i < wake_device_count; i++) {
        if (!wake_devices[i].enabled || (arm && state > wake_devices[i].deepest)) continue;
        uacpi_object* args[3] = {uacpi_object_create_integer(arm), uacpi_object_create_integer(arm ? state : 0), uacpi_object_create_integer(0)};
        if (args[0] && args[1] && args[2]) {
            uacpi_object_array list = {args, 3};
            if (uacpi_execute(wake_devices[i].node, "_DSW", &list) == UACPI_STATUS_NOT_FOUND) eval_int_arg(wake_devices[i].node, "_PSW", arm, NULL);
        }
        for (unsigned k = 0; k < 3; k++) if (args[k]) uacpi_object_unref(args[k]);
    }
}

/* ---- bring-up ---- */
static bool step(uacpi_status status, const char* what) {
    if (status == UACPI_STATUS_OK) return true;
    failure = what; say("[64] ACPI: "); say(what); say(" failed: "); say(uacpi_status_to_string(status)); say("\n");
    return false;
}
bool acpi_aml_init(void) {
    if (boot_option("acpi=off") || boot_option("acpi.noaml")) { say("[64] ACPI: AML interpreter disabled on the command line (table-only mode)\n"); return false; }
    uint64_t start = native_clock_ns();
    if (!acpi_os_init(acpi_table_aml_bytes())) { say("[64] ACPI: no memory for the interpreter heap (table-only mode)\n"); return false; }
    bool ok = step(uacpi_initialize(0), "uacpi_initialize") && step(uacpi_namespace_load(), "namespace load");
    if (ok) {
        osi_policy();
        uacpi_status pic = uacpi_set_interrupt_model(irq_ioapic_mode() ? UACPI_INTERRUPT_MODEL_IOAPIC : UACPI_INTERRUPT_MODEL_PIC);
        if (pic != UACPI_STATUS_OK && pic != UACPI_STATUS_NOT_FOUND) say("[64] ACPI: _PIC failed\n");
        ec_init();
        ok = step(uacpi_namespace_initialize(), "namespace initialize");
        if (ok) uacpi_find_devices("ACPI0006", gpe_block_found, NULL);
        if (ok) uacpi_namespace_for_each_child(uacpi_namespace_root(), wake_found, NULL, UACPI_OBJECT_DEVICE_BIT, UACPI_MAX_DEPTH_ANY, NULL);
        ok = ok && step(uacpi_finalize_gpe_initialization(), "GPE initialization");
        if (ok) wake_runtime();
    }
    if (!ok || boot_option("acpi.testfail")) {
        if (ok) { failure = "acpi.testfail"; say("[64] ACPI: simulated initialization failure (acpi.testfail)\n"); }
        uacpi_state_reset(); ec = (struct ec){0}; wake_device_count = 0; memset(&power_devices, 0, sizeof(power_devices)); root_count = 0;
        say("[64] ACPI: AML interpreter unavailable ("); say(failure); say("); continuing in table-only mode\n");
        return false;
    }
    ready = true;
    ec_events_enable();
    struct acpi_fadt* fadt = NULL; uacpi_table_fadt(&fadt);
    if (fadt && !(fadt->flags & ACPI_PWR_BUTTON) && uacpi_install_fixed_event_handler(UACPI_FIXED_EVENT_POWER_BUTTON, fixed_button, (uacpi_handle)(uintptr_t)POWER_EV_BUTTON_POWER) == UACPI_STATUS_OK)
        power_devices.fixed_power_button = true;
    if (fadt && !(fadt->flags & ACPI_SLP_BUTTON) && uacpi_install_fixed_event_handler(UACPI_FIXED_EVENT_SLEEP_BUTTON, fixed_button, (uacpi_handle)(uintptr_t)POWER_EV_BUTTON_SLEEP) == UACPI_STATUS_OK)
        power_devices.fixed_sleep_button = power_devices.sleep_button = true;
    uacpi_install_notify_handler(uacpi_namespace_root(), system_notify, NULL);
    static const char* const roots_ids[] = {"PNP0A08", "PNP0A03", NULL};
    uacpi_find_devices_at(uacpi_namespace_get_predefined(UACPI_PREDEFINED_NAMESPACE_SB), roots_ids, root_found, NULL);
    uacpi_namespace_for_each_child(uacpi_namespace_root(), scan, NULL, UACPI_OBJECT_DEVICE_BIT | UACPI_OBJECT_THERMAL_ZONE_BIT | UACPI_OBJECT_PROCESSOR_BIT, UACPI_MAX_DEPTH_ANY, NULL);
    uacpi_u8 bitness = 64; uacpi_get_aml_bitness(&bitness);
    say("[64] ACPI AML ready: uACPI "); say_number(UACPI_MAJOR); say("."); say_number(UACPI_MINOR); say("."); say_number(UACPI_PATCH);
    say(", "); say_number(bitness); say("-bit AML, "); say_number((native_clock_ns() - start) / 1000000); say(" ms, heap ");
    say_number(acpi_os_heap_pages() * 4ULL); say(" KiB, model "); say(irq_ioapic_mode() ? "IOAPIC" : "PIC");
    say(power_devices.fixed_power_button ? ", fixed power button" : ""); say(power_devices.fixed_sleep_button ? ", fixed sleep button" : "");
    say(acpi_sleep_state_supported(3) ? ", S3" : ""); say(acpi_sleep_state_supported(4) ? ", S4" : ""); say(acpi_sleep_state_supported(5) ? ", S5" : "");
    say("\n");
    return true;
}
/* RTC alarm: RTC_STS raises an SCI while running, RTC_EN lets the alarm wake the platform (PM1, ACPI 4.8.3.1). */
static bool rtc_handler;
static uacpi_interrupt_ret rtc_event(uacpi_handle context) { (void)context; power_event(POWER_EV_RTC, 0, 0, 0); return UACPI_INTERRUPT_HANDLED; }
void acpi_rtc_wake_enable(bool enable) {
    if (!ready) return;
    if (enable && !rtc_handler) rtc_handler = uacpi_install_fixed_event_handler(UACPI_FIXED_EVENT_RTC, rtc_event, NULL) == UACPI_STATUS_OK;
    if (!rtc_handler) return;
    uacpi_clear_fixed_event(UACPI_FIXED_EVENT_RTC);
    if (enable) uacpi_enable_fixed_event(UACPI_FIXED_EVENT_RTC); else uacpi_disable_fixed_event(UACPI_FIXED_EVENT_RTC);
}
