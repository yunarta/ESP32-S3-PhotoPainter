#include "cron_scheduler.h"

#include "application.h"
#include "board.h"
#include "display.h"
#include "device_state.h"
#include "settings.h"

#include <esp_log.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#define TAG "CronScheduler"

namespace {
constexpr int64_t kCheckIntervalUs = 30 * 1000000LL;
constexpr time_t kMinValidEpoch = 1609459200; // 2021-01-01 00:00:00 UTC
constexpr time_t kMissedGraceSeconds = 60;

std::string JsonToString(cJSON* json) {
    char* str = cJSON_PrintUnformatted(json);
    std::string result = str ? str : "";
    cJSON_free(str);
    return result;
}

std::string SanitizePromptText(const std::string& text) {
    std::string result;
    result.reserve(text.size());
    for (char ch : text) {
        if (ch == '"' || ch == '\\' || ch == '\n' || ch == '\r' || ch == '\t') {
            result.push_back(' ');
        } else {
            result.push_back(ch);
        }
    }
    return result;
}

std::string BuildReminderPrompt(const CronScheduler::Reminder& reminder, time_t now) {
    struct tm tm_value;
    localtime_r(&now, &tm_value);
    char time_text[16];
    strftime(time_text, sizeof(time_text), "%H:%M", &tm_value);

    std::string prompt = "Sekarang jam ";
    prompt += time_text;
    prompt += ". Tolong check memory dulu, lalu kasih reminder ke user";
    if (!reminder.label.empty()) {
        prompt += " tentang ";
        prompt += SanitizePromptText(reminder.label);
    }
    prompt += ". Setelah itu kasih semangat ke user.";
    return prompt;
}

void AddReminderToJson(cJSON* array, const CronScheduler::Reminder& reminder) {
    cJSON* item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "id", reminder.id.c_str());
    cJSON_AddStringToObject(item, "label", reminder.label.c_str());
    cJSON_AddNumberToObject(item, "hour", reminder.hour);
    cJSON_AddNumberToObject(item, "minute", reminder.minute);
    cJSON_AddStringToObject(item, "days", reminder.days.c_str());
    cJSON_AddBoolToObject(item, "enabled", reminder.enabled);
    cJSON_AddNumberToObject(item, "last_fired_at", static_cast<double>(reminder.last_fired_at));
    cJSON_AddNumberToObject(item, "next_fire_at", static_cast<double>(reminder.next_fire_at));
    cJSON_AddItemToArray(array, item);
}
} // namespace

CronScheduler::CronScheduler() {
    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            static_cast<CronScheduler*>(arg)->CheckDueReminders();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "cron_scheduler",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle_));
}

CronScheduler::~CronScheduler() {
    if (timer_handle_) {
        esp_timer_stop(timer_handle_);
        esp_timer_delete(timer_handle_);
    }
}

void CronScheduler::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
        return;
    }

    Load();
    RecalculateNextFireLocked(time(nullptr));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer_handle_, kCheckIntervalUs));
    started_ = true;
    ESP_LOGI(TAG, "RTC reminder scheduler started");
}

std::string CronScheduler::AddReminder(const std::string& id, const std::string& label, int hour, int minute,
                                       const std::string& days, bool enabled) {
    if (hour < 0 || hour > 23) {
        throw std::runtime_error("hour must be between 0 and 23");
    }
    if (minute < 0 || minute > 59) {
        throw std::runtime_error("minute must be between 0 and 59");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    Reminder reminder;
    reminder.id = id.empty() ? GenerateId() : id;
    reminder.label = label.empty() ? "Start Xiaozhi" : label;
    reminder.hour = hour;
    reminder.minute = minute;
    reminder.days = days.empty() ? "*" : days;
    reminder.enabled = enabled;
    reminder.next_fire_at = CalculateNextFire(reminder, time(nullptr));

    auto existing = std::find_if(reminders_.begin(), reminders_.end(), [&reminder](const Reminder& item) {
        return item.id == reminder.id;
    });
    if (existing == reminders_.end()) {
        reminders_.push_back(reminder);
    } else {
        *existing = reminder;
    }

    SaveLocked();
    return reminder.id;
}

bool CronScheduler::RemoveReminder(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto old_size = reminders_.size();
    reminders_.erase(std::remove_if(reminders_.begin(), reminders_.end(), [&id](const Reminder& reminder) {
        return reminder.id == id;
    }), reminders_.end());
    pending_reminders_.erase(std::remove_if(pending_reminders_.begin(), pending_reminders_.end(), [&id](const Reminder& reminder) {
        return reminder.id == id;
    }), pending_reminders_.end());
    bool removed = reminders_.size() != old_size;
    if (removed) {
        SaveLocked();
    }
    return removed;
}

bool CronScheduler::SetReminderEnabled(const std::string& id, bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& reminder : reminders_) {
        if (reminder.id == id) {
            reminder.enabled = enabled;
            reminder.next_fire_at = CalculateNextFire(reminder, time(nullptr));
            if (!enabled) {
                pending_reminders_.erase(std::remove_if(pending_reminders_.begin(), pending_reminders_.end(), [&id](const Reminder& pending) {
                    return pending.id == id;
                }), pending_reminders_.end());
            }
            SaveLocked();
            return true;
        }
    }
    return false;
}

