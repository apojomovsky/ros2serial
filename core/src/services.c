/* services: the MCU side of the services channel. The table below is the
 * firmware's announced set; the slots hold one waiting REQUEST, one call
 * outcome and the outstanding MCU-to-host frame. The session calls in for
 * channel 7 frames, handshake transitions and the retry timer. */
#include "r2s/r2s_services.h"

#include <stddef.h>
#include <string.h>

#include "r2s/r2s.h"
#include "r2s/r2s_framer.h"
#include "r2s/r2s_port.h"
#include "r2s/r2s_session.h"
#include "r2s/r2s_tx.h"
#include "r2s/r2s_varint.h"

#ifdef R2S_WITH_SERVICES
/* The frame payload a REQUEST or RESPONSE may carry: the four leading bytes
 * travel as the frame's own, so the caller's bytes stop here. */
#define SVC_MAX_CALL_PAYLOAD (R2S_MAX_FRAME - R2S_TX_OVERHEAD - 4u)

/* A reliable frame waits this long for its ack before it goes out again,
 * and is dropped after this many retries (REL-2). */
#define SVC_REL_RETRY_MS 500u
#define SVC_REL_RETRIES 3u

static struct {
    const r2s_service_t *table;
    uint8_t count;
    uint8_t verdict[R2S_SERVICE_MAX_COUNT];
    /* The MCU-to-host stream of channel 7 (REL-2, REL-3, SVR-4): one frame
     * outstanding with its timer and retries, one rel_seq stream. A second
     * send while one is outstanding fails busy, nothing queues. */
    uint8_t tx_seq;
    uint8_t rel_next;
    uint8_t rx_expected;
    uint8_t out_active;
    uint8_t out_rel;
    uint8_t out_retries;
    uint32_t out_deadline;
    uint16_t out_len;
    uint8_t out_payload[R2S_MAX_FRAME];
    /* One waiting server REQUEST (SVR-5): the slot frees on a take. */
    uint8_t req_active;
    uint8_t req_index;
    uint8_t req_id;
    uint16_t req_len;
    uint8_t req_payload[R2S_MAX_FRAME];
    /* One call outcome (SVR-5, SVR-8): a stored RESPONSE, or a timeout the
     * take reports and retires. */
    uint8_t resp_active;
    uint8_t resp_req;
    uint8_t resp_status;
    uint16_t resp_len;
    uint8_t resp_payload[R2S_MAX_FRAME];
    /* The outstanding client call: its req_id and the tick it started on,
     * for the SVR-8 timeout. */
    uint8_t call_active;
    uint8_t call_req;
    uint32_t call_start;
    uint8_t next_req;
} services;

/* The varint length of value, so declare sizes stay exact. */
static uint8_t varint_len(uint16_t value)
{
    uint8_t out[R2S_VARINT_MAX_BYTES];

    return r2s_varint_encode(value, out);
}

/* The DECLARE payload length for one entry: opcode, index, role, the hash,
 * the length varint and bytes of each name. */
static uint32_t declare_len(const r2s_service_t *service)
{
    size_t type_len = strlen(service->type_name);
    size_t name_len = strlen(service->service_name);

    return (uint32_t)11u + (uint32_t)varint_len((uint16_t)type_len) + (uint32_t)type_len +
           (uint32_t)varint_len((uint16_t)name_len) + (uint32_t)name_len;
}

/* Whether the entry can be announced and answered: a known role, names that
 * fit the strings the frames carry, and a DECLARE frame that fits. */
static r2s_status_t check_entry(const r2s_service_t *service)
{
    if (service->type_name == NULL || service->service_name == NULL) {
        return R2S_ERR_BOUNDS;
    }
    if (strlen(service->type_name) > R2S_VARINT_MAX || strlen(service->service_name) > R2S_VARINT_MAX) {
        return R2S_ERR_BOUNDS;
    }
    if (service->role != R2S_SVC_SERVER && service->role != R2S_SVC_CLIENT) {
        return R2S_ERR_BOUNDS;
    }
    /* 11 fixed bytes, the hashes and each name length varint with its name:
     * exact, so names of 128 bytes or more, whose varint takes two bytes,
     * fit exactly. The sums run in 32 bits, because on a 16-bit size_t a
     * name near 64 KB would wrap a size_t sum back under the limit. */
    if (declare_len(service) > (uint32_t)(R2S_MAX_FRAME - R2S_TX_OVERHEAD)) {
        return R2S_ERR_BOUNDS;
    }
    return R2S_OK;
}

