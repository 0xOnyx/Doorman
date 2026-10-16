/*
 * Accessoire HomeKit (spec phase 3) : sonnette et serrure de la porte de rue.
 *
 * Taches et contextes :
 * - serveur HAP du SDK (tache "httpd", port CONFIG_HAP_HTTP_SERVER_PORT) : appelle lock_write ;
 *   ne doit jamais bloquer -> la demande d'ouverture est postee dans s_queue (timeout 0).
 * - tache "hk_lock" (pile 4 Ko) : seule a appeler tc_bus_open_door (bloquant ~3,7 s au plus),
 *   puis tient l'etat "deverrouillee" 5 s (ou "bloquee" 3 s en cas de refus) avant de reverrouiller.
 *   Elle reactive aussi le mode appairage expire (hap_pair_setup_re_enable).
 * - tache de reception tc_bus : app_homekit_on_telegram -> hap_char_update_val (non bloquant :
 *   le SDK ecrit la valeur puis poste la notification dans ses files avec un delai nul).
 * - minuteur FreeRTOS du SDK (expiration du mode appairage) : hk_event ne fait que poster.
 *
 * File s_queue de profondeur 2 : au plus une ouverture en attente (garde s_busy, une demande
 * pendant une ouverture est acceptee sans nouvelle emission) + la reactivation de l'appairage.
 */
#include "app_homekit.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "mdns.h"
#include "hap.h"
#include "hap_apple_servs.h"
#include "hap_apple_chars.h"
#include "tc_bus.h"
#include "hk_code.h"

static const char *TAG = "homekit";

#define HK_SERV_UUID_DOORBELL   "121"            /* pas de hap_serv_doorbell_create dans le SDK */
#define HK_RING_DEBOUNCE_US     (5LL * 1000000)  /* au plus une notification de sonnette / 5 s */
#define HK_UNLOCKED_MS          5000             /* porte affichee ouverte apres une emission */
#define HK_JAMMED_MS            3000             /* serrure affichee bloquee apres un refus */
#define HK_TASK_STACK           4096
#define HK_TASK_PRIO            5
#define HK_QUEUE_LEN            2

/* Lock Current State : 0 deverrouillee, 1 verrouillee, 2 bloquee ; Lock Target State : 0 / 1 */
#define LOCK_UNSECURED          0
#define LOCK_SECURED            1
#define LOCK_JAMMED             2

typedef enum {
    HK_CMD_OPEN = 1,
    HK_CMD_PAIRING_REENABLE,
} hk_cmd_t;

static hap_char_t *s_ring_event;      /* Programmable Switch Event du service Doorbell */
static hap_char_t *s_lock_current;
static hap_char_t *s_lock_target;
static QueueHandle_t s_queue;
static volatile bool s_started;
static bool s_busy;                   /* ouverture en cours ou fenetre "deverrouillee" : sous s_lock */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_last_ring_us;        /* tache de reception tc_bus uniquement */

static void set_u8(hap_char_t *hc, uint32_t v)
{
    hap_val_t val = { .u = v };
    if (hap_char_update_val(hc, &val) != HAP_SUCCESS) {
        ESP_LOGW(TAG, "mise a jour d'une caracteristique refusee (%" PRIu32 ")", v);
    }
}

static void log_pairing_state(const char *what)
{
    int n = hap_get_paired_controller_count();
    if (n > 0) {
        ESP_LOGI(TAG, "%s : appaire (%d)", what, n);
    } else {
        ESP_LOGI(TAG, "%s : non appaire", what);
    }
}

/* --- serrure --- */

/* Une seule ouverture a la fois. false si la file refuse (ne devrait pas arriver). */
static bool request_open(bool *already)
{
    taskENTER_CRITICAL(&s_lock);
    *already = s_busy;
    s_busy = true;
    taskEXIT_CRITICAL(&s_lock);
    if (*already) {
        return true;
    }
    hk_cmd_t c = HK_CMD_OPEN;
    if (xQueueSend(s_queue, &c, 0) != pdTRUE) {
        taskENTER_CRITICAL(&s_lock);
        s_busy = false;
        taskEXIT_CRITICAL(&s_lock);
        return false;
    }
    return true;
}

