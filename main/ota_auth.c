#include "ota_auth.h"
#include <string.h>

bool ota_auth_check(const char *authorization_header, const char *token)
{
    static const char prefix[] = "Bearer ";
    const size_t plen = sizeof(prefix) - 1;

    if (authorization_header == NULL || token == NULL) {
        return false;
    }
    size_t tlen = strlen(token);
    if (tlen == 0) {
        return false;
    }
    if (strncmp(authorization_header, prefix, plen) != 0) {
        return false;
    }
    const char *h = authorization_header + plen;
    size_t hlen = strlen(h);

    /* Temps constant : on parcourt toujours tlen octets, quel que soit hlen. */
    unsigned char diff = (unsigned char)(hlen != tlen);
    for (size_t i = 0; i < tlen; i++) {
        unsigned char c = (i < hlen) ? (unsigned char)h[i] : 0;
        diff |= (unsigned char)(c ^ (unsigned char)token[i]);
    }
    return diff == 0;
}

bool ota_size_ok(size_t content_length, size_t partition_size)
{
    return content_length > 0 && content_length <= partition_size;
}