r2s_status_t r2s_services_configure(const r2s_service_t *table, uint8_t count)
{
    uint8_t i;

    if (count > 0u && table == NULL) {
        return R2S_ERR_BOUNDS;
    }
    if (count > R2S_SERVICE_MAX_COUNT) {
        return R2S_ERR_BOUNDS;
    }
    for (i = 0u; i < count; i++) {
        if (check_entry(&table[i]) != R2S_OK) {
            return R2S_ERR_BOUNDS;
        }
    }
    services.table = table;
    services.count = count;
    for (i = 0u; i < count; i++) {
        services.verdict[i] = 0xFFu;
    }
    return R2S_OK;
}

uint8_t r2s_services_count(void)
{
    return services.count;
}

const r2s_service_t *r2s_services_get(uint8_t index)
{
    if (index >= services.count) {
        return NULL;
    }
    return &services.table[index];
}

uint8_t r2s_services_status(uint8_t index)
{
    if (index >= services.count) {
        return 0xFFu;
    }
    return services.verdict[index];
}

uint16_t r2s_services_declare_len(uint8_t index)
{
    if (index >= services.count) {
        return 0u;
    }
    return (uint16_t)declare_len(&services.table[index]);
}

void r2s_services_note_status(uint8_t index, uint8_t status)
{
    if (index < services.count && status <= R2S_SVC_STATUS_NAME_CONFLICT) {
        services.verdict[index] = status;
    }
}

/* Milliseconds to ticks: on 16-bit ints (AVR) ms * R2S_TICK_HZ would
 * overflow, so the product is 32-bit, the way the session converts its own
 * timers. */
static uint32_t ms_to_ticks(uint32_t ms)
{
    return (ms * (uint32_t)R2S_TICK_HZ) / 1000u;
}

/* Streams one channel 7 frame of payload_len bytes, advancing the frame seq
 * past a frame the transmitter accepted. */
static r2s_status_t send_frame(const uint8_t *payload, uint16_t payload_len)
{
    uint16_t i;

    if (r2s_tx_begin(7u, services.tx_seq, payload_len) != R2S_OK) {
        return R2S_ERR_TX_BUSY;
    }
    for (i = 0u; i < payload_len; i++) {
        (void)r2s_tx_byte(payload[i]);
    }
    /* The begin reserved the frame and the loop feeds exactly its length,
     * while port errors never surface, so the end below cannot fail. */
    (void)r2s_tx_end();
    services.tx_seq++;
    return R2S_OK;
}

/* Streams one reliable frame and holds its bytes for the retransmit timer:
 * at most one reliable payload is outstanding per direction (REL-2). The
 * frame carries the next rel_seq of this direction's own stream (REL-3),
 * which moves in lockstep with the host's under stop-and-wait. The rel_seq
 * is spent only when the frame goes out, so a busy transmitter leaves the
 * stream numbering whole. */
static r2s_status_t queue_reliable(uint8_t op, uint8_t second, uint8_t third, const uint8_t *payload,
                                   uint16_t len, uint32_t now)
{
    uint16_t i;
    uint8_t rel;

    if (r2s_tx_begin(7u, services.tx_seq, (uint16_t)(len + 4u)) != R2S_OK) {
        return R2S_ERR_TX_BUSY;
    }
    rel = services.rel_next++;
    services.out_payload[0] = op;
    services.out_payload[1] = rel;
    services.out_payload[2] = second;
    services.out_payload[3] = third;
    (void)r2s_tx_byte(op);
    (void)r2s_tx_byte(rel);
    (void)r2s_tx_byte(second);
    (void)r2s_tx_byte(third);
    for (i = 0u; i < len; i++) {
        services.out_payload[4u + i] = payload[i];
        (void)r2s_tx_byte(payload[i]);
    }
    /* The begin reserved the frame and the loop feeds exactly its length,
     * while port errors never surface, so the end below cannot fail. */
    (void)r2s_tx_end();
    services.tx_seq++;
    services.out_active = 1u;
    services.out_rel = rel;
    services.out_retries = 0u;
    services.out_len = (uint16_t)(len + 4u);
    services.out_deadline = now;
    return R2S_OK;
}

/* One DECLARE answering a QUERY, streamed byte by byte so no buffer holds
 * the frame. */