/* Serveur HAP : repond aussitot, l'emission se fait dans hk_lock. */
static int lock_write(hap_write_data_t write_data[], int count, void *serv_priv, void *write_priv)
{
    (void)serv_priv;
    (void)write_priv;
    int ret = HAP_SUCCESS;
    for (int i = 0; i < count; i++) {
        hap_write_data_t *w = &write_data[i];
        if (w->hc != s_lock_target) {
            *(w->status) = HAP_STATUS_RES_ABSENT;
            ret = HAP_FAIL;
            continue;
        }
        if (w->val.u == LOCK_UNSECURED) {
            bool already;
            if (!request_open(&already)) {
                ESP_LOGE(TAG, "ouverture demandee : file pleine, refusee");
                *(w->status) = HAP_STATUS_RES_BUSY;
                ret = HAP_FAIL;
                continue;
            }
            ESP_LOGI(TAG, "ouverture demandee depuis HomeKit%s%s", w->remote ? " (a distance)" : "",
                     already ? " : deja en cours, pas de nouvelle emission" : "");
        } else if (w->val.u == LOCK_SECURED) {
            /* rien a emettre : la porte se reverrouille seule */
            ESP_LOGI(TAG, "verrouillage demande depuis HomeKit : rien a emettre");
        } else {
            *(w->status) = HAP_STATUS_VAL_INVALID;
            ret = HAP_FAIL;
            continue;
        }
        *(w->status) = HAP_STATUS_SUCCESS;
        hap_char_update_val(w->hc, &w->val);
    }
    return ret;
}

static void do_open(void)
{
    uint32_t hold_ms;
#if CONFIG_TC_BUS_TX_ENABLE
    tc_bus_tx_result_t r;
    esp_err_t e = tc_bus_open_door(&r);
    if (e == ESP_OK) {
        ESP_LOGI(TAG, "porte ouverte depuis HomeKit (0x%0*lX, echo %s, reponse %s)", r.is_long ? 8 : 4,
                 (unsigned long)r.raw, r.echo ? "oui" : "non", r.reply ? "oui" : "non");
        set_u8(s_lock_current, LOCK_UNSECURED);
        hold_ms = HK_UNLOCKED_MS;
    } else {
        ESP_LOGW(TAG, "ouverture depuis HomeKit refusee : %s",
                 e == ESP_ERR_INVALID_STATE ? tc_tx_verdict_name(r.verdict) : esp_err_to_name(e));
        set_u8(s_lock_current, LOCK_JAMMED);
        hold_ms = HK_JAMMED_MS;
    }
#else
    ESP_LOGW(TAG, "ouverture depuis HomeKit impossible : emission non compilee (TC_BUS_TX_ENABLE)");
    set_u8(s_lock_current, LOCK_JAMMED);
    hold_ms = HK_JAMMED_MS;
#endif
    vTaskDelay(pdMS_TO_TICKS(hold_ms));
    set_u8(s_lock_target, LOCK_SECURED);
    set_u8(s_lock_current, LOCK_SECURED);
    taskENTER_CRITICAL(&s_lock);
    s_busy = false;
    taskEXIT_CRITICAL(&s_lock);
}

