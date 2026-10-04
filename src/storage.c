#include <dirent.h>
#include "esp_littlefs.h"
#include "esp_log.h"
#include "storage.h"

static const char *TAG = "storage";

static int count_entries(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) {
        return -1;
    }
    int n = 0;
    while (readdir(dir)) {
        n++;
    }
    closedir(dir);
    return n;
}

bool storage_init(void)
{
    const esp_vfs_littlefs_conf_t conf = {
        .base_path = STORAGE_ROOT,
        .partition_label = "storage",
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(err));
        return false;
    }
    size_t total = 0, used = 0;
    esp_littlefs_info(conf.partition_label, &total, &used);
    ESP_LOGI(TAG, "LittleFS mounted at %s: %u KB used of %u KB", STORAGE_ROOT,
             (unsigned)(used / 1024), (unsigned)(total / 1024));
    ESP_LOGI(TAG, "%s/plugiator/minimax: %d patches", STORAGE_ROOT,
             count_entries(STORAGE_ROOT "/plugiator/minimax"));
    return true;
}
