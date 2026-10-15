#include "SlopAuras.h"

#include <PluginUtils.h>

#include <Windows.h>
#include <commdlg.h>
#include <cmath>
#include <d3d9.h>
#include <mmsystem.h>

#include <cctype>
#include <cfloat>
#include <iomanip>
#include <limits>
#include <sstream>

#include <GWCA/Constants/Constants.h>
#include <GWCA/Context/WorldContext.h>
#include <GWCA/GameEntities/Agent.h>
#include <GWCA/GameEntities/Attribute.h>
#include <GWCA/GameEntities/Camera.h>
#include <GWCA/GameEntities/Skill.h>
#include <GWCA/GameEntities/Title.h>
#include <GWCA/Managers/AgentMgr.h>
#include <GWCA/Managers/CameraMgr.h>
#include <GWCA/Managers/EffectMgr.h>
#include <GWCA/Managers/GameThreadMgr.h>
#include <GWCA/Managers/ItemMgr.h>
#include <GWCA/Managers/MapMgr.h>
#include <GWCA/Managers/MemoryMgr.h>
#include <GWCA/Managers/PartyMgr.h>
#include <GWCA/Managers/PlayerMgr.h>
#include <GWCA/Managers/RenderMgr.h>
#include <GWCA/Managers/SkillbarMgr.h>
#include <GWCA/Packets/StoC.h>
#include <GWCA/Managers/UIMgr.h>

#include <DirectXMath.h>
#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "Winmm.lib")

extern "C" __declspec(dllimport) IDirect3DTexture9** __cdecl GetSkillImage(GW::Constants::SkillID skill_id);

namespace {
    bool IsGameWorldReady()
    {
        return GW::Map::GetInstanceType() != GW::Constants::InstanceType::Loading
            && GW::Map::GetIsMapLoaded()
            && GW::Agents::GetControlledCharacter();
    }

    bool IsEnemyAgent(const uint32_t agent_id)
    {
        const auto* agent = GW::Agents::GetAgentByID(agent_id);
        const auto* living = agent ? agent->GetAsAgentLiving() : nullptr;
        return living && living->allegiance == GW::Constants::Allegiance::Enemy;
    }

    // Effect tracking is being reworked. Until the classification CSV is wired in, no skill is available and
    // every entry is reported as unavailable in settings.
    bool IsEffectTrackingAvailable(const int skill_id)
    {
        (void)skill_id;
        return false;
    }

    // True for players and heroes in the local party (the same scope GetPartyEffectsArray() covers); used to
    // decide whether an agent that isn't an enemy counts as a tracked "ally" for cooldowns/nameplates.
    bool IsPartyAgent(const uint32_t agent_id)
    {
        const auto* party_effects = GW::Effects::GetPartyEffectsArray();
        if (!party_effects || !party_effects->valid()) {
            return false;
        }
        return std::ranges::any_of(*party_effects, [agent_id](const GW::AgentEffects& agent_effects) {
            return agent_effects.agent_id == agent_id;
        });
    }

    struct AgentProjection {
        DirectX::XMMATRIX view_projection;
        uint32_t viewport_width;
        uint32_t viewport_height;
    };

    bool GetAgentProjection(AgentProjection& projection)
    {
        const auto* camera = GW::CameraMgr::GetCamera();
        const auto viewport_width = GW::Render::GetViewportWidth();
        const auto viewport_height = GW::Render::GetViewportHeight();
        const auto fov = GW::Render::GetFieldOfView();
        if (!camera || !viewport_width || !viewport_height || !std::isfinite(fov) || fov <= 0.f) {
            return false;
        }
        const auto eye = DirectX::XMVectorSet(camera->position.x, camera->position.y, camera->position.z, 1.f);
        const auto target = DirectX::XMVectorSet(camera->look_at_target.x, camera->look_at_target.y, camera->look_at_target.z, 1.f);
        const auto up = DirectX::XMVectorSet(0.f, 0.f, -1.f, 0.f);
        const auto view = DirectX::XMMatrixLookAtLH(eye, target, up);
        const auto projection_matrix = DirectX::XMMatrixPerspectiveFovLH(
            fov, static_cast<float>(viewport_width) / static_cast<float>(viewport_height), 0.1f, 100000.f);
        projection.view_projection = DirectX::XMMatrixMultiply(view, projection_matrix);
        projection.viewport_width = viewport_width;
        projection.viewport_height = viewport_height;
        return true;
    }

    bool ProjectAgentNameToScreen(const AgentProjection& projection, const GW::Agent* agent, ImVec2& screen_position)
    {
        if (!agent) {
            return false;
        }
        const auto world_position = DirectX::XMVectorSet(agent->name_tag_x, agent->name_tag_y, agent->name_tag_z, 1.f);
        const auto clip_position = DirectX::XMVector4Transform(world_position, projection.view_projection);
        DirectX::XMFLOAT4 clip;
        DirectX::XMStoreFloat4(&clip, clip_position);
        if (!std::isfinite(clip.x) || !std::isfinite(clip.y) || !std::isfinite(clip.z)
            || !std::isfinite(clip.w) || clip.w <= 0.f) {
            return false;
        }

        const auto depth = clip.z / clip.w;
        const auto normalized_x = clip.x / clip.w;
        const auto normalized_y = clip.y / clip.w;
        if (depth < 0.f || depth > 1.f || normalized_x < -1.f || normalized_x > 1.f
            || normalized_y < -1.f || normalized_y > 1.f) {
            return false;
        }
        screen_position.x = (normalized_x + 1.f) * static_cast<float>(projection.viewport_width) * 0.5f;
        screen_position.y = (1.f - normalized_y) * static_cast<float>(projection.viewport_height) * 0.5f;
        return std::isfinite(screen_position.x) && std::isfinite(screen_position.y);
    }

    constexpr uint32_t recharge_factor_ttl_ms = 30000; // speed-up effects are short-lived; forget what we learned after this
    constexpr float min_recharge_factor = 0.5f;        // recharge reductions do not stack beyond -50%
    constexpr uint32_t interrupt_match_window_ms = 1500;
    constexpr uint32_t cast_in_progress_timeout_ms = 10000; // generous upper bound on any skill's cast time

    struct SkillName {
        std::mutex mutex;
        std::wstring encoded;
        std::string value;
        bool requested = false;
        bool decoded = false;
    };

    void OnSkillNameDecoded(void* param, const wchar_t* decoded)
    {
        auto* name = static_cast<SkillName*>(param);
        if (!name) {
            return;
        }
        std::lock_guard lock(name->mutex);
        if (decoded && decoded[0]) {
            name->value = PluginUtils::WStringToString(decoded);
        }
        name->decoded = true;
    }

    std::string GetDecodedName(const std::wstring& encoded)
    {
        // Reached from the UI-message hooks and from Draw, which can run on different threads.
        static std::mutex names_mutex;
        static std::unordered_map<std::wstring, std::unique_ptr<SkillName>> names;
        SkillName* cached_name = nullptr;
        {
            std::lock_guard names_lock(names_mutex);
            auto& name = names[encoded];
            if (!name) {
                auto new_name = std::make_unique<SkillName>();
                new_name->encoded = encoded;
                name = std::move(new_name);
            }
            cached_name = name.get();
        }
        bool request_decode = false;
        std::string decoded_name;
        bool decode_complete = false;
        {
            std::lock_guard lock(cached_name->mutex);
            if (!cached_name->requested) {
                cached_name->requested = true;
                request_decode = true;
            }
            decoded_name = cached_name->value;
            decode_complete = cached_name->decoded;
        }
        if (request_decode) {
            GW::GameThread::Enqueue([cached_name] {
                GW::UI::AsyncDecodeStr(cached_name->encoded.c_str(), OnSkillNameDecoded, cached_name);
            });
        }
        return decoded_name.empty() ? (decode_complete ? std::string{} : std::string("Loading name...")) : decoded_name;
    }

