/**
 * @file  provision_gen.c
 * @brief Host-side tool: build valid config-bank images (header + JSON) for
 *        pre-provisioning sub-devices at flash time.
 *
 * It links the REAL firmware config code, so the CRC is exactly the one the
 * gateway recomputes on load (config_store_calc_crc()).
 *
 * Usage: provision_gen <input.json> <output-prefix>
 *   -> <output-prefix>_A.bin / <output-prefix>_B.bin  (8 KB banks, rest 0xFF)
 *
 * Input JSON may contain only the domains you want to override (e.g. sensors,
 * devices, meta); config_store_from_json() fills the rest from factory default.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

#include "config_store.h"
#include "config_internal.h"
#include "utils.h"

#define BANK_SIZE   8192u
#define HDR_MAGIC   0x47435452u   /* 'R''T''C''G' */

typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint32_t crc32;
    uint32_t updated_at;
    uint32_t json_len;
    uint32_t seq;
    uint32_t rsv;
    uint32_t hdr_crc;
} bank_hdr_t;

static int read_file(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    size_t n;
    if (f == NULL) { return -1; }
    n = fread(buf, 1u, cap - 1u, f);
    fclose(f);
    buf[n] = '\0';
    return (int)n;
}

static int write_bank(const char *path, const bank_hdr_t *h,
                      const char *json, uint16_t len)
{
    uint8_t buf[BANK_SIZE];
    FILE   *f;

    memset(buf, 0xFF, sizeof(buf));
    memcpy(buf, h, sizeof(*h));
    memcpy(buf + sizeof(*h), json, len);

    f = fopen(path, "wb");
    if (f == NULL) { return -1; }
    if (fwrite(buf, 1u, sizeof(buf), f) != sizeof(buf)) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    static char    in[CFG_JSON_MAX];
    static char    out[CFG_JSON_MAX];
    static config_store_t c;
    static config_store_t chk;
    bank_hdr_t     h;
    uint32_t       crc = 0u;
    uint16_t       n;
    int            len;
    char           path[512];

    if (argc != 3)
    {
        fprintf(stderr, "usage: %s <input.json> <output-prefix>\n", argv[0]);
        return 2;
    }

    len = read_file(argv[1], in, sizeof(in));
    if (len <= 0)
    {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }

    if (config_store_from_json(&c, in) != CFG_OK)
    {
        fprintf(stderr, "config_store_from_json failed (bad field/range/dup?)\n");
        return 3;
    }
    if (config_store_validate(&c) != CFG_OK)
    {
        fprintf(stderr, "config_store_validate failed\n");
        return 3;
    }

    if (c.meta.config_version == 0u) { c.meta.config_version = 1u; }
    if (c.meta.updated_at == 0u)     { c.meta.updated_at = (uint32_t)time(NULL); }

    n = cfg_serialize(&c, out, (uint16_t)sizeof(out), &crc);
    if ((n == 0u) || (crc == 0u))
    {
        fprintf(stderr, "cfg_serialize failed\n");
        return 3;
    }
    if (((uint32_t)n + sizeof(h)) > BANK_SIZE)
    {
        fprintf(stderr, "config too large: %u + %u\n", (unsigned)n, (unsigned)sizeof(h));
        return 3;
    }

    memset(&h, 0, sizeof(h));
    h.magic      = HDR_MAGIC;
    h.version    = c.meta.config_version;
    h.crc32      = crc;
    h.updated_at = c.meta.updated_at;
    h.json_len   = n;
    h.seq        = 1u;
    h.rsv        = 0u;
    h.hdr_crc    = crc32_ieee((const uint8_t *)&h, (uint32_t)offsetof(bank_hdr_t, hdr_crc));

    /* SELF-CHECK: the bank JSON must parse back to the same CRC the gateway
     * recomputes on load (cfg_bank_read does exactly this comparison). */
    if (config_store_from_json(&chk, out) != CFG_OK ||
        config_store_calc_crc(&chk) != h.crc32)
    {
        fprintf(stderr, "SELF-CHECK failed: serialized JSON/CRC mismatch\n");
        return 4;
    }

    snprintf(path, sizeof(path), "%s_A.bin", argv[2]);
    if (write_bank(path, &h, out, n) != 0) { perror(path); return 5; }
    snprintf(path, sizeof(path), "%s_B.bin", argv[2]);
    if (write_bank(path, &h, out, n) != 0) { perror(path); return 5; }

    printf("bank ok: sensors=%u devices=%u version=%u json=%u crc=%08lX seq=%u\n",
           (unsigned)c.sensor_count, (unsigned)c.device_count,
           (unsigned)c.meta.config_version, (unsigned)n,
           (unsigned long)crc, (unsigned)h.seq);
    return 0;
}
