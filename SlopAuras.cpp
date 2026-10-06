#include "SlopAuras.h"

#include <PluginUtils.h>

#include <Windows.h>
#include <commdlg.h>
#include <mmsystem.h>

#include <cctype>
#include <iomanip>
#include <sstream>

#include <GWCA/Constants/Constants.h>
#include <GWCA/Context/WorldContext.h>
#include <GWCA/GameEntities/Agent.h>
#include <GWCA/GameEntities/Attribute.h>
#include <GWCA/GameEntities/Skill.h>
#include <GWCA/GameEntities/Title.h>
#include <GWCA/Managers/AgentMgr.h>
#include <GWCA/Managers/EffectMgr.h>
#include <GWCA/Managers/GameThreadMgr.h>
#include <GWCA/Managers/ItemMgr.h>
#include <GWCA/Managers/MapMgr.h>
#include <GWCA/Managers/MemoryMgr.h>
#include <GWCA/Managers/PartyMgr.h>
#include <GWCA/Managers/PlayerMgr.h>
#include <GWCA/Managers/SkillbarMgr.h>
#include <GWCA/Managers/UIMgr.h>

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

    std::string NormalizeAgentName(const std::string& name)
    {
        auto normalized = name;
        std::ranges::transform(normalized, normalized.begin(), [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        const auto first = normalized.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return {};
        }
        const auto last = normalized.find_last_not_of(" \t\r\n");
        return normalized.substr(first, last - first + 1);
    }

    bool IsEnemyAgent(const uint32_t agent_id)
    {
        const auto* agent = GW::Agents::GetAgentByID(agent_id);
        const auto* living = agent ? agent->GetAsAgentLiving() : nullptr;
        return living && living->allegiance == GW::Constants::Allegiance::Enemy;
    }

    bool IsTrackedCastType(const GW::Skill* skill)
    {
        return skill && (skill->type == GW::Constants::SkillType::Hex
            || skill->type == GW::Constants::SkillType::Enchantment
            || skill->type == GW::Constants::SkillType::WeaponSpell
            || skill->type == GW::Constants::SkillType::Shout
            || skill->type == GW::Constants::SkillType::Ritual
            || skill->condition != 0);
    }

    bool IsMultiAllyEffect(const GW::Skill& skill)
    {
        switch (skill.skill_id) {
        case GW::Constants::SkillID::Save_Yourselves_kurzick:
        case GW::Constants::SkillID::Save_Yourselves_luxon:
        case GW::Constants::SkillID::Dark_Fury:
            return true;
        default:
            return (skill.type == GW::Constants::SkillType::Shout && skill.condition == 0)
                || (skill.type == GW::Constants::SkillType::Enchantment && skill.aoe_range > 0.f);
        }
    }

    bool IsEffectTimestampNearCast(const uint32_t effect_timestamp, const uint32_t cast_timestamp)
    {
        constexpr uint32_t match_window_ms = 3000;
        return effect_timestamp - cast_timestamp <= match_window_ms
            || cast_timestamp - effect_timestamp <= match_window_ms;
    }

    uint32_t GetAttributeLevel(const GW::Constants::AttributeByte attribute)
    {
        const auto* attributes = GW::PartyMgr::GetAgentAttributes(GW::Agents::GetControlledCharacterId());
        const auto attribute_id = static_cast<uint32_t>(attribute);
        return attributes && attribute_id < 54 ? attributes[attribute_id].level : 0;
    }

    float GetSkillDuration(const GW::Skill& skill)
    {
        const auto duration0 = static_cast<float>(skill.duration0);
        const auto duration_delta = static_cast<float>(skill.duration15) - duration0;
        if (skill.title != 0) {
            const auto* title = GW::PlayerMgr::GetTitleTrack(static_cast<GW::Constants::TitleID>(skill.title));
            const auto* world = GW::GetWorldContext();
            if (!title || !world || !title->max_title_rank
                || title->current_title_tier_index >= world->title_tiers.size()) {
                return duration0;
            }
            // Vampirism's duration stops scaling at Sunspear rank 5.
            const auto max_effective_rank = skill.skill_id == GW::Constants::SkillID::Vampirism
                ? std::min(title->max_title_rank, 5u)
                : title->max_title_rank;
            const auto rank = std::min(world->title_tiers[title->current_title_tier_index].tier_number, max_effective_rank);
            return duration0 + duration_delta * static_cast<float>(rank) / static_cast<float>(max_effective_rank);
        }

        if (skill.attribute == GW::Constants::AttributeByte::None) {
            return duration0;
        }
        const auto attribute_level = GetAttributeLevel(skill.attribute);
        return attribute_level > 0
            ? duration0 + duration_delta * static_cast<float>(attribute_level) / 15.f
            : duration0;
    }

    float GetEnchantingWeaponBonus()
    {
        const auto* inventory = GW::Items::GetInventory();
        if (!inventory || inventory->active_weapon_set >= std::size(inventory->weapon_sets)) {
            return 0.f;
        }
        const auto* weapon = inventory->weapon_sets[inventory->active_weapon_set].weapon;
        if (!weapon || !weapon->mod_struct) {
            return 0.f;
        }
        for (uint32_t i = 0; i < weapon->mod_struct_size; ++i) {
            if (weapon->mod_struct[i].identifier() == 0x2468) {
                return 0.2f;
            }
        }
        return 0.f;
    }

    bool IsAgentEffectActive(const uint32_t agent_id, const GW::Skill& skill)
    {
        const auto* agent = GW::Agents::GetAgentByID(agent_id);
        const auto* living = agent ? agent->GetAsAgentLiving() : nullptr;
        if (!living || !living->GetIsAlive()) {
            return false;
        }
        if (skill.type == GW::Constants::SkillType::Hex) {
            return living->GetIsHexed();
        }
        if (skill.condition != 0) {
            return living->GetIsConditioned();
        }
        const auto* effects = GW::Effects::GetAgentEffects(agent_id);
        if (effects) {
            return std::ranges::any_of(*effects, [&skill](const GW::Effect& effect) {
                return effect.skill_id == skill.skill_id && effect.GetTimeRemaining() > 0;
            });
        }
        switch (skill.type) {
        case GW::Constants::SkillType::Enchantment:
            return living->GetIsEnchanted();
        case GW::Constants::SkillType::WeaponSpell:
            return living->GetIsWeaponSpelled();
        default:
            return true;
        }
    }

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
        static std::unordered_map<std::wstring, std::unique_ptr<SkillName>> names;
        auto& name = names[encoded];
        if (!name) {
            auto new_name = std::make_unique<SkillName>();
            new_name->encoded = encoded;
            name = std::move(new_name);
        }

        auto* cached_name = name.get();
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
                L"SlopAuras: notification WAV path must be shorter than 256 characters.", L"SlopAuras");
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
        GW::Chat::WriteChat(GW::Chat::CHANNEL_GWCA2, L"/sa hide - Hide the SlopAuras window.", L"SlopAuras");
        GW::Chat::WriteChat(GW::Chat::CHANNEL_GWCA2, L"/sa show - Show the SlopAuras window.", L"SlopAuras");
        GW::Chat::WriteChat(GW::Chat::CHANNEL_GWCA2, L"/sa print - Print tracked effects and enemy cooldowns.", L"SlopAuras");
        GW::Chat::WriteChat(GW::Chat::CHANNEL_GWCA2, L"/sa mute - Mute SlopAuras sound notifications.", L"SlopAuras");
        GW::Chat::WriteChat(GW::Chat::CHANNEL_GWCA2, L"/sa unmute - Unmute SlopAuras sound notifications.", L"SlopAuras");
    };

    if (argc != 2 || !argv || !argv[1]) {
        print_help();
        return;
    }

    const auto subcommand = PluginUtils::ToLower(argv[1]);
    auto* instance = static_cast<SlopAuras*>(ToolboxPluginInstance());
    if (subcommand == L"hide") {
        *instance->GetVisiblePtr() = false;
    }
    else if (subcommand == L"show") {
        *instance->GetVisiblePtr() = true;
    }
    else if (subcommand == L"print") {
        instance->PrintTrackedEffects();
    }
    else if (subcommand == L"mute") {
        instance->SetNotificationsMuted(true);
        GW::Chat::WriteChat(GW::Chat::CHANNEL_GWCA2, L"SlopAuras: Sound notifications muted.", L"SlopAuras");
    }
    else if (subcommand == L"unmute") {
        instance->SetNotificationsMuted(false);
        GW::Chat::WriteChat(GW::Chat::CHANNEL_GWCA2, L"SlopAuras: Sound notifications unmuted.", L"SlopAuras");
    }
    else {
        print_help();
    }
}

