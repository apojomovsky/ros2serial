/* service: the registry behind the ROS 2-named service API. It holds the
 * RAM service table the core announces and the records the request pump
 * dispatches through. The core itself keeps no function pointer; the
 * callbacks live here. Without R2S_WITH_SERVICES the core carries no
 * services channel, so this stays an empty translation unit and the
 * sketches that need none are untouched. */
#ifdef R2S_WITH_SERVICES

#include "r2s/service.hpp"

#include "r2s/r2s_services.h"

/* The RAM the services cost, named so a size report can find it: the table
 * the core announces, and the records beside it. */
extern "C" {
r2s_service_t r2s_service_table[R2S_SERVICE_MAX_COUNT];
}

/* One server's typed dispatch, kept beside the entry it announces. */
struct service_server_record
{
  void *server;
  void (*dispatch_request)(void *, uint8_t, uint8_t, const uint8_t *, uint16_t);
  void (*dispatch_flush)(void *);
};

extern "C" {
service_server_record r2s_service_servers[R2S_SERVICE_MAX_COUNT];
}

namespace
{
/* The registry: the entries the table holds, whether the first spin closed
 * it, and the flag the firmware asks about. */
struct registry
{
  uint8_t count;
  uint8_t bound;
  uint8_t error;
};

registry reg;
} // namespace

namespace r2s
{
namespace service
{
namespace detail
{
uint8_t register_server(
  const char *service_name, const char *type_name, const uint8_t *type_hash,
  void *record,
  void (*dispatch_request)(void *, uint8_t, uint8_t, const uint8_t *, uint16_t),
  void (*dispatch_flush)(void *))
{
  uint8_t i;
  r2s_service_t entry;

  if (reg.bound != 0u || reg.count >= R2S_SERVICE_MAX_COUNT) {
    reg.error = 1u;
    return 0xFFu;
  }
  if (service_name == nullptr || type_name == nullptr || type_hash == nullptr ||
    record == nullptr || dispatch_request == nullptr || dispatch_flush == nullptr)
  {
    reg.error = 1u;
    return 0xFFu;
  }
  for (i = 0u; i < 8u; i++) {
    entry.type_hash[i] = type_hash[i];
  }
  entry.type_name = type_name;
  entry.service_name = service_name;
  entry.role = R2S_SVC_SERVER;
  r2s_service_table[reg.count] = entry;
  if (r2s_services_configure(r2s_service_table, (uint8_t)(reg.count + 1u)) != R2S_OK) {
    reg.error = 1u;
    return 0xFFu;
  }
  r2s_service_servers[reg.count].server = record;
  r2s_service_servers[reg.count].dispatch_request = dispatch_request;
  r2s_service_servers[reg.count].dispatch_flush = dispatch_flush;
  return reg.count++;
}

uint8_t register_client(
  const char *service_name, const char *type_name, const uint8_t *type_hash)
{
  uint8_t i;
  r2s_service_t entry;

  if (reg.bound != 0u || reg.count >= R2S_SERVICE_MAX_COUNT) {
    reg.error = 1u;
    return 0xFFu;
  }
  if (service_name == nullptr || type_name == nullptr || type_hash == nullptr) {
    reg.error = 1u;
    return 0xFFu;
  }
  for (i = 0u; i < 8u; i++) {
    entry.type_hash[i] = type_hash[i];
  }
  entry.type_name = type_name;
  entry.service_name = service_name;
  entry.role = R2S_SVC_CLIENT;
  r2s_service_table[reg.count] = entry;
  if (r2s_services_configure(r2s_service_table, (uint8_t)(reg.count + 1u)) != R2S_OK) {
    reg.error = 1u;
    return 0xFFu;
  }
  r2s_service_servers[reg.count].server = NULL;
  r2s_service_servers[reg.count].dispatch_request = NULL;
  r2s_service_servers[reg.count].dispatch_flush = NULL;
  return reg.count++;
}

bool registration_error(void)
{
  return reg.error != 0u;
}

void note_bound(void)
{
  reg.bound = 1u;
}

void pump_services(void)
{
  uint8_t i;

  for (i = 0u; i < reg.count; i++) {
    if (r2s_service_servers[i].dispatch_flush != NULL) {
      r2s_service_servers[i].dispatch_flush(r2s_service_servers[i].server);
    }
  }
  for (;; ) {
    uint8_t index = 0xFFu;
    uint8_t req_id = 0xFFu;
    uint8_t payload[R2S_MAX_FRAME];
    uint16_t len = 0u;
    uint8_t j;

    if (r2s_service_take_request(&index, &req_id, payload, (uint16_t)sizeof payload,
      &len) != R2S_OK)
    {
      return;
    }
    for (j = 0u; j < reg.count; j++) {
      if (j == index && r2s_service_servers[j].dispatch_request != NULL) {
        r2s_service_servers[j].dispatch_request(r2s_service_servers[j].server, index, req_id,
          payload, len);
        break;
      }
    }
  }
}
} // namespace detail
} // namespace service
} // namespace r2s

#ifdef R2S_TESTING
extern "C" void r2s_service_test_reset(void)
{
  uint8_t i;

  for (i = 0u; i < R2S_SERVICE_MAX_COUNT; i++) {
    r2s_service_table[i].type_name = NULL;
    r2s_service_table[i].service_name = NULL;
    r2s_service_table[i].role = 0u;
    r2s_service_servers[i].server = NULL;
    r2s_service_servers[i].dispatch_request = NULL;
    r2s_service_servers[i].dispatch_flush = NULL;
  }
  reg.count = 0u;
  reg.bound = 0u;
  reg.error = 0u;
}
#endif

#endif
