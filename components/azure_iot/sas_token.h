#pragma once

#include <stddef.h>
#include <stdint.h>

int sas_token_generate(const char *hostname,
                       const char *device_id,
                       const char *sak_b64,
                       uint64_t expiry_secs,
                       char *out,
                       size_t out_size);

int sas_token_is_expired(const char *token);