static void send_declare(uint8_t index)
{
    const r2s_service_t *service = &services.table[index];
    size_t type_len = strlen(service->type_name);
    size_t name_len = strlen(service->service_name);
    uint8_t head[12];
    uint8_t varint[R2S_VARINT_MAX_BYTES];
    uint8_t n;
    uint16_t i;

    /* The caller answers a QUERY for an announced index, so the entry exists
     * and its DECLARE does not fit in zero bytes. */
    if (r2s_tx_begin(7u, services.tx_seq, (uint16_t)declare_len(service)) != R2S_OK) {
        return;
    }
    head[0] = R2S_SVC_OP_DECLARE;
    head[1] = index;
    head[2] = service->role;
    for (i = 0u; i < 8u; i++) {
        head[3u + i] = service->type_hash[i];
    }
    for (i = 0u; i < 11u; i++) {
        (void)r2s_tx_byte(head[i]);
    }
    n = r2s_varint_encode((uint16_t)type_len, varint);
    for (i = 0u; i < n; i++) {
        (void)r2s_tx_byte(varint[i]);
    }
    for (i = 0u; i < (uint16_t)type_len; i++) {
        (void)r2s_tx_byte((uint8_t)service->type_name[i]);
    }
    n = r2s_varint_encode((uint16_t)name_len, varint);
    for (i = 0u; i < n; i++) {
        (void)r2s_tx_byte(varint[i]);
    }
    for (i = 0u; i < (uint16_t)name_len; i++) {
        (void)r2s_tx_byte((uint8_t)service->service_name[i]);
    }
    /* The begin reserved the frame and the loop feeds exactly its length,
     * while port errors never surface, so the end below cannot fail. */
    (void)r2s_tx_end();
    services.tx_seq++;
}

/* One plain STATUS answering a QUERY past the table: under the request
 * retry, never a reliable frame, rel_seq 0 (SVR-7). */
static void send_plain_status(uint8_t index, uint8_t status)
{
    const uint8_t payload[] = {R2S_SVC_OP_STATUS, 0u, index, status};

    (void)send_frame(payload, sizeof payload);
}

void r2s_services_answer_query(uint8_t index)
{
    if (index < services.count) {
        send_declare(index);
    } else {
        send_plain_status(index, R2S_SVC_UNKNOWN_SERVICE);
    }
}

r2s_status_t r2s_service_call(uint8_t index, const uint8_t *payload, uint16_t len, uint8_t *req_id)
{
    uint8_t req;
    uint32_t now;

    if (r2s_connected() == 0u) {
        return R2S_ERR_NOT_CONNECTED;
    }
    if (req_id == NULL || index >= services.count || services.table[index].role != R2S_SVC_CLIENT ||
        (payload == NULL && len > 0u) || len > SVC_MAX_CALL_PAYLOAD) {
        return R2S_ERR_BOUNDS;
    }
    /* A second send while one is outstanding fails busy and sends nothing:
     * at most one reliable payload is outstanding per direction (REL-2),
     * and at most one call is outstanding (SVR-10). */
    if (services.out_active != 0u || services.call_active != 0u) {
        return R2S_ERR_TX_BUSY;
    }
    req = services.next_req;
    now = r2s_port_tick();
    if (queue_reliable(R2S_SVC_OP_REQUEST, req, index, payload, len, now) != R2S_OK) {
        return R2S_ERR_TX_BUSY;
    }
    services.next_req++;
    *req_id = req;
    services.call_active = 1u;
    services.call_req = req;
    services.call_start = now;
    return R2S_OK;
}

r2s_status_t r2s_service_take_request(uint8_t *index, uint8_t *req_id, uint8_t *payload, uint16_t cap,
                                      uint16_t *len)
{
    uint16_t i;

    if (services.req_active == 0u) {
        return R2S_SVC_EMPTY;
    }
    /* A buffer the REQUEST does not fit in fails without freeing the slot,
     * so the application retries with room. */
    if (cap < services.req_len) {
        return R2S_ERR_BOUNDS;
    }
    *index = services.req_index;
    *req_id = services.req_id;
    for (i = 0u; i < services.req_len; i++) {
        payload[i] = services.req_payload[i];
    }
    *len = services.req_len;
    services.req_active = 0u;
    return R2S_OK;
}

r2s_status_t r2s_service_send_response(uint8_t req_id, uint8_t status, const uint8_t *payload, uint16_t len)
{
    if (r2s_connected() == 0u) {
        return R2S_ERR_NOT_CONNECTED;
    }
    if (status > R2S_SVC_BUSY || (payload == NULL && len > 0u) || len > SVC_MAX_CALL_PAYLOAD) {
        return R2S_ERR_BOUNDS;
    }
    if (services.out_active != 0u) {
        return R2S_ERR_TX_BUSY;
    }
    return queue_reliable(R2S_SVC_OP_RESPONSE, req_id, status, payload, len, r2s_port_tick());
}

