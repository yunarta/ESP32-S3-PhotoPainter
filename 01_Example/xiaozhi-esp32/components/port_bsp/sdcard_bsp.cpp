#include <stdio.h>
#include <assert.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <dirent.h>
#include <string.h>
#include <strings.h>
#include <algorithm>
#include <string>
#include <vector>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include "sdcard_bsp.h"

CustomSDPort::CustomSDPort(const char *SdName,int clk,int cmd,int d0,int d1,int d2,int d3,int width) :
SdName_(SdName)
{
    ScanListHandle = list_new();

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed           = false;
    mount_config.max_files                        = 5;
    mount_config.allocation_unit_size             = 16 * 1024 * 3;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width               = width;
    slot_config.clk                 = (gpio_num_t)clk;
    slot_config.cmd                 = (gpio_num_t)cmd;
    slot_config.d0                  = (gpio_num_t)d0;
    slot_config.d1                  = (gpio_num_t)d1;
    slot_config.d2                  = (gpio_num_t)d2;
    slot_config.d3                  = (gpio_num_t)d3;

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_vfs_fat_sdmmc_mount(SdName_, &host, &slot_config, &mount_config, &sdcard_host));

    if (sdcard_host != NULL) {
        sdmmc_card_print_info(stdout, sdcard_host);
        is_SdcardInitOK = 1;
    } else {
        is_SdcardInitOK = 0;
    }
}

CustomSDPort::~CustomSDPort() {
    SDPort_ClearScanList();
    if (ScanListHandle != NULL) {
        list_destroy(ScanListHandle);
        ScanListHandle = NULL;
    }
}

