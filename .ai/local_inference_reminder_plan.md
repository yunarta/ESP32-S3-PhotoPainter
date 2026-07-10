# Local Inference Plan for RTC Reminder Flow

## Context

We added an RTC-based `CronScheduler` so the device can wake/start Xiaozhi at configured local clock times. The scheduler can persist reminders, compute next-fire times, retry pending reminders after busy states, and integrate with sleep wakeup timers.

The current gap is not the RTC wakeup itself. The gap is the **inference path**: after the reminder wakes the device, the AI still needs a valid way to receive reminder context and generate a spoken response.

## Current Problems

### 1. Long reminder prompts cannot be sent through wake-word detect

`listen.detect` is only for wake-word text. It should receive short wake words, not long instructions.

Bad example:

```text
Sekarang jam 07:30. Tolong check memory dulu, lalu kasih reminder ke user. Setelah itu kasih semangat ke user.
```

That caused this runtime error:

```text
Detect is only for wake words, do not send long texts.
```

### 2. `StartListening()` starts audio listening, not LLM inference

The current safe flow is:

1. Reminder becomes due.
2. Scheduler displays a local system message.
3. Scheduler calls `StartListening()`.

This avoids the wake-word detect error, but it does **not** automatically pass the reminder prompt into the AI model. If the user does not speak, there is no new inference input.

### 3. Reminder management MCP tools are not enough for due reminder inference

The existing MCP tools manage reminder configuration:

- `self.reminder.add`
- `self.reminder.list`
- `self.reminder.remove`
- `self.reminder.set_enabled`
- `self.reminder.clear`

These are useful for CRUD, but they do not provide an explicit runtime flow for:

- retrieving the currently due reminder,
- giving the model the due reminder context,
- marking that due reminder as consumed/delivered.

### 4. “Check memory” is not defined yet

The desired prompt says the AI should “check memory,” but we have not defined what memory means in this firmware:

- cloud memory?
- local NVS memory?
- conversation memory?
- user profile?
- vector/semantic memory?
- reminder history?

Before local inference can use memory, we need a `MemoryProvider` or equivalent interface.

### 5. Local inference and TTS are not defined yet

For fully local reminder output, we need both:

1. a local inference API, for example `LocalInferenceService::Generate(prompt)`;
2. a way to speak the generated text locally, for example local TTS or another text-to-speech path.

Without those, the scheduler can wake and display context, but it cannot make the assistant speak a locally generated reminder.

## Desired Two-Step Flow

The clean reminder flow should be split into two phases.

### Step 1: Wake / activation

Use a wake-word-compatible signal only:

```json
{
  "type": "listen",
  "state": "detect",
  "text": "ni hao xiao zhi"
}
```

Or skip detect and call normal listening start when appropriate.

### Step 2: Inference context

Pass reminder context through a valid inference path, not through `listen.detect`.

Example context:

```json
{
  "event": "rtc_reminder_due",
  "time": "07:30",
  "label": "minum obat",
  "instruction": "Sebutkan jam sekarang, check memory, kasih reminder ke user, lalu kasih semangat."
}
```

## Recommended Architecture for Local Inference

### 1. Add pending reminder context APIs

Add MCP/local APIs such as:

- `self.reminder.get_pending`
- `self.reminder.complete_pending`

`self.reminder.get_pending` should return the oldest pending reminder with a prebuilt prompt and structured fields.

Example response:

```json
{
  "has_pending": true,
  "reminder": {
    "id": "reminder_ld",
    "label": "minum obat",
    "local_time": "07:30",
    "prompt": "Sekarang jam 07:30. Check memory. Ingatkan user untuk minum obat. Kasih semangat ke user."
  }
}
```

`self.reminder.complete_pending` should mark the reminder as delivered after AI response/TTS succeeds.

### 2. Add pending reminder status to device status

Add a field to `self.get_device_status`, for example:

```json
{
  "pending_reminder": true,
  "pending_reminder_id": "reminder_ld"
}
```

This gives the AI a reason to call `self.reminder.get_pending` after wake.

### 3. Add a local `ReminderInferenceRunner`

A local runner can orchestrate the flow:

```cpp
class ReminderInferenceRunner {
public:
    void RunPendingReminder();
};
```

Responsibilities:

1. read pending reminder context;
2. query memory;
3. build prompt;
4. run local inference;
5. send result to TTS/display;
6. complete pending reminder.

### 4. Add a `MemoryProvider`

Placeholder interface:

```cpp
class MemoryProvider {
public:
    std::string GetRelevantMemory(const std::string& query);
};
```

The first implementation can be empty/stubbed until local memory storage is decided.

### 5. Add local TTS/output integration

Local inference needs an output path. Options:

- local TTS engine, if available;
- server TTS via protocol, if local TTS is not available;
- display-only fallback.

## Minimal Next Patch

The safest next patch is not full local inference. It should add the missing context bridge:

1. keep pending reminders in `CronScheduler`;
2. add `GetPendingReminderJson()`;
3. add `CompletePendingReminder(id)` as an MCP tool;
4. add `self.reminder.get_pending` and `self.reminder.complete_pending` tools;
5. optionally expose `pending_reminder` in device status.

After that, either the cloud AI or a future local AI runner can reliably retrieve due reminder context without abusing `listen.detect`.

## Open Questions

1. What local AI engine will run inference?
2. Where is memory stored?
3. Is local TTS available, or should reminder speech still use the server?
4. Should pending reminders survive reboot, or is in-memory pending enough?
5. If the device is busy for a long time, should all missed reminders be delivered or only the latest?

## Current Recommendation

Do this in layers:

1. **Scheduler/wakeup:** already mostly done.
2. **Pending reminder context tool:** next.
3. **Device status pending flag:** next.
4. **Local inference runner:** after local model/TTS/memory decisions are clear.
5. **Memory integration:** last, because the memory backend is still undefined.
