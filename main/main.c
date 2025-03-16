#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_bt_defs.h"

#define TAG "BLE_CLIENT"
#define TARGET_DEVICE_NAME ""
#define PIN_CODE ""

static esp_ble_scan_params_t ble_scan_params = {
    .scan_type = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval = 0x50, // 50 ms
    .scan_window = 0x25    // 25 ms
};

static esp_gatt_if_t gattc_if;
static uint16_t conn_id = 0;

// Função para formatar UUIDs em string hexadecimal
static void format_uuid(const esp_bt_uuid_t *uuid, char *uuid_str, size_t max_len) {
    if (uuid->len == ESP_UUID_LEN_16) {
        snprintf(uuid_str, max_len, "%04x", uuid->uuid.uuid16);
    } else if (uuid->len == ESP_UUID_LEN_32) {
        snprintf(uuid_str, max_len, "%08lx", (unsigned long)uuid->uuid.uuid32);
    } else if (uuid->len == ESP_UUID_LEN_128) {
        snprintf(uuid_str, max_len,
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 uuid->uuid.uuid128[15], uuid->uuid.uuid128[14], uuid->uuid.uuid128[13], uuid->uuid.uuid128[12],
                 uuid->uuid.uuid128[11], uuid->uuid.uuid128[10], uuid->uuid.uuid128[9], uuid->uuid.uuid128[8],
                 uuid->uuid.uuid128[7], uuid->uuid.uuid128[6], uuid->uuid.uuid128[5], uuid->uuid.uuid128[4],
                 uuid->uuid.uuid128[3], uuid->uuid.uuid128[2], uuid->uuid.uuid128[1], uuid->uuid.uuid128[0]);
    }
}

// Verifica se o dispositivo encontrado é o alvo
static bool is_target_device(const uint8_t *adv_data, uint8_t adv_data_len) {
    uint8_t length;
    uint8_t type;
    const uint8_t *data;

    for (int i = 0; i < adv_data_len;) {
        length = adv_data[i];
        if (length == 0) break;
        type = adv_data[i + 1];
        data = &adv_data[i + 2];

        if (type == ESP_BLE_AD_TYPE_NAME_CMPL || type == ESP_BLE_AD_TYPE_NAME_SHORT) {
            if (strncmp((const char *)data, TARGET_DEVICE_NAME, length - 1) == 0) {
                return true;
            }
        }
        i += length + 1;
    }
    return false;
}

// Listar todas as características de um serviço
static void list_all_characteristics(uint16_t start_handle, uint16_t end_handle) {
    uint16_t count = 0;
    esp_gattc_char_elem_t *char_elem_result = NULL;

    // Obter a contagem de características
    esp_err_t ret = esp_ble_gattc_get_attr_count(
        gattc_if, conn_id, ESP_GATT_DB_CHARACTERISTIC,
        start_handle, end_handle, 0x0000, &count
    );

    if (ret != ESP_OK || count == 0) {
        ESP_LOGE(TAG, "Nenhuma característica encontrada no serviço (handles 0x%04x - 0x%04x): %s", start_handle, end_handle, esp_err_to_name(ret));
        return;
    }

    // Alocar memória para as características
    char_elem_result = malloc(sizeof(esp_gattc_char_elem_t) * count);
    if (!char_elem_result) {
        ESP_LOGE(TAG, "Falha ao alocar memória para características.");
        return;
    }

    // Obter todas as características
    ret = esp_ble_gattc_get_all_char(gattc_if, conn_id, start_handle, end_handle, char_elem_result, &count, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao listar características: %s", esp_err_to_name(ret));
        free(char_elem_result);
        return;
    }

    // Exibir as características encontradas
    for (int i = 0; i < count; i++) {
        char uuid_str[37];
        format_uuid(&char_elem_result[i].uuid, uuid_str, sizeof(uuid_str));
        ESP_LOGI(TAG, "Característica encontrada: %s, handle=0x%04x",
                 uuid_str, char_elem_result[i].char_handle);
    }

    free(char_elem_result);
}

// Callback do GAP
static void gap_callback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    switch (event) {
        case ESP_GAP_BLE_SCAN_RESULT_EVT: {
            esp_ble_gap_cb_param_t *scan_result = param;
            if (scan_result->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
                if (is_target_device(scan_result->scan_rst.ble_adv, scan_result->scan_rst.adv_data_len)) {
                    ESP_LOGI(TAG, "Dispositivo alvo encontrado: %s", TARGET_DEVICE_NAME);
                    esp_ble_gap_stop_scanning();

                    esp_err_t ret = esp_ble_gattc_open(
                        gattc_if,
                        scan_result->scan_rst.bda,
                        scan_result->scan_rst.ble_addr_type,
                        true
                    );

                    if (ret != ESP_OK) {
                        ESP_LOGE(TAG, "Falha ao abrir conexão GATT: %s", esp_err_to_name(ret));
                    }
                }
            }
            break;
        }
        default:
            break;
    }
}

// Callback do GATTC
static void gattc_callback(esp_gattc_cb_event_t event, esp_gatt_if_t interface, esp_ble_gattc_cb_param_t *param) {
    switch (event) {
        case ESP_GATTC_REG_EVT:
            ESP_LOGI(TAG, "Cliente GATTC registrado, iniciando escaneamento.");
            gattc_if = interface;
            esp_ble_gap_start_scanning(60);
            break;
        case ESP_GATTC_OPEN_EVT:
            ESP_LOGI(TAG, "Conexão GATT aberta.");
            conn_id = param->open.conn_id;
            esp_ble_gattc_search_service(gattc_if, conn_id, NULL);
            break;
        case ESP_GATTC_SEARCH_RES_EVT: {
            char uuid_str[37];
            format_uuid(&param->search_res.srvc_id.uuid, uuid_str, sizeof(uuid_str));
            ESP_LOGI(TAG, "Serviço encontrado: UUID=%s, StartHandle=0x%04x, EndHandle=0x%04x",
                     uuid_str, param->search_res.start_handle, param->search_res.end_handle);

            // Listar todas as características do serviço encontrado
            list_all_characteristics(param->search_res.start_handle, param->search_res.end_handle);
            break;
        }
        case ESP_GATTC_SEARCH_CMPL_EVT:
            ESP_LOGI(TAG, "Busca de serviços concluída.");
            break;
        default:
            break;
    }
}

void app_main() {
    esp_err_t ret;

    // Inicializa NVS
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Libera memória do Bluetooth clássico
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    // Inicializa o controlador Bluetooth
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));

    // Inicializa Bluedroid
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    // Registra os callbacks do GAP e GATTC
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_callback));
    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(gattc_callback));
    ESP_ERROR_CHECK(esp_ble_gattc_app_register(0));

    // Configura os parâmetros de escaneamento
    ESP_ERROR_CHECK(esp_ble_gap_set_scan_params(&ble_scan_params));
}
