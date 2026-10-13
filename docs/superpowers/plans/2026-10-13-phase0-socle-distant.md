# Phase 0 : socle distant — plan d'implémentation

> **Amendement 2026-10-13 : cible ESP32-S3.** La carte installée est une ESP32-S3 (ESP32-S3FH4R2). Les
> mentions d'ESP32-C3 ci-dessous sont historiques : cible `esp32s3`, broches interdites S3, console
> principale USB-Serial-JTAG et bouton BOOT GPIO0. Détail dans l'amendement en tête de la spec.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produire le firmware ESP32-C3 de la phase 0 : accès distant complet (Wi-Fi, logs TCP, OTA signée, rollback, SoftAP de secours), sniffer du bus TC:Bus, et bibliothèque de protocole pure testée sur PC.

**Architecture:** Un projet ESP-IDF v5.5.5 en C11. Trois fichiers de logique pure (`tc_proto.c`, `logbuf.c`, `ota_auth.c`) sans dépendance ESP-IDF, testés sur PC avec `gcc`. Autour, des modules ESP-IDF minces : `netlog` (hook de log + serveur TCP), `tc_bus` (RMT RX), `app_net` (Wi-Fi), `app_ota` (serveur HTTP 8080), `app_selftest` (validation ou rollback de l'image). L'émission sur le bus n'est **pas** compilée.

**Tech Stack:** ESP-IDF v5.5.5 (`~/esp/esp-idf`), cible `esp32c3`, FreeRTOS, `esp_driver_rmt`, `esp_http_server`, `app_update`, `espressif/mdns` (Component Manager), gcc MinGW de CLion pour les tests PC.

**Spec :** `docs/superpowers/specs/2026-10-13-phase0-socle-distant-design.md` (référencée « spec » ci-dessous). En cas de doute, la spec fait foi.

---

## Environnement et commandes

**Build firmware (PowerShell)** — activer ESP-IDF une fois par session :

```powershell
. "$HOME\esp\esp-idf\export.ps1"      # affiche "ESP-IDF v5.5.5" ; un exit code non nul juste après est anodin
cd C:\Users\jdossantos\CLionProjects\DoormanJerdos
idf.py build
```

Résultat attendu d'un build réussi : la dernière ligne contient `Project build complete.` et une ligne
`interphone-homekit.bin binary size 0x..... bytes`.

**Tests PC (Git Bash)** — `gcc` et `mingw32-make` viennent du MinGW de CLion :

```bash
export PATH="/c/Users/jdossantos/AppData/Local/Programs/CLion/bin/mingw/bin:$PATH"
cd /c/Users/jdossantos/CLionProjects/DoormanJerdos
mingw32-make -C test/host test
```

Résultat attendu : chaque exécutable affiche `N checks, 0 failures` et `mingw32-make` se termine sans erreur.

**Git** : un commit par tâche, message en français, suffixe `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`.

**Règles absolues** (spec §3 et §14) : `CONFIG_TC_BUS_TX_ENABLE` reste à `n` ; jamais `CONFIG_SECURE_BOOT`,
`CONFIG_SECURE_FLASH_ENC_ENABLED`, `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK` ; aucun secret dans les logs ni dans Git.

## Structure des fichiers

| Fichier | Responsabilité |
|---|---|
| `CMakeLists.txt`, `version.txt`, `sdkconfig.defaults`, `partitions.csv`, `.gitignore`, `.gitattributes` | Projet ESP-IDF |
| `keys/README.md` | Rappel de sauvegarde de la clé (la clé elle-même n'est pas commitée) |
| `main/Kconfig.projbuild` | Options `APP_*` |
| `main/main.c` | Séquence de démarrage (spec §6), battement de cœur |
| `main/app_status.c/.h` | Noms lisibles des raisons de reset et des états OTA (partagés par `main` et `app_ota`) |
| `main/ota_auth.c/.h` | **Pur** : token en temps constant, contrôle de taille |
| `main/app_net.c/.h` | Wi-Fi STA, backoff, mDNS, SoftAP de secours (spec §7) |
| `main/app_ota.c/.h` | httpd 8080 : `/info`, `/ota`, `/reboot` (spec §9) |
| `main/app_selftest.c/.h` | Auto-test et rollback (spec §10) |
| `components/netlog/{include/logbuf.h,logbuf.c}` | **Pur** : tampon circulaire |
| `components/netlog/{include/netlog.h,netlog.c}` | Hook vprintf + serveur TCP (spec §8) |
| `components/tc_bus/Kconfig` | Options `TC_BUS_*` |
| `components/tc_bus/{include/tc_proto.h,tc_proto.c}` | **Pur** : protocole (spec §11.1) |
| `components/tc_bus/{include/tc_bus.h,tc_bus.c}` | RMT RX, tâche, compteurs (spec §11.2) |
| `test/host/{Makefile,t.h,test_*.c}` | Tests PC |
| `tools/{ota.sh,logs.py}` | Outils PC |
| `README.md` | Usage et crédits |

---

### Task 1 : Squelette ESP-IDF qui compile

**Files:**
- Create: `.gitignore`, `.gitattributes`, `version.txt`, `CMakeLists.txt`, `sdkconfig.defaults`, `partitions.csv`, `keys/README.md`
- Create: `main/CMakeLists.txt`, `main/idf_component.yml`, `main/Kconfig.projbuild`, `main/main.c`
- Create: `components/tc_bus/Kconfig`, `components/tc_bus/CMakeLists.txt`, `components/netlog/CMakeLists.txt`

- [ ] **Step 1 : fichiers de projet**

`.gitignore` :
```
build/
build-*/
sdkconfig
sdkconfig.old
managed_components/
dependencies.lock
keys/*.pem
test/host/*.exe
test/host/test_tc_proto
test/host/test_logbuf
test/host/test_ota_auth
.idea/
```

`.gitattributes` :
```
* text=auto eol=lf
*.png binary
```

`version.txt` :
```
0.1.0
```

`CMakeLists.txt` :
```cmake
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(interphone-homekit)
```

`partitions.csv` :
```
# Name,      Type, SubType,  Offset,   Size,    Flags
sec_cert,    0x3F, ,         0xd000,   0x3000,
nvs,         data, nvs,      0x10000,  0x6000,
otadata,     data, ota,      ,         0x2000,
phy_init,    data, phy,      ,         0x1000,
ota_0,       app,  ota_0,    0x20000,  1600K,
ota_1,       app,  ota_1,    ,         1600K,
factory_nvs, data, nvs,      0x340000, 0x6000,
nvs_keys,    data, nvs_keys, 0x346000, 0x1000,
```

`sdkconfig.defaults` :
```
CONFIG_IDF_TARGET="esp32c3"
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_COMPILER_OPTIMIZATION_SIZE=y

# Images signees sans Secure Boot materiel (spec 5.2). Aucun eFuse n'est touche.
CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y
CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME=y
CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=y
CONFIG_SECURE_BOOT_SIGNING_KEY="keys/signing_key.pem"
CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT=y

# Rollback automatique (pas d'anti-rollback : il ecrit des eFuses)
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y

# Console sur UART0 et sur l'USB natif
CONFIG_ESP_CONSOLE_UART_DEFAULT=y
CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y

# Logs sans codes couleur (lisibles tels quels sur le port 3333)
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
CONFIG_LOG_COLORS=n

# Reseau
CONFIG_LWIP_MAX_SOCKETS=16
CONFIG_LWIP_SO_REUSE=y
CONFIG_MBEDTLS_HARDWARE_MPI=y
```

`keys/README.md` :
```markdown
# Clé de signature

`signing_key.pem` (RSA-3072) signe chaque image. Elle n'est **jamais** commitée.

**Sauvegarde-la hors du PC** (gestionnaire de mots de passe, clé USB). Sans elle, plus aucune OTA
n'est possible : il faudrait rebrancher l'USB.

Génération (une seule fois) :

    idf.py secure-generate-signing-key --version 2 --scheme rsa3072 keys/signing_key.pem
```

- [ ] **Step 2 : composant `main`**

`main/CMakeLists.txt` :
```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_gpio esp_app_format app_update nvs_flash esp_timer)
```

`main/idf_component.yml` :
```yaml
dependencies:
  espressif/mdns: "^1.4.0"
```

`main/Kconfig.projbuild` :
```
menu "Interphone"

    menu "Wi-Fi"
        config APP_WIFI_SSID
            string "SSID (2.4 GHz)"
            default ""
        config APP_WIFI_PASSWORD
            string "Mot de passe Wi-Fi"
            default ""
        config APP_HOSTNAME
            string "Nom d'hote mDNS (<nom>.local)"
            default "interphone"
        config APP_WIFI_TX_POWER
            int "Puissance d'emission max (unites de 0.25 dBm, 52 = 13 dBm)"
            range 8 84
            default 52
        config APP_RECOVERY_AP_PASSWORD
            string "Mot de passe du SoftAP de secours (>= 8 caracteres, vide = desactive)"
            default ""
        config APP_RECOVERY_AP_DELAY_S
            int "Delai sans connexion avant SoftAP de secours (s)"
            range 30 86400
            default 600
    endmenu

    menu "Logs reseau"
        config APP_NETLOG_ENABLE
            bool "Serveur de logs TCP"
            default y
        config APP_NETLOG_PORT
            int "Port TCP des logs"
            depends on APP_NETLOG_ENABLE
            default 3333
        config APP_NETLOG_BUFFER_KB
            int "Tampon circulaire (Ko)"
            depends on APP_NETLOG_ENABLE
            range 4 32
            default 12
    endmenu

    menu "OTA et auto-test"
        config APP_HTTP_PORT
            int "Port du serveur HTTP (OTA, info)"
            default 8080
        config APP_OTA_TOKEN
            string "Token OTA (>= 32 caracteres, openssl rand -hex 24)"
            default ""
        config APP_SELFTEST_TIMEOUT_S
            int "Delai max de l'auto-test (s)"
            range 10 3600
            default 90
        config APP_SELFTEST_FORCE_FAIL
            bool "TEST UNIQUEMENT : forcer l'echec de l'auto-test"
            default n
    endmenu

    config APP_HEARTBEAT_S
        int "Periode du battement de coeur dans les logs (s, 0 = desactive)"
        range 0 3600
        default 300

endmenu
```

`main/main.c` (version minimale, complétée dans les tâches suivantes) :
```c
#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

/* Spec 3 regle 1 : GPIO10 (TX bus) en sortie a 0 avant toute autre chose. */
static void bus_gpio_safe(void)
{
    gpio_set_level(CONFIG_TC_BUS_TX_GPIO, 0);
    gpio_config_t tx = {
        .pin_bit_mask = 1ULL << CONFIG_TC_BUS_TX_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&tx);
    gpio_set_level(CONFIG_TC_BUS_TX_GPIO, 0);

    gpio_config_t rx = {
        .pin_bit_mask = 1ULL << CONFIG_TC_BUS_RX_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&rx);
}

static void nvs_init(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS a effacer (%s)", esp_err_to_name(e));
        nvs_flash_erase();
        e = nvs_flash_init();
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(e));
    }
}

void app_main(void)
{
    bus_gpio_safe();
    nvs_init();
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "interphone-homekit %s sur %s, reset=%d, heap=%" PRIu32,
             app->version, run ? run->label : "?", (int)esp_reset_reason(),
             esp_get_free_heap_size());
}
```

- [ ] **Step 3 : composants vides mais enregistrés**

`components/tc_bus/Kconfig` :
```
menu "Bus TC (tc_bus)"
    config TC_BUS_RX_GPIO
        int "GPIO de reception (diviseur)"
        range 0 21
        default 4
    config TC_BUS_TX_GPIO
        int "GPIO d'emission (base du transistor)"
        range 0 21
        default 10
    config TC_BUS_TX_ENABLE
        bool "Compiler l'emission (DOIT rester desactive en phase 0)"
        default n
    config TC_BUS_GLITCH_US
        int "Segments plus courts que ceci sont des parasites (us)"
        range 100 2000
        default 1000
endmenu
```

`components/tc_bus/CMakeLists.txt` (les sources arrivent aux tâches 4 et 13) :
```cmake
idf_component_register(
    SRCS "tc_proto.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_rmt esp_timer)
```

`components/netlog/CMakeLists.txt` (les sources arrivent aux tâches 3 et 7) :
```cmake
idf_component_register(
    SRCS "logbuf.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES lwip)
```

Pour que cette tâche compile seule, créer aussi deux fichiers provisoires, remplacés plus tard :

`components/tc_bus/tc_proto.c` :
```c
/* remplace a la tache 4 */
```
`components/netlog/logbuf.c` :
```c
/* remplace a la tache 3 */
```

- [ ] **Step 4 : clé de signature et premier build**

PowerShell :
```powershell
. "$HOME\esp\esp-idf\export.ps1"
cd C:\Users\jdossantos\CLionProjects\DoormanJerdos
New-Item -ItemType Directory -Force keys | Out-Null
idf.py secure-generate-signing-key --version 2 --scheme rsa3072 keys/signing_key.pem
idf.py set-target esp32c3
idf.py build
```
Attendu : `keys/signing_key.pem` existe ; `set-target` crée `sdkconfig` ; le build se termine par
`Project build complete.` et affiche `Generating signed binary image` (ou équivalent) pour l'app.
Le Component Manager télécharge `espressif/mdns` dans `managed_components/` (connexion Internet requise).

- [ ] **Step 5 : vérifier que la clé n'est pas suivie par Git**

```bash
git status --short | grep -c signing_key   # attendu : 0
```

- [ ] **Step 6 : commit**

```bash
git add -A
git commit -m "feat: squelette ESP-IDF esp32c3 (signature, rollback, partitions, Kconfig)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

**Rappel à transmettre à l'utilisateur à la fin de cette tâche** : sauvegarder `keys/signing_key.pem` hors du PC.

---

### Task 2 : Harnais de test PC et `ota_auth`

**Files:**
- Create: `test/host/t.h`, `test/host/Makefile`, `test/host/test_ota_auth.c`
- Create: `main/ota_auth.h`, `main/ota_auth.c`

- [ ] **Step 1 : harnais**

`test/host/t.h` :
```c
#ifndef T_H
#define T_H
#include <stdio.h>

static int t_failures = 0;
static int t_checks = 0;

#define CHECK(cond) do { t_checks++; if (!(cond)) { t_failures++; \
    printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define CHECK_EQ(a, b) do { t_checks++; long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { t_failures++; \
    printf("  FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

#define RUN(fn) do { printf("- %s\n", #fn); fn(); } while (0)

#define T_END() do { printf("%d checks, %d failures\n", t_checks, t_failures); \
    return t_failures ? 1 : 0; } while (0)

#endif
```

`test/host/Makefile` :
```make
CC := gcc
CFLAGS := -std=c11 -Wall -Wextra -Werror -O1 \
          -I../../components/tc_bus/include -I../../components/netlog/include -I../../main

ifeq ($(OS),Windows_NT)
EXE := .exe
else
EXE :=
endif

TESTS := test_ota_auth$(EXE) test_logbuf$(EXE) test_tc_proto$(EXE)

all: $(TESTS)

test_ota_auth$(EXE): test_ota_auth.c ../../main/ota_auth.c ../../main/ota_auth.h t.h
	$(CC) $(CFLAGS) -o $@ test_ota_auth.c ../../main/ota_auth.c

test_logbuf$(EXE): test_logbuf.c ../../components/netlog/logbuf.c ../../components/netlog/include/logbuf.h t.h
	$(CC) $(CFLAGS) -o $@ test_logbuf.c ../../components/netlog/logbuf.c

test_tc_proto$(EXE): test_tc_proto.c ../../components/tc_bus/tc_proto.c ../../components/tc_bus/include/tc_proto.h t.h
	$(CC) $(CFLAGS) -o $@ test_tc_proto.c ../../components/tc_bus/tc_proto.c

test: $(TESTS)
	./test_ota_auth$(EXE)
	./test_logbuf$(EXE)
	./test_tc_proto$(EXE)

clean:
	rm -f $(TESTS)

.PHONY: all test clean
```

Tant que `test_logbuf.c` et `test_tc_proto.c` n'existent pas, lancer une cible à la fois :
`mingw32-make -C test/host test_ota_auth.exe && ./test/host/test_ota_auth.exe`.

- [ ] **Step 2 : test qui échoue**

`test/host/test_ota_auth.c` :
```c
#include "t.h"
#include "ota_auth.h"

#define TOK "0123456789abcdef0123456789abcdef"   /* 32 caracteres */

static void test_header_exact(void)
{
    CHECK(ota_auth_check("Bearer " TOK, TOK));
}

static void test_refus(void)
{
    CHECK(!ota_auth_check("Bearer 0123456789abcdef0123456789abcdeX", TOK)); /* dernier char faux */
    CHECK(!ota_auth_check("Bearer 0123456789abcdef", TOK));                 /* tronque */
    CHECK(!ota_auth_check("Bearer " TOK "0", TOK));                          /* trop long */
    CHECK(!ota_auth_check(TOK, TOK));                                        /* sans Bearer */
    CHECK(!ota_auth_check("bearer " TOK, TOK));                              /* casse */
    CHECK(!ota_auth_check("Bearer ", TOK));
    CHECK(!ota_auth_check("", TOK));
    CHECK(!ota_auth_check(NULL, TOK));
    CHECK(!ota_auth_check("Bearer " TOK, NULL));
    CHECK(!ota_auth_check("Bearer ", ""));                                   /* token vide refuse */
}

static void test_size(void)
{
    CHECK(!ota_size_ok(0, 1638400));
    CHECK(!ota_size_ok(1638401, 1638400));
    CHECK(ota_size_ok(1638400, 1638400));
    CHECK(ota_size_ok(1, 1638400));
}

int main(void)
{
    RUN(test_header_exact);
    RUN(test_refus);
    RUN(test_size);
    T_END();
}
```

- [ ] **Step 3 : vérifier l'échec**

```bash
export PATH="/c/Users/jdossantos/AppData/Local/Programs/CLion/bin/mingw/bin:$PATH"
mingw32-make -C test/host test_ota_auth.exe
```
Attendu : erreur de compilation, `ota_auth.h: No such file or directory`.

- [ ] **Step 4 : implémentation**

`main/ota_auth.h` :
```c
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
```

`main/ota_auth.c` :
```c
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
```

- [ ] **Step 5 : vérifier le succès**

```bash
mingw32-make -C test/host test_ota_auth.exe && ./test/host/test_ota_auth.exe
```
Attendu : `15 checks, 0 failures`.

- [ ] **Step 6 : commit**

```bash
git add test/host main/ota_auth.c main/ota_auth.h
git commit -m "feat: harnais de test PC et ota_auth (token en temps constant, taille)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 3 : `logbuf`, tampon circulaire pur

**Files:**
- Create: `test/host/test_logbuf.c`, `components/netlog/include/logbuf.h`
- Replace: `components/netlog/logbuf.c`

- [ ] **Step 1 : test qui échoue**

`test/host/test_logbuf.c` :
```c
#include "t.h"
#include "logbuf.h"
#include <string.h>

static void test_write_read(void)
{
    uint8_t st[16];
    logbuf_t lb;
    logbuf_init(&lb, st, sizeof st);
    logbuf_write(&lb, "hello", 5);
    CHECK_EQ(logbuf_total(&lb), 5);

    char out[32] = {0};
    uint64_t cur = 0, lost = 99;
    size_t n = logbuf_read(&lb, &cur, out, sizeof out, &lost);
    CHECK_EQ(n, 5);
    CHECK_EQ(lost, 0);
    CHECK_EQ(cur, 5);
    CHECK(memcmp(out, "hello", 5) == 0);

    /* plus rien a lire */
    n = logbuf_read(&lb, &cur, out, sizeof out, &lost);
    CHECK_EQ(n, 0);
    CHECK_EQ(lost, 0);
}

static void test_wrap(void)
{
    uint8_t st[8];
    logbuf_t lb;
    logbuf_init(&lb, st, sizeof st);
    logbuf_write(&lb, "abcde", 5);
    logbuf_write(&lb, "fgh", 3);
    logbuf_write(&lb, "ij", 2);        /* total 10, le tampon garde "cdefghij" */
    CHECK_EQ(logbuf_total(&lb), 10);

    char out[16] = {0};
    uint64_t cur = 0, lost = 0;
    size_t n = logbuf_read(&lb, &cur, out, sizeof out, &lost);
    CHECK_EQ(lost, 2);
    CHECK_EQ(n, 8);
    CHECK(memcmp(out, "cdefghij", 8) == 0);
    CHECK_EQ(cur, 10);
}

static void test_overflow_double(void)
{
    uint8_t st[16];
    logbuf_t lb;
    logbuf_init(&lb, st, sizeof st);
    logbuf_write(&lb, "hello", 5);
    logbuf_write(&lb, "0123456789abcdef0123456789abcdef", 32);   /* 2 x cap */
    char out[32] = {0};
    uint64_t cur = 0, lost = 0;
    size_t n = logbuf_read(&lb, &cur, out, sizeof out, &lost);
    CHECK_EQ(lost, 21);                 /* 37 ecrits, 16 conserves */
    CHECK_EQ(n, 16);
    CHECK(memcmp(out, "0123456789abcdef", 16) == 0);
}

static void test_block_larger_than_cap(void)
{
    uint8_t st[8];
    logbuf_t lb;
    logbuf_init(&lb, st, sizeof st);
    logbuf_write(&lb, "0123456789abcdefghij", 20);
    char out[8];
    uint64_t cur = 0, lost = 0;
    size_t n = logbuf_read(&lb, &cur, out, sizeof out, &lost);
    CHECK_EQ(lost, 12);
    CHECK_EQ(n, 8);
    CHECK(memcmp(out, "cdefghij", 8) == 0);
}

static void test_incremental_reads(void)
{
    uint8_t st[8];
    logbuf_t lb;
    logbuf_init(&lb, st, sizeof st);
    logbuf_write(&lb, "abcdefgh", 8);
    char out[3];
    uint64_t cur = 0, lost = 0;
    CHECK_EQ(logbuf_read(&lb, &cur, out, 3, &lost), 3);
    CHECK(memcmp(out, "abc", 3) == 0);
    logbuf_write(&lb, "ij", 2);        /* ecrase "ab", le curseur (3) reste valide */
    CHECK_EQ(logbuf_read(&lb, &cur, out, 3, &lost), 3);
    CHECK_EQ(lost, 0);
    CHECK(memcmp(out, "def", 3) == 0);
    CHECK_EQ(logbuf_read(&lb, &cur, out, 3, &lost), 3);
    CHECK(memcmp(out, "ghi", 3) == 0);
    CHECK_EQ(logbuf_read(&lb, &cur, out, 3, &lost), 1);
    CHECK(out[0] == 'j');
}

static void test_lost_pointer_optional(void)
{
    uint8_t st[4];
    logbuf_t lb;
    logbuf_init(&lb, st, sizeof st);
    logbuf_write(&lb, "abcdef", 6);
    char out[4];
    uint64_t cur = 0;
    CHECK_EQ(logbuf_read(&lb, &cur, out, 4, NULL), 4);
    CHECK(memcmp(out, "cdef", 4) == 0);
}

int main(void)
{
    RUN(test_write_read);
    RUN(test_wrap);
    RUN(test_overflow_double);
    RUN(test_block_larger_than_cap);
    RUN(test_incremental_reads);
    RUN(test_lost_pointer_optional);
    T_END();
}
```

- [ ] **Step 2 : vérifier l'échec**

```bash
mingw32-make -C test/host test_logbuf.exe
```
Attendu : `logbuf.h: No such file or directory`.

- [ ] **Step 3 : implémentation**

`components/netlog/include/logbuf.h` :
```c
#ifndef LOGBUF_H
#define LOGBUF_H
#include <stdint.h>
#include <stddef.h>

/* Tampon circulaire d'octets, logique pure (aucune dependance ESP-IDF).
 * L'appelant assure l'exclusion mutuelle entre write et read. */
typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t head;     /* index de la prochaine ecriture */
    size_t len;      /* octets valides (<= cap) */
    uint64_t total;  /* octets ecrits depuis init : curseur absolu */
} logbuf_t;

void logbuf_init(logbuf_t *lb, uint8_t *storage, size_t cap);

/* Ecrit n octets, ecrase les plus anciens si necessaire. */
void logbuf_write(logbuf_t *lb, const void *data, size_t n);

/* Copie au plus n octets a partir de la position absolue *cursor et avance *cursor.
 * Si *cursor est plus ancien que la fenetre disponible, il saute au plus ancien
 * et *lost (si non NULL) recoit le nombre d'octets sautes, sinon 0.
 * Renvoie le nombre d'octets copies. */
size_t logbuf_read(const logbuf_t *lb, uint64_t *cursor, void *out, size_t n, uint64_t *lost);

uint64_t logbuf_total(const logbuf_t *lb);

#endif
```

`components/netlog/logbuf.c` :
```c
#include "logbuf.h"
#include <string.h>

void logbuf_init(logbuf_t *lb, uint8_t *storage, size_t cap)
{
    lb->buf = storage;
    lb->cap = cap;
    lb->head = 0;
    lb->len = 0;
    lb->total = 0;
}

void logbuf_write(logbuf_t *lb, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    lb->total += n;

    if (n >= lb->cap) {
        /* seuls les cap derniers octets survivent */
        memcpy(lb->buf, p + (n - lb->cap), lb->cap);
        lb->head = 0;
        lb->len = lb->cap;
        return;
    }
    size_t first = lb->cap - lb->head;
    if (first > n) {
        first = n;
    }
    memcpy(lb->buf + lb->head, p, first);
    memcpy(lb->buf, p + first, n - first);
    lb->head = (lb->head + n) % lb->cap;
    lb->len = (lb->len + n > lb->cap) ? lb->cap : lb->len + n;
}

size_t logbuf_read(const logbuf_t *lb, uint64_t *cursor, void *out, size_t n, uint64_t *lost)
{
    uint64_t oldest = lb->total - lb->len;
    uint64_t skipped = 0;

    if (*cursor < oldest) {
        skipped = oldest - *cursor;
        *cursor = oldest;
    }
    if (*cursor > lb->total) {
        *cursor = lb->total;
    }
    if (lost) {
        *lost = skipped;
    }
    uint64_t avail = lb->total - *cursor;
    if (n > avail) {
        n = (size_t)avail;
    }
    if (n == 0) {
        return 0;
    }
    size_t oldest_idx = (lb->head + lb->cap - lb->len) % lb->cap;
    size_t start = (oldest_idx + (size_t)(*cursor - oldest)) % lb->cap;
    size_t first = lb->cap - start;
    if (first > n) {
        first = n;
    }
    memcpy(out, lb->buf + start, first);
    memcpy((uint8_t *)out + first, lb->buf, n - first);
    *cursor += n;
    return n;
}

uint64_t logbuf_total(const logbuf_t *lb)
{
    return lb->total;
}
```

- [ ] **Step 4 : vérifier le succès**

```bash
mingw32-make -C test/host test_logbuf.exe && ./test/host/test_logbuf.exe
```
Attendu : `0 failures`.

- [ ] **Step 5 : commit**

```bash
git add test/host/test_logbuf.c components/netlog
git commit -m "feat: logbuf, tampon circulaire pur avec curseur absolu

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 4 : `tc_proto`, parité et encodage

**Files:**
- Create: `components/tc_bus/include/tc_proto.h`, `test/host/test_tc_proto.c`
- Replace: `components/tc_bus/tc_proto.c`

- [ ] **Step 1 : en-tête complet (toutes les fonctions des tâches 4 à 6)**

`components/tc_bus/include/tc_proto.h` :
```c
#ifndef TC_PROTO_H
#define TC_PROTO_H
/* Protocole TCS/Koch TC:Bus : logique pure, aucune dependance ESP-IDF (spec 11.1). */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Durees d'emission (us) */
#define TC_START_CMD_TX_US   5985
#define TC_START_ACK_TX_US   6200
#define TC_BIT1_TX_US        4000
#define TC_BIT0_TX_US        2000

/* Fenetres de reception (us) */
#define TC_BIT0_MIN_US       1000
#define TC_BIT0_MAX_US       2999
#define TC_BIT1_MIN_US       3000
#define TC_BIT1_MAX_US       4999
#define TC_START_CMD_MIN_US  5700
#define TC_START_CMD_MAX_US  6089
#define TC_START_ACK_MIN_US  6090
#define TC_START_ACK_MAX_US  6900

#define TC_MAX_SEGMENTS      96
#define TC_SEGMENTS_ACK      7     /* START LEN b3..b0 CHK */
#define TC_SEGMENTS_SHORT    19    /* START LEN 16 bits CHK */
#define TC_SEGMENTS_LONG     35    /* START LEN 32 bits CHK */

typedef enum {
    TC_TYPE_UNKNOWN = 0,
    TC_TYPE_ACK,
    TC_TYPE_DOOR_CALL,
    TC_TYPE_INTERNAL_CALL,
    TC_TYPE_FLOOR_CALL,
    TC_TYPE_START_TALKING,
    TC_TYPE_STOP_TALKING,
    TC_TYPE_OPEN_DOOR,
    TC_TYPE_OPEN_DOOR_LONG,
    TC_TYPE_LIGHT,
    TC_TYPE_CONTROL_FUNCTION,
    TC_TYPE_DOOR_OPENED,
    TC_TYPE_DOOR_CLOSED,
    TC_TYPE_END_OF_RINGTONE,
    TC_TYPE_END_OF_DOOR_READINESS,
    TC_TYPE_INIT_DOOR_STATION,
    TC_TYPE_PROGRAMMING_MODE,
    TC_TYPE_READ_MEMORY_BLOCK,
    TC_TYPE_WRITE_MEMORY,
    TC_TYPE_SELECT_MEMORY_PAGE,
    TC_TYPE_RESET,
    TC_TYPE_SEARCH_DEVICES,
    TC_TYPE_REQUEST_VERSION,
    TC_TYPE_COUNT
} tc_type_t;

typedef struct {
    uint32_t  raw;
    bool      is_long;      /* 32 bits ; false = 16 bits ou ACK 4 bits */
    bool      is_response;  /* depart 6090..6900 us */
    tc_type_t type;
    uint32_t  serial;       /* 20 bits, 0 si absent */
    uint8_t   address;      /* 6 bits, 0 si absent */
    uint32_t  payload;
} tc_telegram_t;

/* Un segment = une duree a un niveau. mark = bus tire a 0 V (TX GPIO = 1, RX GPIO = 0). */
typedef struct {
    uint16_t duration_us;
    bool     mark;
} tc_segment_t;

typedef enum {
    TC_ERR_NONE = 0,
    TC_ERR_LENGTH,    /* nombre de segments != 7, 19, 35 */
    TC_ERR_START,     /* premier segment hors fenetres de depart */
    TC_ERR_LEN_BIT,   /* bit LEN incoherent avec le nombre de segments */
    TC_ERR_TIMING,    /* un segment hors fenetres bit 0 / bit 1 */
    TC_ERR_PARITY
} tc_decode_error_t;

/* CHK = 1 XOR b(nbits-1) XOR ... XOR b0 */
uint8_t tc_proto_parity(uint32_t raw, unsigned nbits);

/* Construit la sequence de segments de la spec 4.3. Renvoie le nombre de segments,
 * 0 si refuse (ACK > 0xF, court > 0xFFFF, is_ack && is_long, tampon trop petit). */
size_t tc_proto_encode(uint32_t raw, bool is_long, bool is_ack, tc_segment_t *out, size_t max);

/* Supprime les segments < glitch_us en les fondant dans leurs voisins. Renvoie le nouveau nombre. */
size_t tc_proto_merge_glitches(tc_segment_t *segs, size_t n, uint16_t glitch_us);

/* Supprime les segments de fin > TC_START_ACK_MAX_US (repos enregistre par le RMT). */
size_t tc_proto_strip_trailing_idle(tc_segment_t *segs, size_t n);

/* Decode une trame complete. true si valide ; *out rempli (raw, is_long, is_response), le reste a 0. */
bool tc_proto_decode(const tc_segment_t *segs, size_t n, tc_telegram_t *out, tc_decode_error_t *err);

/* Remplit type, serial, address, payload a partir de raw / is_long / is_response. */
void tc_proto_classify(tc_telegram_t *t);

/* Liste blanche d'emission : OPEN_DOOR, OPEN_DOOR_LONG, LIGHT, et rien d'autre. */
bool tc_proto_tx_allowed(const tc_telegram_t *t);

const char *tc_type_name(tc_type_t t);
const char *tc_decode_error_name(tc_decode_error_t e);

#endif
```

- [ ] **Step 2 : tests de parité et d'encodage (échouent)**

`test/host/test_tc_proto.c` (les tâches 5 et 6 ajoutent des fonctions dans ce fichier) :
```c
#include "t.h"
#include "tc_proto.h"
#include <string.h>

static size_t enc(uint32_t raw, bool is_long, bool is_ack, tc_segment_t *s)
{
    return tc_proto_encode(raw, is_long, is_ack, s, TC_MAX_SEGMENTS);
}

static void test_parity(void)
{
    CHECK_EQ(tc_proto_parity(0x0000, 16), 1);
    CHECK_EQ(tc_proto_parity(0x0001, 16), 0);
    CHECK_EQ(tc_proto_parity(0x1100, 16), 1);       /* 2 bits a 1 */
    CHECK_EQ(tc_proto_parity(0x1200, 16), 1);
    CHECK_EQ(tc_proto_parity(0x0C30BA80, 32), 1);   /* 10 bits a 1 */
    CHECK_EQ(tc_proto_parity(0x1, 4), 0);
    CHECK_EQ(tc_proto_parity(0xFFFF0001, 16), 0);   /* bits > nbits ignores */
}

static void test_encode_0x1100(void)
{
    tc_segment_t s[TC_MAX_SEGMENTS];
    size_t n = enc(0x1100, false, false, s);
    CHECK_EQ(n, 19);
    CHECK_EQ(s[0].duration_us, 5985);
    CHECK(s[0].mark);
    CHECK_EQ(s[1].duration_us, 2000);     /* LEN = 0 (court) */
    CHECK(!s[1].mark);
    /* 0x1100 = 0001 0001 0000 0000, MSB en premier */
    static const uint16_t bits[16] = { 2000, 2000, 2000, 4000, 2000, 2000, 2000, 4000,
                                       2000, 2000, 2000, 2000, 2000, 2000, 2000, 2000 };
    for (int i = 0; i < 16; i++) {
        CHECK_EQ(s[2 + i].duration_us, bits[i]);
    }
    CHECK_EQ(s[18].duration_us, 4000);    /* CHK = 1 */
    CHECK(s[18].mark);
    /* alternance stricte : index pair = mark, impair = space */
    for (size_t k = 0; k < n; k++) {
        CHECK(s[k].mark == (k % 2 == 0));
    }
}

static void test_encode_long_and_ack(void)
{
    tc_segment_t s[TC_MAX_SEGMENTS];
    size_t n = enc(0x0C30BA80, true, false, s);
    CHECK_EQ(n, 35);
    CHECK_EQ(s[1].duration_us, 4000);     /* LEN = 1 (long) */
    CHECK_EQ(s[2].duration_us, 2000);     /* bit 31 = 0 */
    CHECK_EQ(s[34].duration_us, 4000);    /* CHK = 1 */
    for (size_t k = 0; k < n; k++) {
        CHECK(s[k].mark == (k % 2 == 0));
    }

    n = enc(0x1, false, true, s);
    CHECK_EQ(n, 7);
    CHECK_EQ(s[0].duration_us, 6200);
    CHECK_EQ(s[1].duration_us, 2000);
    CHECK_EQ(s[5].duration_us, 4000);     /* b0 = 1 */
    CHECK_EQ(s[6].duration_us, 2000);     /* CHK = 1 XOR 1 = 0 */
    for (size_t k = 0; k < n; k++) {
        CHECK(s[k].mark == (k % 2 == 0));
    }
}

static void test_encode_refus(void)
{
    tc_segment_t s[TC_MAX_SEGMENTS];
    CHECK_EQ(enc(0x12345, false, false, s), 0);     /* court > 0xFFFF */
    CHECK_EQ(enc(0x10, false, true, s), 0);         /* ACK > 0xF */
    CHECK_EQ(enc(0x1, true, true, s), 0);           /* ACK long n'existe pas */
    CHECK_EQ(tc_proto_encode(0x1100, false, false, s, 18), 0);  /* tampon trop petit */
}

int main(void)
{
    RUN(test_parity);
    RUN(test_encode_0x1100);
    RUN(test_encode_long_and_ack);
    RUN(test_encode_refus);
    T_END();
}
```

- [ ] **Step 3 : vérifier l'échec**

```bash
mingw32-make -C test/host test_tc_proto.exe
```
Attendu : erreurs `undefined reference to tc_proto_parity` (et autres).

- [ ] **Step 4 : implémentation (parité, encodage, noms)**

`components/tc_bus/tc_proto.c` :
```c
#include "tc_proto.h"
#include <string.h>

uint8_t tc_proto_parity(uint32_t raw, unsigned nbits)
{
    uint8_t p = 1;
    for (unsigned i = 0; i < nbits; i++) {
        p ^= (uint8_t)((raw >> i) & 1u);
    }
    return p;
}

static unsigned data_bits(bool is_long, bool is_ack)
{
    return is_ack ? 4u : (is_long ? 32u : 16u);
}

size_t tc_proto_encode(uint32_t raw, bool is_long, bool is_ack, tc_segment_t *out, size_t max)
{
    if (out == NULL || (is_ack && is_long)) {
        return 0;
    }
    if (is_ack && raw > 0xF) {
        return 0;
    }
    if (!is_long && !is_ack && raw > 0xFFFF) {
        return 0;
    }
    unsigned n = data_bits(is_long, is_ack);
    size_t total = 3 + n;
    if (max < total) {
        return 0;
    }
    size_t k = 0;
    out[k].duration_us = is_ack ? TC_START_ACK_TX_US : TC_START_CMD_TX_US;
    out[k].mark = true;
    k++;
    out[k].duration_us = is_long ? TC_BIT1_TX_US : TC_BIT0_TX_US;
    out[k].mark = false;
    k++;
    for (int i = (int)n - 1; i >= 0; i--) {
        bool bit = ((raw >> i) & 1u) != 0;
        out[k].duration_us = bit ? TC_BIT1_TX_US : TC_BIT0_TX_US;
        out[k].mark = (i & 1) != 0;       /* i impair = mark, pair = space */
        k++;
    }
    out[k].duration_us = tc_proto_parity(raw, n) ? TC_BIT1_TX_US : TC_BIT0_TX_US;
    out[k].mark = true;
    k++;
    return k;
}

static const char *const s_type_names[TC_TYPE_COUNT] = {
    [TC_TYPE_UNKNOWN] = "UNKNOWN",
    [TC_TYPE_ACK] = "ACK",
    [TC_TYPE_DOOR_CALL] = "DOOR_CALL",
    [TC_TYPE_INTERNAL_CALL] = "INTERNAL_CALL",
    [TC_TYPE_FLOOR_CALL] = "FLOOR_CALL",
    [TC_TYPE_START_TALKING] = "START_TALKING",
    [TC_TYPE_STOP_TALKING] = "STOP_TALKING",
    [TC_TYPE_OPEN_DOOR] = "OPEN_DOOR",
    [TC_TYPE_OPEN_DOOR_LONG] = "OPEN_DOOR_LONG",
    [TC_TYPE_LIGHT] = "LIGHT",
    [TC_TYPE_CONTROL_FUNCTION] = "CONTROL_FUNCTION",
    [TC_TYPE_DOOR_OPENED] = "DOOR_OPENED",
    [TC_TYPE_DOOR_CLOSED] = "DOOR_CLOSED",
    [TC_TYPE_END_OF_RINGTONE] = "END_OF_RINGTONE",
    [TC_TYPE_END_OF_DOOR_READINESS] = "END_OF_DOOR_READINESS",
    [TC_TYPE_INIT_DOOR_STATION] = "INIT_DOOR_STATION",
    [TC_TYPE_PROGRAMMING_MODE] = "PROGRAMMING_MODE",
    [TC_TYPE_READ_MEMORY_BLOCK] = "READ_MEMORY_BLOCK",
    [TC_TYPE_WRITE_MEMORY] = "WRITE_MEMORY",
    [TC_TYPE_SELECT_MEMORY_PAGE] = "SELECT_MEMORY_PAGE",
    [TC_TYPE_RESET] = "RESET",
    [TC_TYPE_SEARCH_DEVICES] = "SEARCH_DEVICES",
    [TC_TYPE_REQUEST_VERSION] = "REQUEST_VERSION",
};

const char *tc_type_name(tc_type_t t)
{
    if ((unsigned)t >= TC_TYPE_COUNT || s_type_names[t] == NULL) {
        return "?";
    }
    return s_type_names[t];
}

const char *tc_decode_error_name(tc_decode_error_t e)
{
    switch (e) {
    case TC_ERR_NONE:    return "none";
    case TC_ERR_LENGTH:  return "length";
    case TC_ERR_START:   return "start";
    case TC_ERR_LEN_BIT: return "len_bit";
    case TC_ERR_TIMING:  return "timing";
    case TC_ERR_PARITY:  return "parity";
    default:             return "?";
    }
}

/* --- decodage, parasites, classification : taches 5 et 6 --- */
```

Pour que l'édition de liens passe dès maintenant, ajouter en fin de fichier des **stubs provisoires**
(remplacés à la tâche 5 et 6) :
```c
size_t tc_proto_merge_glitches(tc_segment_t *segs, size_t n, uint16_t glitch_us) { (void)segs; (void)glitch_us; return n; }
size_t tc_proto_strip_trailing_idle(tc_segment_t *segs, size_t n) { (void)segs; return n; }
bool tc_proto_decode(const tc_segment_t *segs, size_t n, tc_telegram_t *out, tc_decode_error_t *err) { (void)segs; (void)n; (void)out; if (err) *err = TC_ERR_LENGTH; return false; }
void tc_proto_classify(tc_telegram_t *t) { (void)t; }
bool tc_proto_tx_allowed(const tc_telegram_t *t) { (void)t; return false; }
```

- [ ] **Step 5 : vérifier le succès**

```bash
mingw32-make -C test/host test_tc_proto.exe && ./test/host/test_tc_proto.exe
```
Attendu : `0 failures`.

- [ ] **Step 6 : commit**

```bash
git add components/tc_bus test/host/test_tc_proto.c
git commit -m "feat: tc_proto, parite et encodage des telegrammes

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 5 : `tc_proto`, décodage, parasites, repos de fin

**Files:**
- Modify: `components/tc_bus/tc_proto.c` (remplacer les stubs de decode, merge, strip)
- Modify: `test/host/test_tc_proto.c`

- [ ] **Step 1 : tests (échouent)**

Ajouter dans `test/host/test_tc_proto.c`, avant `main` :
```c
static bool dec(const tc_segment_t *s, size_t n, tc_telegram_t *t, tc_decode_error_t *e)
{
    return tc_proto_decode(s, n, t, e);
}

static void test_decode_roundtrip(void)
{
    struct { uint32_t raw; bool is_long; bool is_ack; } v[] = {
        { 0x1100, false, false }, { 0x1200, false, false }, { 0x0C30BA80, true, false },
        { 0x1C30BA41, true, false }, { 0x1C30BA80, true, false }, { 0x1, false, true },
        { 0xFFFFFFFF, true, false }, { 0x0000, false, false },
    };
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        tc_segment_t s[TC_MAX_SEGMENTS];
        size_t n = enc(v[i].raw, v[i].is_long, v[i].is_ack, s);
        tc_telegram_t t;
        tc_decode_error_t e;
        CHECK(dec(s, n, &t, &e));
        CHECK_EQ(e, TC_ERR_NONE);
        CHECK_EQ(t.raw, v[i].raw);
        CHECK(t.is_long == v[i].is_long);
        CHECK(t.is_response == v[i].is_ack);
    }
}

static void test_decode_tolerance(void)
{
    tc_segment_t s[TC_MAX_SEGMENTS];
    size_t n = enc(0x1100, false, false, s);
    tc_telegram_t t;
    tc_decode_error_t e;
    const int scale[4] = { 80, 120, 90, 110 };     /* pourcentages */
    for (int k = 0; k < 4; k++) {
        tc_segment_t m[TC_MAX_SEGMENTS];
        memcpy(m, s, sizeof m);
        for (size_t i = 1; i < n; i++) {           /* LEN, donnees, CHK */
            m[i].duration_us = (uint16_t)(m[i].duration_us * scale[k] / 100);
        }
        CHECK(dec(m, n, &t, &e));
        CHECK_EQ(t.raw, 0x1100);
    }
    /* depart commande a +-100 us */
    s[0].duration_us = 5885; CHECK(dec(s, n, &t, &e));
    s[0].duration_us = 6085; CHECK(dec(s, n, &t, &e));
    /* depart ACK a +-100 us */
    n = enc(0x1, false, true, s);
    s[0].duration_us = 6100; CHECK(dec(s, n, &t, &e)); CHECK(t.is_response);
    s[0].duration_us = 6300; CHECK(dec(s, n, &t, &e)); CHECK(t.is_response);
}

static void test_decode_rejects(void)
{
    tc_segment_t s[TC_MAX_SEGMENTS];
    tc_telegram_t t;
    tc_decode_error_t e;
    memset(&t, 0, sizeof t);
    size_t n = enc(0x1100, false, false, s);

    tc_segment_t m[TC_MAX_SEGMENTS];
    memcpy(m, s, sizeof m);
    m[5].duration_us = 5500;                       /* bit hors fenetres */
    CHECK(!dec(m, n, &t, &e)); CHECK_EQ(e, TC_ERR_TIMING);

    memcpy(m, s, sizeof m);
    m[0].duration_us = 5000;                       /* depart hors fenetres */
    CHECK(!dec(m, n, &t, &e)); CHECK_EQ(e, TC_ERR_START);

    memcpy(m, s, sizeof m);
    m[18].duration_us = 2000;                      /* CHK inverse */
    CHECK(!dec(m, n, &t, &e)); CHECK_EQ(e, TC_ERR_PARITY);

    memcpy(m, s, sizeof m);
    m[1].duration_us = 4000;                       /* LEN = 1 sur 19 segments */
    CHECK(!dec(m, n, &t, &e)); CHECK_EQ(e, TC_ERR_LEN_BIT);

    memcpy(m, s, sizeof m);
    m[19].duration_us = 2000; m[19].mark = false;  /* 20 segments */
    CHECK(!dec(m, 20, &t, &e)); CHECK_EQ(e, TC_ERR_LENGTH);

    CHECK(!dec(m, 0, &t, &e)); CHECK_EQ(e, TC_ERR_LENGTH);
}

static void test_merge_glitches(void)
{
    tc_segment_t s[TC_MAX_SEGMENTS];
    size_t n = enc(0x1100, false, false, s);
    tc_telegram_t t;
    tc_decode_error_t e;

    /* parasite au milieu du segment 5 (bit 12 = 1, 4000 space) : 1800 space, 300 mark, 1900 space.
       Les deux morceaux restent >= 1000 : seul le 300 est un parasite. */
    tc_segment_t m[TC_MAX_SEGMENTS];
    memset(&t, 0, sizeof t);
    memcpy(m, s, 5 * sizeof m[0]);
    m[5] = (tc_segment_t){ 1800, false };
    m[6] = (tc_segment_t){ 300, true };
    m[7] = (tc_segment_t){ 1900, false };
    memcpy(&m[8], &s[6], (n - 6) * sizeof m[0]);
    size_t k = tc_proto_merge_glitches(m, n + 2, 1000);
    CHECK_EQ(k, 19);
    CHECK_EQ(m[5].duration_us, 4000);
    CHECK(dec(m, k, &t, &e));
    CHECK_EQ(t.raw, 0x1100);

    /* parasite en tete */
    m[0] = (tc_segment_t){ 300, false };
    memcpy(&m[1], s, n * sizeof m[0]);
    k = tc_proto_merge_glitches(m, n + 1, 1000);
    CHECK_EQ(k, 19);
    CHECK(dec(m, k, &t, &e));

    /* parasite en queue */
    memcpy(m, s, n * sizeof m[0]);
    m[n] = (tc_segment_t){ 200, false };
    k = tc_proto_merge_glitches(m, n + 1, 1000);
    CHECK_EQ(k, 19);

    /* deux parasites consecutifs : 1500 space, 200 mark, 300 space, 2500 mark -> 4500 ? non :
       1500+200+300 = 2000 space, puis 2500 mark reste distinct */
    m[0] = (tc_segment_t){ 1500, false };
    m[1] = (tc_segment_t){ 200, true };
    m[2] = (tc_segment_t){ 300, false };
    m[3] = (tc_segment_t){ 2500, true };
    k = tc_proto_merge_glitches(m, 4, 1000);
    CHECK_EQ(k, 2);
    CHECK_EQ(m[0].duration_us, 2000);
    CHECK_EQ(m[1].duration_us, 2500);

    /* aucun parasite : inchange */
    memcpy(m, s, n * sizeof m[0]);
    CHECK_EQ(tc_proto_merge_glitches(m, n, 1000), n);
}

static void test_strip_trailing_idle(void)
{
    tc_segment_t s[TC_MAX_SEGMENTS];
    size_t n = enc(0x1100, false, false, s);
    s[n] = (tc_segment_t){ 7000, false };
    CHECK_EQ(tc_proto_strip_trailing_idle(s, n + 1), 19);
    s[n] = (tc_segment_t){ 32767, false };
    s[n + 1] = (tc_segment_t){ 32767, true };
    CHECK_EQ(tc_proto_strip_trailing_idle(s, n + 2), 19);
    CHECK_EQ(tc_proto_strip_trailing_idle(s, n), 19);
    CHECK_EQ(tc_proto_strip_trailing_idle(s, 0), 0);
}
```
Et dans `main` : `RUN(test_decode_roundtrip); RUN(test_decode_tolerance); RUN(test_decode_rejects); RUN(test_merge_glitches); RUN(test_strip_trailing_idle);`

Note sur le cas « deux parasites consécutifs » : avec la fusion séquentielle, le 200 est fondu dans
ses voisins (1500 + 200 + 300 = 2000), ce qui laisse 2000 space suivi de 2500 mark. C'est le
comportement attendu.

- [ ] **Step 2 : vérifier l'échec**

```bash
mingw32-make -C test/host test_tc_proto.exe && ./test/host/test_tc_proto.exe
```
Attendu : des `FAIL` (les stubs renvoient faux).

- [ ] **Step 3 : implémentation**

Remplacer les stubs de `tc_proto_merge_glitches`, `tc_proto_strip_trailing_idle` et
`tc_proto_decode` dans `components/tc_bus/tc_proto.c` par :
```c
size_t tc_proto_merge_glitches(tc_segment_t *segs, size_t n, uint16_t glitch_us)
{
    bool changed = true;
    while (changed && n > 0) {
        changed = false;
        for (size_t i = 0; i < n; i++) {
            if (segs[i].duration_us >= glitch_us) {
                continue;
            }
            if (i > 0 && i + 1 < n) {
                /* fondre i-1, i, i+1 dans i-1 (les voisins ont le meme niveau) */
                uint32_t sum = (uint32_t)segs[i - 1].duration_us + segs[i].duration_us + segs[i + 1].duration_us;
                segs[i - 1].duration_us = (sum > 0xFFFF) ? 0xFFFF : (uint16_t)sum;
                memmove(&segs[i], &segs[i + 2], (n - i - 2) * sizeof(*segs));
                n -= 2;
            } else {
                /* en tete ou en queue : simple suppression */
                memmove(&segs[i], &segs[i + 1], (n - i - 1) * sizeof(*segs));
                n -= 1;
            }
            changed = true;
            break;
        }
    }
    return n;
}

size_t tc_proto_strip_trailing_idle(tc_segment_t *segs, size_t n)
{
    while (n > 0 && segs[n - 1].duration_us > TC_START_ACK_MAX_US) {
        n--;
    }
    return n;
}

static int classify_bit(uint16_t d)
{
    if (d >= TC_BIT0_MIN_US && d <= TC_BIT0_MAX_US) {
        return 0;
    }
    if (d >= TC_BIT1_MIN_US && d <= TC_BIT1_MAX_US) {
        return 1;
    }
    return -1;
}

bool tc_proto_decode(const tc_segment_t *segs, size_t n, tc_telegram_t *out, tc_decode_error_t *err)
{
    tc_decode_error_t e = TC_ERR_NONE;
    tc_telegram_t t;
    unsigned nbits = 0;
    uint32_t raw = 0;
    memset(&t, 0, sizeof t);

    if (n == TC_SEGMENTS_ACK) {
        nbits = 4;
    } else if (n == TC_SEGMENTS_SHORT) {
        nbits = 16;
    } else if (n == TC_SEGMENTS_LONG) {
        nbits = 32;
    } else {
        e = TC_ERR_LENGTH;
        goto done;
    }

    {
        uint16_t s0 = segs[0].duration_us;
        if (s0 >= TC_START_CMD_MIN_US && s0 <= TC_START_CMD_MAX_US) {
            t.is_response = false;
        } else if (s0 >= TC_START_ACK_MIN_US && s0 <= TC_START_ACK_MAX_US) {
            t.is_response = true;
        } else {
            e = TC_ERR_START;
            goto done;
        }
    }

    {
        int len = classify_bit(segs[1].duration_us);
        if (len < 0) {
            e = TC_ERR_TIMING;
            goto done;
        }
        if ((nbits == 32) != (len == 1)) {
            e = TC_ERR_LEN_BIT;
            goto done;
        }
        t.is_long = (nbits == 32);
    }

    for (unsigned i = 0; i < nbits; i++) {
        int b = classify_bit(segs[2 + i].duration_us);
        if (b < 0) {
            e = TC_ERR_TIMING;
            goto done;
        }
        raw = (raw << 1) | (uint32_t)b;
    }

    {
        int chk = classify_bit(segs[n - 1].duration_us);
        if (chk < 0) {
            e = TC_ERR_TIMING;
            goto done;
        }
        if ((uint8_t)chk != tc_proto_parity(raw, nbits)) {
            e = TC_ERR_PARITY;
            goto done;
        }
    }
    t.raw = raw;

done:
    if (err) {
        *err = e;
    }
    if (e != TC_ERR_NONE) {
        return false;
    }
    if (out) {
        *out = t;
    }
    return true;
}
```

- [ ] **Step 4 : vérifier le succès**

```bash
mingw32-make -C test/host test_tc_proto.exe && ./test/host/test_tc_proto.exe
```
Attendu : `0 failures`.

- [ ] **Step 5 : commit**

```bash
git add components/tc_bus/tc_proto.c test/host/test_tc_proto.c
git commit -m "feat: tc_proto, decodage robuste, fusion des parasites, repos de fin

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 6 : `tc_proto`, classification et liste blanche

**Files:**
- Modify: `components/tc_bus/tc_proto.c` (remplacer les stubs de classify et tx_allowed)
- Modify: `test/host/test_tc_proto.c`

- [ ] **Step 1 : tests (échouent)**

Ajouter avant `main` :
```c
static tc_telegram_t cls(uint32_t raw, bool is_long, bool is_response)
{
    tc_telegram_t t;
    memset(&t, 0, sizeof t);
    t.raw = raw;
    t.is_long = is_long;
    t.is_response = is_response;
    tc_proto_classify(&t);
    return t;
}

static void test_classify_long(void)
{
    tc_telegram_t t = cls(0x0C30BA80, true, false);
    CHECK_EQ(t.type, TC_TYPE_DOOR_CALL); CHECK_EQ(t.serial, 0xC30BA); CHECK_EQ(t.address, 0);
    t = cls(0x0C30BA43, true, false);
    CHECK_EQ(t.type, TC_TYPE_INTERNAL_CALL); CHECK_EQ(t.serial, 0xC30BA); CHECK_EQ(t.address, 3);
    t = cls(0x1C30BA41, true, false);
    CHECK_EQ(t.type, TC_TYPE_FLOOR_CALL); CHECK_EQ(t.serial, 0xC30BA);
    t = cls(0x1C30BA80, true, false);
    CHECK_EQ(t.type, TC_TYPE_OPEN_DOOR_LONG); CHECK_EQ(t.serial, 0xC30BA); CHECK_EQ(t.address, 0); CHECK_EQ(t.payload, 0);
    t = cls(0x1C30BAC2, true, false);
    CHECK_EQ(t.type, TC_TYPE_OPEN_DOOR_LONG); CHECK_EQ(t.address, 2); CHECK_EQ(t.payload, 1);
    t = cls(0x3C30BA80, true, false);
    CHECK_EQ(t.type, TC_TYPE_START_TALKING); CHECK_EQ(t.serial, 0xC30BA);
    t = cls(0x5C30BAC0, true, false);
    CHECK_EQ(t.type, TC_TYPE_REQUEST_VERSION); CHECK_EQ(t.serial, 0xC30BA);
    t = cls(0x6C30BA08, true, false);
    CHECK_EQ(t.type, TC_TYPE_CONTROL_FUNCTION); CHECK_EQ(t.serial, 0xC30BA); CHECK_EQ(t.payload, 8);
    t = cls(0x82140A0B, true, false);
    CHECK_EQ(t.type, TC_TYPE_WRITE_MEMORY); CHECK_EQ(t.payload, 0x140A0B);
    t = cls(0x810C30BA, true, false);
    CHECK_EQ(t.type, TC_TYPE_SELECT_MEMORY_PAGE); CHECK_EQ(t.serial, 0xC30BA);
    t = cls(0x890C30BA, true, false);
    CHECK_EQ(t.type, TC_TYPE_SELECT_MEMORY_PAGE);
    t = cls(0x9ABCDEF0, true, false);
    CHECK_EQ(t.type, TC_TYPE_UNKNOWN); CHECK_EQ(t.serial, 0); CHECK_EQ(t.payload, 0x9ABCDEF0);
    t = cls(0x1C30BA00, true, false);           /* type 1, ni 0x41 ni bit 7 */
    CHECK_EQ(t.type, TC_TYPE_UNKNOWN);
}

static void test_classify_short(void)
{
    tc_telegram_t t = cls(0x1100, false, false);
    CHECK_EQ(t.type, TC_TYPE_OPEN_DOOR); CHECK_EQ(t.address, 0); CHECK_EQ(t.payload, 0);
    t = cls(0x1181, false, false);
    CHECK_EQ(t.type, TC_TYPE_OPEN_DOOR); CHECK_EQ(t.address, 1); CHECK_EQ(t.payload, 1);
    t = cls(0x1200, false, false);
    CHECK_EQ(t.type, TC_TYPE_LIGHT);
    t = cls(0x3040, false, false);
    CHECK_EQ(t.type, TC_TYPE_STOP_TALKING); CHECK_EQ(t.address, 0); CHECK_EQ(t.payload, 1);
    t = cls(0x3005, false, false);
    CHECK_EQ(t.type, TC_TYPE_STOP_TALKING); CHECK_EQ(t.address, 5); CHECK_EQ(t.payload, 0);
    t = cls(0x2180, false, false);
    CHECK_EQ(t.type, TC_TYPE_DOOR_CLOSED);
    t = cls(0x2100, false, false);
    CHECK_EQ(t.type, TC_TYPE_DOOR_OPENED);
    CHECK_EQ(cls(0x2200, false, false).type, TC_TYPE_END_OF_RINGTONE);
    CHECK_EQ(cls(0x2400, false, false).type, TC_TYPE_END_OF_DOOR_READINESS);
    CHECK_EQ(cls(0x2800, false, false).type, TC_TYPE_INIT_DOOR_STATION);
    t = cls(0x5041, false, false);
    CHECK_EQ(t.type, TC_TYPE_PROGRAMMING_MODE); CHECK_EQ(t.payload, 1);
    CHECK_EQ(cls(0x5100, false, false).type, TC_TYPE_RESET);
    CHECK_EQ(cls(0x5200, false, false).type, TC_TYPE_SEARCH_DEVICES);
    t = cls(0x840C, false, false);
    CHECK_EQ(t.type, TC_TYPE_READ_MEMORY_BLOCK); CHECK_EQ(t.payload, 0x0C);
    t = cls(0x7FFF, false, false);
    CHECK_EQ(t.type, TC_TYPE_UNKNOWN); CHECK_EQ(t.payload, 0x7FFF);
    /* ACK : seulement avec un depart reponse */
    t = cls(0x5, false, true);
    CHECK_EQ(t.type, TC_TYPE_ACK); CHECK_EQ(t.payload, 5);
    t = cls(0x5, false, false);
    CHECK_EQ(t.type, TC_TYPE_UNKNOWN);
    t = cls(0x1100, false, true);               /* reponse > 0xF : pas un ACK */
    CHECK_EQ(t.type, TC_TYPE_OPEN_DOOR);
}

static void test_whitelist(void)
{
    CHECK(tc_proto_tx_allowed(&(tc_telegram_t){ .raw = 0x1100, .type = TC_TYPE_OPEN_DOOR }));
    CHECK(tc_proto_tx_allowed(&(tc_telegram_t){ .raw = 0x1181, .type = TC_TYPE_OPEN_DOOR }));
    CHECK(tc_proto_tx_allowed(&(tc_telegram_t){ .raw = 0x1C30BA80, .is_long = true, .type = TC_TYPE_OPEN_DOOR_LONG }));
    CHECK(tc_proto_tx_allowed(&(tc_telegram_t){ .raw = 0x1200, .type = TC_TYPE_LIGHT }));

    CHECK(!tc_proto_tx_allowed(&(tc_telegram_t){ .raw = 0x5041, .type = TC_TYPE_PROGRAMMING_MODE }));
    CHECK(!tc_proto_tx_allowed(&(tc_telegram_t){ .raw = 0x82140A0B, .is_long = true, .type = TC_TYPE_WRITE_MEMORY }));
    CHECK(!tc_proto_tx_allowed(&(tc_telegram_t){ .raw = 0x840C, .type = TC_TYPE_READ_MEMORY_BLOCK }));
    CHECK(!tc_proto_tx_allowed(&(tc_telegram_t){ .raw = 0x810C30BA, .is_long = true, .type = TC_TYPE_SELECT_MEMORY_PAGE }));
    CHECK(!tc_proto_tx_allowed(&(tc_telegram_t){ .raw = 0x5100, .type = TC_TYPE_RESET }));
    CHECK(!tc_proto_tx_allowed(&(tc_telegram_t){ .raw = 0x2800, .type = TC_TYPE_INIT_DOOR_STATION }));
    CHECK(!tc_proto_tx_allowed(&(tc_telegram_t){ .raw = 0x5200, .type = TC_TYPE_SEARCH_DEVICES }));
    CHECK(!tc_proto_tx_allowed(&(tc_telegram_t){ .raw = 0x7FFF, .type = TC_TYPE_UNKNOWN }));
    CHECK(!tc_proto_tx_allowed(&(tc_telegram_t){ .raw = 0x0C30BA80, .is_long = true, .type = TC_TYPE_DOOR_CALL }));
    CHECK(!tc_proto_tx_allowed(&(tc_telegram_t){ .raw = 0x1, .is_response = true, .type = TC_TYPE_ACK }));
    /* type menteur : raw dit PROGRAMMING_MODE, type dit OPEN_DOOR -> refuse */
    CHECK(!tc_proto_tx_allowed(&(tc_telegram_t){ .raw = 0x5041, .type = TC_TYPE_OPEN_DOOR }));
    /* is_long incoherent avec le type */
    CHECK(!tc_proto_tx_allowed(&(tc_telegram_t){ .raw = 0x1100, .is_long = true, .type = TC_TYPE_OPEN_DOOR }));
}

static void test_names(void)
{
    CHECK(strcmp(tc_type_name(TC_TYPE_OPEN_DOOR), "OPEN_DOOR") == 0);
    CHECK(strcmp(tc_type_name(TC_TYPE_UNKNOWN), "UNKNOWN") == 0);
    CHECK(strcmp(tc_type_name((tc_type_t)999), "?") == 0);
    CHECK(strcmp(tc_decode_error_name(TC_ERR_PARITY), "parity") == 0);
}
```
Et dans `main` : `RUN(test_classify_long); RUN(test_classify_short); RUN(test_whitelist); RUN(test_names);`

- [ ] **Step 2 : vérifier l'échec**

```bash
mingw32-make -C test/host test_tc_proto.exe && ./test/host/test_tc_proto.exe
```
Attendu : des `FAIL` dans `test_classify_*` et `test_whitelist`.

- [ ] **Step 3 : implémentation**

Remplacer les stubs `tc_proto_classify` et `tc_proto_tx_allowed` par :
```c
void tc_proto_classify(tc_telegram_t *t)
{
    uint32_t raw = t->raw;
    t->type = TC_TYPE_UNKNOWN;
    t->serial = 0;
    t->address = 0;
    t->payload = raw;

    if (t->is_response && !t->is_long && raw <= 0xF) {
        t->type = TC_TYPE_ACK;
        return;
    }

    if (t->is_long) {
        uint8_t hi = (raw >> 28) & 0xF;
        uint8_t top = (raw >> 24) & 0xFF;
        uint8_t low = raw & 0xFF;

        if (top == 0x82) {
            t->type = TC_TYPE_WRITE_MEMORY;
            t->payload = raw & 0xFFFFFF;
            return;
        }
        if (top == 0x81 || top == 0x89) {
            t->type = TC_TYPE_SELECT_MEMORY_PAGE;
            t->serial = raw & 0xFFFFF;
            t->payload = (raw >> 20) & 0xF;
            return;
        }
        uint32_t serial = (raw >> 8) & 0xFFFFF;
        uint8_t address = raw & 0x3F;
        switch (hi) {
        case 0x0:
            t->type = (raw & 0x40) ? TC_TYPE_INTERNAL_CALL : TC_TYPE_DOOR_CALL;
            t->serial = serial;
            t->address = address;
            t->payload = (raw >> 7) & 1;
            return;
        case 0x1:
            if (low == 0x41) {
                t->type = TC_TYPE_FLOOR_CALL;
                t->serial = serial;
                t->payload = 0;
                return;
            }
            if (raw & 0x80) {
                t->type = TC_TYPE_OPEN_DOOR_LONG;
                t->serial = serial;
                t->address = address;
                t->payload = (raw >> 6) & 1;
                return;
            }
            break;
        case 0x3:
            t->type = TC_TYPE_START_TALKING;
            t->serial = serial;
            t->address = address;
            t->payload = (raw >> 6) & 0x3;
            return;
        case 0x5:
            if (low == 0xC0) {
                t->type = TC_TYPE_REQUEST_VERSION;
                t->serial = serial;
                t->payload = 0;
                return;
            }
            break;
        case 0x6:
            t->type = TC_TYPE_CONTROL_FUNCTION;
            t->serial = serial;
            t->payload = low;
            return;
        default:
            break;
        }
        return;   /* UNKNOWN, payload = raw */
    }

    /* trame courte (16 bits) */
    uint8_t hi8 = (raw >> 8) & 0xFF;
    switch (hi8) {
    case 0x11:
        t->type = TC_TYPE_OPEN_DOOR;
        t->address = raw & 0x3F;
        t->payload = (raw >> 7) & 1;
        return;
    case 0x12:
        t->type = TC_TYPE_LIGHT;
        t->payload = 0;
        return;
    case 0x21:
        t->type = (raw & 0x80) ? TC_TYPE_DOOR_CLOSED : TC_TYPE_DOOR_OPENED;
        t->address = raw & 0x3F;
        t->payload = 0;
        return;
    case 0x22:
        t->type = TC_TYPE_END_OF_RINGTONE;
        t->address = raw & 0x3F;
        t->payload = 0;
        return;
    case 0x24:
        t->type = TC_TYPE_END_OF_DOOR_READINESS;
        t->address = raw & 0x3F;
        t->payload = 0;
        return;
    case 0x28:
        t->type = TC_TYPE_INIT_DOOR_STATION;
        t->address = raw & 0x3F;
        t->payload = 0;
        return;
    case 0x50:
        t->type = TC_TYPE_PROGRAMMING_MODE;
        t->payload = raw & 1;
        return;
    case 0x51:
        t->type = TC_TYPE_RESET;
        t->payload = 0;
        return;
    case 0x52:
        t->type = TC_TYPE_SEARCH_DEVICES;
        t->payload = 0;
        return;
    case 0x84:
        t->type = TC_TYPE_READ_MEMORY_BLOCK;
        t->payload = raw & 0xFF;
        return;
    default:
        break;
    }
    if (((raw >> 12) & 0xF) == 0x3) {
        t->type = TC_TYPE_STOP_TALKING;
        t->address = raw & 0x3F;
        t->payload = (raw >> 6) & 1;
        return;
    }
    /* UNKNOWN, payload = raw */
}

bool tc_proto_tx_allowed(const tc_telegram_t *t)
{
    if (t == NULL || t->is_response) {
        return false;
    }
    /* On ne fait pas confiance au champ type : on reclasse le raw. */
    tc_telegram_t c = *t;
    tc_proto_classify(&c);
    if (c.type != t->type) {
        return false;
    }
    switch (c.type) {
    case TC_TYPE_OPEN_DOOR:
    case TC_TYPE_LIGHT:
        return !c.is_long;
    case TC_TYPE_OPEN_DOOR_LONG:
        return c.is_long;
    default:
        return false;
    }
}
```
Supprimer aussi le commentaire « stubs provisoires » et le stub restant s'il en reste.

- [ ] **Step 4 : vérifier le succès et l'ensemble des tests PC**

```bash
mingw32-make -C test/host test
```
Attendu : trois lignes `... 0 failures`, pas d'erreur.

- [ ] **Step 5 : vérifier que le firmware compile toujours**

PowerShell : `idf.py build` → `Project build complete.`

- [ ] **Step 6 : commit**

```bash
git add components/tc_bus/tc_proto.c test/host/test_tc_proto.c
git commit -m "feat: tc_proto, classification des telegrammes et liste blanche d'emission

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 7 : `netlog`, hook de log et serveur TCP

**Files:**
- Create: `components/netlog/include/netlog.h`, `components/netlog/netlog.c`
- Modify: `components/netlog/CMakeLists.txt`, `main/CMakeLists.txt`, `main/main.c`

Pas de test PC (code ESP-IDF). Critère : build OK, puis vérification sur l'établi à la tâche 15.

- [ ] **Step 1 : en-tête**

`components/netlog/include/netlog.h` :
```c
#ifndef NETLOG_H
#define NETLOG_H
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Alloue le tampon circulaire et redirige esp_log vers console + tampon. A appeler tres tot. */
esp_err_t netlog_init(size_t buffer_bytes);

/* Demarre la tache serveur TCP (un client a la fois) sur le port donne. */
esp_err_t netlog_server_start(int port);

/* true des que le socket d'ecoute est pret (pour l'auto-test). */
bool netlog_server_is_listening(void);

#endif
```

- [ ] **Step 2 : implémentation**

`components/netlog/netlog.c` :
```c
#include "netlog.h"
#include "logbuf.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lwip/sockets.h"

static const char *TAG = "netlog";

#define NETLOG_LINE_MAX   256     /* pas LINE_MAX : deja defini par newlib */
#define CHUNK_MAX         512
#define POLL_MS           50

static logbuf_t s_lb;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_orig_vprintf;
static volatile bool s_listening;
static int s_port;

/* Hook esp_log : console d'origine + tampon circulaire. Ne bloque jamais. */
static int netlog_vprintf(const char *fmt, va_list ap)
{
    char line[NETLOG_LINE_MAX];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(line, sizeof line, fmt, ap2);
    va_end(ap2);

    int r = s_orig_vprintf ? s_orig_vprintf(fmt, ap) : vprintf(fmt, ap);

    if (n > 0) {
        size_t len = ((size_t)n < sizeof line) ? (size_t)n : sizeof line - 1;
        portENTER_CRITICAL(&s_lock);
        logbuf_write(&s_lb, line, len);
        portEXIT_CRITICAL(&s_lock);
    }
    return r;
}

esp_err_t netlog_init(size_t buffer_bytes)
{
    uint8_t *storage = malloc(buffer_bytes);
    if (storage == NULL) {
        return ESP_ERR_NO_MEM;
    }
    logbuf_init(&s_lb, storage, buffer_bytes);
    s_orig_vprintf = esp_log_set_vprintf(netlog_vprintf);
    return ESP_OK;
}

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Refuse poliment un second client pendant qu'un premier est servi. */
static void reject_pending(int lfd)
{
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(lfd, &rf);
    struct timeval tv = { 0, 0 };
    if (select(lfd + 1, &rf, NULL, NULL, &tv) > 0) {
        int c = accept(lfd, NULL, NULL);
        if (c >= 0) {
            static const char busy[] = "busy: un seul client a la fois\n";
            send(c, busy, sizeof busy - 1, MSG_DONTWAIT);
            close(c);
        }
    }
}

static void serve_client(int lfd, int c)
{
    uint8_t chunk[CHUNK_MAX];
    size_t pending = 0, off = 0;
    uint64_t cursor;

    set_nonblock(c);
    portENTER_CRITICAL(&s_lock);
    cursor = s_lb.total - s_lb.len;          /* tout l'historique disponible */
    portEXIT_CRITICAL(&s_lock);

    for (;;) {
        if (pending == 0) {
            uint64_t lost = 0;
            portENTER_CRITICAL(&s_lock);
            size_t n = logbuf_read(&s_lb, &cursor, chunk, sizeof chunk, &lost);
            portEXIT_CRITICAL(&s_lock);
            if (lost > 0) {
                char msg[64];
                int m = snprintf(msg, sizeof msg, "\n[netlog: %llu octets perdus]\n", (unsigned long long)lost);
                send(c, msg, (size_t)m, MSG_DONTWAIT);
            }
            if (n == 0) {
                char tmp[16];
                int r = recv(c, tmp, sizeof tmp, MSG_DONTWAIT);
                if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    return;                   /* client parti */
                }
                reject_pending(lfd);
                vTaskDelay(pdMS_TO_TICKS(POLL_MS));
                continue;
            }
            pending = n;
            off = 0;
        }
        int w = send(c, chunk + off, pending, MSG_DONTWAIT);
        if (w > 0) {
            off += (size_t)w;
            pending -= (size_t)w;
        } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        } else {
            return;
        }
    }
}

static void netlog_task(void *arg)
{
    (void)arg;
    int lfd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (lfd < 0) {
        ESP_LOGE(TAG, "socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    int yes = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)s_port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(lfd, 1) < 0) {
        ESP_LOGE(TAG, "bind/listen: errno %d", errno);
        close(lfd);
        vTaskDelete(NULL);
        return;
    }
    s_listening = true;
    ESP_LOGI(TAG, "serveur de logs sur le port %d", s_port);
    for (;;) {
        int c = accept(lfd, NULL, NULL);
        if (c < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        ESP_LOGI(TAG, "client connecte");
        serve_client(lfd, c);
        close(c);
        ESP_LOGI(TAG, "client deconnecte");
    }
}

esp_err_t netlog_server_start(int port)
{
    s_port = port;
    BaseType_t ok = xTaskCreate(netlog_task, "netlog", 3072, NULL, 4, NULL);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

bool netlog_server_is_listening(void)
{
    return s_listening;
}
```

- [ ] **Step 3 : CMake et câblage dans `main.c`**

`components/netlog/CMakeLists.txt` :
```cmake
idf_component_register(
    SRCS "logbuf.c" "netlog.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES lwip log)
```

`main/CMakeLists.txt` : ajouter `netlog` à `PRIV_REQUIRES`.

Dans `main/main.c` : ajouter `#include "netlog.h"` et, dans `app_main`, juste après `bus_gpio_safe();` :
```c
#if CONFIG_APP_NETLOG_ENABLE
    if (netlog_init((size_t)CONFIG_APP_NETLOG_BUFFER_KB * 1024) != ESP_OK) {
        ESP_LOGE(TAG, "netlog_init: pas de memoire");
    }
#endif
```
Ne pas appeler `netlog_server_start` dans cette tâche : sans pile TCP/IP initialisée
(`esp_netif_init`, tâche 8), `socket()` ne doit pas être appelé. La tâche 8 l'ajoute après `app_net_start`.

- [ ] **Step 4 : build**

`idf.py build` → `Project build complete.`

- [ ] **Step 5 : commit**

```bash
git add components/netlog main
git commit -m "feat: netlog, logs vers tampon circulaire et serveur TCP 3333

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 8 : `app_status` et `app_net` (station, backoff, mDNS)

**Files:**
- Create: `main/app_status.h`, `main/app_status.c`, `main/app_net.h`, `main/app_net.c`
- Modify: `main/CMakeLists.txt`, `main/main.c`

Le SoftAP de secours arrive à la tâche 11 ; cette tâche prépare déjà ses hooks (`app_net_recovery_ap_active`, `app_net_ota_finished`) pour que `app_ota` (tâche 9) compile.

- [ ] **Step 1 : `app_status`**

`main/app_status.h` :
```c
#ifndef APP_STATUS_H
#define APP_STATUS_H
#include "esp_system.h"
#include "esp_ota_ops.h"

const char *app_status_reset_reason_name(esp_reset_reason_t r);
const char *app_status_ota_state_name(esp_ota_img_states_t s);
/* Etat d'une partition : "none" si jamais ecrite, "error" si lecture impossible. */
const char *app_status_partition_state(const esp_partition_t *p);

#endif
```

`main/app_status.c` :
```c
#include "app_status.h"

const char *app_status_reset_reason_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "poweron";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int_wdt";
    case ESP_RST_TASK_WDT:  return "task_wdt";
    case ESP_RST_WDT:       return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_USB:       return "usb";
    case ESP_RST_JTAG:      return "jtag";
    case ESP_RST_EFUSE:     return "efuse";
    case ESP_RST_PWR_GLITCH: return "pwr_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    default:                return "unknown";
    }
}

const char *app_status_ota_state_name(esp_ota_img_states_t s)
{
    switch (s) {
    case ESP_OTA_IMG_NEW:            return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
    case ESP_OTA_IMG_VALID:          return "valid";
    case ESP_OTA_IMG_INVALID:        return "invalid";
    case ESP_OTA_IMG_ABORTED:        return "aborted";
    case ESP_OTA_IMG_UNDEFINED:
    default:                         return "undefined";
    }
}

const char *app_status_partition_state(const esp_partition_t *p)
{
    esp_ota_img_states_t st;
    if (p == NULL) {
        return "none";
    }
    esp_err_t e = esp_ota_get_state_partition(p, &st);
    if (e == ESP_ERR_NOT_FOUND) {
        return "none";
    }
    if (e != ESP_OK) {
        return "error";
    }
    return app_status_ota_state_name(st);
}
```

- [ ] **Step 2 : `app_net.h`**

```c
#ifndef APP_NET_H
#define APP_NET_H
#include <stdbool.h>

void app_net_start(void);
bool app_net_is_connected(void);          /* station avec adresse IP */
bool app_net_recovery_ap_active(void);
int  app_net_rssi(void);                  /* 0 si non connecte */
/* Appele par app_ota quand une OTA se termine sans reboot (arret differe du SoftAP). */
void app_net_ota_finished(void);

#endif
```

- [ ] **Step 3 : `app_net.c` (station seulement ; le SoftAP est complété à la tâche 11)**

```c
#include "app_net.h"
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "mdns.h"

static const char *TAG = "net";

#define BACKOFF_MAX_S 60

#define LOG_IF_ERR(x) do { esp_err_t _e = (x); if (_e != ESP_OK) { \
    ESP_LOGE(TAG, "%s: %s", #x, esp_err_to_name(_e)); } } while (0)

static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static esp_timer_handle_t s_reconnect_timer;
static esp_timer_handle_t s_recovery_timer;
static int s_backoff_s = 1;
static volatile bool s_connected;
static volatile bool s_ap_active;
static volatile bool s_ap_stop_deferred;
static bool s_ap_enabled;

/* --- SoftAP de secours : complete a la tache 11 --- */
static void start_recovery_ap(void) { }
static void stop_recovery_ap(void) { }

static void reconnect_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

static void recovery_cb(void *arg)
{
    (void)arg;
    if (s_connected || s_ap_active) {
        return;
    }
    start_recovery_ap();
}

/* Armee au boot et quand une connexion etablie est perdue, jamais rearmee si elle tourne deja. */
static void arm_recovery_timer(void)
{
    if (!s_ap_enabled || s_ap_active || esp_timer_is_active(s_recovery_timer)) {
        return;
    }
    esp_timer_start_once(s_recovery_timer, (uint64_t)CONFIG_APP_RECOVERY_AP_DELAY_S * 1000000ULL);
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    switch (id) {
    case WIFI_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *d = data;
        bool was_connected = s_connected;
        s_connected = false;
        ESP_LOGW(TAG, "deconnecte (raison %d), nouvel essai dans %d s", d->reason, s_backoff_s);
        if (was_connected) {
            arm_recovery_timer();
        }
        esp_timer_stop(s_reconnect_timer);
        esp_timer_start_once(s_reconnect_timer, (uint64_t)s_backoff_s * 1000000ULL);
        s_backoff_s = (s_backoff_s * 2 > BACKOFF_MAX_S) ? BACKOFF_MAX_S : s_backoff_s * 2;
        break;
    }
    case WIFI_EVENT_AP_STACONNECTED:
        ESP_LOGI(TAG, "client connecte au SoftAP de secours");
        break;
    case WIFI_EVENT_AP_STADISCONNECTED:
        ESP_LOGI(TAG, "client parti du SoftAP de secours");
        break;
    default:
        break;
    }
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;
    const ip_event_got_ip_t *e = data;
    s_connected = true;
    s_backoff_s = 1;
    esp_timer_stop(s_recovery_timer);
    ESP_LOGI(TAG, "connecte, IP " IPSTR ", rssi %d", IP2STR(&e->ip_info.ip), app_net_rssi());
    if (s_ap_active) {
        stop_recovery_ap();
    }
}

void app_net_start(void)
{
    s_ap_enabled = strlen(CONFIG_APP_RECOVERY_AP_PASSWORD) >= 8;
    if (!s_ap_enabled) {
        ESP_LOGE(TAG, "APP_RECOVERY_AP_PASSWORD trop court (< 8) : SoftAP de secours DESACTIVE");
    }
    if (strlen(CONFIG_APP_WIFI_SSID) == 0) {
        ESP_LOGE(TAG, "APP_WIFI_SSID vide : aucune connexion possible");
    }

    LOG_IF_ERR(esp_netif_init());
    LOG_IF_ERR(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta_netif, CONFIG_APP_HOSTNAME);

    wifi_init_config_t icfg = WIFI_INIT_CONFIG_DEFAULT();
    LOG_IF_ERR(esp_wifi_init(&icfg));
    LOG_IF_ERR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL, NULL));
    LOG_IF_ERR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip, NULL, NULL));

    const esp_timer_create_args_t rc = { .callback = reconnect_cb, .name = "reconnect" };
    const esp_timer_create_args_t rv = { .callback = recovery_cb, .name = "recovery" };
    LOG_IF_ERR(esp_timer_create(&rc, &s_reconnect_timer));
    LOG_IF_ERR(esp_timer_create(&rv, &s_recovery_timer));

    wifi_config_t wc;
    memset(&wc, 0, sizeof wc);
    strlcpy((char *)wc.sta.ssid, CONFIG_APP_WIFI_SSID, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, CONFIG_APP_WIFI_PASSWORD, sizeof wc.sta.password);
    wc.sta.threshold.authmode = strlen(CONFIG_APP_WIFI_PASSWORD) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;

    LOG_IF_ERR(esp_wifi_set_storage(WIFI_STORAGE_RAM));     /* toujours les identifiants du Kconfig */
    LOG_IF_ERR(esp_wifi_set_mode(WIFI_MODE_STA));
    LOG_IF_ERR(esp_wifi_set_config(WIFI_IF_STA, &wc));
    LOG_IF_ERR(esp_wifi_start());
    LOG_IF_ERR(esp_wifi_set_max_tx_power(CONFIG_APP_WIFI_TX_POWER));
    ESP_LOGI(TAG, "station demarree, ssid '%s', puissance max %d", CONFIG_APP_WIFI_SSID, CONFIG_APP_WIFI_TX_POWER);

    arm_recovery_timer();

    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(CONFIG_APP_HOSTNAME);
        mdns_instance_name_set("Interphone");
        ESP_LOGI(TAG, "mDNS %s.local", CONFIG_APP_HOSTNAME);
    } else {
        ESP_LOGE(TAG, "mdns_init a echoue");
    }
}

