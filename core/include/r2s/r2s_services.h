/* services: the MCU side of the services channel (spec/protocol.md, section
 * 7.8). The table lives in RAM with the firmware's live values; the session
 * calls in for channel 7 frames. Compiled only with R2S_WITH_SERVICES. */
#ifndef R2S_SERVICES_H
#define R2S_SERVICES_H

#include <stdint.h>

#include "r2s/r2s.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The opcodes of the services channel, in payload order. */
#define R2S_SVC_OP_QUERY 0x01u
#define R2S_SVC_OP_DECLARE 0x02u
#define R2S_SVC_OP_STATUS 0x03u
#define R2S_SVC_OP_REQUEST 0x04u
#define R2S_SVC_OP_RESPONSE 0x05u

/* The roles a DECLARE names [SVR-2]: the MCU serves the service, or the MCU
 * calls it. */
#define R2S_SVC_SERVER 0u
#define R2S_SVC_CLIENT 1u

/* The walk STATUS verdicts [SVR-7] name, mirroring the topic verdicts. */
#define R2S_SVC_STATUS_OK 0u
#define R2S_SVC_STATUS_UNKNOWN_TYPE 1u
#define R2S_SVC_STATUS_HASH_MISMATCH 2u
#define R2S_SVC_STATUS_NAME_CONFLICT 3u

/* The RESPONSE status codes [SVR-6] names. */
#define R2S_SVC_OK 0u
#define R2S_SVC_UNKNOWN_SERVICE 1u
#define R2S_SVC_BAD_PAYLOAD 2u
#define R2S_SVC_BUSY 3u

/* What the take calls return with nothing waiting: not an error, so the
 * application polls them every spin. */
#define R2S_SVC_EMPTY 1

/* What take_response reports for a call the timeout retired [SVR-8]: a local
 * report, never a wire status, so it shares no value with [SVR-6]. */
#define R2S_SVC_TIMEOUT 4u

/* Milliseconds without a RESPONSE before a call reports its timeout
 * [SVR-8]. */
#ifndef R2S_SVC_CALL_TIMEOUT_MS
#define R2S_SVC_CALL_TIMEOUT_MS 5000u
#endif

/* One service: the ROS type and service names, the role the MCU plays, and
 * the first 8 bytes of the type's RIHS01 hash. */
typedef struct {
    const char *type_name;
    const char *service_name;
    uint8_t role;
    uint8_t type_hash[8];
} r2s_service_t;

#ifdef R2S_WITH_SERVICES
/// Hands the session its service table: each entry names a served or called
/// service with a known role and names that fit the DECLARE frame.
/**
 * The table stays where the caller keeps it; the session reads it on every
 * walk. Returns R2S_OK, or R2S_ERR_BOUNDS when the table is missing, a role
 * is unknown, a name is missing or unfit, or a DECLARE frame would not fit,
 * leaving the old table unchanged.
 *
 * \param[in] services the table to install.
 * \param[in] count how many entries services holds.
 * \return R2S_OK on success, R2S_ERR_BOUNDS on a bad table.
 */
r2s_status_t r2s_services_configure(const r2s_service_t *services, uint8_t count);

/// How many services the table holds, 0 before any configuration.
/**
 * \return The configured service count.
 */
uint8_t r2s_services_count(void);

/// The table entry at index, or NULL past the count.
/**
 * \param[in] index the entry to read.
 * \return The entry, or NULL past the count.
 */
const r2s_service_t *r2s_services_get(uint8_t index);

/// The host's walk verdict for the entry at index, or 0xFF when the host
/// answered nothing yet or the index is past the count.
/**
 * \param[in] index the entry to read.
 * \return The last STATUS verdict, or 0xFF when unknown.
 */
uint8_t r2s_services_status(uint8_t index);

/// The DECLARE payload length for the entry at index, or 0 when the entry
/// is missing. The session reserves the frame from this before streaming
/// it, so no buffer holds a frame.
/**
 * \param[in] index the entry to size.
 * \return The DECLARE payload length, or 0 past the count.
 */
