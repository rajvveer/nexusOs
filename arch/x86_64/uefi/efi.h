/* Minimal UEFI 2.x definitions used by BOOTX64.EFI (written from the UEFI specification; no
 * external headers). Built with zig's x86_64-uefi-msvc target, so every function pointer below
 * already uses the Microsoft x64 calling convention the firmware expects. */
#ifndef NEXUS_EFI_H
#define NEXUS_EFI_H
#include "../../../kernel/types.h"

typedef uint64_t EFI_STATUS;
typedef void* EFI_HANDLE;
typedef uint16_t CHAR16;
typedef uint64_t UINTN;
#define EFI_SUCCESS 0ULL
#define EFI_ERROR_BIT (1ULL << 63)
#define EFI_INVALID_PARAMETER (EFI_ERROR_BIT | 2)
#define EFI_BUFFER_TOO_SMALL (EFI_ERROR_BIT | 5)
#define EFI_NOT_FOUND (EFI_ERROR_BIT | 14)
#define EFI_ERROR(status) (((status) & EFI_ERROR_BIT) != 0)

typedef struct { uint32_t a; uint16_t b, c; uint8_t d[8]; } EFI_GUID;
typedef struct { uint64_t signature; uint32_t revision, header_size, crc32, reserved; } EFI_TABLE_HEADER;