bool app_net_is_connected(void)
{
    return s_connected;
}

bool app_net_recovery_ap_active(void)
{
    return s_ap_active;
}

int app_net_rssi(void)
{
    wifi_ap_record_t r;
    if (s_connected && esp_wifi_sta_get_ap_info(&r) == ESP_OK) {
        return r.rssi;
    }
    return 0;
}

void app_net_ota_finished(void)
{
    if (s_ap_stop_deferred && s_connected) {
        stop_recovery_ap();
    }
}
```

- [ ] **Step 4 : CMake et `main.c`**

`main/CMakeLists.txt` :
```cmake
idf_component_register(
    SRCS "main.c" "app_status.c" "app_net.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_gpio esp_app_format app_update nvs_flash esp_timer
                  esp_wifi esp_netif esp_event netlog tc_bus)
```

Dans `main/main.c` : ajouter `#include "app_status.h"` et `#include "app_net.h"`. Remplacer le log
de démarrage et la fin de `app_main` par :
```c
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "interphone-homekit %s | partition %s (%s) | reset %s | heap %" PRIu32,
             app->version, run ? run->label : "?", app_status_partition_state(run),
             app_status_reset_reason_name(esp_reset_reason()), esp_get_free_heap_size());

    app_net_start();
#if CONFIG_APP_NETLOG_ENABLE
    netlog_server_start(CONFIG_APP_NETLOG_PORT);
#endif
```