int CustomSDPort::SDPort_WriteFile(const char *path, const void *data, size_t data_len) {
    if (sdcard_host == NULL) {
        ESP_LOGE(TAG, "SD card not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (sdmmc_get_status(sdcard_host) != ESP_OK) {
        ESP_LOGE(TAG, "SD card not ready");
        return ESP_FAIL;
    }

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    size_t written = fwrite(data, 1, data_len, f);
    fclose(f);

    if (written != data_len) {
        ESP_LOGE(TAG, "Write failed (%zu/%zu bytes)", written, data_len);
        return ESP_FAIL;
    }
    return ESP_OK;
}

int CustomSDPort::SDPort_ReadFile(const char *path, uint8_t *buffer, size_t *outLen) {
    if (sdcard_host == NULL) {
        ESP_LOGE(TAG, "SD card not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (sdmmc_get_status(sdcard_host) != ESP_OK) {
        ESP_LOGE(TAG, "SD card not ready");
        return ESP_FAIL;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    if (file_size <= 0) {
        ESP_LOGE(TAG, "Invalid file size");
        fclose(f);
        return ESP_FAIL;
    }
    fseek(f, 0, SEEK_SET);

    size_t bytes_read = fread(buffer, 1, file_size, f);
    fclose(f);

    if (outLen) *outLen = bytes_read;
    return (bytes_read > 0) ? ESP_OK : ESP_FAIL;
}

int CustomSDPort::SDPort_ReadOffset(const char *path, void *buffer, size_t len, size_t offset) {
    if (sdcard_host == NULL) {
        ESP_LOGE(TAG, "SD card not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (sdmmc_get_status(sdcard_host) != ESP_OK) {
        ESP_LOGE(TAG, "SD card not ready");
        return ESP_FAIL;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    fseek(f, offset, SEEK_SET);
    size_t bytes_read = fread(buffer, 1, len, f);
    fclose(f);
    return bytes_read;
}

int CustomSDPort::SDPort_WriteOffset(const char *path, const void *data, size_t len, bool append) {
    if (sdcard_host == NULL) {
        ESP_LOGE(TAG, "SD card not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (sdmmc_get_status(sdcard_host) != ESP_OK) {
        ESP_LOGE(TAG, "SD card not ready");
        return ESP_FAIL;
    }

    const char *mode = append ? "ab" : "wb";
    FILE *f = fopen(path, mode);
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    size_t bytes_written = fwrite(data, 1, len, f);
    fclose(f);

    if (!append && len == 0) {
        ESP_LOGI(TAG, "File cleared: %s", path);
        return ESP_OK;
    }
    return bytes_written;
}

sdmmc_card_t* CustomSDPort::SDPort_GetSdMMCHost() {
    return sdcard_host;
}

int CustomSDPort::SDPort_GetScanListValue(void) {
    int              Quantity = 0;
    list_iterator_t *it       = list_iterator_new(ScanListHandle, LIST_HEAD); 
    list_node_t     *node     = list_iterator_next(it);
    while (node != NULL) {
        CustomSDPortNode_t *sdcard_node = (CustomSDPortNode_t *) node->val;
        ESP_LOGI(TAG, "File: %s", sdcard_node->sdcard_name);
        node = list_iterator_next(it);
        Quantity++;
    }
    list_iterator_destroy(it); 
    return Quantity;
}

void CustomSDPort::SDPort_ClearScanList() {
    if (ScanListHandle == NULL) {
        return;
    }

    list_node_t *node = NULL;
    while ((node = list_lpop(ScanListHandle)) != NULL) {
        if (node->val != NULL) {
            LIST_FREE(node->val);
        }
        LIST_FREE(node);
    }
    CurrentlyNode = NULL;
    ImgValue = 0;
}

typedef struct {
    std::string path;
    time_t mtime;
} SdImageEntry_t;

static bool IsImageFileName(const char *name) {
    if (strstr(name, "sys_decode.bmp") != NULL) {
        return false;
    }

    const char *ext = strrchr(name, '.');
    if (ext == NULL) {
        return false;
    }
    return strcasecmp(ext, ".bmp") == 0 || strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".png") == 0;
}

static void ShuffleSdImages(std::vector<SdImageEntry_t> &images) {
    for (size_t i = images.size(); i > 1; --i) {
        size_t j = esp_random() % i;
        std::swap(images[i - 1], images[j]);
    }
}

static void CollectSdImages(const char *path, bool recursive, std::vector<SdImageEntry_t> &images) {
    struct dirent *entry;
    DIR           *dir = opendir(path);

    if (dir == NULL) {
        ESP_LOGE("SDPort", "Failed to open directory: %s", path);
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        std::string full_path = std::string(path) + "/" + entry->d_name;
        struct stat st = {};
        if (stat(full_path.c_str(), &st) != 0) {
            ESP_LOGE("SDPort", "Failed to stat: %s", full_path.c_str());
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            ESP_LOGI("SDPort", "Directory: %s", full_path.c_str());
            if (recursive) {
                CollectSdImages(full_path.c_str(), recursive, images);
            }
            continue;
        }

        if (!S_ISREG(st.st_mode) || !IsImageFileName(entry->d_name)) {
            continue;
        }

        if (full_path.size() >= sizeof(((CustomSDPortNode_t *)0)->sdcard_name)) {
            ESP_LOGE("SDPort", "scan file path too long:%d", static_cast<int>(full_path.size()));
            continue;
        }
        images.push_back({full_path, st.st_mtime});
    }
    closedir(dir);
}

void CustomSDPort::SDPort_ScanListDir(const char *path) {
    SDPort_ScanListDir(path, false, 0, 0);
}

void CustomSDPort::SDPort_ScanListDir(const char *path, bool recursive, int order_mode, int recent_limit) {
    SDPort_ClearScanList();

    std::vector<SdImageEntry_t> images;
    CollectSdImages(path, recursive, images);

    if (order_mode == 1 || order_mode == 4) {
        std::sort(images.begin(), images.end(), [](const SdImageEntry_t &a, const SdImageEntry_t &b) {
            return a.mtime > b.mtime;
        });
    } else if (order_mode == 2) {
        std::sort(images.begin(), images.end(), [](const SdImageEntry_t &a, const SdImageEntry_t &b) {
            return a.mtime < b.mtime;
        });
    } else if (order_mode == 3) {
        ShuffleSdImages(images);
    }

    if (order_mode == 4) {
        if (recent_limit > 0 && images.size() > static_cast<size_t>(recent_limit)) {
            images.resize(recent_limit);
        }
        ShuffleSdImages(images);
    } else if (recent_limit > 0 && images.size() > static_cast<size_t>(recent_limit)) {
        images.resize(recent_limit);
    }

    for (const auto &image : images) {
        CustomSDPortNode_t *node_data = (CustomSDPortNode_t *) LIST_MALLOC(sizeof(CustomSDPortNode_t));
        assert(node_data);
        snprintf(node_data->sdcard_name, sizeof(node_data->sdcard_name), "%s", image.path.c_str());
        list_rpush(ScanListHandle, list_node_new(node_data));
        ESP_LOGW("Scan_Dir", "DirDoc:%s,size:%d", node_data->sdcard_name, strlen(node_data->sdcard_name));
        ImgValue++;
    }
}

list_t* CustomSDPort::SDPort_GetListHost() {
    return ScanListHandle;
}

int CustomSDPort::SDPort_GetSdcardInitOK() {
    return is_SdcardInitOK;
}

void CustomSDPort::SDPort_SetCurrentlyNode(list_node_t *node) {
    CurrentlyNode = node;
}

list_node_t* CustomSDPort::SDPort_GetCurrentlyNode(void) {
    return CurrentlyNode;
}

uint16_t CustomSDPort::Get_Sdcard_ImgValue(void) {
    return ImgValue;
}
