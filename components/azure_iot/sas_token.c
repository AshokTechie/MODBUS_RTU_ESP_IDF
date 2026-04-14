#include "sas_token.h"

#include "mbedtls/base64.h"
#include "mbedtls/md.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void url_encode(const char *in, char *out, size_t out_size)
{
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 4 < out_size; i++) {
        unsigned char c = (unsigned char)in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[j++] = (char)c;
        } else {
            j += snprintf(out + j, out_size - j, "%%%02X", c);
        }
    }
    out[j] = '\0';
}

static int hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len, uint8_t out[32])
{
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (mbedtls_md_setup(&ctx, info, 1) != 0) {
        mbedtls_md_free(&ctx);
        return -1;
    }
    mbedtls_md_hmac_starts(&ctx, key, key_len);
    mbedtls_md_hmac_update(&ctx, data, data_len);
    mbedtls_md_hmac_finish(&ctx, out);
    mbedtls_md_free(&ctx);
    return 0;
}

int sas_token_generate(const char *hostname,
                       const char *device_id,
                       const char *sak_b64,
                       uint64_t expiry_secs,
                       char *out,
                       size_t out_size)
{
    char resource[192];
    char resource_enc[256];
    char to_sign[320];
    uint8_t key_buf[64];
    size_t key_len = 0;
    uint8_t sig_raw[32];
    uint8_t sig_b64[64];
    size_t sig_b64_len = 0;
    char sig_enc[128];

    snprintf(resource, sizeof(resource), "%s/devices/%s", hostname, device_id);
    url_encode(resource, resource_enc, sizeof(resource_enc));
    int to_sign_len = snprintf(to_sign, sizeof(to_sign), "%s\n%llu", resource_enc, (unsigned long long)expiry_secs);

    if (mbedtls_base64_decode(key_buf, sizeof(key_buf), &key_len, (const uint8_t *)sak_b64, strlen(sak_b64)) != 0) {
        return -1;
    }
    if (hmac_sha256(key_buf, key_len, (const uint8_t *)to_sign, (size_t)to_sign_len, sig_raw) != 0) {
        return -2;
    }
    if (mbedtls_base64_encode(sig_b64, sizeof(sig_b64), &sig_b64_len, sig_raw, sizeof(sig_raw)) != 0) {
        return -3;
    }
    sig_b64[sig_b64_len] = '\0';
    url_encode((const char *)sig_b64, sig_enc, sizeof(sig_enc));

    int written = snprintf(out, out_size,
                           "SharedAccessSignature sr=%s&sig=%s&se=%llu",
                           resource_enc, sig_enc, (unsigned long long)expiry_secs);
    return (written < 0 || (size_t)written >= out_size) ? -4 : 0;
}

int sas_token_is_expired(const char *token)
{
    const char *se = strstr(token, "&se=");
    if (!se) {
        return 1;
    }
    uint64_t expiry = (uint64_t)strtoull(se + 4, NULL, 10);
    time_t now = time(NULL);
    return now >= 0 && (uint64_t)now >= expiry;
}