- [ ] **Step 5 : build**

`idf.py build` → `Project build complete.`

- [ ] **Step 6 : commit**

```bash
git add main
git commit -m "feat: app_net (station, backoff, mDNS) et app_status

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 9 : `app_ota`, serveur HTTP avec `/info` et `/reboot`

**Files:**
- Create: `main/app_ota.h`, `main/app_ota.c`
- Create: `components/tc_bus/include/tc_bus.h` (API seulement, stub pour `tc_bus.c` jusqu'à la tâche 13)
- Modify: `components/tc_bus/CMakeLists.txt`, `main/CMakeLists.txt`, `main/main.c`

- [ ] **Step 1 : API `tc_bus` et stub**

`components/tc_bus/include/tc_bus.h` :
```c
#ifndef TC_BUS_H
#define TC_BUS_H
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "tc_proto.h"

typedef struct {
    uint32_t frames_ok;
    uint32_t frames_err;
    int64_t  last_rx_us;     /* esp_timer_get_time() de la derniere trame, 0 si aucune */
} tc_bus_stats_t;

typedef void (*tc_bus_rx_cb_t)(const tc_telegram_t *t, void *ctx);

esp_err_t tc_bus_start(tc_bus_rx_cb_t cb, void *ctx);
bool      tc_bus_is_running(void);
void      tc_bus_get_stats(tc_bus_stats_t *s);

