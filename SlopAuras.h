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
        // EffectApplied and EffectExpiring belonged to effect tracking, which is being reworked and currently
        // produces no notifications. They stay in the enum so the saved notification arrays keep their layout.
        EffectApplied,
        EffectExpiring,
        CooldownReady,
        Count
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

    // Tracks "this agent is currently casting this skill", for any agent (not just the player). Used solely to
    // start a cooldown when a cast is cut short by a knockdown, which (unlike a real interrupt) never sends the
    // server's "interrupted" confirmation that the existing confirmed-interrupt path relies on.
    struct InProgressCast {
        uint32_t agent_id;
        uint32_t skill_id;
        uint32_t timestamp;
    };

    // Which agents a cooldown is tracked on.
    enum class CooldownTarget {
        Enemy,
        Ally
    };

    struct CooldownConfig {
        int skill_id = 0;
        std::string ready_sound;
        // Default preserves pre-existing behavior: cooldowns were only ever tracked on enemies.
        CooldownTarget target = CooldownTarget::Enemy;
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

    struct QueuedNotification {
        NotificationType type;
        uint32_t skill_id;
        std::wstring path;
    };

    // Tracks a cooldown for any agent (enemy or party ally); which skills are tracked for which target
    // category is decided by cooldown_configs. Called unconditionally for every observed skill activation.
    void TrackAgentCooldown(uint32_t agent_id, uint32_t skill_id, bool from_interrupt = false);
    const CooldownConfig* FindCooldownConfig(int skill_id) const;
    // Overload used when creating a new tracked cooldown, so an Enemy-only or Ally-only config entry
    // doesn't get matched against the wrong target category.
    const CooldownConfig* FindCooldownConfig(int skill_id, CooldownTarget target) const;
    float GetAgentRechargeFactor(uint32_t agent_id, uint32_t now) const;
    void RecordAgentRechargeFactor(uint32_t agent_id, float factor, uint32_t now);
    void PrintTrackedCooldowns();
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
    std::vector<CooldownConfig> cooldown_configs;
    std::vector<TrackedCooldown> tracked_cooldowns;
    std::vector<TrackedKnockdown> tracked_knockdowns;
    std::vector<InProgressCast> in_progress_casts;
    std::vector<InterruptFlag> confirmed_interrupts;
    std::vector<AgentRechargeFactor> agent_recharge_factors;
    // Effect IDs entered in settings. Stored so they survive the rework; tracking of them is not implemented yet
    // and every entry is reported as unavailable.
    std::vector<int> effect_ids;
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
    bool settings_window_visible = false;
    bool pending_welcome_message = false;
    bool widget_mode = false;
    bool enemy_nameplates_enabled = true;
    // Default ON: showing tracked cooldowns on party nameplates is opt-out, not opt-in.
    bool ally_nameplates_enabled = true;
    bool nameplate_show_knockdown = true;
    bool nameplate_show_cooldowns = true;
    bool adaptive_recharge_enabled = true;
    float cooldown_icon_size = 22.f;
};
