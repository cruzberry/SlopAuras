#include "SlopAuras.h"

#include <PluginUtils.h>

#include <Windows.h>

#include <GWCA/Constants/Constants.h>
#include <GWCA/GameEntities/Agent.h>
#include <GWCA/GameEntities/Attribute.h>
#include <GWCA/GameEntities/Skill.h>
#include <GWCA/Managers/AgentMgr.h>
#include <GWCA/Managers/EffectMgr.h>
#include <GWCA/Managers/GameThreadMgr.h>
#include <GWCA/Managers/ItemMgr.h>
#include <GWCA/Managers/MapMgr.h>
#include <GWCA/Managers/MemoryMgr.h>
#include <GWCA/Managers/PartyMgr.h>
#include <GWCA/Managers/SkillbarMgr.h>
#include <GWCA/Managers/UIMgr.h>

extern "C" __declspec(dllimport) IDirect3DTexture9** __cdecl GetSkillImage(GW::Constants::SkillID skill_id);

namespace {
    bool IsGameWorldReady()
    {
        return GW::Map::GetInstanceType() != GW::Constants::InstanceType::Loading
            && GW::Map::GetIsMapLoaded()
            && GW::Agents::GetControlledCharacter();
    }

    uint32_t GetAgentModelId(const uint32_t agent_id)
    {
        const auto* agent = GW::Agents::GetAgentByID(agent_id);
        const auto* living = agent ? agent->GetAsAgentLiving() : nullptr;
        return living ? living->player_number : 0;
    }

    bool IsEnemyAgent(const uint32_t agent_id)
    {
        const auto* agent = GW::Agents::GetAgentByID(agent_id);
        const auto* living = agent ? agent->GetAsAgentLiving() : nullptr;
        return living && living->allegiance == GW::Constants::Allegiance::Enemy;
    }

    bool IsTrackedCastType(const GW::Constants::SkillType type)
    {
        return type == GW::Constants::SkillType::Hex
            || type == GW::Constants::SkillType::Enchantment
            || type == GW::Constants::SkillType::WeaponSpell
            || type == GW::Constants::SkillType::Shout
            || type == GW::Constants::SkillType::Ritual;
    }