/* Emission : no-op en phase 0 (TX non compile), appele par l'OTA. */
void      tc_bus_tx_block(void);
void      tc_bus_tx_unblock(void);

#endif
```

`components/tc_bus/tc_bus.c` (stub, remplacé à la tâche 13) :
```c
#include "tc_bus.h"
#include <string.h>

#if CONFIG_TC_BUS_TX_ENABLE
#error "L'emission n'est pas implementee en phase 0 : TC_BUS_TX_ENABLE doit rester a n"
#endif

static tc_bus_stats_t s_stats;
static bool s_running;

esp_err_t tc_bus_start(tc_bus_rx_cb_t cb, void *ctx) { (void)cb; (void)ctx; s_running = true; return ESP_OK; }
bool tc_bus_is_running(void) { return s_running; }
void tc_bus_get_stats(tc_bus_stats_t *s) { *s = s_stats; }
void tc_bus_tx_block(void) { }
void tc_bus_tx_unblock(void) { }
```

`components/tc_bus/CMakeLists.txt` : `SRCS "tc_proto.c" "tc_bus.c"`.

- [ ] **Step 2 : `app_ota.h`**

```c
#ifndef APP_OTA_H
#define APP_OTA_H
#include <stdbool.h>

void app_ota_start(void);
bool app_ota_is_running(void);     /* serveur HTTP demarre */
bool app_ota_in_progress(void);    /* un transfert OTA est en cours */