void CronScheduler::ClearReminders() {
    std::lock_guard<std::mutex> lock(mutex_);
    reminders_.clear();
    pending_reminders_.clear();
    SaveLocked();
}

cJSON* CronScheduler::GetRemindersJson() {
    std::lock_guard<std::mutex> lock(mutex_);
    RecalculateNextFireLocked(time(nullptr));

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "time_valid", IsValidTime());
    cJSON_AddNumberToObject(root, "now", static_cast<double>(time(nullptr)));
    cJSON* array = cJSON_CreateArray();
    for (const auto& reminder : reminders_) {
        AddReminderToJson(array, reminder);
    }
    cJSON_AddItemToObject(root, "reminders", array);
    return root;
}

int64_t CronScheduler::GetNextWakeupDelayUs() {
    std::lock_guard<std::mutex> lock(mutex_);
    time_t now = time(nullptr);
    if (!IsValidTime()) {
        return -1;
    }

    RecalculateNextFireLocked(now);
    time_t next_fire = 0;
    for (const auto& reminder : reminders_) {
        if (!reminder.enabled || reminder.next_fire_at <= 0) {
            continue;
        }
        if (next_fire == 0 || reminder.next_fire_at < next_fire) {
            next_fire = reminder.next_fire_at;
        }
    }

    if (next_fire <= now) {
        return 1000000LL;
    }
    return static_cast<int64_t>(next_fire - now) * 1000000LL;
}

void CronScheduler::CompletePendingReminder(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_reminders_.erase(std::remove_if(pending_reminders_.begin(), pending_reminders_.end(), [&id](const Reminder& reminder) {
        return reminder.id == id;
    }), pending_reminders_.end());
}

void CronScheduler::Load() {
    Settings settings("cron", false);
    auto jobs = settings.GetString("reminders", "[]");
    cJSON* root = cJSON_Parse(jobs.c_str());
    reminders_.clear();
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return;
    }

    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, root) {
        if (!cJSON_IsObject(item)) {
            continue;
        }
        auto id = cJSON_GetObjectItem(item, "id");
        auto label = cJSON_GetObjectItem(item, "label");
        auto hour = cJSON_GetObjectItem(item, "hour");
        auto minute = cJSON_GetObjectItem(item, "minute");
        auto days = cJSON_GetObjectItem(item, "days");
        auto enabled = cJSON_GetObjectItem(item, "enabled");
        auto last_fired_at = cJSON_GetObjectItem(item, "last_fired_at");
        if (!cJSON_IsString(id) || !cJSON_IsNumber(hour) || !cJSON_IsNumber(minute)) {
            continue;
        }
        Reminder reminder;
        reminder.id = id->valuestring;
        reminder.label = cJSON_IsString(label) ? label->valuestring : "Start Xiaozhi";
        reminder.hour = hour->valueint;
        reminder.minute = minute->valueint;
        reminder.days = cJSON_IsString(days) ? days->valuestring : "*";
        reminder.enabled = !cJSON_IsBool(enabled) || enabled->valueint == 1;
        reminder.last_fired_at = cJSON_IsNumber(last_fired_at) ? static_cast<time_t>(last_fired_at->valuedouble) : 0;
        if (reminder.hour >= 0 && reminder.hour <= 23 && reminder.minute >= 0 && reminder.minute <= 59) {
            reminders_.push_back(reminder);
        }
    }
    cJSON_Delete(root);
}

