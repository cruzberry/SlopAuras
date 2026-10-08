#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
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
        // Hex/condition on a foe: Natural Resistance is resolved lazily (the target name decodes asynchronously).
        bool resistance_pending = false;
        // duration_ms was taken from the game's own effect data instead of being predicted.
        bool duration_exact = false;
    };

    struct TrackedCooldown {
        uint32_t skill_id;
        uint32_t agent_id;
        uint32_t timestamp;
        uint32_t duration_ms;
        // Started by a confirmed interrupt. Interrupt skills can disable the skill for longer than its
        // recharge, which is not observable, so this duration is a lower bound.
        bool from_interrupt = false;
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

    struct EffectConfig {
        int skill_id = 0;
        bool cast_by_me = false;
        // Empty sounds fall back to the group-level sound in notification_sound_paths.
        std::string applied_sound;
        std::string expiring_sound;
    };

    struct CooldownConfig {
        int skill_id = 0;
        std::string ready_sound;
    };

    struct ActiveEffect {
        int skill_id;
        uint32_t target_agent_id; // 0 = every ally
        DWORD remaining;
    };

    // Set from the raw StoC "interrupted" packet; consumed by the kAgentSkillInterrupted UI message.
    struct InterruptFlag {
        uint32_t agent_id;
        uint32_t timestamp;
    };

    // Learned from enemies recasting a skill sooner than predicted (recharge speed-ups we cannot read directly).
    struct AgentRechargeFactor {
        uint32_t agent_id;
        float factor;
        uint32_t observed_at;
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
    void TrackEnemyCooldown(uint32_t agent_id, uint32_t skill_id, bool from_interrupt = false);
    // Single source of truth for "what is active right now", shared by the widget and the chat command.
    std::vector<ActiveEffect> CollectActiveEffects(uint32_t now, bool validate_casts);
    // The helpers below read the tracked-skill config; callers must hold tracking_mutex.
    const EffectConfig* FindEffectConfig(int skill_id) const;
    bool IsTrackedEffect(int skill_id) const { return FindEffectConfig(skill_id) != nullptr; }
    bool IsCastByMe(int skill_id) const;
    const CooldownConfig* FindCooldownConfig(int skill_id) const;
    float GetAgentRechargeFactor(uint32_t agent_id, uint32_t now) const;
    void RecordAgentRechargeFactor(uint32_t agent_id, float factor, uint32_t now);
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
    GW::HookEntry generic_value_hook;
    std::atomic<uint32_t> map_generation = 0;
    std::atomic<uint32_t> ready_map_generation = 0;
    std::mutex tracking_mutex;
    std::vector<EffectConfig> effect_configs;
    std::vector<CooldownConfig> cooldown_configs;
    std::vector<std::string> natural_resistance_agent_names;
    std::vector<TrackedCast> tracked_casts;
    std::vector<TrackedCooldown> tracked_cooldowns;
    std::vector<TrackedKnockdown> tracked_knockdowns;
    std::vector<PendingCast> pending_casts;
    std::vector<PlayerEffectNotification> player_effect_notifications;
    std::vector<std::pair<int, bool>> player_effect_config_snapshot;
    std::vector<InterruptFlag> confirmed_interrupts;
    std::vector<AgentRechargeFactor> agent_recharge_factors;
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
    bool adaptive_recharge_enabled = true;
    float effect_icon_size = 22.f;
    float cooldown_icon_size = 22.f;
};