#endif
```

- [ ] **Step 3 : `app_ota.c` avec `/info` et `/reboot` (`/ota` à la tâche 10)**

```c
#include "app_ota.h"
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_http_server.h"
#include "ota_auth.h"
#include "app_status.h"
#include "app_net.h"
#include "tc_bus.h"

static const char *TAG = "ota";

#define OTA_CHUNK 4096
#define TOKEN_MIN_LEN 32

static httpd_handle_t s_server;
static esp_timer_handle_t s_reboot_timer;
static esp_timer_handle_t s_token_warn_timer;
static volatile bool s_in_progress;
static volatile bool s_running;
static bool s_token_ok;
static uint8_t *s_buf;

/* Repond et ferme la connexion (ESP_FAIL => httpd ferme le socket, utile quand le corps n'a pas ete lu). */
static esp_err_t reply_close(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, msg);
    return ESP_FAIL;
}

static bool check_auth(httpd_req_t *req)
{
    if (!s_token_ok) {
        reply_close(req, "503 Service Unavailable", "OTA token not configured (min 32 chars)\n");
        return false;
    }
    char hdr[160];
    size_t len = httpd_req_get_hdr_value_len(req, "Authorization");
    bool ok = false;
    if (len > 0 && len < sizeof hdr &&
        httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof hdr) == ESP_OK) {
        ok = ota_auth_check(hdr, CONFIG_APP_OTA_TOKEN);
    }
    if (!ok) {
        reply_close(req, "401 Unauthorized", "unauthorized\n");
    }
    return ok;
}

