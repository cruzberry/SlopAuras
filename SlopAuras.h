#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <ToolboxUIPlugin.h>
#include <GWCA/Managers/ChatMgr.h>
#include <GWCA/Managers/StoCMgr.h>
#include <GWCA/Managers/UIMgr.h>

class SlopAuras : public ToolboxUIPlugin {
public:
    ~SlopAuras() override;

    const char* Name() const override { return "SlopAuras"; }

    void Initialize(ImGuiContext* ctx, ImGuiAllocFns allocator_fns, HMODULE toolbox_dll) override;
    void SignalTerminate() override;
    void Terminate() override;
    void Update(float delta) override;
    void Draw(IDirect3DDevice9* pDevice) override;
    void DrawSettings() override;
    std::filesystem::path GetSettingFile(const wchar_t* folder) const override;
    void LoadSettings(const wchar_t* folder) override;
    void SaveSettings(const wchar_t* folder) override;

private:
    enum class NotificationType : size_t {
        EffectApplied,
        EffectExpiring,
        CooldownReady,
        Count
    };

    struct TrackedCast {
        uint32_t skill_id;
        uint32_t target_agent_id;
        uint32_t timestamp;
        uint32_t duration_ms;
        bool expiration_notified = false;
        bool multi_ally = false;
        uint32_t observed_remaining_ms = 0;
        uint32_t observed_updated_timestamp = 0;
    };

    struct TrackedCooldown {
        uint32_t skill_id;
        uint32_t agent_id;
        uint32_t timestamp;
        uint32_t duration_ms;
    };

    struct TrackedKnockdown {
        uint32_t agent_id;
        uint32_t timestamp;
        uint32_t duration_ms;
    };

    struct PendingCast {
        uint32_t skill_id;
        uint32_t target_agent_id;
        uint32_t timestamp;
    };

    struct PlayerEffectNotification {
        uint32_t skill_id;
        uint32_t timestamp;
        bool expiration_notified = false;
    };

    struct QueuedNotification {
        NotificationType type;
        uint32_t skill_id;
        std::wstring path;
    };

    void TrackCast(int skill_id, uint32_t target_agent_id);
    void TrackEnemyCooldown(uint32_t agent_id, uint32_t skill_id);
    void PrintTrackedEffects();
    void PlayNotification(NotificationType type, uint32_t skill_id = 0);
    std::string ResolveNotificationSoundPath(NotificationType type, uint32_t skill_id) const;
    void NotificationWorker();
    void StopNotificationWorker();
    void SetNotificationsMuted(bool muted);
    bool IsMapReady() const;
    void DrawEnemyNameplates(IDirect3DDevice9* device);
    static void HandleChatCommand(GW::HookStatus* status, const wchar_t* command, int argc, const LPWSTR* argv);
    void DrawSettingsWindow();

    GW::HookEntry chat_command_hook;
    GW::HookEntry skill_activated_hook;
    GW::HookEntry skill_started_cast_hook;
    GW::HookEntry map_loading_hook;
    GW::HookEntry knockdown_hook;
    std::atomic<uint32_t> map_generation = 0;
    std::atomic<uint32_t> ready_map_generation = 0;
    std::mutex tracking_mutex;
    std::vector<int> effect_ids;
    std::vector<int> cast_by_me_entries;
    std::vector<int> cooldown_ids;
    std::vector<std::string> natural_resistance_agent_names;
    // Per-entry sound overrides, parallel to effect_ids / cooldown_ids. Empty entries fall back to the
    // matching group-level sound in notification_sound_paths.
    std::vector<std::string> effect_applied_sound_overrides;
    std::vector<std::string> effect_expiring_sound_overrides;
    std::vector<std::string> cooldown_sound_overrides;
    std::vector<TrackedCast> tracked_casts;
    std::vector<TrackedCooldown> tracked_cooldowns;
    std::vector<TrackedKnockdown> tracked_knockdowns;
    std::vector<PendingCast> pending_casts;
    std::vector<PlayerEffectNotification> player_effect_notifications;
    std::vector<int> player_effect_ids_snapshot;
    std::vector<int> player_cast_by_me_snapshot;
    bool player_effect_snapshot_initialized = false;
    std::array<bool, static_cast<size_t>(NotificationType::Count)> notification_enabled{};
    std::array<std::string, static_cast<size_t>(NotificationType::Count)> notification_sound_paths;
    std::atomic<bool> notifications_muted = false;
    std::mutex notification_queue_mutex;
    std::condition_variable notification_condition;
    std::deque<QueuedNotification> notification_queue;
    std::thread notification_worker;
    bool notification_worker_stopping = false;
    bool notification_playing = false;
    NotificationType notification_playing_type = NotificationType::EffectApplied;
    uint32_t notification_playing_skill_id = 0;
    float notification_lead_seconds = 3.f;
    bool settings_window_visible = false;
    bool pending_welcome_message = false;
    bool widget_mode = false;
    bool enemy_nameplates_enabled = true;
    bool nameplate_show_knockdown = true;
    bool nameplate_show_effects = true;
    bool nameplate_show_cooldowns = true;
    float effect_icon_size = 22.f;
    float cooldown_icon_size = 22.f;
};