    std::string GetSkillName(const int skill_id)
    {
        if (!IsGameWorldReady() || skill_id <= 0 || static_cast<uint32_t>(skill_id) >= GW::SkillbarMgr::GetSkillCount()) {
            return "Unknown skill";
        }
        const auto skill = GW::SkillbarMgr::GetSkillConstantData(static_cast<GW::Constants::SkillID>(skill_id));
        if (!skill || !skill->name) {
            return "Unknown skill";
        }
        std::array<wchar_t, 16> encoded{};
        if (!GW::UI::UInt32ToEncStr(skill->name, encoded.data(), encoded.size())) {
            return "Unknown skill";
        }
        const auto name = GetDecodedName(encoded.data());
        return name.empty() ? "Unknown skill" : name;
    }

    std::string GetAgentName(const uint32_t agent_id)
    {
        if (!IsGameWorldReady() || !agent_id) {
            return "Unknown target";
        }
        const auto* encoded = GW::Agents::GetAgentEncName(agent_id);
        if (!encoded || !encoded[0]) {
            return "Agent " + std::to_string(agent_id);
        }
        const auto name = GetDecodedName(encoded);
        return name.empty() ? "Unknown target" : name;
    }

    std::wstring ToWideString(const std::string& value)
    {
        if (value.empty()) {
            return {};
        }
        const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), nullptr, 0);
        if (!length) {
            return L"Unknown name";
        }
        std::wstring result(static_cast<size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), length);
        return result;
    }

    bool BrowseForWaveFile(std::string& selected_path)
    {
        std::array<wchar_t, 32768> path{};
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = GetForegroundWindow();
        dialog.lpstrFilter = L"Wave audio (*.wav)\0*.wav\0\0";
        dialog.lpstrFile = path.data();
        dialog.nMaxFile = static_cast<DWORD>(path.size());
        dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&dialog)) {
            if (CommDlgExtendedError()) {
                OutputDebugStringW(L"SlopAuras: WAV file picker failed.\n");
            }
            return false;
        }
        const auto extension = PluginUtils::ToLower(std::filesystem::path(path.data()).extension().wstring());
        if (extension != L".wav") {
            return false;
        }
        const std::wstring wide_path(path.data());
        if (wide_path.size() >= 256) {
            GW::Chat::WriteChat(GW::Chat::CHANNEL_GWCA2,
                L"notification WAV path must be shorter than 256 characters.", L"SlopAuras");
            return false;
        }
        selected_path = PluginUtils::WStringToString(wide_path);
        return !selected_path.empty();
    }
}

DLLAPI ToolboxPlugin* ToolboxPluginInstance()
{
    static SlopAuras instance;
    return &instance;
}

SlopAuras::~SlopAuras()
{
    StopNotificationWorker();
}

void SlopAuras::HandleChatCommand(GW::HookStatus* status, const wchar_t*, const int argc, const LPWSTR* argv)
{
    if (!status) {
        return;
    }
    status->blocked = true;

    const auto print_help = [] {
        GW::Chat::WriteChat(GW::Chat::CHANNEL_GWCA2, L"/sa help - What you just typed!", L"SlopAuras");
        GW::Chat::WriteChat(GW::Chat::CHANNEL_GWCA2, L"/sa (no argument) / show / hide - Toggle the SlopAuras window.", L"SlopAuras");
        GW::Chat::WriteChat(GW::Chat::CHANNEL_GWCA2, L"/sa settings / s - Toggle the SlopAuras settings window.", L"SlopAuras");
        GW::Chat::WriteChat(GW::Chat::CHANNEL_GWCA2, L"/sa print - Print tracked enemy cooldowns.", L"SlopAuras");
        GW::Chat::WriteChat(GW::Chat::CHANNEL_GWCA2, L"/sa mute / unmute - Toggle SlopAuras sound notifications.", L"SlopAuras");
    };

    auto* instance = static_cast<SlopAuras*>(ToolboxPluginInstance());

    // Bare "/sa" behaves the same as "/sa show".
    if (argc < 2 || !argv || !argv[1]) {
        auto* visible = instance->GetVisiblePtr();
        *visible = true;
        GW::Chat::WriteChat(GW::Chat::CHANNEL_GWCA2, L"Window shown.", L"SlopAuras");
        return;
    }

    const auto subcommand = PluginUtils::ToLower(argv[1]);
    if (subcommand == L"hide" || subcommand == L"show") {
        auto* visible = instance->GetVisiblePtr();
        *visible = !*visible;
        GW::Chat::WriteChat(GW::Chat::CHANNEL_GWCA2, *visible ? L"Window shown." : L"Window hidden.", L"SlopAuras");
    }
    else if (subcommand == L"settings" || subcommand == L"s") {
        instance->settings_window_visible = !instance->settings_window_visible;
        GW::Chat::WriteChat(GW::Chat::CHANNEL_GWCA2, instance->settings_window_visible
            ? L"Settings window shown."
            : L"Settings window hidden.", L"SlopAuras");
    }
    else if (subcommand == L"print") {
        instance->PrintTrackedCooldowns();
    }
    else if (subcommand == L"mute" || subcommand == L"unmute") {
        const auto muted = !instance->notifications_muted.load(std::memory_order_acquire);
        instance->SetNotificationsMuted(muted);
        GW::Chat::WriteChat(GW::Chat::CHANNEL_GWCA2, muted
            ? L"Sound notifications muted."
            : L"Sound notifications unmuted.", L"SlopAuras");
    }
    else if (subcommand == L"bing") {
        GW::Chat::WriteChat(GW::Chat::CHANNEL_GWCA2, L"Wǒ fēicháng xǐhuān BING CHILLING", L"SlopAuras");
    }
    else if (subcommand == L"help") {
        print_help();
    }
    else {
        GW::Chat::WriteChat(GW::Chat::CHANNEL_GWCA2, L"Invalid command. Type /sa help for a list of commands.", L"SlopAuras");
    }
}

const SlopAuras::CooldownConfig* SlopAuras::FindCooldownConfig(const int skill_id) const
{
    const auto it = std::ranges::find(cooldown_configs, skill_id, &CooldownConfig::skill_id);
    return it != cooldown_configs.end() ? &*it : nullptr;
}

const SlopAuras::CooldownConfig* SlopAuras::FindCooldownConfig(const int skill_id, const CooldownTarget target) const
{
    const auto it = std::ranges::find_if(cooldown_configs, [skill_id, target](const CooldownConfig& config) {
        return config.skill_id == skill_id && config.target == target;
    });
    return it != cooldown_configs.end() ? &*it : nullptr;
}

float SlopAuras::GetAgentRechargeFactor(const uint32_t agent_id, const uint32_t now) const
{
    for (const auto& entry : agent_recharge_factors) {
        if (entry.agent_id == agent_id && now - entry.observed_at <= recharge_factor_ttl_ms) {
            return entry.factor;
        }
    }
    return 1.f;
}

void SlopAuras::RecordAgentRechargeFactor(const uint32_t agent_id, const float factor, const uint32_t now)
{
    std::erase_if(agent_recharge_factors, [now](const AgentRechargeFactor& entry) {
        return now - entry.observed_at > recharge_factor_ttl_ms;
    });
    const auto existing = std::ranges::find(agent_recharge_factors, agent_id, &AgentRechargeFactor::agent_id);
    if (existing != agent_recharge_factors.end()) {
        existing->factor = factor;
        existing->observed_at = now;
    }
    else {
        agent_recharge_factors.push_back({agent_id, factor, now});
    }
}

