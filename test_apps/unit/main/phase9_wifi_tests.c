#include <string.h>

#include "nvs_flash.h"
#include "unity.h"
#include "wifi_station.h"

#define P9_WIFI_NAMESPACE "xj_wifitest"

static wifi_station_credentials_t p9_wifi_credentials(void)
{
    wifi_station_credentials_t credentials;
    memset(&credentials, 0, sizeof(credentials));
    strcpy(credentials.ssid, "Xiaojing-Test");
    strcpy(credentials.password, "test-pass-123");
    return credentials;
}

static void p9_wifi_nvs_ready(void)
{
    esp_err_t error = nvs_flash_init();
    TEST_ASSERT_TRUE(
        error == ESP_OK || error == ESP_ERR_INVALID_STATE);
    error = xiaojing_wifi_erase_credentials(P9_WIFI_NAMESPACE);
    TEST_ASSERT_EQUAL(ESP_OK, error);
}

TEST_CASE("VOICE-WIFI-P9W-01 credential bounds",
          "[voice][phase9][group_b][wifi]")
{
    wifi_station_credentials_t credentials = p9_wifi_credentials();
    TEST_ASSERT_TRUE(xiaojing_wifi_credentials_valid(&credentials));
    credentials.ssid[0] = '\0';
    TEST_ASSERT_FALSE(xiaojing_wifi_credentials_valid(&credentials));
    credentials = p9_wifi_credentials();
    strcpy(credentials.password, "short");
    TEST_ASSERT_FALSE(xiaojing_wifi_credentials_valid(&credentials));
    credentials = p9_wifi_credentials();
    strcpy(credentials.password, "bad\r\npassword");
    TEST_ASSERT_FALSE(xiaojing_wifi_credentials_valid(&credentials));
}

TEST_CASE("VOICE-WIFI-P9W-02 open network allowed",
          "[voice][phase9][group_b][wifi]")
{
    wifi_station_credentials_t credentials = p9_wifi_credentials();
    credentials.password[0] = '\0';
    TEST_ASSERT_TRUE(xiaojing_wifi_credentials_valid(&credentials));
}

TEST_CASE("VOICE-WIFI-P9W-03 NVS roundtrip and erase",
          "[voice][phase9][group_b][wifi][nvs]")
{
    p9_wifi_nvs_ready();
    wifi_station_credentials_t credentials = p9_wifi_credentials();
    TEST_ASSERT_EQUAL(
        ESP_OK,
        xiaojing_wifi_save_credentials(P9_WIFI_NAMESPACE, &credentials));
    wifi_station_credentials_t loaded;
    memset(&loaded, 0xa5, sizeof(loaded));
    TEST_ASSERT_EQUAL(
        ESP_OK,
        xiaojing_wifi_load_credentials(P9_WIFI_NAMESPACE, &loaded));
    TEST_ASSERT_EQUAL_STRING(credentials.ssid, loaded.ssid);
    TEST_ASSERT_EQUAL_STRING(credentials.password, loaded.password);
    TEST_ASSERT_EQUAL(
        ESP_OK, xiaojing_wifi_erase_credentials(P9_WIFI_NAMESPACE));
    TEST_ASSERT_EQUAL(
        ESP_ERR_NVS_NOT_FOUND,
        xiaojing_wifi_load_credentials(P9_WIFI_NAMESPACE, &loaded));
}

TEST_CASE("VOICE-WIFI-P9W-04 load failure leaves output untouched",
          "[voice][phase9][group_b][wifi][nvs]")
{
    p9_wifi_nvs_ready();
    wifi_station_credentials_t loaded;
    memset(&loaded, 0x5a, sizeof(loaded));
    wifi_station_credentials_t before = loaded;
    TEST_ASSERT_EQUAL(
        ESP_ERR_NVS_NOT_FOUND,
        xiaojing_wifi_load_credentials(P9_WIFI_NAMESPACE, &loaded));
    TEST_ASSERT_EQUAL_MEMORY(&before, &loaded, sizeof(loaded));
}

TEST_CASE("VOICE-WIFI-P9W-05 missing credentials fail closed",
          "[voice][phase9][group_b][wifi][lifecycle]")
{
    p9_wifi_nvs_ready();
    xiaojing_wifi_test_reset();
    wifi_station_config_t config = {
        .nvs_namespace = P9_WIFI_NAMESPACE,
        .connect_timeout_ms = 5000,
        .max_retries = 2,
    };
    TEST_ASSERT_EQUAL(ESP_OK, xiaojing_wifi_init(&config));
    wifi_station_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(ESP_OK, xiaojing_wifi_get_snapshot(&snapshot));
    TEST_ASSERT_FALSE(snapshot.provisioned);
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, xiaojing_wifi_start());
    TEST_ASSERT_FALSE(xiaojing_wifi_is_connected());
    TEST_ASSERT_EQUAL(ESP_OK, xiaojing_wifi_stop());
    xiaojing_wifi_test_reset();
}

TEST_CASE("VOICE-WIFI-P9W-06 async start allocates no task when unprovisioned",
          "[voice][phase9][group_b][wifi][lifecycle]")
{
    p9_wifi_nvs_ready();
    xiaojing_wifi_test_reset();
    wifi_station_config_t config = {
        .nvs_namespace = P9_WIFI_NAMESPACE,
        .connect_timeout_ms = 5000,
        .max_retries = 2,
    };
    TEST_ASSERT_EQUAL(ESP_OK, xiaojing_wifi_init(&config));
    TEST_ASSERT_EQUAL(
        ESP_ERR_NVS_NOT_FOUND, xiaojing_wifi_start_async());
    wifi_station_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(
        ESP_OK, xiaojing_wifi_get_snapshot(&snapshot));
    TEST_ASSERT_EQUAL(WIFI_STATION_INITIALIZED, snapshot.state);
    TEST_ASSERT_FALSE(snapshot.provisioned);
    TEST_ASSERT_FALSE(snapshot.connected);
    TEST_ASSERT_EQUAL(ESP_OK, xiaojing_wifi_stop());
    xiaojing_wifi_test_reset();
}

void phase9_wifi_tests_force_link(void)
{
}
