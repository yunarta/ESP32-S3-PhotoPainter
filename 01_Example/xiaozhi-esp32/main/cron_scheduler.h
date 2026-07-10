#ifndef CRON_SCHEDULER_H
#define CRON_SCHEDULER_H

#include <esp_timer.h>
#include <cJSON.h>

#include <ctime>
#include <mutex>
#include <string>
#include <vector>

class CronScheduler {
public:
    struct Reminder {
        std::string id;
        std::string label;
        int hour = 0;
        int minute = 0;
        std::string days = "*";
        bool enabled = true;
        time_t last_fired_at = 0;
        time_t next_fire_at = 0;
    };

    static CronScheduler& GetInstance() {
        static CronScheduler instance;
        return instance;
    }

    void Start();
    std::string AddReminder(const std::string& id, const std::string& label, int hour, int minute,
                            const std::string& days, bool enabled);
    bool RemoveReminder(const std::string& id);
    bool SetReminderEnabled(const std::string& id, bool enabled);
    void ClearReminders();
    cJSON* GetRemindersJson();
    int64_t GetNextWakeupDelayUs();
    void CompletePendingReminder(const std::string& id);

private:
    CronScheduler();
    ~CronScheduler();

    CronScheduler(const CronScheduler&) = delete;
    CronScheduler& operator=(const CronScheduler&) = delete;

    void Load();
    void SaveLocked();
    void CheckDueReminders();
    void RecalculateNextFireLocked(time_t now);
    time_t CalculateNextFire(const Reminder& reminder, time_t now) const;
    bool IsValidTime() const;
    bool MatchesDay(const std::string& days, int weekday) const;
    bool HasPendingReminderLocked(const std::string& id) const;
    std::string GenerateId() const;

    std::mutex mutex_;
    std::vector<Reminder> reminders_;
    std::vector<Reminder> pending_reminders_;
    esp_timer_handle_t timer_handle_ = nullptr;
    bool started_ = false;
};

#endif // CRON_SCHEDULER_H