    uint32_t GetAttributeLevel(const GW::Constants::AttributeByte attribute)
    {
        const auto* attributes = GW::PartyMgr::GetAgentAttributes(GW::Agents::GetControlledCharacterId());
        const auto attribute_id = static_cast<uint32_t>(attribute);
        return attributes && attribute_id < 54 ? attributes[attribute_id].level : 0;
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

    bool IsAgentEffectActive(const uint32_t agent_id, const uint32_t skill_id, const GW::Constants::SkillType type)
    {
        const auto* agent = GW::Agents::GetAgentByID(agent_id);
        const auto* living = agent ? agent->GetAsAgentLiving() : nullptr;
        if (!living || !living->GetIsAlive()) {
            return false;
        }
        const auto* effects = GW::Effects::GetAgentEffects(agent_id);
        if (effects) {
            return std::ranges::any_of(*effects, [skill_id](const GW::Effect& effect) {
                return static_cast<uint32_t>(effect.skill_id) == skill_id && effect.GetTimeRemaining() > 0;
            });
        }
        switch (type) {
        case GW::Constants::SkillType::Hex:
            return living->GetIsHexed();
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
}

DLLAPI ToolboxPlugin* ToolboxPluginInstance()
{
    static SlopAuras instance;
    return &instance;
}

void SlopAuras::Initialize(ImGuiContext* ctx, ImGuiAllocFns allocator_fns, HMODULE toolbox_dll)
{
    ToolboxUIPlugin::Initialize(ctx, allocator_fns, toolbox_dll);
    GW::UI::RegisterUIMessageCallback(&map_loading_hook, GW::UI::UIMessage::kLoadMapContext, [this](GW::HookStatus*, GW::UI::UIMessage, void*, void*) {
        map_generation.fetch_add(1, std::memory_order_acq_rel);
        std::lock_guard lock(tracking_mutex);
        tracked_casts.clear();
        tracked_cooldowns.clear();
        pending_casts.clear();
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
        if (!skill || !IsTrackedCastType(skill->type)) {
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

void SlopAuras::Terminate()
{
    GW::UI::RemoveUIMessageCallback(&skill_activated_hook);
    GW::UI::RemoveUIMessageCallback(&skill_started_cast_hook);
    GW::UI::RemoveUIMessageCallback(&map_loading_hook);
    {
        std::lock_guard lock(tracking_mutex);
        tracked_casts.clear();
        tracked_cooldowns.clear();
        pending_casts.clear();
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
    std::lock_guard lock(tracking_mutex);
    std::erase_if(tracked_cooldowns, [this, generation, now](const TrackedCooldown& cooldown) {
        if (now - cooldown.timestamp >= cooldown.duration_ms) {
            return true;
        }
        if (generation != map_generation.load(std::memory_order_acquire) || !IsMapReady()) {
            return true;
        }
        const auto* agent = GW::Agents::GetAgentByID(cooldown.agent_id);
        const auto* living = agent ? agent->GetAsAgentLiving() : nullptr;
        return !living || !living->GetIsAlive() || living->allegiance != GW::Constants::Allegiance::Enemy;
    });
}

void SlopAuras::TrackCast(const int skill_id, const uint32_t target_agent_id)
{
    const auto generation = map_generation.load(std::memory_order_acquire);
    if (!IsMapReady()
        || skill_id <= 0 || static_cast<uint32_t>(skill_id) >= GW::SkillbarMgr::GetSkillCount()) {
        return;
    }

    const auto* skill = GW::SkillbarMgr::GetSkillConstantData(static_cast<GW::Constants::SkillID>(skill_id));
    if (!skill || !IsTrackedCastType(skill->type) || skill->duration0 == 0
        || skill->duration0 >= 0x20000 || skill->duration15 >= 0x20000) {
        return;
    }

    uint32_t attribute_level = 0;
    if (skill->attribute != GW::Constants::AttributeByte::None) {
        attribute_level = GetAttributeLevel(skill->attribute);
    }

    auto duration = static_cast<float>(skill->duration0);
    if (attribute_level > 0) {
        duration += (static_cast<float>(skill->duration15) - static_cast<float>(skill->duration0)) * attribute_level / 15.f;
    }
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

    const auto target_id = skill->type == GW::Constants::SkillType::Ritual
        ? GW::Agents::GetControlledCharacterId()
        : target_agent_id ? target_agent_id : GW::Agents::GetControlledCharacterId();
    const auto resolved_target_id = (skill->type == GW::Constants::SkillType::WeaponSpell
        || skill->type == GW::Constants::SkillType::Enchantment) && IsEnemyAgent(target_id)
        ? GW::Agents::GetControlledCharacterId()
        : target_id;
    std::lock_guard lock(tracking_mutex);
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
    const auto target_model_id = GetAgentModelId(resolved_target_id);
    if (skill->type == GW::Constants::SkillType::Hex
        && target_model_id
        && std::ranges::find(natural_resistance_model_ids, static_cast<int>(target_model_id)) != natural_resistance_model_ids.end()) {
        duration_ms = static_cast<uint32_t>(std::floor(duration / 2.f + 0.5f) * 1000.f);
    }
    if (duration_ms == 0) {
        return;
    }
    const auto now = GW::MemoryMgr::GetSkillTimer();
    std::erase_if(tracked_casts, [now](const TrackedCast& cast) {
        return now - cast.timestamp >= cast.duration_ms;
    });
    if (skill->type == GW::Constants::SkillType::WeaponSpell) {
        std::erase_if(tracked_casts, [resolved_target_id](const TrackedCast& cast) {
            const auto* tracked_skill = GW::SkillbarMgr::GetSkillConstantData(static_cast<GW::Constants::SkillID>(cast.skill_id));
            return cast.target_agent_id == resolved_target_id && tracked_skill
                && tracked_skill->type == GW::Constants::SkillType::WeaponSpell;
        });
    }
    const auto existing = std::ranges::find_if(tracked_casts, [skill_id, resolved_target_id](const TrackedCast& cast) {
        return cast.skill_id == static_cast<uint32_t>(skill_id) && cast.target_agent_id == resolved_target_id;
    });
    if (existing != tracked_casts.end()) {
        existing->timestamp = now;
        existing->duration_ms = duration_ms;
    }
    else {
        tracked_casts.push_back({static_cast<uint32_t>(skill_id), resolved_target_id, now, duration_ms});
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
    std::lock_guard lock(tracking_mutex);
    if (generation != map_generation.load(std::memory_order_acquire) || !IsMapReady()) {
        return;
    }
    if (std::ranges::find(cooldown_ids, static_cast<int>(skill_id)) == cooldown_ids.end()) {
        return;
    }
    std::erase_if(tracked_cooldowns, [now](const TrackedCooldown& cooldown) {
        return now - cooldown.timestamp >= cooldown.duration_ms;
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
            std::erase_if(tracked_casts, [this, generation, now](const TrackedCast& cast) {
                if (now - cast.timestamp >= cast.duration_ms) {
                    return true;
                }
                if (now - cast.timestamp < 1500) {
                    return false;
                }
                if (generation != map_generation.load(std::memory_order_acquire) || !IsMapReady()) {
                    return true;
                }
                const auto* skill = GW::SkillbarMgr::GetSkillConstantData(static_cast<GW::Constants::SkillID>(cast.skill_id));
                return skill && skill->type != GW::Constants::SkillType::Ritual
                    && !IsAgentEffectActive(cast.target_agent_id, cast.skill_id, skill->type);
            });
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
                    const auto remaining = cast.duration_ms - (now - cast.timestamp);
                    if (remaining > 0) {
                        add_active_effect(skill_id, cast.target_agent_id, remaining);
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
            const auto target_name = effect.target_agent_id == GW::Agents::GetControlledCharacterId()
                ? "you"
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
}

void SlopAuras::DrawSettings()
{
    ToolboxUIPlugin::DrawSettings();

    bool settings_changed = ImGui::Checkbox("Widget mode", &widget_mode);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Hides the title bar and background; icon sizes can be adjusted below.");
    }
    settings_changed |= ImGui::SliderFloat("Effect icon size", &effect_icon_size, 12.f, 64.f, "%.0f px");
    settings_changed |= ImGui::SliderFloat("Cooldown icon size", &cooldown_icon_size, 12.f, 64.f, "%.0f px");
    if (settings_changed) {
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
        if (skill && IsTrackedCastType(skill->type)) {
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
    ImGui::TextUnformatted("Natural Resistance agents (Model IDs):");
    ImGui::TextUnformatted("Hex duration is halved for casts targeting these agent model IDs, rounded half up to a whole second.");
    for (auto it = natural_resistance_model_ids.begin(); it != natural_resistance_model_ids.end(); ++it) {
        ImGui::PushID(input_id++);
        ImGui::SetNextItemWidth(150.f);
        settings_changed |= ImGui::InputInt("Agent model ID", &*it, 0);
        const int previous_id = *it;
        *it = std::max(0, *it);
        settings_changed |= *it != previous_id;
        ImGui::SameLine();
        if (ImGui::Button("Remove agent")) {
            natural_resistance_model_ids.erase(it);
            settings_changed = true;
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (ImGui::Button("Add agent model ID")) {
        natural_resistance_model_ids.emplace_back(0);
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
    std::vector<int> legacy_cast_by_me_ids;
    {
        std::lock_guard lock(tracking_mutex);
        LoadSetting("effect_ids", effect_ids);
        LoadSetting("cast_by_me_entries", cast_by_me_entries);
        LoadSetting("cast_by_me_ids", legacy_cast_by_me_ids);
        LoadSetting("cooldown_ids", cooldown_ids);
        LoadSetting("natural_resistance_model_ids", natural_resistance_model_ids);
        LoadSetting("widget_mode", widget_mode);
        LoadSetting("effect_icon_size", effect_icon_size);
        LoadSetting("cooldown_icon_size", cooldown_icon_size);
    }
    bool migrated_legacy_settings = false;
    {
        std::lock_guard lock(tracking_mutex);
        for (auto& skill_id : effect_ids) {
            skill_id = std::max(0, skill_id);
        }
        if (cast_by_me_entries.empty()) {
            for (const auto skill_id : legacy_cast_by_me_ids) {
                const auto entry = std::ranges::find(effect_ids, skill_id);
                if (entry != effect_ids.end()) {
                    const auto entry_index = static_cast<int>(std::distance(effect_ids.begin(), entry));
                    if (std::ranges::find(cast_by_me_entries, entry_index) == cast_by_me_entries.end()) {
                        cast_by_me_entries.push_back(entry_index);
                    }
                }
            }
            migrated_legacy_settings = !legacy_cast_by_me_ids.empty();
        }
        std::erase_if(cast_by_me_entries, [this](const int index) {
            return index < 0 || static_cast<size_t>(index) >= effect_ids.size();
        });
        for (auto& skill_id : cooldown_ids) {
            skill_id = std::max(0, skill_id);
        }
        for (auto& model_id : natural_resistance_model_ids) {
            model_id = std::max(0, model_id);
        }
        effect_icon_size = std::clamp(effect_icon_size, 12.f, 64.f);
        cooldown_icon_size = std::clamp(cooldown_icon_size, 12.f, 64.f);
    }
    if (migrated_legacy_settings) {
        SaveSettings(folder);
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
        SaveSetting("cast_by_me_ids", std::vector<int>{});
        SaveSetting("cooldown_ids", cooldown_ids);
        SaveSetting("natural_resistance_model_ids", natural_resistance_model_ids);
        SaveSetting("widget_mode", widget_mode);
        SaveSetting("effect_icon_size", effect_icon_size);
        SaveSetting("cooldown_icon_size", cooldown_icon_size);
    }
    ToolboxUIPlugin::SaveSettings(folder);
}