void SlopAuras::PlayNotification(const NotificationType type)
{
    if (notifications_muted.load(std::memory_order_acquire)) {
        return;
    }

    std::string path;
    {
        std::lock_guard lock(tracking_mutex);
        const auto index = static_cast<size_t>(type);
        if (index >= notification_enabled.size() || !notification_enabled[index]) {
            return;
        }
        path = notification_sound_paths[index];
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
        if ((notification_playing && notification_playing_type == type)
            || std::ranges::any_of(notification_queue, [type](const QueuedNotification& queued) {
                return queued.type == type;
            })) {
            return;
        }
        notification_queue.push_back({type, std::move(wide_path)});
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

void SlopAuras::PrintTrackedEffects()
{
    if (!IsMapReady()) {
        GW::Chat::WriteChat(GW::Chat::CHANNEL_GWCA2, L"SlopAuras: Map is not ready.", L"SlopAuras");
        return;
    }

    const auto generation = map_generation.load(std::memory_order_acquire);
    const auto now = GW::MemoryMgr::GetSkillTimer();
    std::vector<int> tracked_skill_ids;
    std::vector<int> cast_by_me_snapshot;
    std::vector<int> cooldown_ids_snapshot;
    std::vector<std::string> natural_resistance_names;
    std::vector<TrackedCast> tracked_casts_snapshot;
    std::vector<TrackedCooldown> tracked_cooldowns_snapshot;
    {
        std::lock_guard lock(tracking_mutex);
        tracked_skill_ids = effect_ids;
        cast_by_me_snapshot = cast_by_me_entries;
        cooldown_ids_snapshot = cooldown_ids;
        natural_resistance_names = natural_resistance_agent_names;
        tracked_casts_snapshot = tracked_casts;
        tracked_cooldowns_snapshot = tracked_cooldowns;
    }

    struct ActiveEffect {
        int skill_id;
        uint32_t target_agent_id;
        DWORD remaining;
    };
    std::vector<ActiveEffect> active_effects;
    const auto add_active_effect = [&active_effects](const int skill_id, const uint32_t target_agent_id, const DWORD remaining) {
        const auto existing = std::ranges::find_if(active_effects, [skill_id, target_agent_id](const ActiveEffect& effect) {
            return effect.skill_id == skill_id && effect.target_agent_id == target_agent_id;
        });
        if (existing == active_effects.end()) {
            active_effects.push_back({skill_id, target_agent_id, remaining});
        }
        else {
            existing->remaining = std::max(existing->remaining, remaining);
        }
    };
    const auto effects = GW::Effects::GetPlayerEffects();
    for (size_t entry_index = 0; entry_index < tracked_skill_ids.size(); ++entry_index) {
        if (generation != map_generation.load(std::memory_order_acquire) || !IsMapReady()) {
            return;
        }
        const auto skill_id = tracked_skill_ids[entry_index];
        if (skill_id <= 0) {
            continue;
        }

        const auto cast_by_me = std::ranges::find(cast_by_me_snapshot, static_cast<int>(entry_index)) != cast_by_me_snapshot.end();
        if (effects && !cast_by_me) {
            DWORD remaining = 0;
            for (const auto& effect : *effects) {
                if (static_cast<int>(effect.skill_id) == skill_id) {
                    remaining = std::max(remaining, effect.GetTimeRemaining());
                }
            }
            if (remaining > 0) {
                add_active_effect(skill_id, GW::Agents::GetControlledCharacterId(), remaining);
            }
        }

        for (const auto& cast : tracked_casts_snapshot) {
            if (static_cast<int>(cast.skill_id) != skill_id) {
                continue;
            }
            const auto elapsed = now - cast.timestamp;
            const auto predicted_remaining = elapsed < cast.duration_ms ? cast.duration_ms - elapsed : 0;
            const auto remaining = std::max(predicted_remaining, cast.observed_remaining_ms);
            if (!remaining) {
                continue;
            }
            if (!cast.multi_ally && elapsed >= 1500) {
                const auto* skill = GW::SkillbarMgr::GetSkillConstantData(static_cast<GW::Constants::SkillID>(cast.skill_id));
                if (skill && (skill->type != GW::Constants::SkillType::Ritual || skill->condition != 0)
                    && !IsAgentEffectActive(cast.target_agent_id, *skill)) {
                    continue;
                }
            }
            add_active_effect(skill_id, cast.multi_ally ? 0 : cast.target_agent_id, remaining);
        }
    }

    const auto is_natural_resistance_enemy = [&natural_resistance_names](const uint32_t agent_id) {
        if (!IsEnemyAgent(agent_id)) {
            return false;
        }
        const auto target_name = NormalizeAgentName(GetAgentName(agent_id));
        return !target_name.empty() && target_name != "loading name..." && target_name != "unknown target"
            && std::ranges::any_of(natural_resistance_names, [&target_name](const std::string& name) {
                return NormalizeAgentName(name) == target_name;
            });
    };

    std::vector<std::wstring> messages;
    for (const auto& effect : active_effects) {
        if (is_natural_resistance_enemy(effect.target_agent_id)) {
            continue;
        }
        const auto target_name = effect.target_agent_id == 0
            ? std::string("allies")
            : effect.target_agent_id == GW::Agents::GetControlledCharacterId()
                ? std::string("you")
                : GetAgentName(effect.target_agent_id);
        std::ostringstream message;
        message << "Effect: " << GetSkillName(effect.skill_id) << " on " << target_name << " - "
            << std::fixed << std::setprecision(1) << static_cast<double>(effect.remaining) / 1000.0 << "s remaining";
        messages.push_back(ToWideString(message.str()));
    }

    for (const auto& cooldown : tracked_cooldowns_snapshot) {
        if (std::ranges::find(cooldown_ids_snapshot, static_cast<int>(cooldown.skill_id)) == cooldown_ids_snapshot.end()
            || is_natural_resistance_enemy(cooldown.agent_id)) {
            continue;
        }
        const auto elapsed = now - cooldown.timestamp;
        if (elapsed >= cooldown.duration_ms) {
            continue;
        }
        std::ostringstream message;
        message << "Cooldown: " << GetSkillName(static_cast<int>(cooldown.skill_id)) << " on "
            << GetAgentName(cooldown.agent_id) << " - " << std::fixed << std::setprecision(1)
            << static_cast<double>(cooldown.duration_ms - elapsed) / 1000.0 << "s remaining";
        messages.push_back(ToWideString(message.str()));
    }

    if (generation != map_generation.load(std::memory_order_acquire) || !IsMapReady()) {
        return;
    }
    if (messages.empty()) {
        GW::Chat::WriteChat(GW::Chat::CHANNEL_GWCA2, L"SlopAuras: No tracked effects or cooldowns active.", L"SlopAuras");
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
        tracked_casts.clear();
        tracked_cooldowns.clear();
        pending_casts.clear();
        player_effect_notifications.clear();
        player_effect_snapshot_initialized = false;
    });
    GW::UI::RegisterUIMessageCallback(&skill_started_cast_hook, GW::UI::UIMessage::kAgentSkillStartedCast, [this](GW::HookStatus*, GW::UI::UIMessage, void* wparam, void*) {
        const auto* packet = static_cast<GW::UI::UIPacket::kAgentSkillStartedCast*>(wparam);
        if (!packet || !IsMapReady()
            || packet->agent_id != GW::Agents::GetControlledCharacterId()) {
            return;
        }
        const auto generation = map_generation.load(std::memory_order_acquire);
        const auto skill_id = static_cast<uint32_t>(packet->skill_id);
        if (skill_id >= GW::SkillbarMgr::GetSkillCount()) {
            return;
        }
        const auto* skill = GW::SkillbarMgr::GetSkillConstantData(packet->skill_id);
        if (!IsTrackedCastType(skill)) {
            return;
        }
        const auto now = GW::MemoryMgr::GetSkillTimer();
        std::lock_guard lock(tracking_mutex);
        if (generation != map_generation.load(std::memory_order_acquire) || !IsMapReady()) {
            return;
        }
        const auto is_cast_by_me = std::ranges::any_of(cast_by_me_entries, [this, skill_id](const int entry) {
            return entry >= 0 && static_cast<size_t>(entry) < effect_ids.size()
                && effect_ids[entry] == static_cast<int>(skill_id);
        });
        if (!is_cast_by_me || std::ranges::find(effect_ids, static_cast<int>(skill_id)) == effect_ids.end()) {
            return;
        }
        std::erase_if(pending_casts, [now](const PendingCast& cast) {
            return now - cast.timestamp > 10000;
        });
        pending_casts.push_back({skill_id, GW::Agents::GetTargetId(), now});
    });
    GW::UI::RegisterUIMessageCallback(&skill_activated_hook, GW::UI::UIMessage::kAgentSkillActivated, [this](GW::HookStatus*, GW::UI::UIMessage, void* wparam, void*) {
        const auto* packet = static_cast<GW::UI::UIPacket::kAgentSkillPacket*>(wparam);
        if (packet && IsMapReady()) {
            const auto generation = map_generation.load(std::memory_order_acquire);
            TrackEnemyCooldown(packet->agent_id, static_cast<uint32_t>(packet->skill_id));
            if (generation != map_generation.load(std::memory_order_acquire) || !IsMapReady()) {
                return;
            }
            if (packet->agent_id != GW::Agents::GetControlledCharacterId()) {
                return;
            }
            const auto skill_id = static_cast<uint32_t>(packet->skill_id);
            const auto now = GW::MemoryMgr::GetSkillTimer();
            uint32_t target_agent_id = 0;
            {
                std::lock_guard lock(tracking_mutex);
                const auto pending = std::ranges::find_if(pending_casts.rbegin(), pending_casts.rend(), [skill_id](const PendingCast& cast) {
                    return cast.skill_id == skill_id;
                });
                if (pending != pending_casts.rend()) {
                    if (generation == map_generation.load(std::memory_order_acquire)
                        && now - pending->timestamp <= 10000) {
                        target_agent_id = pending->target_agent_id;
                    }
                    pending_casts.erase(std::next(pending).base());
                }
            }
            TrackCast(static_cast<int>(packet->skill_id), target_agent_id);
        }
    });
    GW::UI::RegisterUIMessageCallback(&skill_activated_hook, GW::UI::UIMessage::kAgentSkillActivatedInstantly, [this](GW::HookStatus*, GW::UI::UIMessage, void* wparam, void*) {
        const auto* packet = static_cast<GW::UI::UIPacket::kAgentSkillPacket*>(wparam);
        if (packet && IsMapReady()) {
            const auto generation = map_generation.load(std::memory_order_acquire);
            TrackEnemyCooldown(packet->agent_id, static_cast<uint32_t>(packet->skill_id));
            if (generation != map_generation.load(std::memory_order_acquire) || !IsMapReady()) {
                return;
            }
            if (packet->agent_id != GW::Agents::GetControlledCharacterId()) {
                return;
            }
            TrackCast(static_cast<int>(packet->skill_id), GW::Agents::GetTargetId());
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
    {
        std::lock_guard lock(tracking_mutex);
        tracked_casts.clear();
        tracked_cooldowns.clear();
        pending_casts.clear();
        player_effect_notifications.clear();
        player_effect_snapshot_initialized = false;
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
    const auto now = GW::MemoryMgr::GetSkillTimer();
    std::array<bool, static_cast<size_t>(NotificationType::Count)> pending_notifications{};
    const auto player_effects = GW::Effects::GetPlayerEffects();
    bool has_multi_ally_cast = false;
    {
        std::lock_guard lock(tracking_mutex);
        has_multi_ally_cast = std::ranges::any_of(tracked_casts, [](const TrackedCast& cast) {
            return cast.multi_ally;
        });
    }
    struct PartyEffectSnapshot {
        uint32_t skill_id;
        uint32_t timestamp;
        uint32_t remaining_ms;
    };
    std::vector<PartyEffectSnapshot> party_effect_snapshot;
    if (has_multi_ally_cast) {
        const auto* party_effects = GW::Effects::GetPartyEffectsArray();
        if (party_effects && party_effects->valid()) {
            for (const auto& agent_effects : *party_effects) {
                if (!agent_effects.effects.valid()) {
                    continue;
                }
                for (const auto& effect : agent_effects.effects) {
                    const auto remaining = effect.GetTimeRemaining();
                    if (remaining) {
                        party_effect_snapshot.push_back({
                            static_cast<uint32_t>(effect.skill_id),
                            effect.timestamp,
                            remaining
                        });
                    }
                }
            }
        }
    }
    {
        std::lock_guard lock(tracking_mutex);
        const auto tracked_effects_changed = player_effect_ids_snapshot != effect_ids
            || player_cast_by_me_snapshot != cast_by_me_entries;
        player_effect_ids_snapshot = effect_ids;
        player_cast_by_me_snapshot = cast_by_me_entries;

        if (player_effects) {
            std::vector<PlayerEffectNotification> current_player_effects;
            for (const auto& effect : *player_effects) {
                const auto skill_id = static_cast<uint32_t>(effect.skill_id);
                if (!effect.duration || !effect.GetTimeRemaining()
                    || std::ranges::find(effect_ids, static_cast<int>(skill_id)) == effect_ids.end()
                    || std::ranges::any_of(cast_by_me_entries, [this, skill_id](const int entry) {
                        return entry >= 0 && static_cast<size_t>(entry) < effect_ids.size()
                            && effect_ids[entry] == static_cast<int>(skill_id);
                    })) {
                    continue;
                }
                if (std::ranges::find_if(current_player_effects, [skill_id, &effect](const PlayerEffectNotification& tracked) {
                    return tracked.skill_id == skill_id && tracked.timestamp == effect.timestamp;
                }) != current_player_effects.end()) {
                    continue;
                }
                const auto previous = std::ranges::find_if(player_effect_notifications, [skill_id, &effect](const PlayerEffectNotification& tracked) {
                    return tracked.skill_id == skill_id && tracked.timestamp == effect.timestamp;
                });
                const auto is_new = previous == player_effect_notifications.end();
                current_player_effects.push_back({
                    skill_id,
                    effect.timestamp,
                    !is_new && previous->expiration_notified
                });
                if (is_new && player_effect_snapshot_initialized && !tracked_effects_changed
                    && notification_enabled[static_cast<size_t>(NotificationType::EffectApplied)]) {
                    pending_notifications[static_cast<size_t>(NotificationType::EffectApplied)] = true;
                }
                auto& tracked = current_player_effects.back();
                const auto remaining = effect.GetTimeRemaining();
                if (!tracked.expiration_notified
                    && remaining <= static_cast<DWORD>(notification_lead_seconds * 1000.f)
                    && notification_enabled[static_cast<size_t>(NotificationType::EffectExpiring)]) {
                    tracked.expiration_notified = true;
                    pending_notifications[static_cast<size_t>(NotificationType::EffectExpiring)] = true;
                }
            }
            player_effect_notifications = std::move(current_player_effects);
            player_effect_snapshot_initialized = true;
        }

        for (auto cast = tracked_casts.begin(); cast != tracked_casts.end();) {
            const auto elapsed = now - cast->timestamp;
            const auto since_observed = now - cast->observed_updated_timestamp;
            cast->observed_remaining_ms = since_observed < cast->observed_remaining_ms
                ? cast->observed_remaining_ms - since_observed
                : 0;
            cast->observed_updated_timestamp = now;
            if (cast->multi_ally) {
                for (const auto& effect : party_effect_snapshot) {
                    if (effect.skill_id == cast->skill_id
                        && IsEffectTimestampNearCast(effect.timestamp, cast->timestamp)) {
                        cast->observed_remaining_ms = std::max(
                            cast->observed_remaining_ms,
                            effect.remaining_ms);
                    }
                }
            }
            if (elapsed >= cast->duration_ms && !cast->observed_remaining_ms) {
                cast = tracked_casts.erase(cast);
                continue;
            }
            if (!cast->multi_ally && elapsed >= 1500) {
                if (generation != map_generation.load(std::memory_order_acquire) || !IsMapReady()) {
                    cast = tracked_casts.erase(cast);
                    continue;
                }
                const auto* skill = GW::SkillbarMgr::GetSkillConstantData(static_cast<GW::Constants::SkillID>(cast->skill_id));
                if (skill && (skill->type != GW::Constants::SkillType::Ritual || skill->condition != 0)
                    && !IsAgentEffectActive(cast->target_agent_id, *skill)) {
                    cast = tracked_casts.erase(cast);
                    continue;
                }
            }
            const auto predicted_remaining = elapsed < cast->duration_ms ? cast->duration_ms - elapsed : 0;
            const auto remaining = std::max(predicted_remaining, cast->observed_remaining_ms);
            if (!cast->expiration_notified
                && remaining <= static_cast<uint32_t>(notification_lead_seconds * 1000.f)
                && notification_enabled[static_cast<size_t>(NotificationType::EffectExpiring)]) {
                cast->expiration_notified = true;
                pending_notifications[static_cast<size_t>(NotificationType::EffectExpiring)] = true;
            }
            ++cast;
        }
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
                pending_notifications[static_cast<size_t>(NotificationType::CooldownReady)] = true;
            }
            return true;
        });
    }
    for (size_t index = 0; index < pending_notifications.size(); ++index) {
        if (pending_notifications[index]) {
            PlayNotification(static_cast<NotificationType>(index));
        }
    }
}

void SlopAuras::TrackCast(const int skill_id, const uint32_t target_agent_id)
{
    const auto generation = map_generation.load(std::memory_order_acquire);
    if (!IsMapReady()
        || skill_id <= 0 || static_cast<uint32_t>(skill_id) >= GW::SkillbarMgr::GetSkillCount()) {
        return;
    }

    const auto* skill = GW::SkillbarMgr::GetSkillConstantData(static_cast<GW::Constants::SkillID>(skill_id));
    if (!IsTrackedCastType(skill) || skill->duration0 == 0
        || skill->duration0 >= 0x20000 || skill->duration15 >= 0x20000) {
        return;
    }

    auto duration = GetSkillDuration(*skill);
    if (skill->type == GW::Constants::SkillType::WeaponSpell) {
        duration *= 1.f + GetAttributeLevel(GW::Constants::AttributeByte::SpawningPower) * 0.04f;
    }
    if (skill->type == GW::Constants::SkillType::Enchantment) {
        duration *= 1.f + GetEnchantingWeaponBonus();
    }
    if (duration <= 0.f || duration >= 0x20000) {
        return;
    }

    auto duration_ms = static_cast<uint32_t>(duration * 1000.f);
    if (duration_ms == 0) {
        return;
    }

    const auto multi_ally = IsMultiAllyEffect(*skill);
    const auto target_id = skill->type == GW::Constants::SkillType::Ritual
        ? GW::Agents::GetControlledCharacterId()
        : target_agent_id ? target_agent_id : GW::Agents::GetControlledCharacterId();
    auto resolved_target_id = target_id;
    if (multi_ally) {
        resolved_target_id = 0;
    }
    else if ((skill->type == GW::Constants::SkillType::WeaponSpell
        || skill->type == GW::Constants::SkillType::Enchantment) && IsEnemyAgent(target_id)) {
        resolved_target_id = GW::Agents::GetControlledCharacterId();
    }
    std::unique_lock lock(tracking_mutex);
    if (generation != map_generation.load(std::memory_order_acquire) || !IsMapReady()) {
        return;
    }
    if (std::ranges::find(effect_ids, skill_id) == effect_ids.end()) {
        return;
    }
    if (!std::ranges::any_of(cast_by_me_entries, [this, skill_id](const int entry) {
        return entry >= 0 && static_cast<size_t>(entry) < effect_ids.size() && effect_ids[entry] == skill_id;
    })) {
        return;
    }
    const auto target_name = resolved_target_id ? NormalizeAgentName(GetAgentName(resolved_target_id)) : std::string{};
    if (!multi_ally && skill->type == GW::Constants::SkillType::Hex
        && !target_name.empty() && target_name != "loading name..." && target_name != "unknown target"
        && std::ranges::any_of(natural_resistance_agent_names, [&target_name](const std::string& name) {
            return NormalizeAgentName(name) == target_name;
        })) {
        duration_ms = static_cast<uint32_t>(std::floor(duration / 2.f + 0.5f) * 1000.f);
    }
    if (duration_ms == 0) {
        return;
    }
    const auto now = GW::MemoryMgr::GetSkillTimer();
    std::erase_if(tracked_casts, [now](const TrackedCast& cast) {
        return now - cast.timestamp >= cast.duration_ms && !cast.observed_remaining_ms;
    });
    if (skill->type == GW::Constants::SkillType::WeaponSpell) {
        std::erase_if(tracked_casts, [resolved_target_id](const TrackedCast& cast) {
            const auto* tracked_skill = GW::SkillbarMgr::GetSkillConstantData(static_cast<GW::Constants::SkillID>(cast.skill_id));
            return cast.target_agent_id == resolved_target_id && tracked_skill
                && tracked_skill->type == GW::Constants::SkillType::WeaponSpell;
        });
    }
    const auto existing = std::ranges::find_if(tracked_casts, [skill_id, resolved_target_id, multi_ally](const TrackedCast& cast) {
        return cast.skill_id == static_cast<uint32_t>(skill_id)
            && cast.target_agent_id == resolved_target_id
            && cast.multi_ally == multi_ally;
    });
    if (existing != tracked_casts.end()) {
        existing->timestamp = now;
        existing->duration_ms = duration_ms;
        existing->observed_remaining_ms = 0;
        existing->observed_updated_timestamp = now;
        existing->expiration_notified = false;
    }
    else {
        tracked_casts.push_back({
            static_cast<uint32_t>(skill_id),
            resolved_target_id,
            now,
            duration_ms,
            false,
            multi_ally,
            0,
            now
        });
    }
    const auto notify_applied = notification_enabled[static_cast<size_t>(NotificationType::EffectApplied)];
    lock.unlock();
    if (notify_applied) {
        PlayNotification(NotificationType::EffectApplied);
    }
}

void SlopAuras::TrackEnemyCooldown(const uint32_t agent_id, const uint32_t skill_id)
{
    const auto generation = map_generation.load(std::memory_order_acquire);
    if (!IsMapReady() || !skill_id || skill_id >= GW::SkillbarMgr::GetSkillCount()) {
        return;
    }
    const auto* agent = GW::Agents::GetAgentByID(agent_id);
    const auto* living = agent ? agent->GetAsAgentLiving() : nullptr;
    const auto* skill = GW::SkillbarMgr::GetSkillConstantData(static_cast<GW::Constants::SkillID>(skill_id));
    if (!living || living->allegiance != GW::Constants::Allegiance::Enemy || !skill || !skill->recharge
        || skill->recharge > std::numeric_limits<uint32_t>::max() / 1000u) {
        return;
    }

    const auto now = GW::MemoryMgr::GetSkillTimer();
    const auto duration_ms = skill->recharge * 1000u;
    std::unique_lock lock(tracking_mutex);
    if (generation != map_generation.load(std::memory_order_acquire) || !IsMapReady()) {
        return;
    }
    if (std::ranges::find(cooldown_ids, static_cast<int>(skill_id)) == cooldown_ids.end()) {
        return;
    }
    bool notify_cooldown_ready = false;
    std::erase_if(tracked_cooldowns, [this, now, &notify_cooldown_ready](const TrackedCooldown& cooldown) {
        if (now - cooldown.timestamp < cooldown.duration_ms) {
            return false;
        }
        const auto* agent = GW::Agents::GetAgentByID(cooldown.agent_id);
        const auto* living = agent ? agent->GetAsAgentLiving() : nullptr;
        notify_cooldown_ready |= notification_enabled[static_cast<size_t>(NotificationType::CooldownReady)]
            && living && living->GetIsAlive() && living->allegiance == GW::Constants::Allegiance::Enemy;
        return true;
    });
    std::erase_if(tracked_cooldowns, [this, generation](const TrackedCooldown& cooldown) {
        if (generation != map_generation.load(std::memory_order_acquire) || !IsMapReady()) {
            return true;
        }
        const auto* agent = GW::Agents::GetAgentByID(cooldown.agent_id);
        const auto* living = agent ? agent->GetAsAgentLiving() : nullptr;
        return !living || !living->GetIsAlive() || living->allegiance != GW::Constants::Allegiance::Enemy;
    });
    const auto existing = std::ranges::find_if(tracked_cooldowns, [skill_id, agent_id](const TrackedCooldown& cooldown) {
        return cooldown.skill_id == skill_id && cooldown.agent_id == agent_id;
    });
    if (existing != tracked_cooldowns.end()) {
        existing->timestamp = now;
        existing->duration_ms = duration_ms;
    }
    else {
        tracked_cooldowns.push_back({skill_id, agent_id, now, duration_ms});
    }
    lock.unlock();
    if (notify_cooldown_ready) {
        PlayNotification(NotificationType::CooldownReady);
    }
}

void SlopAuras::Draw(IDirect3DDevice9*)
{
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
        if (!map_is_current()) {
            ImGui::End();
            if (widget_mode) {
                ImGui::PopStyleColor();
            }
            return;
        }
        const auto effects = GW::Effects::GetPlayerEffects();
        const auto now = GW::MemoryMgr::GetSkillTimer();
        std::vector<int> tracked_skill_ids;
        std::vector<int> cast_by_me_snapshot;
        std::vector<int> cooldown_ids_snapshot;
        std::vector<TrackedCast> tracked_casts_snapshot;
        std::vector<TrackedCooldown> tracked_cooldowns_snapshot;
        {
            std::lock_guard lock(tracking_mutex);
            tracked_skill_ids = effect_ids;
            cast_by_me_snapshot = cast_by_me_entries;
            cooldown_ids_snapshot = cooldown_ids;
            tracked_casts_snapshot = tracked_casts;
            tracked_cooldowns_snapshot = tracked_cooldowns;
        }

        if (!map_is_current()) {
            ImGui::End();
            if (widget_mode) {
                ImGui::PopStyleColor();
            }
            return;
        }

        struct ActiveEffect {
            int skill_id;
            uint32_t target_agent_id;
            DWORD remaining;
        };
        std::vector<ActiveEffect> active_effects;
        const auto add_active_effect = [&active_effects](const int skill_id, const uint32_t target_agent_id, const DWORD remaining) {
            const auto existing = std::ranges::find_if(active_effects, [skill_id, target_agent_id](const ActiveEffect& effect) {
                return effect.skill_id == skill_id && effect.target_agent_id == target_agent_id;
            });
            if (existing == active_effects.end()) {
                active_effects.push_back({skill_id, target_agent_id, remaining});
            }
            else {
                existing->remaining = std::max(existing->remaining, remaining);
            }
        };
        bool has_active_effect = false;

        for (size_t entry_index = 0; entry_index < tracked_skill_ids.size() && map_is_current(); ++entry_index) {
            const auto skill_id = tracked_skill_ids[entry_index];
            if (skill_id <= 0) {
                continue;
            }

            const auto cast_by_me = std::ranges::find(cast_by_me_snapshot, static_cast<int>(entry_index)) != cast_by_me_snapshot.end();
            if (effects && !cast_by_me) {
                DWORD remaining = 0;
                for (const auto& effect : *effects) {
                    if (!map_is_current()) {
                        break;
                    }
                    if (static_cast<int>(effect.skill_id) == skill_id) {
                        remaining = std::max(remaining, effect.GetTimeRemaining());
                    }
                }
                if (remaining > 0 && map_is_current()) {
                    add_active_effect(skill_id, GW::Agents::GetControlledCharacterId(), remaining);
                }
            }

            for (const auto& cast : tracked_casts_snapshot) {
                if (!map_is_current()) {
                    break;
                }
                if (static_cast<int>(cast.skill_id) == skill_id) {
                    const auto elapsed = now - cast.timestamp;
                    const auto predicted_remaining = elapsed < cast.duration_ms
                        ? cast.duration_ms - elapsed
                        : 0;
                    const auto remaining = std::max(predicted_remaining, cast.observed_remaining_ms);
                    if (remaining > 0) {
                        add_active_effect(skill_id, cast.multi_ally ? 0 : cast.target_agent_id, remaining);
                    }
                }
            }
        }

        std::stable_sort(active_effects.begin(), active_effects.end(), [](const ActiveEffect& lhs, const ActiveEffect& rhs) {
            return lhs.remaining < rhs.remaining;
        });
        for (const auto& effect : active_effects) {
            if (!map_is_current()) {
                break;
            }
            const auto target_name = effect.target_agent_id == 0
                ? std::string("allies")
                : effect.target_agent_id == GW::Agents::GetControlledCharacterId()
                    ? std::string("you")
                    : GetAgentName(effect.target_agent_id);
            const auto icon = GetSkillImage(static_cast<GW::Constants::SkillID>(effect.skill_id));
            if (icon && *icon) {
                ImGui::Image((ImTextureID)(intptr_t)*icon, ImVec2(effect_icon_size, effect_icon_size));
                ImGui::SameLine();
            }
            ImGui::Text("on %s: %.1fs", target_name.c_str(), static_cast<double>(effect.remaining) / 1000.0);
            has_active_effect = true;
        }

        struct ActiveCooldown {
            uint32_t skill_id;
            uint32_t agent_id;
            DWORD remaining;
        };
        std::vector<ActiveCooldown> active_cooldowns;
        for (const auto& cooldown : tracked_cooldowns_snapshot) {
            if (std::ranges::find(cooldown_ids_snapshot, static_cast<int>(cooldown.skill_id)) == cooldown_ids_snapshot.end()) {
                continue;
            }
            const auto remaining = cooldown.duration_ms - (now - cooldown.timestamp);
            if (remaining) {
                active_cooldowns.push_back({cooldown.skill_id, cooldown.agent_id, remaining});
            }
        }
        std::stable_sort(active_cooldowns.begin(), active_cooldowns.end(), [](const ActiveCooldown& lhs, const ActiveCooldown& rhs) {
            return lhs.remaining < rhs.remaining;
        });
        if (!active_cooldowns.empty()) {
            ImGui::Separator();
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
            ImGui::Text("%s: %.1fs", target_name.c_str(), static_cast<double>(cooldown.remaining) / 1000.0);
            has_active_effect = true;
        }

        if (!has_active_effect && map_is_current()) {
            ImGui::TextUnformatted("No tracked effects active");
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
        ImGui::SetTooltip("Hides the title bar and background; icon sizes can be adjusted below.");
    }
    settings_changed |= ImGui::SliderFloat("Effect icon size", &effect_icon_size, 12.f, 64.f, "%.0f px");
    settings_changed |= ImGui::SliderFloat("Cooldown icon size", &cooldown_icon_size, 12.f, 64.f, "%.0f px");
    if (settings_changed) {
        SaveSettings(nullptr);
    }

    std::array<bool, static_cast<size_t>(NotificationType::Count)> notification_enabled_snapshot;
    std::array<std::string, static_cast<size_t>(NotificationType::Count)> notification_sound_paths_snapshot;
    float notification_lead_seconds_snapshot = 3.f;
    {
        std::lock_guard settings_lock(tracking_mutex);
        notification_enabled_snapshot = notification_enabled;
        notification_sound_paths_snapshot = notification_sound_paths;
        notification_lead_seconds_snapshot = notification_lead_seconds;
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
    draw_notification_option(NotificationType::EffectApplied, "Play when a tracked effect is applied");
    draw_notification_option(NotificationType::EffectExpiring, "Play when a tracked effect is about to expire");
    if (notification_enabled_snapshot[static_cast<size_t>(NotificationType::EffectExpiring)]) {
        notification_settings_changed |= ImGui::SliderFloat("Expiration lead time", &notification_lead_seconds_snapshot, 0.5f, 30.f, "%.1f s");
    }
    draw_notification_option(NotificationType::CooldownReady, "Play when a tracked enemy cooldown ends");
    ImGui::Text("Sound notifications are currently %s. Use /sa mute or /sa unmute to change this.",
        notifications_muted.load(std::memory_order_acquire) ? "muted" : "unmuted");
    if (notification_settings_changed) {
        notification_lead_seconds_snapshot = std::clamp(notification_lead_seconds_snapshot, 0.5f, 30.f);
        {
            std::lock_guard settings_lock(tracking_mutex);
            notification_enabled = notification_enabled_snapshot;
            notification_sound_paths = std::move(notification_sound_paths_snapshot);
            notification_lead_seconds = notification_lead_seconds_snapshot;
        }
        SaveSettings(nullptr);
    }

    ImGui::TextUnformatted("Track player effects by skill ID:");
    int input_id = 0;
    std::unique_lock lock(tracking_mutex);
    for (size_t entry_index = 0; entry_index < effect_ids.size(); ++entry_index) {
        auto it = effect_ids.begin() + entry_index;
        ImGui::PushID(input_id++);
        const auto name = IsMapReady() ? GetSkillName(*it) : std::string("Unknown skill");
        ImGui::TextUnformatted(name.c_str());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100.f);
        settings_changed |= ImGui::InputInt("Effect skill ID", &*it, 0);
        const int previous_id = *it;
        *it = std::max(0, *it);
        settings_changed |= *it != previous_id;
        const auto* skill = IsMapReady()
            && *it > 0 && static_cast<uint32_t>(*it) < GW::SkillbarMgr::GetSkillCount()
            ? GW::SkillbarMgr::GetSkillConstantData(static_cast<GW::Constants::SkillID>(*it))
            : nullptr;
        if (IsTrackedCastType(skill)) {
            bool cast_by_me = std::ranges::find(cast_by_me_entries, static_cast<int>(entry_index)) != cast_by_me_entries.end();
            ImGui::SameLine();
            if (ImGui::Checkbox("Cast by me", &cast_by_me)) {
                if (cast_by_me) {
                    if (std::ranges::find(cast_by_me_entries, static_cast<int>(entry_index)) == cast_by_me_entries.end()) {
                        cast_by_me_entries.push_back(static_cast<int>(entry_index));
                    }
                }
                else {
                    std::erase(cast_by_me_entries, static_cast<int>(entry_index));
                }
                settings_changed = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Remove")) {
            effect_ids.erase(it);
            std::erase_if(cast_by_me_entries, [entry_index](const int index) {
                return index == static_cast<int>(entry_index);
            });
            for (auto& index : cast_by_me_entries) {
                if (index > static_cast<int>(entry_index)) {
                    --index;
                }
            }
            settings_changed = true;
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }

    if (ImGui::Button("Add effect ID")) {
        effect_ids.emplace_back(0);
        settings_changed = true;
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Track enemy skill cooldowns by skill ID:");
    for (auto it = cooldown_ids.begin(); it != cooldown_ids.end(); ++it) {
        ImGui::PushID(input_id++);
        const auto name = IsMapReady() ? GetSkillName(*it) : std::string("Unknown skill");
        ImGui::TextUnformatted(name.c_str());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100.f);
        settings_changed |= ImGui::InputInt("Cooldown skill ID", &*it, 0);
        const int previous_id = *it;
        *it = std::max(0, *it);
        settings_changed |= *it != previous_id;
        ImGui::SameLine();
        if (ImGui::Button("Remove cooldown")) {
            cooldown_ids.erase(it);
            settings_changed = true;
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (ImGui::Button("Add cooldown skill ID")) {
        cooldown_ids.emplace_back(0);
        settings_changed = true;
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Natural Resistance agents (names):");
    ImGui::TextUnformatted("Hex duration is halved for casts targeting agents with these names, rounded half up to a whole second.");
    for (auto it = natural_resistance_agent_names.begin(); it != natural_resistance_agent_names.end(); ++it) {
        ImGui::PushID(input_id++);
        std::array<char, 128> name_buffer{};
        const auto name_length = std::min(it->size(), name_buffer.size() - 1);
        std::copy_n(it->data(), name_length, name_buffer.data());
        ImGui::SetNextItemWidth(240.f);
        if (ImGui::InputText("Agent name", name_buffer.data(), name_buffer.size())) {
            *it = name_buffer.data();
            settings_changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Remove agent name")) {
            natural_resistance_agent_names.erase(it);
            settings_changed = true;
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (ImGui::Button("Add agent name")) {
        natural_resistance_agent_names.emplace_back();
        settings_changed = true;
    }
    if (settings_changed) {
        std::erase_if(cast_by_me_entries, [this](const int index) {
            return index < 0 || static_cast<size_t>(index) >= effect_ids.size();
        });
        std::erase_if(tracked_casts, [this](const TrackedCast& cast) {
            return std::ranges::none_of(cast_by_me_entries, [this, &cast](const int index) {
                return index >= 0 && static_cast<size_t>(index) < effect_ids.size()
                    && effect_ids[index] == static_cast<int>(cast.skill_id);
            });
        });
        std::erase_if(tracked_cooldowns, [this](const TrackedCooldown& cooldown) {
            return std::ranges::find(cooldown_ids, static_cast<int>(cooldown.skill_id)) == cooldown_ids.end();
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
    if (GetSettingFile(folder).empty()) {
        return;
    }
    ToolboxUIPlugin::LoadSettings(folder);
    {
        std::lock_guard lock(tracking_mutex);
        LoadSetting("effect_ids", effect_ids);
        LoadSetting("cast_by_me_entries", cast_by_me_entries);
        LoadSetting("cooldown_ids", cooldown_ids);
        LoadSetting("natural_resistance_agent_names", natural_resistance_agent_names);
        LoadSetting("widget_mode", widget_mode);
        LoadSetting("effect_icon_size", effect_icon_size);
        LoadSetting("cooldown_icon_size", cooldown_icon_size);
        LoadSetting("notification_enabled", notification_enabled);
        LoadSetting("notification_sound_paths", notification_sound_paths);
        LoadSetting("notification_lead_seconds", notification_lead_seconds);
    }
    {
        std::lock_guard lock(tracking_mutex);
        for (auto& skill_id : effect_ids) {
            skill_id = std::max(0, skill_id);
        }
        std::erase_if(cast_by_me_entries, [this](const int index) {
            return index < 0 || static_cast<size_t>(index) >= effect_ids.size();
        });
        for (auto& skill_id : cooldown_ids) {
            skill_id = std::max(0, skill_id);
        }
        effect_icon_size = std::clamp(effect_icon_size, 12.f, 64.f);
        cooldown_icon_size = std::clamp(cooldown_icon_size, 12.f, 64.f);
        notification_lead_seconds = std::clamp(notification_lead_seconds, 0.5f, 30.f);
        player_effect_ids_snapshot = effect_ids;
        player_cast_by_me_snapshot = cast_by_me_entries;
        player_effect_snapshot_initialized = false;
    }
}

void SlopAuras::SaveSettings(const wchar_t* folder)
{
    if (GetSettingFile(folder).empty()) {
        return;
    }
    {
        std::lock_guard lock(tracking_mutex);
        SaveSetting("effect_ids", effect_ids);
        SaveSetting("cast_by_me_entries", cast_by_me_entries);
        SaveSetting("cooldown_ids", cooldown_ids);
        SaveSetting("natural_resistance_agent_names", natural_resistance_agent_names);
        SaveSetting("widget_mode", widget_mode);
        SaveSetting("effect_icon_size", effect_icon_size);
        SaveSetting("cooldown_icon_size", cooldown_icon_size);
        SaveSetting("notification_enabled", notification_enabled);
        SaveSetting("notification_sound_paths", notification_sound_paths);
        SaveSetting("notification_lead_seconds", notification_lead_seconds);
    }
    ToolboxUIPlugin::SaveSettings(folder);
}
