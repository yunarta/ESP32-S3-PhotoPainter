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
#include <algorithm>
#include <cctype>
#include <stdio.h>
#include <string>
#include <vector>
#include <sys/stat.h>

#include "mcp_server.h"

#define TAG "esp-s3-PhotoPainter"

static const char *USER_AI_IMG_ROOT = "/sdcard/05_user_ai_img";
static std::string current_user_ai_img_dir = USER_AI_IMG_ROOT;
static bool current_user_ai_img_recursive = true;
static int current_user_ai_img_order = 0;
static int current_user_ai_img_recent_limit = 20;

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


static int ResolveImageOrderMode(const std::string &mode) {
    if (mode == "default" || mode == "all") {
        return 0;
    }
    if (mode == "newest" || mode == "terbaru") {
        return 1;
    }
    if (mode == "oldest" || mode == "terlama") {
        return 2;
    }
    if (mode == "random" || mode == "acak") {
        return 3;
    }
    if (mode == "recent_random" || mode == "recent" || mode == "terbaru_random") {
        return 4;
    }
    return -1;
}

static std::string RefreshImageList(const std::string &path, bool recursive, int order_mode, int recent_limit) {
    SDPort->SDPort_ScanListDir(path.c_str(), recursive, order_mode, recent_limit);
    sdcard_bmp_Quantity = SDPort->SDPort_GetScanListValue();
    img_loopCount = sdcard_bmp_Quantity;
    current_user_ai_img_dir = path;
    current_user_ai_img_recursive = recursive;
    current_user_ai_img_order = order_mode;
    current_user_ai_img_recent_limit = recent_limit;
    return std::string("folder=") + path + ", recursive=" + (recursive ? "true" : "false") + ", images=" + std::to_string(sdcard_bmp_Quantity);
}


static std::string ResolveImageScopePath(const std::string &scope, const std::string &folder, bool &recursive) {
    if (scope == "all" || scope == "default" || scope == "semua") {
        recursive = true;
        return USER_AI_IMG_ROOT;
    }

    recursive = false;
    return ResolveUserAiImgPath(folder);
}

static int ResolveActionOrderMode(const std::string &order, bool random, int recent_limit) {
    if (random && (order == "newest" || order == "terbaru") && recent_limit > 0) {
        return 4;
    }
    if (random) {
        return 3;
    }
    return ResolveImageOrderMode(order);
}

static ReturnValue RefreshAndMaybeDisplay(const std::string &path, bool recursive, int order_mode, int limit, bool display_first, bool start_loop) {
    if (path.empty() || !DirectoryExists(path)) {
        return std::string("invalid folder or scope");
    }

    std::string result = RefreshImageList(path, recursive, order_mode, limit);
    if (sdcard_bmp_Quantity < 1) {
        return result + ", no images found";
    }

    if (display_first) {
        sdcard_doc_count = 1;
        xEventGroupSetBits(epaper_groups, 0x02);
    }
    if (start_loop) {
        xEventGroupSetBits(ai_IMG_LoopGroup, 0x01);
    }
    return result;
}


typedef struct {
    int index;
    int score;
    std::string path;
    std::string title;
} ImageSearchResult_t;

static std::string ToLowerAscii(const std::string &value) {
    std::string lowered = value;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lowered;
}

static std::string ImageTitleFromPath(const std::string &path) {
    size_t slash = path.find_last_of('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) {
        name = name.substr(0, dot);
    }
    return name;
}