enum { AllocateAnyPages, AllocateMaxAddress, AllocateAddress };
enum {
    EfiReservedMemoryType, EfiLoaderCode, EfiLoaderData, EfiBootServicesCode, EfiBootServicesData,
    EfiRuntimeServicesCode, EfiRuntimeServicesData, EfiConventionalMemory, EfiUnusableMemory,
    EfiACPIReclaimMemory, EfiACPIMemoryNVS, EfiMemoryMappedIO, EfiMemoryMappedIOPortSpace,
    EfiPalCode, EfiPersistentMemory, EfiUnacceptedMemoryType
};
#define EFI_MEMORY_WB 0x8ULL
typedef struct {
    uint32_t type, pad;
    uint64_t physical_start, virtual_start, pages, attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef struct EFI_SIMPLE_TEXT_OUTPUT {
    EFI_STATUS (*reset)(struct EFI_SIMPLE_TEXT_OUTPUT*, bool);
    EFI_STATUS (*output_string)(struct EFI_SIMPLE_TEXT_OUTPUT*, const CHAR16*);
    void* test_string; void* query_mode; void* set_mode; void* set_attribute; void* clear_screen;
    void* set_cursor_position; void* enable_cursor; void* mode;
} EFI_SIMPLE_TEXT_OUTPUT;

typedef struct {
    EFI_TABLE_HEADER header;
    void *raise_tpl, *restore_tpl;
    EFI_STATUS (*allocate_pages)(uint32_t type, uint32_t memory_type, UINTN pages, uint64_t* address);
    EFI_STATUS (*free_pages)(uint64_t address, UINTN pages);
    EFI_STATUS (*get_memory_map)(UINTN* size, EFI_MEMORY_DESCRIPTOR* map, UINTN* key, UINTN* descriptor_size, uint32_t* version);
    EFI_STATUS (*allocate_pool)(uint32_t memory_type, UINTN size, void** buffer);
    EFI_STATUS (*free_pool)(void* buffer);
    void *create_event, *set_timer, *wait_for_event, *signal_event, *close_event, *check_event;
    void *install_protocol_interface, *reinstall_protocol_interface, *uninstall_protocol_interface;
    EFI_STATUS (*handle_protocol)(EFI_HANDLE handle, const EFI_GUID* protocol, void** interface);
    void* reserved;
    void *register_protocol_notify, *locate_handle, *locate_device_path, *install_configuration_table;
    void *load_image, *start_image, *exit, *unload_image;
    EFI_STATUS (*exit_boot_services)(EFI_HANDLE image, UINTN map_key);
    void* get_next_monotonic_count;
    EFI_STATUS (*stall)(UINTN microseconds);
    EFI_STATUS (*set_watchdog_timer)(UINTN timeout, uint64_t code, UINTN data_size, CHAR16* data);
    void *connect_controller, *disconnect_controller;
    void *open_protocol, *close_protocol, *open_protocol_information;
    void* protocols_per_handle;
    EFI_STATUS (*locate_handle_buffer)(uint32_t search_type, const EFI_GUID* protocol, void* key, UINTN* count, EFI_HANDLE** buffer);
    EFI_STATUS (*locate_protocol)(const EFI_GUID* protocol, void* registration, void** interface);
    void *install_multiple_protocol_interfaces, *uninstall_multiple_protocol_interfaces;
    void* calculate_crc32;
    void *copy_mem, *set_mem, *create_event_ex;
} EFI_BOOT_SERVICES;

typedef struct { EFI_GUID guid; void* table; } EFI_CONFIGURATION_TABLE;
typedef struct {
    EFI_TABLE_HEADER header;
    CHAR16* firmware_vendor;
    uint32_t firmware_revision;
    EFI_HANDLE console_in_handle; struct EFI_SIMPLE_TEXT_INPUT* con_in;
    EFI_HANDLE console_out_handle; EFI_SIMPLE_TEXT_OUTPUT* con_out;
    EFI_HANDLE standard_error_handle; EFI_SIMPLE_TEXT_OUTPUT* std_err;
    void* runtime_services;
    EFI_BOOT_SERVICES* boot_services;
    UINTN table_count;
    EFI_CONFIGURATION_TABLE* tables;
} EFI_SYSTEM_TABLE;

typedef struct {
    uint32_t revision;
    EFI_HANDLE parent;
    EFI_SYSTEM_TABLE* system_table;
    EFI_HANDLE device;
    void* file_path;
    void* reserved;
    uint32_t load_options_size;
    void* load_options;
    void* image_base;
    uint64_t image_size;
    uint32_t image_code_type, image_data_type;
    void* unload;
} EFI_LOADED_IMAGE;

#define EFI_FILE_MODE_READ 1ULL
#define EFI_FILE_MODE_WRITE 2ULL
#define EFI_FILE_MODE_CREATE 0x8000000000000000ULL
typedef struct EFI_FILE {
    uint64_t revision;
    EFI_STATUS (*open)(struct EFI_FILE*, struct EFI_FILE** handle, const CHAR16* name, uint64_t mode, uint64_t attributes);
    EFI_STATUS (*close)(struct EFI_FILE*);
    EFI_STATUS (*remove)(struct EFI_FILE*);          /* Delete(): also closes the handle */
    EFI_STATUS (*read)(struct EFI_FILE*, UINTN* size, void* buffer);
    EFI_STATUS (*write)(struct EFI_FILE*, UINTN* size, const void* buffer);
    EFI_STATUS (*get_position)(struct EFI_FILE*, uint64_t* position);
    EFI_STATUS (*set_position)(struct EFI_FILE*, uint64_t position);
    void *get_info, *set_info;
    EFI_STATUS (*flush)(struct EFI_FILE*);
} EFI_FILE;
typedef struct { uint16_t scan_code; CHAR16 unicode_char; } EFI_INPUT_KEY;
typedef struct EFI_SIMPLE_TEXT_INPUT {
    EFI_STATUS (*reset)(struct EFI_SIMPLE_TEXT_INPUT*, bool extended);
    EFI_STATUS (*read_key_stroke)(struct EFI_SIMPLE_TEXT_INPUT*, EFI_INPUT_KEY* key);
    void* wait_for_key;
} EFI_SIMPLE_TEXT_INPUT;
typedef struct EFI_SIMPLE_FILE_SYSTEM {
    uint64_t revision;
    EFI_STATUS (*open_volume)(struct EFI_SIMPLE_FILE_SYSTEM*, EFI_FILE** root);
} EFI_SIMPLE_FILE_SYSTEM;

enum { PixelRedGreenBlueReserved8BitPerColor, PixelBlueGreenRedReserved8BitPerColor, PixelBitMask, PixelBltOnly };
typedef struct {
    uint32_t version, width, height, pixel_format;
    uint32_t red_mask, green_mask, blue_mask, reserved_mask;
    uint32_t pixels_per_scan_line;
} EFI_GRAPHICS_MODE_INFO;
typedef struct {
    uint32_t max_mode, mode;
    EFI_GRAPHICS_MODE_INFO* info;
    UINTN info_size;
    uint64_t frame_buffer_base;
    UINTN frame_buffer_size;
} EFI_GRAPHICS_MODE;
typedef struct EFI_GRAPHICS_OUTPUT {
    EFI_STATUS (*query_mode)(struct EFI_GRAPHICS_OUTPUT*, uint32_t mode, UINTN* size, EFI_GRAPHICS_MODE_INFO** info);
    EFI_STATUS (*set_mode)(struct EFI_GRAPHICS_OUTPUT*, uint32_t mode);
    void* blt;
    EFI_GRAPHICS_MODE* mode;
} EFI_GRAPHICS_OUTPUT;

/* EFI_EDID_ACTIVE_PROTOCOL / EFI_EDID_DISCOVERED_PROTOCOL (UEFI 2.x section 12.9), on GOP child handles. */
typedef struct { uint32_t size; uint8_t* edid; } EFI_EDID_PROTOCOL;
enum { AllHandles, ByRegisterNotify, ByProtocol };

#endif