static void reboot_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

static void token_warn_cb(void *arg)
{
    (void)arg;
    ESP_LOGE(TAG, "APP_OTA_TOKEN trop court (< %d caracteres) : OTA et /info refuses", TOKEN_MIN_LEN);
}

static esp_err_t info_get(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_FAIL;
    }
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    tc_bus_stats_t bs;
    tc_bus_get_stats(&bs);

    char body[512];
    int n = snprintf(body, sizeof body,
        "{\"version\":\"%s\",\"idf\":\"%s\",\"partition\":\"%s\",\"ota_state\":\"%s\","
        "\"other_partition\":\"%s\",\"other_state\":\"%s\",\"uptime_s\":%lld,"
        "\"heap_free\":%" PRIu32 ",\"heap_min\":%" PRIu32 ",\"rssi\":%d,\"reset_reason\":\"%s\","
        "\"net\":\"%s\",\"bus_frames_ok\":%" PRIu32 ",\"bus_frames_err\":%" PRIu32 "}\n",
        app->version, esp_get_idf_version(),
        run ? run->label : "?", app_status_partition_state(run),
        other ? other->label : "?", app_status_partition_state(other),
        (long long)(esp_timer_get_time() / 1000000),
        esp_get_free_heap_size(), esp_get_minimum_free_heap_size(),
        app_net_rssi(), app_status_reset_reason_name(esp_reset_reason()),
        app_net_recovery_ap_active() ? "recovery" : "sta",
        bs.frames_ok, bs.frames_err);
    if (n < 0 || n >= (int)sizeof body) {
        return reply_close(req, "500 Internal Server Error", "info too long\n");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, n);
    return ESP_OK;
}

static esp_err_t reboot_post(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "redemarrage demande par /reboot");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"rebooting\",\"reboot_in_ms\":1000}\n");
    esp_timer_start_once(s_reboot_timer, 1000000);
    return ESP_OK;
}

/* --- POST /ota : tache 10 --- */

void app_ota_start(void)
{
    s_token_ok = strlen(CONFIG_APP_OTA_TOKEN) >= TOKEN_MIN_LEN;
    const esp_timer_create_args_t rb = { .callback = reboot_cb, .name = "reboot" };
    esp_timer_create(&rb, &s_reboot_timer);
    if (!s_token_ok) {
        token_warn_cb(NULL);
        const esp_timer_create_args_t tw = { .callback = token_warn_cb, .name = "token_warn" };
        esp_timer_create(&tw, &s_token_warn_timer);
        esp_timer_start_periodic(s_token_warn_timer, 60ULL * 1000000ULL);
    }
    s_buf = malloc(OTA_CHUNK);
    if (s_buf == NULL) {
        ESP_LOGE(TAG, "pas de memoire pour le tampon OTA");
        return;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = CONFIG_APP_HTTP_PORT;
    cfg.ctrl_port = 32700;               /* distinct de celui du SDK HomeKit (32859) */
    cfg.stack_size = 8192;
    cfg.max_open_sockets = 4;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;

    esp_err_t e = httpd_start(&s_server, &cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(e));
        return;
    }
    const httpd_uri_t info = { .uri = "/info", .method = HTTP_GET, .handler = info_get };
    const httpd_uri_t reboot = { .uri = "/reboot", .method = HTTP_POST, .handler = reboot_post };
    httpd_register_uri_handler(s_server, &info);
    httpd_register_uri_handler(s_server, &reboot);
    s_running = true;
    ESP_LOGI(TAG, "serveur HTTP sur le port %d (/info, /ota, /reboot)", CONFIG_APP_HTTP_PORT);
}

bool app_ota_is_running(void)
{
    return s_running;
}

bool app_ota_in_progress(void)
{
    return s_in_progress;
}
```

- [ ] **Step 4 : CMake et `main.c`**

`main/CMakeLists.txt` : `SRCS "main.c" "app_status.c" "app_net.c" "ota_auth.c" "app_ota.c"` et
ajouter `esp_http_server` à `PRIV_REQUIRES`.

Dans `main/main.c` : `#include "app_ota.h"`, `#include "tc_bus.h"`, et après `netlog_server_start(...)` :
```c
    app_ota_start();
    tc_bus_start(NULL, NULL);
```

- [ ] **Step 5 : build**

`idf.py build` → `Project build complete.`

- [ ] **Step 6 : commit**

```bash
git add main components/tc_bus
git commit -m "feat: serveur HTTP 8080 avec /info et /reboot, API tc_bus

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 10 : `POST /ota`

**Files:**
- Modify: `main/app_ota.c`

- [ ] **Step 1 : handler**

Remplacer le commentaire `/* --- POST /ota : tache 10 --- */` dans `main/app_ota.c` par :
```c
/* Fin d'une OTA. reboot=true : la reponse 200 est envoyee et le redemarrage est programme.
 * reboot=false : echec, on libere le bus et on previent app_net (arret differe du SoftAP). */
static esp_err_t ota_finish(httpd_req_t *req, const char *status, const char *msg, bool reboot)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, reboot ? "application/json" : "text/plain");
    httpd_resp_sendstr(req, msg);
    ESP_LOGI(TAG, "pile httpd libre min : %u octets", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    if (reboot) {
        ESP_LOGI(TAG, "OTA OK, redemarrage dans 1 s");
        esp_timer_start_once(s_reboot_timer, 1000000);
        return ESP_OK;
    }
    ESP_LOGW(TAG, "OTA echouee: %s", msg);
    s_in_progress = false;
    tc_bus_tx_unblock();
    app_net_ota_finished();
    return ESP_FAIL;
}

static esp_err_t ota_post(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_FAIL;
    }
    if (s_in_progress) {
        return reply_close(req, "409 Conflict", "ota already in progress\n");
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        return reply_close(req, "500 Internal Server Error", "no ota partition\n");
    }
    if (!ota_size_ok(req->content_len, part->size)) {
        return reply_close(req, "413 Payload Too Large", "missing or oversized Content-Length\n");
    }

    s_in_progress = true;
    tc_bus_tx_block();
    ESP_LOGI(TAG, "OTA: %u octets vers %s", (unsigned)req->content_len, part->label);

    esp_ota_handle_t h = 0;
    esp_err_t e = esp_ota_begin(part, req->content_len, &h);
    if (e == ESP_ERR_OTA_ROLLBACK_INVALID_STATE) {
        return ota_finish(req, "409 Conflict", "self-test pending\n", false);
    }
    if (e != ESP_OK) {
        return ota_finish(req, "500 Internal Server Error", esp_err_to_name(e), false);
    }

    size_t remaining = req->content_len;
    bool first = true;
    while (remaining > 0) {
        size_t want = remaining < OTA_CHUNK ? remaining : OTA_CHUNK;
        int r = httpd_req_recv(req, (char *)s_buf, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            /* 10 s sans un octet : on abandonne, sinon un client bloque monopolise le serveur */
            esp_ota_abort(h);
            return ota_finish(req, "500 Internal Server Error", "receive timeout\n", false);
        }
        if (r <= 0) {
            esp_ota_abort(h);
            return ota_finish(req, "500 Internal Server Error", "receive error\n", false);
        }
        e = esp_ota_write(h, s_buf, (size_t)r);
        if (e != ESP_OK) {
            esp_ota_abort(h);
            bool bad_image = first && e == ESP_ERR_OTA_VALIDATE_FAILED;
            return ota_finish(req, bad_image ? "400 Bad Request" : "500 Internal Server Error",
                              bad_image ? "not a firmware image\n" : esp_err_to_name(e), false);
        }
        first = false;
        remaining -= (size_t)r;
    }

    e = esp_ota_end(h);
    if (e == ESP_ERR_OTA_VALIDATE_FAILED) {
        return ota_finish(req, "400 Bad Request", "image invalid or unsigned\n", false);
    }
    if (e != ESP_OK) {
        return ota_finish(req, "500 Internal Server Error", esp_err_to_name(e), false);
    }
    e = esp_ota_set_boot_partition(part);
    if (e != ESP_OK) {
        return ota_finish(req, "500 Internal Server Error", esp_err_to_name(e), false);
    }
    return ota_finish(req, "200 OK", "{\"status\":\"ok\",\"reboot_in_ms\":1000}\n", true);
}
```

Dans `app_ota_start`, après l'enregistrement de `/reboot` :
```c
    const httpd_uri_t ota = { .uri = "/ota", .method = HTTP_POST, .handler = ota_post };
    httpd_register_uri_handler(s_server, &ota);