static void hk_task(void *arg)
{
    (void)arg;
    hk_cmd_t c;
    for (;;) {
        if (xQueueReceive(s_queue, &c, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (c) {
        case HK_CMD_OPEN:
            do_open();
            break;
        case HK_CMD_PAIRING_REENABLE:
            if (hap_get_paired_controller_count() == 0) {
                ESP_LOGI(TAG, "mode appairage expire (10 min) : reactive");
                hap_pair_setup_re_enable();
            }
            break;
        default:
            break;
        }
    }
}

/* --- evenements du SDK --- */

/* Contextes varies (serveur HAP, boucle HAP, minuteur FreeRTOS a petite pile) : rien de long ici.
 * Ne logge jamais le code d'appairage ni l'identifiant des controleurs. */
static void hk_event(hap_event_t event, void *data)
{
    switch (event) {
    case HAP_EVENT_CTRL_PAIRED:
        log_pairing_state("controleur ajoute");
        break;
    case HAP_EVENT_CTRL_UNPAIRED:
        log_pairing_state("controleur retire");
        break;
    case HAP_EVENT_PAIRING_STARTED:
        ESP_LOGI(TAG, "appairage commence");
        break;
    case HAP_EVENT_PAIRING_ABORTED:
        ESP_LOGW(TAG, "appairage abandonne (code faux ou inactivite)");
        break;
    case HAP_EVENT_PAIRING_MODE_TIMED_OUT: {
        /* minuteur FreeRTOS : pas de log ni de mDNS ici, la tache hk_lock s'en charge */
        hk_cmd_t c = HK_CMD_PAIRING_REENABLE;
        xQueueSend(s_queue, &c, 0);
        break;
    }
    case HAP_EVENT_ACC_REBOOTING:
        ESP_LOGW(TAG, "redemarrage demande par HomeKit (%s)", data ? (const char *)data : "?");
        break;
    default:
        break;
    }
}

static int hk_identify(hap_acc_t *ha)
{
    (void)ha;
    ESP_LOGI(TAG, "identification demandee par l'app Maison");
    return HAP_SUCCESS;
}

/* --- sonnette --- */

void app_homekit_on_telegram(const tc_telegram_t *t)
{
    if (!s_started || !tc_proto_is_ring_for_me(t, CONFIG_TC_BUS_MY_SERIAL)) {
        return;
    }
    int64_t now = esp_timer_get_time();
    if (s_last_ring_us != 0 && now - s_last_ring_us < HK_RING_DEBOUNCE_US) {
        ESP_LOGI(TAG, "sonnerie %s repetee : pas de nouvelle notification", tc_type_name(t->type));
        return;
    }
    s_last_ring_us = now;
    hap_val_t v = { .u = 0 };            /* appui simple */
    int rc = hap_char_update_val(s_ring_event, &v);
    ESP_LOGI(TAG, "sonnerie %s : notification HomeKit %s", tc_type_name(t->type),
             rc == HAP_SUCCESS ? "envoyee" : "en echec");
}

/* --- demarrage --- */

/* fw_rev HomeKit : x[.y[.z]] ; on garde le debut chiffres-et-points de la version de l'image. */
static void fw_rev_from_version(char *out, size_t len)
{
    const char *v = esp_app_get_description()->version;
    size_t n = 0;
    while (v[n] != '\0' && n + 1 < len && ((v[n] >= '0' && v[n] <= '9') || v[n] == '.')) {
        out[n] = v[n];
        n++;
    }
    while (n > 0 && out[n - 1] == '.') {
        n--;
    }
    if (n == 0) {
        out[n++] = '0';
    }
    out[n] = '\0';
}

static hap_serv_t *create_doorbell(void)
{
    static const uint8_t valid_vals[] = { 0 };   /* appui simple seulement */
    hap_serv_t *hs = hap_serv_create(HK_SERV_UUID_DOORBELL);
    if (hs == NULL) {
        return NULL;
    }
    s_ring_event = hap_char_programmable_switch_event_create(0);
    if (s_ring_event == NULL) {
        hap_serv_delete(hs);
        return NULL;
    }
    /* pas hap_char_add_valid_vals_range : debordement d'un octet dans le SDK */
    hap_char_add_valid_vals(s_ring_event, valid_vals, sizeof valid_vals);
    if (hap_serv_add_char(hs, s_ring_event) != HAP_SUCCESS ||
        hap_serv_add_char(hs, hap_char_name_create("Sonnette")) != HAP_SUCCESS) {
        hap_serv_delete(hs);
        return NULL;
    }
    hap_serv_mark_primary(hs);
    return hs;
}

static hap_serv_t *create_lock(void)
{
    hap_serv_t *hs = hap_serv_lock_mechanism_create(LOCK_SECURED, LOCK_SECURED);
    if (hs == NULL) {
        return NULL;
    }
    if (hap_serv_add_char(hs, hap_char_name_create("Porte")) != HAP_SUCCESS) {
        hap_serv_delete(hs);
        return NULL;
    }
    s_lock_current = hap_serv_get_char_by_uuid(hs, HAP_CHAR_UUID_LOCK_CURRENT_STATE);
    s_lock_target = hap_serv_get_char_by_uuid(hs, HAP_CHAR_UUID_LOCK_TARGET_STATE);
    if (s_lock_current == NULL || s_lock_target == NULL) {
        hap_serv_delete(hs);
        return NULL;
    }
    hap_serv_set_write_cb(hs, lock_write);
    return hs;
}

esp_err_t app_homekit_start(void)
{
    /* double du garde-fou de compilation (hk_config_check.cpp) */
    if (!hk_setup_code_valid(CONFIG_APP_HK_SETUP_CODE) || !hk_setup_id_valid(CONFIG_APP_HK_SETUP_ID)) {
        ESP_LOGE(TAG, "code d'appairage ou setup ID invalide (menuconfig > Interphone > HomeKit) : "
                 "HomeKit non demarre");
        return ESP_ERR_INVALID_ARG;
    }
#if CONFIG_TC_BUS_MY_SERIAL == 0
    ESP_LOGW(TAG, "TC_BUS_MY_SERIAL non renseigne : la sonnette ne notifiera jamais");
#endif
#if !CONFIG_TC_BUS_TX_ENABLE
    ESP_LOGW(TAG, "emission non compilee (TC_BUS_TX_ENABLE) : la serrure ne pourra pas ouvrir");
#endif

    s_queue = xQueueCreate(HK_QUEUE_LEN, sizeof(hk_cmd_t));
    if (s_queue == NULL ||
        xTaskCreate(hk_task, "hk_lock", HK_TASK_STACK, NULL, HK_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "file ou tache hk_lock impossible : HomeKit non demarre");
        return ESP_ERR_NO_MEM;
    }

    if (hap_init(HAP_TRANSPORT_WIFI) != HAP_SUCCESS) {
        ESP_LOGE(TAG, "hap_init a echoue : HomeKit non demarre");
        return ESP_FAIL;
    }

    uint8_t mac[6] = { 0 };
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char serial[13];
    snprintf(serial, sizeof serial, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    char fw_rev[32];
    fw_rev_from_version(fw_rev, sizeof fw_rev);

    hap_acc_cfg_t cfg = {
        .name = CONFIG_APP_HK_NAME,
        .manufacturer = "DIY",
        .model = "Koch TC:Bus",
        .serial_num = serial,
        .fw_rev = fw_rev,
        .hw_rev = NULL,
        .pv = "1.1.0",
        .cid = HAP_CID_LOCK,
        .identify_routine = hk_identify,
    };
    hap_acc_t *acc = hap_acc_create(&cfg);
    if (acc == NULL) {
        ESP_LOGE(TAG, "hap_acc_create a echoue : HomeKit non demarre");
        return ESP_ERR_NO_MEM;
    }
    hap_serv_t *bell = create_doorbell();
    hap_serv_t *lock = create_lock();
    if (bell == NULL || lock == NULL ||
        hap_acc_add_serv(acc, bell) != HAP_SUCCESS || hap_acc_add_serv(acc, lock) != HAP_SUCCESS) {
        ESP_LOGE(TAG, "creation des services impossible : HomeKit non demarre");
        return ESP_ERR_NO_MEM;
    }
    hap_add_accessory(acc);

    hap_set_setup_code(CONFIG_APP_HK_SETUP_CODE);
    if (hap_set_setup_id(CONFIG_APP_HK_SETUP_ID) != HAP_SUCCESS) {
        ESP_LOGE(TAG, "setup ID refuse par le SDK : HomeKit non demarre");
        return ESP_ERR_INVALID_ARG;
    }
    hap_register_event_handler(hk_event);

    if (hap_start() != HAP_SUCCESS) {
        ESP_LOGE(TAG, "hap_start a echoue : HomeKit non demarre");
        return ESP_FAIL;
    }
    /* hap_start force le nom mDNS "MyHost" (esp_hap_mdns.c) : remettre le notre */
    esp_err_t e = mdns_hostname_set(CONFIG_APP_HOSTNAME);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "mdns_hostname_set: %s", esp_err_to_name(e));
    }
    s_started = true;
    ESP_LOGI(TAG, "HomeKit demarre (serveur HAP port %d, '%s', fw %s)", CONFIG_HAP_HTTP_SERVER_PORT,
             CONFIG_APP_HK_NAME, fw_rev);
    log_pairing_state("etat");
    return ESP_OK;
}

bool app_homekit_is_started(void)
{
    return s_started;
}

int app_homekit_paired_count(void)
{
    return s_started ? hap_get_paired_controller_count() : 0;
}

esp_err_t app_homekit_reset_pairings(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGW(TAG, "effacement des appairages HomeKit puis redemarrage");
    return hap_reset_pairings() == HAP_SUCCESS ? ESP_OK : ESP_FAIL;
}
