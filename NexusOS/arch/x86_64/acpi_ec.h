/* ACPI embedded controller byte protocol (ACPI 6.5 chapter 12), separate from the namespace glue in acpi_aml.c so
 * tests/acpi_ec_host_test.c can run it against a simulated controller. */
#ifndef NEXUS_ACPI_EC_H
#define NEXUS_ACPI_EC_H
#include "../../kernel/types.h"

#define EC_OBF 0x01
#define EC_IBF 0x02
#define EC_BURST 0x10
#define EC_SCI_EVT 0x20
struct ec_io {
    uint8_t (*in)(uint16_t port);
    void (*out)(uint16_t port, uint8_t value);
    uint64_t (*now_ns)(void);
};
struct ec_port { const struct ec_io* io; uint16_t data, command; uint64_t errors, timeout_ns; };
bool ec_read_byte(struct ec_port* ec, uint8_t address, uint8_t* value);     /* RD_EC 0x80 */
bool ec_write_byte(struct ec_port* ec, uint8_t address, uint8_t value);     /* WR_EC 0x81 */
bool ec_burst(struct ec_port* ec, bool enable);                             /* BE_EC 0x82 (ack 0x90) / BD_EC 0x83 */
/* width bytes little-endian at address; multi-byte in burst mode when the EC grants it, always left again.
 * 0, -22 (bad range) or -110 (timeout). */
int ec_transfer_bytes(struct ec_port* ec, uint8_t address, unsigned width, uint64_t* value, bool write);
bool ec_event_pending(struct ec_port* ec);                                 /* SCI_EVT */
int ec_query(struct ec_port* ec, uint8_t* value);                          /* QR_EC 0x84: 1 event, 0 none, -110 timeout */
#endif