```

- [ ] **Step 2 : build**

`idf.py build` → `Project build complete.`

- [ ] **Step 3 : commit**

```bash
git add main/app_ota.c
git commit -m "feat: POST /ota, image signee verifiee par esp_ota_end, bus bloque pendant le transfert

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 11 : SoftAP de secours

**Files:**
- Modify: `main/app_net.c`

- [ ] **Step 1 : remplacer les deux fonctions vides**

Dans `main/app_net.c`, remplacer
```c
static void start_recovery_ap(void) { }
static void stop_recovery_ap(void) { }
```
par :
```c
#define RECOVERY_AP_SSID "interphone-recovery"

static void start_recovery_ap(void)
{
    wifi_config_t ap;
    memset(&ap, 0, sizeof ap);
    strlcpy((char *)ap.ap.ssid, RECOVERY_AP_SSID, sizeof ap.ap.ssid);
    ap.ap.ssid_len = strlen(RECOVERY_AP_SSID);
    strlcpy((char *)ap.ap.password, CONFIG_APP_RECOVERY_AP_PASSWORD, sizeof ap.ap.password);
    ap.ap.channel = 1;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 2;
    ap.ap.pmf_cfg.required = false;

    LOG_IF_ERR(esp_wifi_set_mode(WIFI_MODE_APSTA));
    LOG_IF_ERR(esp_wifi_set_config(WIFI_IF_AP, &ap));
    s_ap_active = true;
    s_ap_stop_deferred = false;
    ESP_LOGW(TAG, "SoftAP de secours '%s' demarre (192.168.4.1), la station continue d'essayer",
             RECOVERY_AP_SSID);
}

static void stop_recovery_ap(void)
{
    extern bool app_ota_in_progress(void);
    if (app_ota_in_progress()) {
        s_ap_stop_deferred = true;
        ESP_LOGI(TAG, "OTA en cours : arret du SoftAP differe");
        return;
    }
    LOG_IF_ERR(esp_wifi_set_mode(WIFI_MODE_STA));
    s_ap_active = false;
    s_ap_stop_deferred = false;
    ESP_LOGI(TAG, "SoftAP de secours arrete");
}
```
Remplacer `extern bool app_ota_in_progress(void);` par `#include "app_ota.h"` en tête de fichier
(même composant, aucune dépendance circulaire de build).

- [ ] **Step 2 : build**

`idf.py build` → `Project build complete.`

- [ ] **Step 3 : commit**

```bash
git add main/app_net.c
git commit -m "feat: SoftAP de secours interphone-recovery apres delai sans connexion

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 12 : `app_selftest`, validation ou rollback

**Files:**
- Create: `main/app_selftest.h`, `main/app_selftest.c`
- Modify: `main/CMakeLists.txt`, `main/main.c`

- [ ] **Step 1 : en-tête et implémentation**

`main/app_selftest.h` :
```c
#ifndef APP_SELFTEST_H
#define APP_SELFTEST_H
/* Si l'image est en attente de validation (premier boot apres OTA), lance l'auto-test :
 * acces distant (station ou SoftAP), logs, HTTP, bus. Succes -> image validee ; echec -> rollback. */
void app_selftest_start(void);
#endif
```

`main/app_selftest.c` :
```c
#include "app_selftest.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app_net.h"
#include "app_ota.h"
#include "netlog.h"
#include "tc_bus.h"

static const char *TAG = "selftest";

static const char *missing_condition(void)
{
    if (!(app_net_is_connected() || app_net_recovery_ap_active())) {
        return "pas d'acces reseau (ni station ni SoftAP)";
    }
#if CONFIG_APP_NETLOG_ENABLE
    if (!netlog_server_is_listening()) {
        return "serveur de logs absent";
    }
#endif
    if (!app_ota_is_running()) {
        return "serveur HTTP absent";
    }
    if (!tc_bus_is_running()) {
        return "tache bus absente";
    }
    return NULL;
}

static void selftest_task(void *arg)
{
    (void)arg;
    int64_t deadline = esp_timer_get_time() + (int64_t)CONFIG_APP_SELFTEST_TIMEOUT_S * 1000000LL;
    const char *reason = NULL;
    bool ok = false;

    while (esp_timer_get_time() < deadline) {
        reason = missing_condition();
        if (reason == NULL) {
            ok = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
#if CONFIG_APP_SELFTEST_FORCE_FAIL
    if (ok) {
        ok = false;
        reason = "echec force par APP_SELFTEST_FORCE_FAIL";
    }
#endif
    if (ok) {
        esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "SELFTEST OK, image validee (%s)", esp_err_to_name(e));
    } else {
        ESP_LOGE(TAG, "SELFTEST FAILED (%s), rollback", reason ? reason : "timeout");
        vTaskDelay(pdMS_TO_TICKS(1000));          /* laisser partir les logs reseau */
        esp_err_t e = esp_ota_mark_app_invalid_rollback_and_reboot();
        /* on ne revient ici que si le rollback est impossible */
        ESP_LOGE(TAG, "rollback impossible (%s) : validation forcee pour garder l'OTA", esp_err_to_name(e));
        esp_ota_mark_app_valid_cancel_rollback();
    }
    vTaskDelete(NULL);
}

void app_selftest_start(void)
{
    if (CONFIG_APP_SELFTEST_TIMEOUT_S >= CONFIG_APP_RECOVERY_AP_DELAY_S) {
        ESP_LOGW(TAG, "invariant non respecte : auto-test (%d s) >= SoftAP (%d s), "
                 "une image sans Wi-Fi pourrait etre validee par le SoftAP",
                 CONFIG_APP_SELFTEST_TIMEOUT_S, CONFIG_APP_RECOVERY_AP_DELAY_S);
    }
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run == NULL || esp_ota_get_state_partition(run, &st) != ESP_OK || st != ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "image deja validee, pas d'auto-test");
        return;
    }
    ESP_LOGW(TAG, "nouvelle image en attente : auto-test (%d s max)", CONFIG_APP_SELFTEST_TIMEOUT_S);
    xTaskCreate(selftest_task, "selftest", 8192, NULL, 5, NULL);
}
```

- [ ] **Step 2 : CMake et `main.c`**

`main/CMakeLists.txt` : ajouter `"app_selftest.c"` aux `SRCS`.

`main/main.c` : `#include "app_selftest.h"` et, après `tc_bus_start(NULL, NULL);` :
```c
    app_selftest_start();
```

- [ ] **Step 3 : build**

`idf.py build` → `Project build complete.`

- [ ] **Step 4 : commit**

```bash
git add main
git commit -m "feat: auto-test au premier boot d'une image, validation ou rollback

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 13 : `tc_bus`, réception RMT

**Files:**
- Replace: `components/tc_bus/tc_bus.c`
- Modify: `main/main.c` (callback de log)

- [ ] **Step 1 : implémentation**

`components/tc_bus/tc_bus.c` :
```c
#include "tc_bus.h"
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "driver/rmt_rx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#if CONFIG_TC_BUS_TX_ENABLE
#error "L'emission n'est pas implementee en phase 0 : TC_BUS_TX_ENABLE doit rester a n"
#endif

static const char *TAG = "tc_bus";

#define RX_SYMBOLS 48                      /* un bloc memoire RMT du C3 ; 96 segments */

static rmt_channel_handle_t s_rx;
static QueueHandle_t s_queue;
static rmt_symbol_word_t s_symbols[RX_SYMBOLS];
static tc_bus_stats_t s_stats;
static volatile bool s_running;
static tc_bus_rx_cb_t s_cb;
static void *s_ctx;

static const rmt_receive_config_t s_rx_cfg = {
    .signal_range_min_ns = 2500,           /* limite materielle ~3.2 us ; le vrai filtre est logiciel */
    .signal_range_max_ns = 7000000,        /* > 7 ms au repos = fin de trame */
};

static bool IRAM_ATTR on_recv_done(rmt_channel_handle_t ch, const rmt_rx_done_event_data_t *ed, void *ctx)
{
    (void)ch; (void)ctx;
    BaseType_t hp = pdFALSE;
    size_t n = ed->num_symbols;
    xQueueSendFromISR(s_queue, &n, &hp);
    return hp == pdTRUE;
}

static size_t symbols_to_segments(size_t nsym, tc_segment_t *segs)
{
    size_t k = 0;
    for (size_t i = 0; i < nsym && k < TC_MAX_SEGMENTS; i++) {
        if (s_symbols[i].duration0 == 0) {
            break;
        }
        segs[k].duration_us = s_symbols[i].duration0;
        segs[k].mark = (s_symbols[i].level0 == 0);      /* bus bas = GPIO RX a 0 */
        k++;
        if (s_symbols[i].duration1 == 0 || k >= TC_MAX_SEGMENTS) {
            break;
        }
        segs[k].duration_us = s_symbols[i].duration1;
        segs[k].mark = (s_symbols[i].level1 == 0);
        k++;
    }
    return k;
}

static void rx_task(void *arg)
{
    (void)arg;
    size_t nsym;
    tc_segment_t segs[TC_MAX_SEGMENTS];
    for (;;) {
        if (xQueueReceive(s_queue, &nsym, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        size_t k = symbols_to_segments(nsym, segs);
        /* rearmer tout de suite : les symboles sont copies */
        esp_err_t e = rmt_receive(s_rx, s_symbols, sizeof s_symbols, &s_rx_cfg);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "rmt_receive: %s", esp_err_to_name(e));
        }
        if (k < 2) {
            continue;                       /* evenement vide ou parasite isole */
        }
        s_stats.last_rx_us = esp_timer_get_time();
        k = tc_proto_merge_glitches(segs, k, CONFIG_TC_BUS_GLITCH_US);   /* d'abord : un parasite dans le repos */
        k = tc_proto_strip_trailing_idle(segs, k);                        /* ensuite : retirer le repos */

        tc_telegram_t t;
        tc_decode_error_t err;
        if (tc_proto_decode(segs, k, &t, &err)) {
            tc_proto_classify(&t);
            s_stats.frames_ok++;
            ESP_LOGI(TAG, "TC RX %s raw=0x%0*lX len=%u serial=0x%05lX addr=%u payload=0x%lX chk=ok%s",
                     tc_type_name(t.type), t.is_long ? 8 : 4, (unsigned long)t.raw,
                     t.is_long ? 32u : (t.type == TC_TYPE_ACK ? 4u : 16u),
                     (unsigned long)t.serial, t.address, (unsigned long)t.payload,
                     t.is_response ? " rsp" : "");
            if (s_cb) {
                s_cb(&t, s_ctx);
            }
        } else {
            s_stats.frames_err++;
            ESP_LOGW(TAG, "TC RX ERR %s n=%u first=%u last=%u", tc_decode_error_name(err),
                     (unsigned)k, k ? segs[0].duration_us : 0, k ? segs[k - 1].duration_us : 0);
        }
    }
}

esp_err_t tc_bus_start(tc_bus_rx_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_ctx = ctx;
    s_queue = xQueueCreate(4, sizeof(size_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    rmt_rx_channel_config_t cfg = {
        .gpio_num = CONFIG_TC_BUS_RX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000,          /* 1 tick = 1 us */
        .mem_block_symbols = RX_SYMBOLS,
    };
    esp_err_t e = rmt_new_rx_channel(&cfg, &s_rx);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_rx_channel: %s", esp_err_to_name(e));
        return e;
    }
    const rmt_rx_event_callbacks_t cbs = { .on_recv_done = on_recv_done };
    e = rmt_rx_register_event_callbacks(s_rx, &cbs, NULL);
    if (e == ESP_OK) {
        e = rmt_enable(s_rx);
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "rmt init: %s", esp_err_to_name(e));
        return e;
    }
    if (xTaskCreate(rx_task, "tc_rx", 4096, NULL, 10, NULL) != pdPASS) {
        return ESP_FAIL;
    }
    e = rmt_receive(s_rx, s_symbols, sizeof s_symbols, &s_rx_cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "rmt_receive: %s", esp_err_to_name(e));
        return e;
    }
    s_running = true;
    ESP_LOGI(TAG, "reception RMT sur GPIO%d, emission NON compilee (TX GPIO%d reste a 0)",
             CONFIG_TC_BUS_RX_GPIO, CONFIG_TC_BUS_TX_GPIO);
    return ESP_OK;
}

bool tc_bus_is_running(void)
{
    return s_running;
}

void tc_bus_get_stats(tc_bus_stats_t *s)
{
    *s = s_stats;
}

void tc_bus_tx_block(void)
{
    /* no-op en phase 0 : rien n'emet */
}

void tc_bus_tx_unblock(void)
{
}
```

Note : si, sur le matériel, le journal montre une rafale continue de `TC RX ERR length n=1` toutes
les 7 ms alors que le bus est au repos, c'est que le RMT démarre son chronomètre dès `rmt_receive`
au lieu du premier front. Le `if (k < 2) continue;` absorbe ce cas sans log ; si la charge CPU
devenait visible, passer `signal_range_max_ns` à 32 000 000 réduirait la fréquence sans changer
le décodage (les trames restent délimitées par un repos > 7 ms car on ne dépasse jamais 6 900 µs
dans une trame).

- [ ] **Step 2 : build**

`idf.py build` → `Project build complete.`

- [ ] **Step 3 : commit**

```bash
git add components/tc_bus/tc_bus.c
git commit -m "feat: tc_bus, reception RMT sur GPIO4 et decodage dans une tache

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 14 : `main.c` final, battement de cœur, outils, README, taille

**Files:**
- Modify: `main/main.c` (version finale complète ci-dessous)
- Create: `tools/ota.sh`, `tools/logs.py`, `README.md` (remplacer)

- [ ] **Step 1 : `main/main.c` final**

Remplacer tout le fichier par :
```c
#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "netlog.h"
#include "tc_bus.h"
#include "app_status.h"
#include "app_net.h"
#include "app_ota.h"
#include "app_selftest.h"

static const char *TAG = "main";

/* Spec 3 regle 1 : GPIO10 (TX bus) en sortie a 0 avant toute autre chose. */
static void bus_gpio_safe(void)
{
    gpio_set_level(CONFIG_TC_BUS_TX_GPIO, 0);
    gpio_config_t tx = {
        .pin_bit_mask = 1ULL << CONFIG_TC_BUS_TX_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&tx);
    gpio_set_level(CONFIG_TC_BUS_TX_GPIO, 0);

    gpio_config_t rx = {
        .pin_bit_mask = 1ULL << CONFIG_TC_BUS_RX_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&rx);
}

static void nvs_init(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS a effacer (%s)", esp_err_to_name(e));
        nvs_flash_erase();
        e = nvs_flash_init();
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(e));
    }
}

/* Phase 0 : on logge seulement. Les phases suivantes brancheront HomeKit ici. */
static void on_telegram(const tc_telegram_t *t, void *ctx)
{
    (void)t; (void)ctx;
}

void app_main(void)
{
    bus_gpio_safe();                                   /* 1. toujours en premier */

#if CONFIG_APP_NETLOG_ENABLE
    if (netlog_init((size_t)CONFIG_APP_NETLOG_BUFFER_KB * 1024) != ESP_OK) {   /* 3. avant le Wi-Fi */
        ESP_LOGE(TAG, "netlog_init: pas de memoire");
    }
#endif
    nvs_init();                                        /* 4. */

    const esp_app_desc_t *app = esp_app_get_description();   /* 2. + 5. log de demarrage */
    const esp_partition_t *run = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "interphone-homekit %s | partition %s (%s) | reset %s | heap %" PRIu32,
             app->version, run ? run->label : "?", app_status_partition_state(run),
             app_status_reset_reason_name(esp_reset_reason()), esp_get_free_heap_size());

    app_net_start();                                   /* 6. */
#if CONFIG_APP_NETLOG_ENABLE
    netlog_server_start(CONFIG_APP_NETLOG_PORT);       /* 7. */
#endif
    app_ota_start();
    tc_bus_start(on_telegram, NULL);                   /* 8. */
    app_selftest_start();                              /* 9. */

#if CONFIG_APP_HEARTBEAT_S > 0
    for (;;) {                                         /* 10. battement de coeur */
        vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_HEARTBEAT_S * 1000));
        tc_bus_stats_t s;
        tc_bus_get_stats(&s);
        ESP_LOGI(TAG, "HB up=%llds heap=%" PRIu32 " min_heap=%" PRIu32 " rssi=%d net=%s bus_ok=%" PRIu32 " bus_err=%" PRIu32,
                 (long long)(esp_timer_get_time() / 1000000), esp_get_free_heap_size(),
                 esp_get_minimum_free_heap_size(), app_net_rssi(),
                 app_net_recovery_ap_active() ? "recovery" : (app_net_is_connected() ? "sta" : "down"),
                 s.frames_ok, s.frames_err);
    }
#endif
}
```

