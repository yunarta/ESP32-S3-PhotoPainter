#include "application.h"
#include "button.h"
#include "codecs/box_audio_codec.h"
#include "config.h"
#include "wifi_board.h"
#include <wifi_station.h>

#include "power_save_timer.h"
#include "user_app.h"
#include <driver/i2c_master.h>
#include <esp_log.h>
#include <dirent.h>
#include <stdio.h>
#include <string>
#include <sys/stat.h>

#include "mcp_server.h"

#define TAG "esp-s3-PhotoPainter"

static const char *USER_AI_IMG_ROOT = "/sdcard/05_user_ai_img";
static std::string current_user_ai_img_dir = USER_AI_IMG_ROOT;

static bool IsSafeRelativeFolder(const std::string &folder) {
    if (folder.empty() || folder.size() > 48) {
        return false;
    }
    if (folder.find("..") != std::string::npos || folder.find('/') != std::string::npos || folder.find('\\') != std::string::npos) {
        return false;
    }
    return true;
}

static std::string ResolveUserAiImgPath(const std::string &folder) {
    if (folder.empty() || folder == "." || folder == "root") {
        return USER_AI_IMG_ROOT;
    }
    if (!IsSafeRelativeFolder(folder)) {
        return "";
    }
    return std::string(USER_AI_IMG_ROOT) + "/" + folder;
}

static bool DirectoryExists(const std::string &path) {
    struct stat st = {};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static bool FileExists(const std::string &path) {
    struct stat st = {};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static std::string ReadTextFile(const std::string &path, size_t max_len) {
    FILE *file = fopen(path.c_str(), "rb");
    if (file == NULL) {
        return "";
    }

    std::string content;
    content.resize(max_len);
    size_t read_len = fread(&content[0], 1, max_len, file);
    fclose(file);
    content.resize(read_len);
    return content;
}

static bool WriteTextFile(const std::string &path, const std::string &content) {
    FILE *file = fopen(path.c_str(), "wb");
    if (file == NULL) {
        return false;
    }
    size_t written = fwrite(content.data(), 1, content.size(), file);
    fclose(file);
    return written == content.size();
}

static bool TakeSdAccessLock() {
    if (epaper_gui_semapHandle == NULL) {
        return true;
    }
    return xSemaphoreTake(epaper_gui_semapHandle, pdMS_TO_TICKS(5000)) == pdTRUE;
}

static void GiveSdAccessLock() {
    if (epaper_gui_semapHandle != NULL) {
        xSemaphoreGive(epaper_gui_semapHandle);
    }
}

static std::string ListUserAiImgFolders() {
    DIR *dir = opendir(USER_AI_IMG_ROOT);
    if (dir == NULL) {
        return "Failed to open /sdcard/05_user_ai_img";
    }

    std::string result = "root";
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_DIR || entry->d_name[0] == '.') {
            continue;
        }
        std::string folder = entry->d_name;
        if (IsSafeRelativeFolder(folder)) {
            result += "\n" + folder;
        }
    }
    closedir(dir);
    return result;
}

class waveshare_PhotoPainter : public WifiBoard {
  private:
    i2c_master_bus_handle_t codec_i2c_bus_;
    Button                  boot_button_;
    PowerSaveTimer         *power_save_timer_;