static int ScoreImageTitle(const std::string &query, const std::string &title, const std::string &path) {
    std::string q = ToLowerAscii(query);
    std::string t = ToLowerAscii(title);
    std::string p = ToLowerAscii(path);
    if (q.empty()) {
        return 0;
    }

    int score = 0;
    if (t == q) {
        score += 1000;
    }
    if (t.find(q) != std::string::npos) {
        score += 500;
    }
    if (p.find(q) != std::string::npos) {
        score += 200;
    }

    size_t start = 0;
    while (start < q.size()) {
        size_t end = q.find(' ', start);
        std::string token = q.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (token.size() > 1) {
            if (t.find(token) != std::string::npos) {
                score += 100;
            }
            if (p.find(token) != std::string::npos) {
                score += 40;
            }
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return score;
}

static std::vector<ImageSearchResult_t> SearchCurrentImageList(const std::string &query, int max_results) {
    std::vector<ImageSearchResult_t> results;
    list_t *list = SDPort->SDPort_GetListHost();
    list_iterator_t *it = list_iterator_new(list, LIST_HEAD);
    list_node_t *node = NULL;
    int index = 1;
    while ((node = list_iterator_next(it)) != NULL) {
        CustomSDPortNode_t *image = (CustomSDPortNode_t *) node->val;
        std::string path = image->sdcard_name;
        std::string title = ImageTitleFromPath(path);
        int score = ScoreImageTitle(query, title, path);
        if (score > 0) {
            results.push_back({index, score, path, title});
        }
        index++;
    }
    list_iterator_destroy(it);

    std::sort(results.begin(), results.end(), [](const ImageSearchResult_t &a, const ImageSearchResult_t &b) {
        if (a.score != b.score) {
            return a.score > b.score;
        }
        return a.title < b.title;
    });
    if (max_results > 0 && results.size() > static_cast<size_t>(max_results)) {
        results.resize(max_results);
    }
    return results;
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

            bool recursive = folder.empty() ? current_user_ai_img_recursive : false;
            int order_mode = folder.empty() ? current_user_ai_img_order : 0;
            int recent_limit = folder.empty() ? current_user_ai_img_recent_limit : 20;
            std::string result = RefreshImageList(path, recursive, order_mode, recent_limit);
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

            std::string result = RefreshImageList(path, false, 0, 20);
            GiveSdAccessLock();
            return result;
        });



        mcp_server.AddTool("self.disp.setAllImageFolders", "恢复默认模式：扫描 /sdcard/05_user_ai_img 以及下面所有子文件夹里的全部图片。", PropertyList(), [this](const PropertyList &) -> ReturnValue {
            if (!TakeSdAccessLock()) {
                return std::string("SD card is busy; try again");
            }
            std::string result = RefreshImageList(USER_AI_IMG_ROOT, true, 0, 20);
            GiveSdAccessLock();
            return result;
        });

        mcp_server.AddTool("self.disp.setImageOrder", "设置当前图片列表顺序。mode 支持 default/all、random/acak、newest/terbaru、oldest/terlama、recent_random/terbaru_random；recent_random 默认取最近20张后打乱。", PropertyList({Property("mode", kPropertyTypeString), Property("recent_limit", kPropertyTypeInteger, 20)}), [this](const PropertyList &properties) -> ReturnValue {
            std::string mode = properties["mode"].value<std::string>();
            int recent_limit = properties["recent_limit"].value<int>();
            if (recent_limit < 1) {
                recent_limit = 20;
            }
            int order_mode = ResolveImageOrderMode(mode);
            if (order_mode < 0) {
                return std::string("invalid mode: ") + mode;
            }
            if (!TakeSdAccessLock()) {
                return std::string("SD card is busy; try again");
            }
            bool recursive = current_user_ai_img_recursive;
            if (order_mode == 0 && current_user_ai_img_dir == USER_AI_IMG_ROOT) {
                recursive = true;
            }
            std::string result = RefreshImageList(current_user_ai_img_dir, recursive, order_mode, recent_limit);
            GiveSdAccessLock();
            return result + ", mode=" + mode;
        });



        mcp_server.AddTool("self.disp.showImages", "按条件选择并立即显示图片。scope=all/default/semua 会扫描 05_user_ai_img 所有子文件夹；scope=folder 用 folder 指定一级子文件夹。order 支持 default/all、newest/terbaru、oldest/terlama；recent_limit 可限制数量；random=true 会打乱候选。", PropertyList({Property("scope", kPropertyTypeString, std::string("all")), Property("folder", kPropertyTypeString, std::string("")), Property("order", kPropertyTypeString, std::string("default")), Property("recent_limit", kPropertyTypeInteger, 0), Property("random", kPropertyTypeBoolean, false)}), [this](const PropertyList &properties) -> ReturnValue {
            std::string scope = properties["scope"].value<std::string>();
            std::string folder = properties["folder"].value<std::string>();
            std::string order = properties["order"].value<std::string>();
            int recent_limit = properties["recent_limit"].value<int>();
            bool random = properties["random"].value<bool>();
            bool recursive = true;
            std::string path = ResolveImageScopePath(scope, folder, recursive);
            int order_mode = ResolveActionOrderMode(order, random, recent_limit);
            if (order_mode < 0) {
                return std::string("invalid order: ") + order;
            }
            if (!TakeSdAccessLock()) {
                return std::string("SD card is busy; try again");
            }
            ReturnValue result = RefreshAndMaybeDisplay(path, recursive, order_mode, recent_limit, true, false);
            GiveSdAccessLock();
            return result;
        });

        mcp_server.AddTool("self.disp.showLatestImages", "显示最新图片。count 默认 20；random=true 表示先取最新 count 张再随机显示其中一张。folder 为空时扫描所有 05_user_ai_img 子文件夹。", PropertyList({Property("count", kPropertyTypeInteger, 20), Property("random", kPropertyTypeBoolean, false), Property("folder", kPropertyTypeString, std::string(""))}), [this](const PropertyList &properties) -> ReturnValue {
            int count = properties["count"].value<int>();
            bool random = properties["random"].value<bool>();
            std::string folder = properties["folder"].value<std::string>();
            if (count < 1) {
                count = 20;
            }
            bool recursive = folder.empty();
            std::string path = folder.empty() ? std::string(USER_AI_IMG_ROOT) : ResolveUserAiImgPath(folder);
            int order_mode = random ? 4 : 1;
            if (!TakeSdAccessLock()) {
                return std::string("SD card is busy; try again");
            }
            ReturnValue result = RefreshAndMaybeDisplay(path, recursive, order_mode, count, true, false);
            GiveSdAccessLock();
            return result;
        });

        mcp_server.AddTool("self.disp.showAllImages", "恢复到全部图片并立即显示第一张；random=true 时会先随机打乱全部图片再显示。", PropertyList({Property("random", kPropertyTypeBoolean, false)}), [this](const PropertyList &properties) -> ReturnValue {
            bool random = properties["random"].value<bool>();
            if (!TakeSdAccessLock()) {
                return std::string("SD card is busy; try again");
            }
            ReturnValue result = RefreshAndMaybeDisplay(USER_AI_IMG_ROOT, true, random ? 3 : 0, 0, true, false);
            GiveSdAccessLock();
            return result;
        });

        mcp_server.AddTool("self.disp.imgloopWithOptions", "启动幻灯片轮播并可指定随机/排序/范围。scope=all 或 folder；order 支持 default、newest、oldest；recent_limit 可配合 newest+random 做最近N张随机轮播。", PropertyList({Property("scope", kPropertyTypeString, std::string("all")), Property("folder", kPropertyTypeString, std::string("")), Property("order", kPropertyTypeString, std::string("default")), Property("recent_limit", kPropertyTypeInteger, 0), Property("random", kPropertyTypeBoolean, false)}), [this](const PropertyList &properties) -> ReturnValue {
            std::string scope = properties["scope"].value<std::string>();
            std::string folder = properties["folder"].value<std::string>();
            std::string order = properties["order"].value<std::string>();
            int recent_limit = properties["recent_limit"].value<int>();
            bool random = properties["random"].value<bool>();
            bool recursive = true;
            std::string path = ResolveImageScopePath(scope, folder, recursive);
            int order_mode = ResolveActionOrderMode(order, random, recent_limit);
            if (order_mode < 0) {
                return std::string("invalid order: ") + order;
            }
            if (!TakeSdAccessLock()) {
                return std::string("SD card is busy; try again");
            }
            ReturnValue result = RefreshAndMaybeDisplay(path, recursive, order_mode, recent_limit, false, true);
            GiveSdAccessLock();
            return result;
        });



        mcp_server.AddTool("self.disp.listImageTitles", "List title/nama file gambar dari scope saat ini atau folder tertentu, supaya AI bisa memilih nama yang paling cocok.", PropertyList({Property("scope", kPropertyTypeString, std::string("all")), Property("folder", kPropertyTypeString, std::string("")), Property("max_results", kPropertyTypeInteger, 50)}), [this](const PropertyList &properties) -> ReturnValue {
            std::string scope = properties["scope"].value<std::string>();
            std::string folder = properties["folder"].value<std::string>();
            int max_results = properties["max_results"].value<int>();
            bool recursive = true;
            std::string path = ResolveImageScopePath(scope, folder, recursive);
            if (!TakeSdAccessLock()) {
                return std::string("SD card is busy; try again");
            }
            ReturnValue refresh = RefreshAndMaybeDisplay(path, recursive, 0, 0, false, false);
            if (sdcard_bmp_Quantity < 1) {
                GiveSdAccessLock();
                return refresh;
            }
            std::string result = "titles=" + std::to_string(sdcard_bmp_Quantity);
            list_t *list = SDPort->SDPort_GetListHost();
            list_iterator_t *it = list_iterator_new(list, LIST_HEAD);
            list_node_t *node = NULL;
            int index = 1;
            while ((node = list_iterator_next(it)) != NULL) {
                if (max_results > 0 && index > max_results) {
                    break;
                }
                CustomSDPortNode_t *image = (CustomSDPortNode_t *) node->val;
                std::string image_path = image->sdcard_name;
                result += "\n" + std::to_string(index) + ": " + ImageTitleFromPath(image_path) + " => " + image_path;
                index++;
            }
            list_iterator_destroy(it);
            GiveSdAccessLock();
            return result;
        });

        mcp_server.AddTool("self.disp.searchImages", "Search gambar berdasarkan title/nama file. Gunakan ini saat user bilang misalnya 'gambar yang titlenya gundam'. Return kandidat dengan score supaya AI bisa inference mana file paling pas.", PropertyList({Property("query", kPropertyTypeString), Property("scope", kPropertyTypeString, std::string("all")), Property("folder", kPropertyTypeString, std::string("")), Property("max_results", kPropertyTypeInteger, 10)}), [this](const PropertyList &properties) -> ReturnValue {
            std::string query = properties["query"].value<std::string>();
            std::string scope = properties["scope"].value<std::string>();
            std::string folder = properties["folder"].value<std::string>();
            int max_results = properties["max_results"].value<int>();
            bool recursive = true;
            std::string path = ResolveImageScopePath(scope, folder, recursive);
            if (!TakeSdAccessLock()) {
                return std::string("SD card is busy; try again");
            }
            ReturnValue refresh = RefreshAndMaybeDisplay(path, recursive, 0, 0, false, false);
            if (sdcard_bmp_Quantity < 1) {
                GiveSdAccessLock();
                return refresh;
            }
            auto results = SearchCurrentImageList(query, max_results);
            if (results.empty()) {
                GiveSdAccessLock();
                return std::string("no image title matched: ") + query;
            }
            std::string response = "query=" + query + ", matches=" + std::to_string(results.size());
            for (const auto &match : results) {
                response += "\n" + std::to_string(match.index) + ": score=" + std::to_string(match.score) + ", title=" + match.title + ", path=" + match.path;
            }
            GiveSdAccessLock();
            return response;
        });

        mcp_server.AddTool("self.disp.showImageByName", "Search gambar berdasarkan title/nama file lalu langsung tampilkan kandidat terbaik. Cocok untuk perintah: tampilkan gambar yang titlenya gundam.", PropertyList({Property("query", kPropertyTypeString), Property("scope", kPropertyTypeString, std::string("all")), Property("folder", kPropertyTypeString, std::string(""))}), [this](const PropertyList &properties) -> ReturnValue {
            std::string query = properties["query"].value<std::string>();
            std::string scope = properties["scope"].value<std::string>();
            std::string folder = properties["folder"].value<std::string>();
            bool recursive = true;
            std::string path = ResolveImageScopePath(scope, folder, recursive);
            if (!TakeSdAccessLock()) {
                return std::string("SD card is busy; try again");
            }
            ReturnValue refresh = RefreshAndMaybeDisplay(path, recursive, 0, 0, false, false);
            if (sdcard_bmp_Quantity < 1) {
                GiveSdAccessLock();
                return refresh;
            }
            auto results = SearchCurrentImageList(query, 1);
            if (results.empty()) {
                GiveSdAccessLock();
                return std::string("no image title matched: ") + query;
            }
            sdcard_doc_count = results[0].index;
            xEventGroupSetBits(epaper_groups, 0x02);
            std::string response = "displaying index=" + std::to_string(results[0].index) + ", score=" + std::to_string(results[0].score) + ", title=" + results[0].title + ", path=" + results[0].path;
            GiveSdAccessLock();
            return response;
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