std::string SlopAuras::ResolveNotificationSoundPath(const NotificationType type, const uint32_t skill_id) const
{
    const auto index = static_cast<size_t>(type);
    if (index >= notification_enabled.size() || !notification_enabled[index]) {
        return {};
    }

    const auto non_empty = [](const std::string& path) -> const std::string* {
        return path.empty() ? nullptr : &path;
    };
    const std::string* override_path = nullptr;
    if (skill_id) {
        switch (type) {
            case NotificationType::CooldownReady:
                if (const auto* config = FindCooldownConfig(static_cast<int>(skill_id))) {
                    override_path = non_empty(config->ready_sound);
                }
                break;
            default:
                break;
        }
    }
    return override_path ? *override_path : notification_sound_paths[index];
}

void SlopAuras::PlayNotification(const NotificationType type, const uint32_t skill_id)
{
    if (notifications_muted.load(std::memory_order_acquire)) {
        return;
    }

    std::string path;
    {
        std::lock_guard lock(tracking_mutex);
        path = ResolveNotificationSoundPath(type, skill_id);
    }
    if (path.empty()) {
        return;
    }

    auto wide_path = PluginUtils::StringToWString(path);
    if (wide_path.empty()) {
        OutputDebugStringW(L"SlopAuras: selected notification WAV path could not be converted.\n");
        return;
    }
    {
        std::lock_guard lock(notification_queue_mutex);
        if (notification_worker_stopping) {
            return;
        }
        if ((notification_playing && notification_playing_type == type && notification_playing_skill_id == skill_id)
            || std::ranges::any_of(notification_queue, [type, skill_id](const QueuedNotification& queued) {
                return queued.type == type && queued.skill_id == skill_id;
            })) {
            return;
        }
        notification_queue.push_back({type, skill_id, std::move(wide_path)});
    }
    notification_condition.notify_one();
}

void SlopAuras::NotificationWorker()
{
    std::unique_lock lock(notification_queue_mutex);
    for (;;) {
        notification_condition.wait(lock, [this] {
            return notification_worker_stopping || !notification_queue.empty();
        });
        if (notification_worker_stopping && notification_queue.empty()) {
            return;
        }
        auto notification = std::move(notification_queue.front());
        notification_queue.pop_front();
        notification_playing = true;
        notification_playing_type = notification.type;
        notification_playing_skill_id = notification.skill_id;
        lock.unlock();
        if (!notifications_muted.load(std::memory_order_acquire)
            && !PlaySoundW(notification.path.c_str(), nullptr, SND_FILENAME | SND_SYNC | SND_NODEFAULT)) {
            OutputDebugStringW(L"SlopAuras: failed to play the selected notification WAV file.\n");
        }
        lock.lock();
        notification_playing = false;
    }
}

void SlopAuras::StopNotificationWorker()
{
    {
        std::lock_guard lock(notification_queue_mutex);
        notification_worker_stopping = true;
        notification_queue.clear();
    }
    PlaySoundW(nullptr, nullptr, 0);
    notification_condition.notify_all();
    if (notification_worker.joinable()) {
        notification_worker.join();
    }
}

void SlopAuras::SetNotificationsMuted(const bool muted)
{
    notifications_muted.store(muted, std::memory_order_release);
    if (!muted) {
        return;
    }
    {
        std::lock_guard lock(notification_queue_mutex);
        notification_queue.clear();
    }
    PlaySoundW(nullptr, nullptr, 0);
}

void SlopAuras::PrintTrackedCooldowns()
{
    if (!IsMapReady()) {
        GW::Chat::WriteChat(GW::Chat::CHANNEL_GWCA2, L"Map is not ready.", L"SlopAuras");
        return;
    }

    const auto generation = map_generation.load(std::memory_order_acquire);
    const auto now = GW::MemoryMgr::GetSkillTimer();
    std::vector<TrackedCooldown> tracked_cooldowns_snapshot;
    {
        std::lock_guard lock(tracking_mutex);
        for (const auto& cooldown : tracked_cooldowns) {
            if (FindCooldownConfig(static_cast<int>(cooldown.skill_id))) {
                tracked_cooldowns_snapshot.push_back(cooldown);
            }
        }
    }

    std::vector<std::wstring> messages;
    for (const auto& cooldown : tracked_cooldowns_snapshot) {
        const auto elapsed = now - cooldown.timestamp;
        if (elapsed >= cooldown.duration_ms) {
            continue;
        }
        std::ostringstream message;
        message << "Cooldown: " << GetSkillName(static_cast<int>(cooldown.skill_id)) << " on "
            << GetAgentName(cooldown.agent_id) << " - " << (cooldown.from_interrupt ? "at least " : "")
            << std::fixed << std::setprecision(1)
            << static_cast<double>(cooldown.duration_ms - elapsed) / 1000.0 << "s remaining";
        messages.push_back(ToWideString(message.str()));
    }

    if (generation != map_generation.load(std::memory_order_acquire) || !IsMapReady()) {
        return;
    }
    if (messages.empty()) {
        GW::Chat::WriteChat(GW::Chat::CHANNEL_GWCA2, L"No tracked cooldowns active.", L"SlopAuras");
        return;
    }
    for (const auto& message : messages) {
        GW::Chat::WriteChat(GW::Chat::CHANNEL_GWCA2, message.c_str(), L"SlopAuras");
    }
}

