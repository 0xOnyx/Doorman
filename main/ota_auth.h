#ifndef OTA_AUTH_H
#define OTA_AUTH_H
#include <stdbool.h>
#include <stddef.h>

/* true si header vaut exactement "Bearer " + token. Comparaison en temps constant
 * sur la longueur du token. NULL ou token vide -> false. */
bool ota_auth_check(const char *authorization_header, const char *token);

/* true si 0 < content_length <= partition_size. */
bool ota_size_ok(size_t content_length, size_t partition_size);

#endif