- [ ] **Step 2 : outils**

`tools/logs.py` :
```python
#!/usr/bin/env python3
"""Suit les logs reseau de l'interphone.

Usage : python tools/logs.py [hote] [port]   (defaut interphone.local 3333)
Se reconnecte tout seul quand la carte redemarre.
"""
import socket
import sys
import time

host = sys.argv[1] if len(sys.argv) > 1 else "interphone.local"
port = int(sys.argv[2]) if len(sys.argv) > 2 else 3333

while True:
    try:
        with socket.create_connection((host, port), timeout=10) as s:
            s.settimeout(None)
            print(f"--- connecte a {host}:{port} ---", flush=True)
            while True:
                data = s.recv(4096)
                if not data:
                    break
                sys.stdout.write(data.decode("utf-8", "replace"))
                sys.stdout.flush()
    except OSError as e:
        print(f"--- pas de connexion ({e}), nouvel essai dans 3 s ---", flush=True)
    except KeyboardInterrupt:
        sys.exit(0)
    time.sleep(3)
```

`tools/ota.sh` :
```bash
#!/usr/bin/env bash
# Envoie build/interphone-homekit.bin par OTA puis suit les logs.
# Usage : tools/ota.sh [hote]        (defaut interphone.local ; 192.168.4.1 via le SoftAP)
# Token : variable OTA_TOKEN, sinon lu dans sdkconfig (CONFIG_APP_OTA_TOKEN).
set -euo pipefail
HOST="${1:-interphone.local}"
cd "$(dirname "$0")/.."

TOKEN="${OTA_TOKEN:-$(sed -n 's/^CONFIG_APP_OTA_TOKEN="\(.*\)"$/\1/p' sdkconfig 2>/dev/null || true)}"
[ -n "$TOKEN" ] || { echo "token OTA introuvable (OTA_TOKEN ou sdkconfig)"; exit 1; }

if command -v idf.py >/dev/null 2>&1; then
    idf.py build
fi
BIN=build/interphone-homekit.bin
[ -f "$BIN" ] || { echo "$BIN introuvable : lancer idf.py build"; exit 1; }

echo "Envoi de $BIN ($(stat -c %s "$BIN") octets) vers $HOST:8080 ..."
curl -sS -X POST -H "Authorization: Bearer $TOKEN" -H "Expect:" \
     --data-binary @"$BIN" "http://$HOST:8080/ota"
echo
echo "Suivi des logs dans 3 s (Ctrl-C pour quitter)"
sleep 3
python tools/logs.py "$HOST"
```
Puis `git update-index --chmod=+x tools/ota.sh` après `git add`.

- [ ] **Step 3 : README**

`README.md` :
```markdown
# Interphone Koch TC:Bus → HomeKit

Firmware ESP32-C3 (ESP-IDF v5.5.5, C11) qui relie un interphone Koch TC:Bus à Apple HomeKit.
Feuille de route : `docs/roadmap.md`. Designs : `docs/superpowers/specs/`.

**État : phase 0** (socle distant + sniffer). L'émission sur le bus et HomeKit arrivent par OTA
dans les phases suivantes.

## Avant le premier flash (une seule fois, par USB)

1. Activer ESP-IDF : `. "$HOME\esp\esp-idf\export.ps1"` (PowerShell).
2. Générer la clé de signature et **la sauvegarder hors du PC** (voir `keys/README.md`) :
   `idf.py secure-generate-signing-key --version 2 --scheme rsa3072 keys/signing_key.pem`
3. `idf.py set-target esp32c3`, puis `idf.py menuconfig` → menu **Interphone** :
   - SSID et mot de passe Wi-Fi (2,4 GHz) ;
   - token OTA, au moins 32 caractères (`openssl rand -hex 24`) ;
   - mot de passe du SoftAP de secours, au moins 8 caractères. **Sans lui, pas de secours si la box change.**
4. `idf.py build flash monitor`.

## Ensuite, tout passe par le réseau

- Logs : `python tools/logs.py` (ou `nc interphone.local 3333`).
- État : `curl -H "Authorization: Bearer $TOKEN" http://interphone.local:8080/info`
- Mise à jour : incrémenter `version.txt`, puis `tools/ota.sh` (Git Bash).
- Redémarrage : `curl -X POST -H "Authorization: Bearer $TOKEN" http://interphone.local:8080/reboot`
- Si la carte ne trouve plus le Wi-Fi pendant 10 min, elle ouvre le réseau `interphone-recovery` ;
  les mêmes commandes marchent alors sur `192.168.4.1`.

## Tests sur PC

```bash
export PATH="/c/Users/<vous>/AppData/Local/Programs/CLion/bin/mingw/bin:$PATH"
mingw32-make -C test/host test
```

## Sécurité du bus

Le bus est partagé par tout l'immeuble. GPIO10 est forcé à 0 en première ligne de `app_main`,
l'émission n'est pas compilée en phase 0, et seules les commandes `OPEN_DOOR` et `LIGHT` seront
jamais autorisées (liste blanche dans `tc_proto.c`).

## Crédits

Le protocole TC:Bus a été rétro-ingénié par la communauté. Ce projet est une implémentation
originale (MIT) qui s'appuie sur leurs travaux comme référence technique :
[Doorman](https://github.com/AzonInc/Doorman) (GPL-3.0),
[TCSintercomArduino](https://github.com/atc1441/TCSintercomArduino),
[peteh/doorman](https://github.com/peteh/doorman) (GPL-3.0).
HomeKit : [esp-homekit-sdk](https://github.com/espressif/esp-homekit-sdk) (Espressif).
```

- [ ] **Step 4 : build, taille, tests PC**

PowerShell :
```powershell
idf.py build
idf.py size
```
Attendu : `Project build complete.` ; dans `idf.py size`, la taille totale de l'image est très
inférieure à 1600 Ko (ordre de grandeur attendu : 700 à 900 Ko). Noter la valeur pour le commit.

Git Bash : `mingw32-make -C test/host test` → `0 failures` partout.

- [ ] **Step 5 : revue « aucun secret dans les logs »**

```bash
grep -rn "CONFIG_APP_WIFI_PASSWORD\|CONFIG_APP_OTA_TOKEN\|CONFIG_APP_RECOVERY_AP_PASSWORD" main components | grep -i "LOG"
```
Attendu : aucune ligne (les seules utilisations sont `strlcpy`, `strlen`, `ota_auth_check`).

- [ ] **Step 6 : commit**

```bash
git add -A
git update-index --chmod=+x tools/ota.sh
git commit -m "feat: main final (sequence de demarrage, battement de coeur), outils ota.sh/logs.py, README

Taille de l'image : <valeur de idf.py size> / 1600 Ko

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 15 : Validation sur l'établi (avec l'utilisateur)

Cette tâche n'écrit pas de code. Elle déroule la spec §13.3. Chaque étape est faite par
l'utilisateur sur la carte **débranchée de la plaque d'interface**, alimentée par l'USB.
Prérequis : `menuconfig` renseigné (SSID, mot de passe, token ≥ 32, mot de passe SoftAP ≥ 8),
clé sauvegardée. Sans ces secrets, la compilation échoue volontairement (`app_config_check.c`).

Dans chaque terminal :
```bash
. ~/esp/esp-idf/export.sh
cd ~/CLionProjects/Doorman
export OTA_TOKEN=$(sed -n 's/^CONFIG_APP_OTA_TOKEN="\(.*\)"$/\1/p' sdkconfig)
```

- [ ] **Étape 1 : premier flash et logs**

```bash
idf.py build
idf.py -p /dev/ttyACM0 erase-flash flash monitor
```
Attendu sur la console : `interphone-homekit 0.1.0 | partition ota_0 (none) | reset poweron`
(`none` : pas encore d'entrée otadata après un flash USB), le tableau `gpio_dump` (GPIO4 entrée
sans pull, GPIO10 sortie à 0), `station demarree`, `connecte, IP ...`, `serveur de logs sur le port 3333`,
`serveur HTTP sur le port 8080`, `reception RMT sur GPIO4`, `image deja validee, pas d'auto-test`.

Depuis le PC :
```bash
python3 tools/logs.py                                   # historique puis flux
curl -H "Authorization: Bearer $OTA_TOKEN" http://interphone.local:8080/info
curl http://interphone.local:8080/info                  # attendu : 401
```
Si `interphone.local` ne résout pas, utiliser l'IP affichée dans les logs.

- [ ] **Étape 2 : OTA valide**

`version.txt` → `0.1.1`, puis `tools/ota.sh`. Attendu : réponse `{"status":"ok",...}`, logs :
`OTA OK, redemarrage dans 1 s`, puis après reboot `interphone-homekit 0.1.1 | partition ota_1 (pending_verify)`,
`nouvelle image en attente : auto-test`, `SELFTEST OK, image validee`. `/info` : `"version":"0.1.1"`,
`"ota_state":"valid"`.

- [ ] **Étape 3 : image non signée ou altérée refusée**

Image altérée (un octet du corps de l'image modifié ; modifier un des derniers octets ne prouverait
rien : ils tombent dans le bourrage du secteur de signature, qui n'est pas vérifié) :
```bash
python3 - <<'EOF'
d = bytearray(open("build/interphone-homekit.bin", "rb").read())
d[0x2000] ^= 0xFF
open("build/tampered.bin", "wb").write(d)
EOF
curl -i -X POST -H "Authorization: Bearer $OTA_TOKEN" -H "Expect:" --data-binary @build/tampered.bin http://interphone.local:8080/ota
```
Attendu : `400 Bad Request`, `image invalid or unsigned`, logs `OTA echouee`, carte toujours en 0.1.1.

Image sans bloc de signature (les 4 derniers Ko retirés) :
```bash
head -c -4096 build/interphone-homekit.bin > build/unsigned.bin
curl -i -X POST -H "Authorization: Bearer $OTA_TOKEN" -H "Expect:" --data-binary @build/unsigned.bin http://interphone.local:8080/ota
```
Attendu : `400 Bad Request`.

- [ ] **Étape 4 : rollback automatique**

`menuconfig` → Interphone → OTA et auto-test → activer **forcer l'échec**. `version.txt` → `0.1.2`,
`tools/ota.sh`. Attendu dans les logs (reconnexion automatique de `logs.py`) :
`interphone-homekit 0.1.2 | partition ota_0 (pending_verify)`, `SELFTEST FAILED (echec force ...)`,
reboot, puis `interphone-homekit 0.1.1 | partition ota_1 (valid)`. `/info` : `"version":"0.1.1"`,
`"other_state":"invalid"`. **Désactiver l'option** ensuite.

- [ ] **Étape 5 : SoftAP de secours**

`menuconfig` : mot de passe Wi-Fi volontairement faux, délai SoftAP `60`, délai auto-test `180`,
et activer **APP_BENCH_TEST** (OTA et auto-test) : sans lui, la compilation refuse un auto-test plus
long que le délai du SoftAP. `version.txt` → `0.1.3`, `tools/ota.sh`. Attendu : la carte redémarre,
ne se connecte pas, le log série montre `image de test APP_BENCH_TEST` (attendu pour ce test) puis,
après 60 s, `SoftAP de secours 'interphone-recovery' demarre`, puis `SELFTEST OK`. Connecter le PC
au réseau `interphone-recovery`, puis :
```bash
curl -H "Authorization: Bearer $OTA_TOKEN" http://192.168.4.1:8080/info      # "net":"recovery"
```
Remettre le bon mot de passe, les délais par défaut (600 / 90) et **désactiver APP_BENCH_TEST**
(une image installée ne doit jamais l'avoir), `version.txt` → `0.1.4`, `tools/ota.sh 192.168.4.1`.
Attendu : OTA acceptée, reboot, connexion à la box, `SELFTEST OK`, `SoftAP de secours arrete`.
Une pause de 1 à 2 s du flux pendant les tentatives de reconnexion de la station est normale.

- [ ] **Étape 6 : clôture**

Commit du résultat (valeurs de `/info`, taille, remarques) dans `docs/phase0-validation.md`,
puis installation de la carte près de l'interphone. La phase 1 commence par son propre design.

- [ ] **Étape 6 bis : vérifications complémentaires**

À consigner aussi dans `docs/phase0-validation.md` :

- GPIO10 jamais à l'état haut depuis la mise sous tension, pendant le boot, une OTA et un
  redémarrage sur panic : oscilloscope ou analyseur logique si disponible ; au minimum multimètre
  et sortie `gpio_dump` du boot (GPIO4 entrée sans pull, GPIO10 sortie à 0).
- Une fois câblé, environ 2,9 V au repos sur GPIO4.
- Image qui plante en boucle (`abort()` 5 s après le boot) envoyée par OTA : retour à la version
  précédente, `/info` montre `other_state` `aborted`.
- Coupure d'alimentation pendant l'auto-test d'une nouvelle image : rollback au redémarrage.
- Ctrl-C sur `curl` en plein envoi : l'OTA suivante fonctionne normalement.
- Vrai test du SoftAP à 600 s, box éteinte : OTA complète via `192.168.4.1`, chronométrée.
- Marges de pile (`HB pile libre`) après une OTA et après démarrage/arrêt du SoftAP : au moins 512 o
  pour chaque tâche.
- Tas (`heap`, `min_heap`) après le boot, après une OTA, après le SoftAP.
- Rechercher dans une capture complète de `logs.py` le token et les deux mots de passe
  (`grep -F "$OTA_TOKEN" capture.log`, idem pour les mots de passe) : aucune occurrence.
- Client de logs fantôme (portable mis en veille avec `logs.py` ouvert) : mesurer le temps avant
  qu'un nouveau client soit accepté.
- À l'installation, raccordement passif au bus : surveiller le rythme de `bus_err` dans les
  battements de cœur et vérifier que `/info` et l'OTA restent réactifs.

---

## Auto-revue du plan

- Chaque tâche compile seule (stubs explicites aux tâches 1, 4, 7, 8, 9) et se termine par un commit.
- Les trois fichiers purs sont testés avant tout code ESP-IDF qui les utilise (tâches 2 à 6 avant 7 et 13).
- Spec §3 : GPIO10 à 0 en première ligne (tâche 1), TX non compilé avec `#error` de garde (tâche 9/13),
  liste blanche testée (tâche 6), blocage TX appelé par l'OTA (tâche 10).
- Spec §7.2 : minuterie de secours armée au boot et sur perte d'une connexion établie seulement (tâche 8),
  arrêt différé pendant une OTA (tâches 10 et 11).
- Spec §9 : codes 401/503/409/413/400/500, `ota_state` et `other_state` lus en direct (tâche 9 et 10).
- Spec §10 : auto-test « station ou SoftAP », `FORCE_FAIL` après conditions remplies, validation forcée
  si rollback impossible, avertissement d'invariant au boot (tâche 12).
- Spec §13.3 : les cinq tests sont déroulés à la tâche 15 ; le test 3 utilise une image altérée et une
  image tronquée plutôt qu'un second dossier de build, ce qui teste la même vérification de signature
  avec moins de manipulations.