void SlopAuras::Initialize(ImGuiContext* ctx, ImGuiAllocFns allocator_fns, HMODULE toolbox_dll)
{
    ToolboxUIPlugin::Initialize(ctx, allocator_fns, toolbox_dll);
    {
        std::lock_guard lock(notification_queue_mutex);
        notification_worker_stopping = false;
    }
    notification_worker = std::thread(&SlopAuras::NotificationWorker, this);
    GW::Chat::CreateCommand(&chat_command_hook, L"sa", HandleChatCommand);
    GW::UI::RegisterUIMessageCallback(&map_loading_hook, GW::UI::UIMessage::kLoadMapContext, [this](GW::HookStatus*, GW::UI::UIMessage, void*, void*) {
        map_generation.fetch_add(1, std::memory_order_acq_rel);
        std::lock_guard lock(tracking_mutex);
        tracked_cooldowns.clear();
        tracked_knockdowns.clear();
        in_progress_casts.clear();
        confirmed_interrupts.clear();
        agent_recharge_factors.clear();
    });
    const auto knockdown_callback_registered = GW::StoC::RegisterPacketCallback<GW::Packet::StoC::GenericFloat>(&knockdown_hook, [this](GW::HookStatus*, GW::Packet::StoC::GenericFloat* packet) {
        if (!packet || packet->type != GW::Packet::StoC::GenericValueID::knocked_down
            || !IsMapReady() || !std::isfinite(packet->value) || packet->value <= 0.f) {
            return;
        }
        const auto duration_ms_value = static_cast<double>(packet->value) * 1000.0;
        if (duration_ms_value > static_cast<double>(std::numeric_limits<uint32_t>::max())) {
            return;
        }
        const auto generation = map_generation.load(std::memory_order_acquire);
        const auto now = GW::MemoryMgr::GetSkillTimer();
        const auto duration_ms = static_cast<uint32_t>(duration_ms_value);
        uint32_t interrupted_skill_id = 0;
        {
            std::lock_guard lock(tracking_mutex);
            if (generation != map_generation.load(std::memory_order_acquire) || !IsMapReady()) {
                return;
            }
            if (IsEnemyAgent(packet->agent_id)) {
                const auto existing = std::ranges::find_if(tracked_knockdowns, [packet](const TrackedKnockdown& knockdown) {
                    return knockdown.agent_id == packet->agent_id;
                });
                if (existing != tracked_knockdowns.end()) {
                    existing->timestamp = now;
                    existing->duration_ms = duration_ms;
                }
                else {
                    tracked_knockdowns.push_back({packet->agent_id, now, duration_ms});
                }
            }
            // A knockdown interrupts whatever skill the agent was casting, but (unlike a real interrupt) never
            // sends the server's "interrupted" confirmation, so the confirmed-interrupt path below never fires
            // for it. Start that skill's cooldown directly from our own in-progress-cast bookkeeping instead.
            std::erase_if(in_progress_casts, [now](const InProgressCast& cast) {
                return now - cast.timestamp > cast_in_progress_timeout_ms;
            });
            const auto casting = std::ranges::find_if(in_progress_casts, [packet](const InProgressCast& cast) {
                return cast.agent_id == packet->agent_id;
            });
            if (casting != in_progress_casts.end()) {
                interrupted_skill_id = casting->skill_id;
                in_progress_casts.erase(casting);
            }
        }
        if (interrupted_skill_id) {
            TrackAgentCooldown(packet->agent_id, interrupted_skill_id, true);
        }
    });
    if (!knockdown_callback_registered) {
        OutputDebugStringW(L"SlopAuras: could not register the enemy knockdown timer callback.\n");
    }
    // kAgentSkillInterrupted fires for real interrupts and for plain self-cancels alike (Toolbox's own SkillMonitor
    // documents this). Only the server's StoC "interrupted" packet, which arrives first, tells them apart. A real
    // interrupt still starts the skill's recharge; a cancel does not.
    const auto generic_value_callback_registered = GW::StoC::RegisterPacketCallback<GW::Packet::StoC::GenericValue>(&generic_value_hook, [this](GW::HookStatus*, GW::Packet::StoC::GenericValue* packet) {
        if (!packet || packet->value_id != GW::Packet::StoC::GenericValueID::interrupted || !IsMapReady()) {
            return;
        }
        const auto now = GW::MemoryMgr::GetSkillTimer();
        std::lock_guard lock(tracking_mutex);
        std::erase_if(confirmed_interrupts, [now](const InterruptFlag& flag) {
            return now - flag.timestamp > interrupt_match_window_ms;
        });
        confirmed_interrupts.push_back({packet->agent_id, now});
    });
    if (!generic_value_callback_registered) {
        OutputDebugStringW(L"SlopAuras: could not register the interrupt callback; interrupted enemy skills will not start cooldowns.\n");
    }
    GW::UI::RegisterUIMessageCallback(&skill_activated_hook, GW::UI::UIMessage::kAgentSkillInterrupted, [this](GW::HookStatus*, GW::UI::UIMessage, void* wparam, void*) {
        const auto* packet = static_cast<GW::UI::UIPacket::kAgentSkillPacket*>(wparam);
        if (!packet || !IsMapReady()) {
            return;
        }
        const auto now = GW::MemoryMgr::GetSkillTimer();
        bool confirmed = false;
        {
            std::lock_guard lock(tracking_mutex);
            std::erase_if(confirmed_interrupts, [now](const InterruptFlag& flag) {
                return now - flag.timestamp > interrupt_match_window_ms;
            });
            const auto flag = std::ranges::find(confirmed_interrupts, packet->agent_id, &InterruptFlag::agent_id);
            if (flag != confirmed_interrupts.end()) {
                confirmed = true;
                confirmed_interrupts.erase(flag);
            }
            // This agent is no longer casting anything, whether this was a real interrupt or a plain cancel.
            std::erase_if(in_progress_casts, [agent_id = packet->agent_id](const InProgressCast& cast) {
                return cast.agent_id == agent_id;
            });
        }
        if (confirmed) {
            TrackAgentCooldown(packet->agent_id, static_cast<uint32_t>(packet->skill_id), true);
        }
    });
    GW::UI::RegisterUIMessageCallback(&skill_started_cast_hook, GW::UI::UIMessage::kAgentSkillStartedCast, [this](GW::HookStatus*, GW::UI::UIMessage, void* wparam, void*) {
        const auto* packet = static_cast<GW::UI::UIPacket::kAgentSkillStartedCast*>(wparam);
        if (!packet || !IsMapReady()) {
            return;
        }
        const auto generation = map_generation.load(std::memory_order_acquire);
        const auto skill_id = static_cast<uint32_t>(packet->skill_id);
        if (skill_id >= GW::SkillbarMgr::GetSkillCount()) {
            return;
        }
        const auto now = GW::MemoryMgr::GetSkillTimer();
        std::lock_guard lock(tracking_mutex);
        if (generation == map_generation.load(std::memory_order_acquire) && IsMapReady()
            && FindCooldownConfig(static_cast<int>(skill_id))) {
            // Any agent whose cooldown we might track: remember it's casting this skill, in case a
            // knockdown cuts the cast short (see the knockdown_hook callback above).
            const auto agent_id = packet->agent_id;
            std::erase_if(in_progress_casts, [now](const InProgressCast& cast) {
                return now - cast.timestamp > cast_in_progress_timeout_ms;
            });
            std::erase_if(in_progress_casts, [agent_id](const InProgressCast& cast) {
                return cast.agent_id == agent_id;
            });
            in_progress_casts.push_back({agent_id, skill_id, now});
        }
    });
    GW::UI::RegisterUIMessageCallback(&skill_activated_hook, GW::UI::UIMessage::kAgentSkillActivated, [this](GW::HookStatus*, GW::UI::UIMessage, void* wparam, void*) {
        const auto* packet = static_cast<GW::UI::UIPacket::kAgentSkillPacket*>(wparam);
        if (packet && IsMapReady()) {
            TrackAgentCooldown(packet->agent_id, static_cast<uint32_t>(packet->skill_id));
            std::lock_guard lock(tracking_mutex);
            std::erase_if(in_progress_casts, [agent_id = packet->agent_id](const InProgressCast& cast) {
                return cast.agent_id == agent_id;
            });
        }
    });
    GW::UI::RegisterUIMessageCallback(&skill_activated_hook, GW::UI::UIMessage::kAgentSkillActivatedInstantly, [this](GW::HookStatus*, GW::UI::UIMessage, void* wparam, void*) {
        const auto* packet = static_cast<GW::UI::UIPacket::kAgentSkillPacket*>(wparam);
        if (packet && IsMapReady()) {
            TrackAgentCooldown(packet->agent_id, static_cast<uint32_t>(packet->skill_id));
            std::lock_guard lock(tracking_mutex);
            std::erase_if(in_progress_casts, [agent_id = packet->agent_id](const InProgressCast& cast) {
                return cast.agent_id == agent_id;
            });
        }
    });
}

void SlopAuras::SignalTerminate()
{
    ToolboxUIPlugin::SignalTerminate();
    GW::Chat::DeleteCommand(&chat_command_hook, L"sa");
}

