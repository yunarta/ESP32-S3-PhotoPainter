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

#include "mcp_server.h"
#include "board.h"
#include <sys/stat.h>
#include <cstdio>
#include <cerrno>

#define TAG "esp-s3-PhotoPainter"

static bool EnsureDirectoryExists(const std::string &directory) {
    if (directory.empty()) {
        return false;
    }

    std::string current;
    size_t start = 0;
    if (directory[0] == '/') {
        current = "/";
        start = 1;
    }

    while (start <= directory.size()) {
        size_t slash = directory.find('/', start);
        std::string part = directory.substr(start, slash - start);
        if (!part.empty()) {
            if (current.size() > 1 && current.back() != '/') {
                current += "/";
            }
            current += part;

            if (mkdir(current.c_str(), 0775) != 0 && errno != EEXIST) {
                ESP_LOGE(TAG, "Failed to create directory %s", current.c_str());
                return false;
            }
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }

    return true;
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
        mcp_server.AddTool("self.disp.SwitchPictures", "切换本地或 SD 卡中的图片，通过整数参数指定图片序号（如 “显示第 1 张图片”）", PropertyList({Property("value", kPropertyTypeInteger, 1, sdcard_bmp_Quantity)}), [this](const PropertyList &properties) -> ReturnValue {
            int value = properties["value"].value<int>();
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

        mcp_server.AddTool("self.disp.isSHTC3", "获取设备温度和湿度", PropertyList(), [this](const PropertyList &) -> ReturnValue {
            ESP_LOGI("MCP", "进入MCP isSHTC3");
            char *str = Get_TemperatureHumidity();
            if(str) return str;
            else return NULL;
        });

        mcp_server.AddUserOnlyTool("self.disp.downloadS3Photo", "Download an S3 photo from a provided URL and save it to a requested SD card directory. The directory can be an absolute /sdcard path or a relative subdirectory under /sdcard. This tool is intended for external MCP orchestration after another MCP obtains the S3 download URL; it is hidden from normal assistant tool lists.", PropertyList({
            Property("url", kPropertyTypeString),
            Property("directory", kPropertyTypeString, "/sdcard/05_user_ai_img"),
            Property("filename", kPropertyTypeString, "s3_photo.jpg")
        }), [this](const PropertyList &properties) -> ReturnValue {
            auto url = properties["url"].value<std::string>();
            auto directory = properties["directory"].value<std::string>();
            auto filename = properties["filename"].value<std::string>();

            if (url.empty() || url.find("http") != 0) {
                throw std::runtime_error("Invalid S3 photo URL");
            }
            if (directory.empty() || directory.find("..") != std::string::npos) {
                throw std::runtime_error("directory must be a path under /sdcard");
            }
            if (directory[0] != '/') {
                directory = "/sdcard/" + directory;
            }
            if (directory.back() == '/') {
                directory.pop_back();
            }
            if (directory != "/sdcard" && directory.find("/sdcard/") != 0) {
                throw std::runtime_error("directory must be a path under /sdcard");
            }
            if (filename.empty() || filename.find('/') != std::string::npos || filename.find("..") != std::string::npos) {
                throw std::runtime_error("filename must be a simple file name");
            }

            if (!EnsureDirectoryExists(directory)) {
                throw std::runtime_error("Failed to create SD card directory: " + directory);
            }
            const std::string path = directory + "/" + filename;
            if (path.size() >= sizeof(CustomSDPortNode_t::sdcard_name)) {
                throw std::runtime_error("SD card image path is too long");
            }

            auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
            if (!http->Open("GET", url)) {
                throw std::runtime_error("Failed to open S3 photo URL");
            }

            int status_code = http->GetStatusCode();
            if (status_code != 200) {
                http->Close();
                throw std::runtime_error("Unexpected S3 photo status code: " + std::to_string(status_code));
            }

            FILE *file = fopen(path.c_str(), "wb");
            if (file == nullptr) {
                http->Close();
                throw std::runtime_error("Failed to open SD card file for writing: " + path);
            }

            char buffer[1024];
            size_t total_written = 0;
            while (true) {
                int ret = http->Read(buffer, sizeof(buffer));
                if (ret < 0) {
                    fclose(file);
                    http->Close();
                    remove(path.c_str());
                    throw std::runtime_error("Failed to read S3 photo response");
                }
                if (ret == 0) {
                    break;
                }
                size_t written = fwrite(buffer, 1, ret, file);
                if (written != static_cast<size_t>(ret)) {
                    fclose(file);
                    http->Close();
                    remove(path.c_str());
                    throw std::runtime_error("Failed to write complete photo to SD card");
                }
                total_written += written;
            }

            fclose(file);
            http->Close();

            if (total_written == 0) {
                remove(path.c_str());
                throw std::runtime_error("S3 photo response was empty");
            }

            if (SDPort->SDPort_AddImagePath(path.c_str()) == ESP_OK) {
                sdcard_bmp_Quantity = SDPort->SDPort_GetScanListValue();
                img_loopCount = sdcard_bmp_Quantity;
            }

            cJSON *json = cJSON_CreateObject();
            cJSON_AddStringToObject(json, "path", path.c_str());
            cJSON_AddNumberToObject(json, "bytes", total_written);
            cJSON_AddNumberToObject(json, "image_count", sdcard_bmp_Quantity);
            return json;
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