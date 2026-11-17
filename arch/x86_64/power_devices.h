/* ACPI device state shared by acpi_aml.c (owner: refreshed from AML) and power.c (sysfs, policy).
 * Only acpi_aml.c touches the namespace nodes; power.c reads the cached state and calls the refresh/set
 * functions (big kernel lock held). Units are ACPI's: capacities mWh or mAh (mah), rates mW or mA,
 * voltages mV, temperatures tenths of Kelvin. 0xFFFFFFFF = unknown. */
#ifndef NEXUS_POWER_DEVICES_H
#define NEXUS_POWER_DEVICES_H
#include "../../kernel/types.h"

#define SUPPLIES_MAX 4
#define THERMAL_ZONES_MAX 4
#define THERMAL_ACTIVE_MAX 10
#define BACKLIGHTS_MAX 2
#define BACKLIGHT_LEVELS_MAX 64
enum { SUPPLY_STATUS_UNKNOWN, SUPPLY_STATUS_CHARGING, SUPPLY_STATUS_DISCHARGING, SUPPLY_STATUS_NOT_CHARGING, SUPPLY_STATUS_FULL };
enum { DEVICE_POWER_BUTTON = 1, DEVICE_SLEEP_BUTTON, DEVICE_LID, DEVICE_AC, DEVICE_BATTERY, DEVICE_THERMAL, DEVICE_BACKLIGHT, DEVICE_VGEN, DEVICE_PROCESSOR };
struct supply_state {
    char name[8];                              /* "AC", "BAT0" */
    bool battery, present, online, mah, info_valid, state_valid, critical;
    uint32_t status;                           /* SUPPLY_STATUS_* */
    uint32_t design_capacity, full_capacity, design_voltage, warning, low, cycle_count, technology;
    uint32_t rate, remaining, voltage;
    char model[32], serial[32], kind[16], manufacturer[32];
    uint64_t updated_ns;
};
struct thermal_state {
    bool valid;
    uint32_t temperature, critical, hot, passive, tc1, tc2, tsp, polling;
    uint32_t active[THERMAL_ACTIVE_MAX]; unsigned active_count; int active_level;  /* -1: all fans off */
    uint64_t updated_ns;
};
struct uacpi_namespace_node;
struct acpi_supply { struct uacpi_namespace_node* node; struct supply_state state; };
struct acpi_zone { struct uacpi_namespace_node* node; struct thermal_state state; };
struct acpi_backlight { struct uacpi_namespace_node* node; uint32_t levels[BACKLIGHT_LEVELS_MAX]; unsigned count, current; };
struct acpi_devices {
    struct acpi_supply supplies[SUPPLIES_MAX]; unsigned supply_count;
    struct acpi_zone zones[THERMAL_ZONES_MAX]; unsigned zone_count;
    struct acpi_backlight backlights[BACKLIGHTS_MAX]; unsigned backlight_count;
    struct uacpi_namespace_node* lid; bool lid_open;
    struct uacpi_namespace_node* vgen; uint64_t vgen_changes;
    bool fixed_power_button, fixed_sleep_button, sleep_button;
};
extern struct acpi_devices power_devices;
void acpi_battery_refresh(unsigned index, bool information);
void acpi_ac_refresh(unsigned index);
void acpi_thermal_refresh(unsigned index, bool trips);
void acpi_thermal_set_active(unsigned index, int level);
void acpi_backlight_refresh(unsigned index);
int acpi_backlight_set(unsigned index, unsigned level);
bool acpi_lid_open(void);
#endif