void SlopAuras::Terminate()
{
    StopNotificationWorker();
    GW::UI::RemoveUIMessageCallback(&skill_activated_hook);
    GW::UI::RemoveUIMessageCallback(&skill_started_cast_hook);
    GW::UI::RemoveUIMessageCallback(&map_loading_hook);
    GW::StoC::RemoveCallback<GW::Packet::StoC::GenericFloat>(&knockdown_hook);
    GW::StoC::RemoveCallback<GW::Packet::StoC::GenericValue>(&generic_value_hook);
    {
        std::lock_guard lock(tracking_mutex);
        tracked_cooldowns.clear();
        tracked_knockdowns.clear();
        in_progress_casts.clear();
        confirmed_interrupts.clear();
        agent_recharge_factors.clear();
    }
    ToolboxUIPlugin::Terminate();
}

bool SlopAuras::IsMapReady() const
{
    const auto generation = map_generation.load(std::memory_order_acquire);
    return generation == ready_map_generation.load(std::memory_order_acquire)
        && IsGameWorldReady()
        && generation == map_generation.load(std::memory_order_acquire);
}

void SlopAuras::Update(float delta)
{
    ToolboxUIPlugin::Update(delta);
    const auto generation = map_generation.load(std::memory_order_acquire);
    if (!IsGameWorldReady() || generation != map_generation.load(std::memory_order_acquire)) {
        return;
    }
    ready_map_generation.store(generation, std::memory_order_release);
    if (!IsMapReady()) {
        return;
    }
    if (pending_welcome_message) {
        pending_welcome_message = false;
        GW::Chat::WriteChat(GW::Chat::CHANNEL_GWCA2,
            L"Welcome to SlopAuras! Type /sa help to list commands and /sa s to open settings. Enjoy!", L"SlopAuras");
    }
    const auto now = GW::MemoryMgr::GetSkillTimer();
    struct PendingNotificationEvent {
        NotificationType type;
        uint32_t skill_id;
    };
    std::vector<PendingNotificationEvent> pending_notifications;
    {
        std::lock_guard lock(tracking_mutex);
        std::erase_if(tracked_cooldowns, [this, generation, now, &pending_notifications](const TrackedCooldown& cooldown) {
            if (generation != map_generation.load(std::memory_order_acquire) || !IsMapReady()) {
                return true;
            }
            const auto* agent = GW::Agents::GetAgentByID(cooldown.agent_id);
            const auto* living = agent ? agent->GetAsAgentLiving() : nullptr;
            if (!living || !living->GetIsAlive() || living->allegiance != GW::Constants::Allegiance::Enemy) {
                return true;
            }
            if (now - cooldown.timestamp < cooldown.duration_ms) {
                return false;
            }
            if (notification_enabled[static_cast<size_t>(NotificationType::CooldownReady)]) {
                pending_notifications.push_back({NotificationType::CooldownReady, cooldown.skill_id});
            }
            return true;
        });
        std::erase_if(tracked_knockdowns, [this, now](const TrackedKnockdown& knockdown) {
            if (now - knockdown.timestamp >= knockdown.duration_ms) {
                return true;
            }
            const auto* agent = GW::Agents::GetAgentByID(knockdown.agent_id);
            const auto* living = agent ? agent->GetAsAgentLiving() : nullptr;
            return !living || !living->GetIsAlive() || living->allegiance != GW::Constants::Allegiance::Enemy;
        });
    }
    for (const auto& event : pending_notifications) {
        PlayNotification(event.type, event.skill_id);
    }
}

void SlopAuras::DrawEnemyNameplates(IDirect3DDevice9* device)
{
    if ((!enemy_nameplates_enabled && !ally_nameplates_enabled) || !device || !IsMapReady()) {
        return;
    }

    const auto generation = map_generation.load(std::memory_order_acquire);
    const auto now = GW::MemoryMgr::GetSkillTimer();
    std::unordered_set<int> tracked_cooldown_ids;
    std::vector<TrackedCooldown> cooldowns;
    std::vector<TrackedKnockdown> knockdowns;
    {
        std::lock_guard lock(tracking_mutex);
        for (const auto& config : cooldown_configs) {
            tracked_cooldown_ids.insert(config.skill_id);
        }
        cooldowns = tracked_cooldowns;
        knockdowns = tracked_knockdowns;
    }

    if (generation != map_generation.load(std::memory_order_acquire) || !IsMapReady()) {
        return;
    }
    const auto* agents = GW::Agents::GetAgentArray();
    AgentProjection projection;
    if (!agents || !GetAgentProjection(projection)) {
        return;
    }
    auto* draw_list = ImGui::GetForegroundDrawList();
    for (const auto* agent : *agents) {
        if (!agent || generation != map_generation.load(std::memory_order_acquire)) {
            continue;
        }
        const auto* living = agent->GetAsAgentLiving();
        if (!living || !living->GetIsAlive()) {
            continue;
        }
        const auto is_enemy = living->allegiance == GW::Constants::Allegiance::Enemy;
        if (is_enemy) {
            if (!enemy_nameplates_enabled) {
                continue;
            }
        }
        else if (ally_nameplates_enabled && IsPartyAgent(agent->agent_id)) {
            // Allowed through below.
        }
        else {
            continue;
        }

        struct NameplateLine {
            std::string text;
            ImU32 color;
        };
        std::vector<NameplateLine> lines;
        if (nameplate_show_knockdown && living->GetIsKnockedDown()) {
            const auto timer = std::ranges::find_if(knockdowns, [agent](const TrackedKnockdown& knockdown) {
                return knockdown.agent_id == agent->agent_id;
            });
            if (timer != knockdowns.end() && now - timer->timestamp < timer->duration_ms) {
                const auto remaining = timer->duration_ms - (now - timer->timestamp);
                lines.push_back({std::format("DOWN  {:.1f}s", static_cast<double>(remaining) / 1000.0),
                    IM_COL32(255, 90, 80, 255)});
            }
            else {
                lines.push_back({"KNOCKED DOWN", IM_COL32(255, 90, 80, 255)});
            }
        }

        if (nameplate_show_cooldowns) {
            for (const auto& cooldown : cooldowns) {
                if (cooldown.agent_id != agent->agent_id
                    || !tracked_cooldown_ids.contains(static_cast<int>(cooldown.skill_id))) {
                    continue;
                }
                const auto elapsed = now - cooldown.timestamp;
                if (elapsed >= cooldown.duration_ms) {
                    continue;
                }
                const auto name = GetSkillName(static_cast<int>(cooldown.skill_id));
                lines.push_back({std::format("{} {}  {:.1f}s", cooldown.from_interrupt ? "CD~" : "CD", name,
                    static_cast<double>(cooldown.duration_ms - elapsed) / 1000.0), IM_COL32(255, 190, 135, 255)});
            }
        }

        if (lines.empty()) {
            continue;
        }
        ImVec2 name_position;
        if (!ProjectAgentNameToScreen(projection, agent, name_position)) {
            continue;
        }

        std::vector<ImVec2> text_sizes;
        text_sizes.reserve(lines.size());
        float max_width = 0.f;
        float total_height = 0.f;
        for (const auto& line : lines) {
            const auto text_size = ImGui::CalcTextSize(line.text.c_str());
            text_sizes.push_back(text_size);
            max_width = std::max(max_width, text_size.x);
            total_height += text_size.y;
        }
        constexpr float vertical_spacing = 1.f;
        total_height += vertical_spacing * static_cast<float>(lines.size() - 1);
        const auto left = name_position.x - max_width * 0.5f - 3.f;
        const auto top = name_position.y + 7.f;
        draw_list->AddRectFilled(ImVec2(left, top - 1.f),
            ImVec2(left + max_width + 6.f, top + total_height + 1.f), IM_COL32(0, 0, 0, 170), 2.f);
        auto y = top;
        for (size_t i = 0; i < lines.size(); ++i) {
            const auto& line = lines[i];
            const auto x = name_position.x - text_sizes[i].x * 0.5f;
            if (!line.text.empty()) {
                draw_list->AddText(ImVec2(x, y), line.color, line.text.c_str());
            }
            y += text_sizes[i].y + vertical_spacing;
        }
    }
}

