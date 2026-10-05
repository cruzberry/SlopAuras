#pragma once

#include <atomic>
#include <ToolboxUIPlugin.h>
#include <GWCA/Managers/ChatMgr.h>
#include <GWCA/Managers/UIMgr.h>

class SlopAuras : public ToolboxUIPlugin {
public:
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
    struct TrackedCast {
        uint32_t skill_id;
        uint32_t target_agent_id;
        uint32_t timestamp;
        uint32_t duration_ms;
    };

    struct TrackedCooldown {
        uint32_t skill_id;
        uint32_t agent_id;
        uint32_t timestamp;
        uint32_t duration_ms;
    };

    struct PendingCast {
        uint32_t skill_id;
        uint32_t target_agent_id;
        uint32_t timestamp;
    };

    void TrackCast(int skill_id, uint32_t target_agent_id);
    void TrackEnemyCooldown(uint32_t agent_id, uint32_t skill_id);
    bool IsMapReady() const;
    static void HandleChatCommand(GW::HookStatus* status, const wchar_t* command, int argc, const LPWSTR* argv);
    void DrawSettingsWindow();

    GW::HookEntry chat_command_hook;
    GW::HookEntry skill_activated_hook;
    GW::HookEntry skill_started_cast_hook;
    GW::HookEntry map_loading_hook;
    std::atomic<uint32_t> map_generation = 0;
    std::atomic<uint32_t> ready_map_generation = 0;
    std::mutex tracking_mutex;
    std::vector<int> effect_ids;
    std::vector<int> cast_by_me_entries;
    std::vector<int> cooldown_ids;
    std::vector<int> natural_resistance_model_ids;
    std::vector<TrackedCast> tracked_casts;
    std::vector<TrackedCooldown> tracked_cooldowns;
    std::vector<PendingCast> pending_casts;
    bool settings_window_visible = false;
    bool widget_mode = false;
    float effect_icon_size = 22.f;
    float cooldown_icon_size = 22.f;
};
