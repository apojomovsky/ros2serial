/* The announced services (SVR-2, SVR-7): the hashes are the first 8 bytes
 * of each service's RIHS01 hash, and the request and response types are
 * descriptor indices into the shared table. */
const r2s_service_t R2S_FLASH r2s_services[R2S_SERVICE_COUNT] = {
  {"demo_srvs/srv/Spin",
    "reset",
    R2S_SERVICE_RESET_ROLE,
    {0x15u, 0x58u, 0xadu, 0xbau, 0x0cu, 0x7bu, 0xf1u, 0xf7u}},
  {"demo_srvs/srv/Spin",
    "trigger",
    R2S_SERVICE_TRIGGER_ROLE,
    {0x15u, 0x58u, 0xadu, 0xbau, 0x0cu, 0x7bu, 0xf1u, 0xf7u}},
};