uint16_t r2s_services_declare_len(uint8_t index);

/// Calls the service at index: streams a REQUEST on the reliable channel
/// with a fresh req_id.
/**
 * The payload travels as the packed encoding of the request message. Fails
 * with R2S_ERR_NOT_CONNECTED while not connected, with R2S_ERR_TX_BUSY
 * while a call or a reliable frame is still outstanding, and with
 * R2S_ERR_BOUNDS for an unknown index, a non-client entry, or a payload
 * the frame cannot carry; every failure sends nothing. The answer arrives
 * through r2s_service_take_response().
 *
 * \param[in] index the called service, a client-role entry.
 * \param[in] payload the packed request, or NULL for an empty request.
 * \param[in] len how many bytes payload holds.
 * \param[out] req_id the req_id the call travels under.
 * \return R2S_OK on success, R2S_ERR_NOT_CONNECTED, R2S_ERR_TX_BUSY or
 * R2S_ERR_BOUNDS as above.
 * \pre The caller keeps payload alive only for the call.
 */
r2s_status_t r2s_service_call(uint8_t index, const uint8_t *payload, uint16_t len, uint8_t *req_id);

/// Takes one waiting server REQUEST into the caller's buffer.
/**
 * Returns R2S_OK with the service index, the req_id and the packed request,
 * or R2S_SVC_EMPTY when nothing waits. The slot frees on a take, so the
 * next REQUEST has room.
 *
 * \param[out] index the served service the REQUEST names.
 * \param[out] req_id the req_id the RESPONSE must echo.
 * \param[out] payload the packed request bytes.
 * \param[in] cap how many bytes payload holds.
 * \param[out] len how many bytes the REQUEST carried.
 * \return R2S_OK with a request, R2S_SVC_EMPTY when nothing waits.
 */
r2s_status_t r2s_service_take_request(uint8_t *index, uint8_t *req_id, uint8_t *payload, uint16_t cap,
                                      uint16_t *len);

/// Answers the taken REQUEST: streams a RESPONSE echoing req_id on the
/// reliable channel.
/**
 * A nonzero status carries no response payload [SVR-6]. Fails with
 * R2S_ERR_NOT_CONNECTED while not connected, with R2S_ERR_TX_BUSY while a
 * reliable frame is still outstanding, and with R2S_ERR_BOUNDS for a
 * reserved status or a payload the frame cannot carry; every failure sends
 * nothing.
 *
 * \param[in] req_id the req_id the take call reported.
 * \param[in] status one of the R2S_SVC_* RESPONSE codes.
 * \param[in] payload the packed response, or NULL for an empty one.
 * \param[in] len how many bytes payload holds.
 * \return R2S_OK on success, R2S_ERR_NOT_CONNECTED, R2S_ERR_TX_BUSY or
 * R2S_ERR_BOUNDS as above.
 * \pre req_id came from r2s_service_take_request() and is still unanswered.
 */
r2s_status_t r2s_service_send_response(uint8_t req_id, uint8_t status, const uint8_t *payload, uint16_t len);

/// Takes one call outcome into the caller's buffer.
/**
 * Returns R2S_OK with the req_id, the outcome status and the packed
 * response: the wire status for a RESPONSE, R2S_SVC_TIMEOUT for a call the
 * timeout retired [SVR-8]. A timeout carries no payload. Returns
 * R2S_SVC_EMPTY when no outcome waits.
 *
 * \param[out] req_id the call the outcome answers.
 * \param[out] status the RESPONSE status, or R2S_SVC_TIMEOUT.
 * \param[out] payload the packed response bytes.
 * \param[in] cap how many bytes payload holds.
 * \param[out] len how many bytes the outcome carried.
 * \return R2S_OK with an outcome, R2S_SVC_EMPTY when nothing waits.
 */
r2s_status_t r2s_service_take_response(uint8_t *req_id, uint8_t *status, uint8_t *payload, uint16_t cap,
                                       uint16_t *len);
#endif

#ifdef R2S_TESTING
/* Host test builds only: the same reset, as r2s_test_reset() calls it. */
void r2s_services_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