void SlopAuras::TrackAgentCooldown(const uint32_t agent_id, const uint32_t skill_id, const bool from_interrupt)
{
    const auto generation = map_generation.load(std::memory_order_acquire);
    if (!IsMapReady() || !skill_id || skill_id >= GW::SkillbarMgr::GetSkillCount()) {
        return;
    }
    const auto* agent = GW::Agents::GetAgentByID(agent_id);
    const auto* living = agent ? agent->GetAsAgentLiving() : nullptr;
    const auto* skill = GW::SkillbarMgr::GetSkillConstantData(static_cast<GW::Constants::SkillID>(skill_id));
    if (!living || !skill || !skill->recharge
        || skill->recharge > std::numeric_limits<uint32_t>::max() / 1000u) {
        return;
    }
    const auto is_enemy = living->allegiance == GW::Constants::Allegiance::Enemy;
    if (!is_enemy && !IsPartyAgent(agent_id)) {
        // Neutral NPCs, minions, spirits, etc.: out of scope for both cooldown categories.
        return;
    }
    const auto cooldown_target = is_enemy ? CooldownTarget::Enemy : CooldownTarget::Ally;

    const uint32_t now = GW::MemoryMgr::GetSkillTimer();
    const uint32_t base_duration_ms = skill->recharge * 1000u;
    std::unique_lock lock(tracking_mutex);
    if (generation != map_generation.load(std::memory_order_acquire) || !IsMapReady()) {
        return;
    }
    if (!FindCooldownConfig(static_cast<int>(skill_id), cooldown_target)) {
        return;
    }
    std::vector<uint32_t> ready_cooldown_skill_ids;
    std::erase_if(tracked_cooldowns, [this, now, &ready_cooldown_skill_ids](const TrackedCooldown& cooldown) {
        if (now - cooldown.timestamp < cooldown.duration_ms) {
            return false;
        }
        const auto* agent = GW::Agents::GetAgentByID(cooldown.agent_id);
        const auto* living = agent ? agent->GetAsAgentLiving() : nullptr;
        if (notification_enabled[static_cast<size_t>(NotificationType::CooldownReady)]
            && living && living->GetIsAlive()) {
            ready_cooldown_skill_ids.push_back(cooldown.skill_id);
        }
        return true;
    });
    std::erase_if(tracked_cooldowns, [this, generation](const TrackedCooldown& cooldown) {
        if (generation != map_generation.load(std::memory_order_acquire) || !IsMapReady()) {
            return true;
        }
        const auto* agent = GW::Agents::GetAgentByID(cooldown.agent_id);
        const auto* living = agent ? agent->GetAsAgentLiving() : nullptr;
        return !living || !living->GetIsAlive();
    });
    const auto existing = std::ranges::find_if(tracked_cooldowns, [skill_id, agent_id](const TrackedCooldown& cooldown) {
        return cooldown.skill_id == skill_id && cooldown.agent_id == agent_id;
    });

    // Recharge speed-ups cannot be read directly for any agent, but recasting a skill before its predicted
    // ready time proves one is active. The gap between casts is an upper bound for the real recharge ratio
    // (it includes the agent's own delay before recasting), so the estimate errs on the cautious side. It
    // applies to that agent's other skills for a short while, since such effects are temporary.
    if (adaptive_recharge_enabled && existing != tracked_cooldowns.end()) {
        const auto observed_ratio = static_cast<float>(now - existing->timestamp) / static_cast<float>(base_duration_ms);
        if (observed_ratio < 0.98f) {
            RecordAgentRechargeFactor(agent_id, std::max(observed_ratio, min_recharge_factor), now);
        }
    }
    const auto factor = adaptive_recharge_enabled ? GetAgentRechargeFactor(agent_id, now) : 1.f;
    // Recharge cannot go below 1 second.
    const auto duration_ms = std::max<uint32_t>(1000u,
        static_cast<uint32_t>(static_cast<double>(base_duration_ms) * static_cast<double>(factor) + 0.5));

    if (existing != tracked_cooldowns.end()) {
        existing->timestamp = now;
        existing->duration_ms = duration_ms;
        existing->from_interrupt = from_interrupt;
    }
    else {
        tracked_cooldowns.push_back({skill_id, agent_id, now, duration_ms, from_interrupt});
    }
    lock.unlock();
    for (const auto ready_skill_id : ready_cooldown_skill_ids) {
        PlayNotification(NotificationType::CooldownReady, ready_skill_id);
    }
}

void SlopAuras::Draw(IDirect3DDevice9* pDevice)
{
    DrawEnemyNameplates(pDevice);
    const auto generation = map_generation.load(std::memory_order_acquire);
    const auto map_is_current = [this, generation] {
        return generation == map_generation.load(std::memory_order_acquire) && IsMapReady();
    };
    if (!GetVisiblePtr() || !*GetVisiblePtr() || !map_is_current()) {
        return;
    }

    const auto widget_flags = widget_mode
        ? ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse
            | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs
        : ImGuiWindowFlags_None;
    if (widget_mode) {
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.f, 0.f, 0.f, 0.f));
    }
    if (ImGui::Begin(Name(), GetVisiblePtr(), GetWinFlags(widget_flags))) {
        const auto end_window = [this]() {
            ImGui::End();
            if (widget_mode) {
                ImGui::PopStyleColor();
            }
        };
        if (!map_is_current()) {
            end_window();
            return;
        }
        const auto now = GW::MemoryMgr::GetSkillTimer();
        std::vector<TrackedCooldown> tracked_cooldowns_snapshot;
        {
            std::lock_guard lock(tracking_mutex);
            for (const auto& cooldown : tracked_cooldowns) {
                if (FindCooldownConfig(static_cast<int>(cooldown.skill_id))) {
                    tracked_cooldowns_snapshot.push_back(cooldown);
                }
            }
        }

        struct ActiveCooldown {
            uint32_t skill_id;
            uint32_t agent_id;
            uint32_t remaining;
            bool from_interrupt;
        };
        std::vector<ActiveCooldown> active_cooldowns;
        for (const auto& cooldown : tracked_cooldowns_snapshot) {
            const auto elapsed = now - cooldown.timestamp;
            if (elapsed >= cooldown.duration_ms) {
                continue; // expired but not yet erased by Update(); unsigned subtraction would wrap
            }
            active_cooldowns.push_back({cooldown.skill_id, cooldown.agent_id, cooldown.duration_ms - elapsed, cooldown.from_interrupt});
        }
        std::stable_sort(active_cooldowns.begin(), active_cooldowns.end(), [](const ActiveCooldown& lhs, const ActiveCooldown& rhs) {
            return lhs.remaining < rhs.remaining;
        });
        if (!active_cooldowns.empty()) {
            ImGui::TextUnformatted("Enemy skill cooldowns");
        }
        for (const auto& cooldown : active_cooldowns) {
            if (!map_is_current()) {
                break;
            }
            const auto target_name = GetAgentName(cooldown.agent_id);
            const auto icon = GetSkillImage(static_cast<GW::Constants::SkillID>(cooldown.skill_id));
            if (icon && *icon) {
                ImGui::Image((ImTextureID)(intptr_t)*icon, ImVec2(cooldown_icon_size, cooldown_icon_size));
                ImGui::SameLine();
            }
            ImGui::Text("%s%s: %.1fs", cooldown.from_interrupt ? "~" : "", target_name.c_str(),
                static_cast<double>(cooldown.remaining) / 1000.0);
        }

        if (active_cooldowns.empty() && map_is_current()) {
            ImGui::TextUnformatted("No tracked cooldowns active");
        }
    }
    ImGui::End();
    if (widget_mode) {
        ImGui::PopStyleColor();
    }
    if (settings_window_visible && map_is_current()) {
        DrawSettingsWindow();
    }
}