r2s_status_t r2s_service_take_response(uint8_t *req_id, uint8_t *status, uint8_t *payload, uint16_t cap,
                                       uint16_t *len)
{
    uint16_t i;

    if (services.resp_active != 0u) {
        /* A buffer the outcome does not fit in fails without freeing the
         * slot, so the application retries with room. */
        if (cap < services.resp_len) {
            return R2S_ERR_BOUNDS;
        }
        *req_id = services.resp_req;
        *status = services.resp_status;
        for (i = 0u; i < services.resp_len; i++) {
            payload[i] = services.resp_payload[i];
        }
        *len = services.resp_len;
        services.resp_active = 0u;
        return R2S_OK;
    }
    /* A call with no RESPONSE within the timeout reports the timeout and
     * retires the req_id (SVR-8). */
    if (services.call_active != 0u &&
        (uint32_t)(r2s_port_tick() - services.call_start) >= ms_to_ticks(R2S_SVC_CALL_TIMEOUT_MS)) {
        *req_id = services.call_req;
        *status = R2S_SVC_TIMEOUT;
        *len = 0u;
        services.call_active = 0u;
        return R2S_OK;
    }
    return R2S_SVC_EMPTY;
}

/* Every reliable frame is acked by the session, duplicates included: the
 * payload applies at most once, so only the expected rel_seq applies and
 * advances the counter (REL-3). A second REQUEST while the slot is full is
 * refused after its ack, since a conforming peer never sends one. */
int r2s_services_apply_request(uint8_t rel_seq, uint8_t index, uint8_t req_id, const uint8_t *payload,
                               uint16_t len)
{
    uint16_t i;

    if (rel_seq != services.rx_expected) {
        return 0;
    }
    services.rx_expected++;
    if (services.req_active != 0u) {
        return -1;
    }
    services.req_active = 1u;
    services.req_index = index;
    services.req_id = req_id;
    services.req_len = len;
    for (i = 0u; i < len; i++) {
        services.req_payload[i] = payload[i];
    }
    return 1;
}

/* A RESPONSE answers only the call outstanding under its own req_id: any
 * other is acked and dropped (SVR-5). */
int r2s_services_apply_response(uint8_t rel_seq, uint8_t req_id, uint8_t status, const uint8_t *payload,
                                uint16_t len)
{
    uint16_t i;

    if (rel_seq != services.rx_expected) {
        return 0;
    }
    services.rx_expected++;
    if (services.call_active == 0u || req_id != services.call_req) {
        return 0;
    }
    services.call_active = 0u;
    services.resp_active = 1u;
    services.resp_req = req_id;
    services.resp_status = status;
    services.resp_len = len;
    for (i = 0u; i < len; i++) {
        services.resp_payload[i] = payload[i];
    }
    return 1;
}

void r2s_services_note_ack(uint8_t rel_seq)
{
    /* An ack for the outstanding frame frees the stream; anything else is a
     * duplicate or a stranger (REL-2). */
    if (services.out_active != 0u && rel_seq == services.out_rel) {
        services.out_active = 0u;
    }
}

/* The 500 ms timer: resends the outstanding frame, or drops and counts it
 * after 3 retries (REL-2). */
void r2s_services_poll(uint32_t now)
{
    if (services.out_active == 0u ||
        (uint32_t)(now - services.out_deadline) < ms_to_ticks(SVC_REL_RETRY_MS)) {
        return;
    }
    if (services.out_retries >= SVC_REL_RETRIES) {
        services.out_active = 0u;
        r2s_framer_count_refused();
        return;
    }
    services.out_retries++;
    if (send_frame(services.out_payload, services.out_len) != R2S_OK) {
        services.out_deadline = now - ms_to_ticks(SVC_REL_RETRY_MS);
        return;
    }
    services.out_deadline = now;
}

void r2s_services_note_reconnect(void)
{
    /* A re-handshake drops all reliable state on both sides (REL-5). */
    services.tx_seq = 0u;
    services.rel_next = 0u;
    services.rx_expected = 0u;
    services.out_active = 0u;
    services.out_retries = 0u;
    services.req_active = 0u;
    services.resp_active = 0u;
    /* The outstanding call retires with the stream: the take reports
     * nothing further for it. */
    services.call_active = 0u;
}
#endif

#ifdef R2S_TESTING
void r2s_services_test_reset(void)
{
#ifdef R2S_WITH_SERVICES
    services.table = NULL;
    services.count = 0u;
    services.tx_seq = 0u;
    services.rel_next = 0u;
    services.rx_expected = 0u;
    services.out_active = 0u;
    services.out_retries = 0u;
    services.req_active = 0u;
    services.resp_active = 0u;
    services.call_active = 0u;
    services.next_req = 0u;
#endif
}
#endif

#if !defined(R2S_WITH_SERVICES) && !defined(R2S_TESTING)
/* ISO C needs at least one declaration in a translation unit. */
typedef int r2s_services_unused;
#endif