void CronScheduler::SaveLocked() {
    cJSON* array = cJSON_CreateArray();
    for (const auto& reminder : reminders_) {
        AddReminderToJson(array, reminder);
    }
    auto jobs = JsonToString(array);
    cJSON_Delete(array);

    Settings settings("cron", true);
    settings.SetString("reminders", jobs);
}

void CronScheduler::CheckDueReminders() {
    std::vector<Reminder> reminders_to_try;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!IsValidTime()) {
            return;
        }

        time_t now = time(nullptr);
        struct tm tm_value;
        localtime_r(&now, &tm_value);
        time_t current_minute = now - tm_value.tm_sec;
        bool should_save = false;

        for (auto& reminder : reminders_) {
            if (!reminder.enabled) {
                reminder.next_fire_at = 0;
                continue;
            }

            bool due_now = reminder.hour == tm_value.tm_hour &&
                reminder.minute == tm_value.tm_min &&
                MatchesDay(reminder.days, tm_value.tm_wday) &&
                tm_value.tm_sec <= kMissedGraceSeconds &&
                reminder.last_fired_at != current_minute;

            if (due_now) {
                if (!HasPendingReminderLocked(reminder.id)) {
                    pending_reminders_.push_back(reminder);
                }
                reminder.last_fired_at = current_minute;
                should_save = true;
            }

            reminder.next_fire_at = CalculateNextFire(reminder, now);
        }

        if (!pending_reminders_.empty()) {
            reminders_to_try.push_back(pending_reminders_.front());
        }
        if (should_save) {
            SaveLocked();
        }
    }

    for (const auto& reminder : reminders_to_try) {
        ESP_LOGI(TAG, "Trying pending reminder %s: %02d:%02d", reminder.id.c_str(), reminder.hour, reminder.minute);
        Application::GetInstance().Schedule([reminder]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                auto prompt = BuildReminderPrompt(reminder, time(nullptr));
                CronScheduler::GetInstance().CompletePendingReminder(reminder.id);
                Board::GetInstance().GetDisplay()->SetChatMessage("system", prompt.c_str());
                app.StartListening();
            } else {
                ESP_LOGW(TAG, "Defer reminder %s because device is busy", reminder.id.c_str());
            }
        });
    }
}

void CronScheduler::RecalculateNextFireLocked(time_t now) {
    for (auto& reminder : reminders_) {
        if (!reminder.enabled) {
            reminder.next_fire_at = 0;
            continue;
        }
        if (reminder.next_fire_at <= now) {
            reminder.next_fire_at = CalculateNextFire(reminder, now);
        }
    }
}

time_t CronScheduler::CalculateNextFire(const Reminder& reminder, time_t now) const {
    if (!IsValidTime()) {
        return 0;
    }

    for (int day_offset = 0; day_offset <= 7; ++day_offset) {
        time_t candidate_base = now + day_offset * 24 * 60 * 60;
        struct tm tm_value;
        localtime_r(&candidate_base, &tm_value);
        tm_value.tm_hour = reminder.hour;
        tm_value.tm_min = reminder.minute;
        tm_value.tm_sec = 0;
        time_t candidate = mktime(&tm_value);
        localtime_r(&candidate, &tm_value);
        if (candidate <= now) {
            continue;
        }
        if (MatchesDay(reminder.days, tm_value.tm_wday)) {
            return candidate;
        }
    }
    return 0;
}

bool CronScheduler::IsValidTime() const {
    return time(nullptr) >= kMinValidEpoch;
}

bool CronScheduler::MatchesDay(const std::string& days, int weekday) const {
    if (days.empty() || days == "*") {
        return true;
    }

    static const char* names[] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
    std::string normalized = days;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (normalized.find(names[weekday]) != std::string::npos) {
        return true;
    }

    char numeric[2] = {static_cast<char>('0' + weekday), '\0'};
    return normalized.find(numeric) != std::string::npos;
}

bool CronScheduler::HasPendingReminderLocked(const std::string& id) const {
    return std::find_if(pending_reminders_.begin(), pending_reminders_.end(), [&id](const Reminder& reminder) {
        return reminder.id == id;
    }) != pending_reminders_.end();
}

std::string CronScheduler::GenerateId() const {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "reminder_%lld", static_cast<long long>(esp_timer_get_time()));
    return buffer;
}