void SlopAuras::DrawSettings()
{
    ToolboxUIPlugin::DrawSettings();
    if (ImGui::Button("Open SlopAuras settings")) {
        settings_window_visible = true;
    }
}

void SlopAuras::DrawSettingsWindow()
{
    if (!ImGui::Begin("SlopAuras Settings", &settings_window_visible)) {
        ImGui::End();
        return;
    }

    bool settings_changed = ImGui::Checkbox("Widget mode", &widget_mode);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Hides the title bar and background; icon size can be adjusted below.");
    }
    settings_changed |= ImGui::SliderFloat("Cooldown icon size", &cooldown_icon_size, 12.f, 64.f, "%.0f px");
    ImGui::Separator();
    ImGui::TextUnformatted("Enemy nameplates");
    settings_changed |= ImGui::Checkbox("Show enemy nameplate overlays", &enemy_nameplates_enabled);
    settings_changed |= ImGui::Checkbox("Show ally nameplate overlays", &ally_nameplates_enabled);
    settings_changed |= ImGui::Checkbox("Show knockdown and timer", &nameplate_show_knockdown);
    settings_changed |= ImGui::Checkbox("Show tracked cooldowns", &nameplate_show_cooldowns);
    settings_changed |= ImGui::Checkbox("Learn recharge speed-ups (heuristic)", &adaptive_recharge_enabled);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("When a tracked agent recasts a skill sooner than predicted, shorten predictions for that agent for a while.\n"
            "Cooldowns started by an interrupt are shown with a ~ because interrupt skills may disable the skill for longer.");
    }
    ImGui::TextWrapped("Effect tracking is being reworked and is currently unavailable. Cooldowns and knockdowns are unaffected.");
    if (settings_changed) {
        SaveSettings(nullptr);
    }

    std::array<bool, static_cast<size_t>(NotificationType::Count)> notification_enabled_snapshot;
    std::array<std::string, static_cast<size_t>(NotificationType::Count)> notification_sound_paths_snapshot;
    {
        std::lock_guard settings_lock(tracking_mutex);
        notification_enabled_snapshot = notification_enabled;
        notification_sound_paths_snapshot = notification_sound_paths;
    }

    bool notification_settings_changed = false;
    ImGui::Separator();
    ImGui::TextUnformatted("Sound notifications");
    ImGui::TextUnformatted("WAV audio plays through Windows independently of Guild Wars volume and mute.");
    const auto draw_notification_option = [&notification_enabled_snapshot, &notification_sound_paths_snapshot, &notification_settings_changed](
        const NotificationType type, const char* label) {
        const auto index = static_cast<size_t>(type);
        ImGui::PushID(label);
        notification_settings_changed |= ImGui::Checkbox(label, &notification_enabled_snapshot[index]);
        ImGui::TextWrapped("WAV: %s", notification_sound_paths_snapshot[index].empty()
            ? "(not selected)"
            : notification_sound_paths_snapshot[index].c_str());
        if (ImGui::Button("Choose WAV...")) {
            std::string selected_path;
            if (BrowseForWaveFile(selected_path)) {
                notification_sound_paths_snapshot[index] = std::move(selected_path);
                notification_settings_changed = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear WAV") && !notification_sound_paths_snapshot[index].empty()) {
            notification_sound_paths_snapshot[index].clear();
            notification_settings_changed = true;
        }
        if (notification_enabled_snapshot[index] && notification_sound_paths_snapshot[index].empty()) {
            ImGui::TextColored(ImVec4(1.f, 0.65f, 0.2f, 1.f), "Choose a WAV file to enable this notification.");
        }
        ImGui::Spacing();
        ImGui::PopID();
    };
    draw_notification_option(NotificationType::CooldownReady, "Play when a tracked enemy cooldown ends");
    ImGui::Text("Sound notifications are currently %s. Use /sa mute or /sa unmute to change this.",
        notifications_muted.load(std::memory_order_acquire) ? "muted" : "unmuted");
    if (notification_settings_changed) {
        {
            std::lock_guard settings_lock(tracking_mutex);
            notification_enabled = notification_enabled_snapshot;
            notification_sound_paths = std::move(notification_sound_paths_snapshot);
        }
        SaveSettings(nullptr);
    }

    ImGui::TextUnformatted("Track effects by skill ID:");
    ImGui::TextWrapped("Entries are saved, but effect tracking is not available yet, so every entry is reported as unavailable.");
    int input_id = 0;
    std::unique_lock lock(tracking_mutex);
    const auto draw_wav_override_button = [](const char* button_label, std::string& override_path, const std::string& group_default_path) {
        const auto has_override = !override_path.empty();
        ImGui::SmallButton(has_override ? "Custom" : "Default");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", has_override ? override_path.c_str()
                : group_default_path.empty() ? "No sound selected (default)" : group_default_path.c_str());
        }
        ImGui::SameLine();
        bool changed = false;
        ImGui::PushID(button_label);
        if (ImGui::SmallButton("Choose...")) {
            std::string selected_path;
            if (BrowseForWaveFile(selected_path)) {
                override_path = std::move(selected_path);
                changed = true;
            }
        }
        if (has_override) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear")) {
                override_path.clear();
                changed = true;
            }
        }
        ImGui::PopID();
        return changed;
    };

    if (ImGui::BeginTable("effect_ids_table", 4,
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Skill", ImGuiTableColumnFlags_WidthStretch, 0.3f);
        ImGui::TableSetupColumn("Skill ID", ImGuiTableColumnFlags_WidthFixed, 90.f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 0.5f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 70.f);
        ImGui::TableHeadersRow();

        for (size_t entry_index = 0; entry_index < effect_ids.size(); ++entry_index) {
            auto& skill_id = effect_ids[entry_index];
            ImGui::TableNextRow();
            ImGui::PushID(input_id++);

            ImGui::TableNextColumn();
            const auto name = IsMapReady() ? GetSkillName(skill_id) : std::string("Unknown skill");
            ImGui::TextUnformatted(name.c_str());

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            settings_changed |= ImGui::InputInt("##effect_id", &skill_id, 0);
            const int previous_id = skill_id;
            skill_id = std::max(0, skill_id);
            settings_changed |= skill_id != previous_id;

            ImGui::TableNextColumn();
            if (IsEffectTrackingAvailable(skill_id)) {
                ImGui::TextUnformatted("Available");
            }
            else {
                ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "Error: tracking not available for this skill.");
            }

            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Remove")) {
                effect_ids.erase(effect_ids.begin() + static_cast<std::ptrdiff_t>(entry_index));
                settings_changed = true;
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (ImGui::Button("Add effect ID")) {
        effect_ids.push_back(0);
        settings_changed = true;
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Track skill cooldowns by skill ID:");
    if (ImGui::BeginTable("cooldown_ids_table", 5,
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Skill", ImGuiTableColumnFlags_WidthStretch, 0.35f);
        ImGui::TableSetupColumn("Skill ID", ImGuiTableColumnFlags_WidthFixed, 90.f);
        ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthFixed, 90.f);
        ImGui::TableSetupColumn("Ready sound", ImGuiTableColumnFlags_WidthStretch, 0.3f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 70.f);
        ImGui::TableHeadersRow();

        for (size_t entry_index = 0; entry_index < cooldown_configs.size(); ++entry_index) {
            auto& config = cooldown_configs[entry_index];
            ImGui::TableNextRow();
            ImGui::PushID(input_id++);

            ImGui::TableNextColumn();
            const auto name = IsMapReady() ? GetSkillName(config.skill_id) : std::string("Unknown skill");
            ImGui::TextUnformatted(name.c_str());

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            settings_changed |= ImGui::InputInt("##cooldown_id", &config.skill_id, 0);
            const int previous_id = config.skill_id;
            config.skill_id = std::max(0, config.skill_id);
            settings_changed |= config.skill_id != previous_id;

            ImGui::TableNextColumn();
            {
                static constexpr std::array<const char*, 2> cooldown_target_labels{"Enemy", "Ally"};
                auto target_index = static_cast<int>(config.target);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::Combo("##cooldown_target", &target_index, cooldown_target_labels.data(),
                    static_cast<int>(cooldown_target_labels.size()))) {
                    config.target = static_cast<CooldownTarget>(target_index);
                    settings_changed = true;
                }
            }

            ImGui::TableNextColumn();
            settings_changed |= draw_wav_override_button("cooldown_wav", config.ready_sound,
                notification_sound_paths[static_cast<size_t>(NotificationType::CooldownReady)]);

            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Remove")) {
                cooldown_configs.erase(cooldown_configs.begin() + static_cast<std::ptrdiff_t>(entry_index));
                settings_changed = true;
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Add cooldown skill ID")) {
        cooldown_configs.emplace_back();
        settings_changed = true;
    }

    if (settings_changed) {
        std::erase_if(tracked_cooldowns, [this](const TrackedCooldown& cooldown) {
            return !FindCooldownConfig(static_cast<int>(cooldown.skill_id));
        });
        lock.unlock();
        SaveSettings(nullptr);
    }
    ImGui::End();
}

std::filesystem::path SlopAuras::GetSettingFile(const wchar_t*) const
{
    if (!plugin_handle) {
        OutputDebugStringW(L"SlopAuras: cannot resolve settings file because the plugin module handle is unavailable.\n");
        return {};
    }

    std::vector<wchar_t> module_path(MAX_PATH);
    for (;;) {
        SetLastError(ERROR_SUCCESS);
        const DWORD path_length = GetModuleFileNameW(plugin_handle, module_path.data(), static_cast<DWORD>(module_path.size()));
        if (!path_length) {
            OutputDebugStringW(L"SlopAuras: failed to resolve the plugin DLL path; settings were not loaded or saved.\n");
            return {};
        }
        if (path_length < module_path.size()) {
            const auto dll_path = std::filesystem::path(module_path.data(), module_path.data() + path_length);
            return dll_path.parent_path() / ToolboxPlugin::GetSettingFile(L"").filename();
        }
        if (module_path.size() >= 32768) {
            OutputDebugStringW(L"SlopAuras: plugin DLL path exceeds the Windows path limit; settings were not loaded or saved.\n");
            return {};
        }
        module_path.resize(std::min<size_t>(module_path.size() * 2, 32768));
    }
}

void SlopAuras::LoadSettings(const wchar_t* folder)
{
    const auto setting_file = GetSettingFile(folder);
    if (setting_file.empty()) {
        return;
    }
    // Absence of the settings file means this is the first time the plugin has ever been loaded.
    pending_welcome_message = !std::filesystem::exists(setting_file);
    ToolboxUIPlugin::LoadSettings(folder);
    {
        std::lock_guard lock(tracking_mutex);
        // The file keeps the original parallel-array layout so existing settings load unchanged.
        std::vector<int> saved_effect_ids;
        std::vector<int> saved_cooldown_ids;
        std::vector<std::string> saved_cooldown_sounds;
        // Per-cooldown-entry target (CooldownTarget). Missing/short entries default to Enemy, matching the
        // only behavior that existed before ally cooldown tracking.
        std::vector<int> saved_cooldown_target_values;
        LoadSetting("effect_ids", saved_effect_ids);
        LoadSetting("cooldown_ids", saved_cooldown_ids);
        LoadSetting("cooldown_sound_overrides", saved_cooldown_sounds);
        LoadSetting("cooldown_target_values", saved_cooldown_target_values);
        effect_ids = std::move(saved_effect_ids);
        for (auto& skill_id : effect_ids) {
            skill_id = std::max(0, skill_id);
        }
        cooldown_configs.clear();
        for (size_t i = 0; i < saved_cooldown_ids.size(); ++i) {
            CooldownConfig config;
            config.skill_id = std::max(0, saved_cooldown_ids[i]);
            config.ready_sound = i < saved_cooldown_sounds.size() ? saved_cooldown_sounds[i] : std::string{};
            config.target = i < saved_cooldown_target_values.size()
                ? static_cast<CooldownTarget>(std::clamp(saved_cooldown_target_values[i], 0, 1))
                : CooldownTarget::Enemy;
            cooldown_configs.push_back(std::move(config));
        }
        LoadSetting("adaptive_recharge_enabled", adaptive_recharge_enabled);
        LoadSetting("widget_mode", widget_mode);
        LoadSetting("cooldown_icon_size", cooldown_icon_size);
        LoadSetting("enemy_nameplates_enabled", enemy_nameplates_enabled);
        LoadSetting("ally_nameplates_enabled", ally_nameplates_enabled);
        LoadSetting("nameplate_show_knockdown", nameplate_show_knockdown);
        LoadSetting("nameplate_show_cooldowns", nameplate_show_cooldowns);
        LoadSetting("notification_enabled", notification_enabled);
        LoadSetting("notification_sound_paths", notification_sound_paths);
    }
    {
        std::lock_guard lock(tracking_mutex);
        cooldown_icon_size = std::clamp(cooldown_icon_size, 12.f, 64.f);
    }
}

void SlopAuras::SaveSettings(const wchar_t* folder)
{
    if (GetSettingFile(folder).empty()) {
        return;
    }
    {
        std::lock_guard lock(tracking_mutex);
        std::vector<int> cooldown_ids;
        std::vector<std::string> cooldown_sound_overrides;
        std::vector<int> cooldown_target_values;
        for (const auto& config : cooldown_configs) {
            cooldown_ids.push_back(config.skill_id);
            cooldown_sound_overrides.push_back(config.ready_sound);
            cooldown_target_values.push_back(static_cast<int>(config.target));
        }
        SaveSetting("effect_ids", effect_ids);
        SaveSetting("cooldown_ids", cooldown_ids);
        SaveSetting("cooldown_sound_overrides", cooldown_sound_overrides);
        SaveSetting("cooldown_target_values", cooldown_target_values);
        SaveSetting("adaptive_recharge_enabled", adaptive_recharge_enabled);
        SaveSetting("widget_mode", widget_mode);
        SaveSetting("cooldown_icon_size", cooldown_icon_size);
        SaveSetting("enemy_nameplates_enabled", enemy_nameplates_enabled);
        SaveSetting("ally_nameplates_enabled", ally_nameplates_enabled);
        SaveSetting("nameplate_show_knockdown", nameplate_show_knockdown);
        SaveSetting("nameplate_show_cooldowns", nameplate_show_cooldowns);
        SaveSetting("notification_enabled", notification_enabled);
        SaveSetting("notification_sound_paths", notification_sound_paths);
    }
    ToolboxUIPlugin::SaveSettings(folder);
}