    void InitializeCodecI2c() {
        ESP_ERROR_CHECK(i2c_master_get_bus_handle(0, &codec_i2c_bus_));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            ResetWifiConfiguration();
        });
    }

    void InitializeTools() {
        auto &mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.disp.SwitchPictures", "切换本地或 SD 卡中的图片，通过整数参数指定图片序号（如 “显示第 1 张图片”）。图片数量会从当前已扫描的 SD 文件夹动态校验。", PropertyList({Property("value", kPropertyTypeInteger)}), [this](const PropertyList &properties) -> ReturnValue {
            int value = properties["value"].value<int>();
            if (value < 1 || value > sdcard_bmp_Quantity) {
                return std::string("invalid image index: ") + std::to_string(value) + ", available images=" + std::to_string(sdcard_bmp_Quantity);
            }
            sdcard_doc_count = value;
            xEventGroupSetBits(epaper_groups, 0x02);        //  0000  0010
            return true;
        });

        mcp_server.AddTool("self.disp.getNumberimages", "获取 SD 卡中存储的图片文件总数，无输入参数，返回整数类型的图片数量", PropertyList(), [this](const PropertyList &) -> ReturnValue {
            xEventGroupSetBits(ai_IMG_Group, 0x02);       //Retrieve the images from the SD card
            if (xSemaphoreTake(ai_img_while_semap, pdMS_TO_TICKS(2000)) == pdTRUE) {
                return sdcard_bmp_Quantity;
            } else {
                return false;
            }
        });

        mcp_server.AddTool("self.disp.aiIMG", "这个是用户可以根据语音生成图片的(图片生成大概需要10-20s时间),比如：帮我生成一张动漫图片,直接生成就好，不要回复乱七八糟的东西", PropertyList(), [this](const PropertyList &) -> ReturnValue {
            ESP_LOGI("MCP", "进入MCP aiIMG");
            if (!is_ai_img) { //Indicates that the image is being refreshed
                ESP_LOGE("MCP", "is_ai_img fill %d", is_ai_img);
                return false;
            }
            xEventGroupSetBits(ai_IMG_Group, 0x01);         //Indicates that access to the volcano is permitted to obtain IMG.
            return true;
        });

        mcp_server.AddTool("self.disp.imgloop", "进入轮询播放图片模式,循环sd卡里面的图片", PropertyList(), [this](const PropertyList &) -> ReturnValue {
            ESP_LOGI("MCP", "进入imgloop");
            xEventGroupSetBits(ai_IMG_LoopGroup, 0x01); 
            return true;
        });

        mcp_server.AddTool("self.disp.imgloopEit", "退出轮询播放图片模式,不在循环sd卡里面的图片", PropertyList(), [this](const PropertyList &) -> ReturnValue {
            ESP_LOGI("MCP", "进入imgloopEit");
            xEventGroupClearBits(ai_IMG_LoopGroup, 0x01); 
            return true;
        });

        mcp_server.AddTool("self.disp.imgsetTimerloop min", "设置轮询间隔时间,单位是分钟", PropertyList({Property("timer", kPropertyTypeInteger, 1, 60)}), [this](const PropertyList &properties) -> ReturnValue {
            ESP_LOGI("MCP", "进入imgsetTimerloop");
            int value = properties["timer"].value<int>();
            ESP_LOGE("min timer", "%d", value);
            img_loopTimer = value * 60 * 1000;
            return true;
        });

        mcp_server.AddTool("self.disp.imgsetTimerloop h", "设置轮询间隔时间,单位是小时", PropertyList({Property("timer", kPropertyTypeInteger, 1, 240)}), [this](const PropertyList &properties) -> ReturnValue {
            ESP_LOGI("MCP", "进入imgsetTimerloop");
            int value = properties["timer"].value<int>();
            ESP_LOGE("h timer", "%d", value);
            img_loopTimer = value * 3600 * 1000;
            return true;
        });



        mcp_server.AddTool("self.disp.listImageFolders", "列出 /sdcard/05_user_ai_img 下可切换的图片文件夹。返回 root 代表 05_user_ai_img 根目录。", PropertyList(), [this](const PropertyList &) -> ReturnValue {
            if (!TakeSdAccessLock()) {
                return std::string("SD card is busy; try again");
            }
            std::string result = ListUserAiImgFolders();
            GiveSdAccessLock();
            return result;
        });

        mcp_server.AddTool("self.disp.listImages", "读取当前或指定用户图片文件夹里的图片清单，返回可传给 SwitchPictures 的序号和路径。folder 可填 root 或 05_user_ai_img 下的一级子文件夹名。", PropertyList({Property("folder", kPropertyTypeString, std::string(""))}), [this](const PropertyList &properties) -> ReturnValue {
            std::string folder = properties["folder"].value<std::string>();
            if (!TakeSdAccessLock()) {
                return std::string("SD card is busy; try again");
            }
            std::string path = folder.empty() ? current_user_ai_img_dir : ResolveUserAiImgPath(folder);
            if (path.empty() || !DirectoryExists(path)) {
                GiveSdAccessLock();
                return std::string("invalid folder: ") + folder;
            }

            SDPort->SDPort_ScanListDir(path.c_str());
            sdcard_bmp_Quantity = SDPort->SDPort_GetScanListValue();
            img_loopCount = sdcard_bmp_Quantity;
            current_user_ai_img_dir = path;

            std::string result = "folder=" + path + ", images=" + std::to_string(sdcard_bmp_Quantity);
            list_t *list = SDPort->SDPort_GetListHost();
            list_iterator_t *it = list_iterator_new(list, LIST_HEAD);
            list_node_t *node = NULL;
            int index = 1;
            while ((node = list_iterator_next(it)) != NULL) {
                CustomSDPortNode_t *image = (CustomSDPortNode_t *) node->val;
                result += "\n" + std::to_string(index++) + ": " + image->sdcard_name;
            }
            list_iterator_destroy(it);
            GiveSdAccessLock();
            return result;
        });

        mcp_server.AddTool("self.disp.setImageFolder", "切换要浏览和显示的用户图片文件夹。folder 填 root 或 05_user_ai_img 下的一级子文件夹名；切换后可用 SwitchPictures 显示指定序号。", PropertyList({Property("folder", kPropertyTypeString, std::string("root"))}), [this](const PropertyList &properties) -> ReturnValue {
            std::string folder = properties["folder"].value<std::string>();
            if (!TakeSdAccessLock()) {
                return std::string("SD card is busy; try again");
            }
            std::string path = ResolveUserAiImgPath(folder);
            if (path.empty() || !DirectoryExists(path)) {
                GiveSdAccessLock();
                return std::string("invalid folder: ") + folder;
            }

            SDPort->SDPort_ScanListDir(path.c_str());
            sdcard_bmp_Quantity = SDPort->SDPort_GetScanListValue();
            img_loopCount = sdcard_bmp_Quantity;
            current_user_ai_img_dir = path;
            std::string result = std::string("folder=") + path + ", images=" + std::to_string(sdcard_bmp_Quantity);
            GiveSdAccessLock();
            return result;
        });

        mcp_server.AddTool("self.disp.readFolderMeta", "读取当前或指定用户图片文件夹里的 META.md，用来理解这个文件夹图片内容。folder 可填 root 或 05_user_ai_img 下的一级子文件夹名。", PropertyList({Property("folder", kPropertyTypeString, std::string(""))}), [this](const PropertyList &properties) -> ReturnValue {
            std::string folder = properties["folder"].value<std::string>();
            if (!TakeSdAccessLock()) {
                return std::string("SD card is busy; try again");
            }
            std::string path = folder.empty() ? current_user_ai_img_dir : ResolveUserAiImgPath(folder);
            if (path.empty() || !DirectoryExists(path)) {
                GiveSdAccessLock();
                return std::string("invalid folder: ") + folder;
            }

            std::string meta_path = path + "/META.md";
            if (!FileExists(meta_path)) {
                GiveSdAccessLock();
                return std::string("META.md not found in ") + path;
            }
            std::string content = ReadTextFile(meta_path, 4096);
            GiveSdAccessLock();
            return content;
        });

        mcp_server.AddTool("self.disp.writeFolderMeta", "写入当前或指定用户图片文件夹里的 META.md。folder 可填 root 或 05_user_ai_img 下的一级子文件夹名；content 是要保存的 Markdown 内容。", PropertyList({Property("folder", kPropertyTypeString, std::string("")), Property("content", kPropertyTypeString)}), [this](const PropertyList &properties) -> ReturnValue {
            std::string folder = properties["folder"].value<std::string>();
            std::string content = properties["content"].value<std::string>();
            if (content.size() > 4096) {
                return std::string("META.md content is too long; max 4096 bytes");
            }

            if (!TakeSdAccessLock()) {
                return std::string("SD card is busy; try again");
            }
            std::string path = folder.empty() ? current_user_ai_img_dir : ResolveUserAiImgPath(folder);
            if (path.empty() || !DirectoryExists(path)) {
                GiveSdAccessLock();
                return std::string("invalid folder: ") + folder;
            }

            bool ok = WriteTextFile(path + "/META.md", content);
            GiveSdAccessLock();
            return ok;
        });

        mcp_server.AddTool("self.disp.isSHTC3", "获取设备温度和湿度", PropertyList(), [this](const PropertyList &) -> ReturnValue {
            ESP_LOGI("MCP", "进入MCP isSHTC3");
            char *str = Get_TemperatureHumidity();
            if(str) return str;
            else return NULL;
        });
    }

  public:
    waveshare_PhotoPainter()
        : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeCodecI2c();
        User_xiaozhi_app_init();
        InitializeButtons();
        InitializeTools();
    }

    virtual AudioCodec *GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            codec_i2c_bus_,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR,
            AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }
};

DECLARE_BOARD(waveshare_PhotoPainter);