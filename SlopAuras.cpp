#include "SlopAuras.h"

#include <optional>
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

    GW::Constants::SkillID GetConditionIconSkill(const uint32_t condition)
    {
        using GW::Constants::EffectID;
        using GW::Constants::SkillID;
        switch (condition) {
        case static_cast<uint32_t>(EffectID::bleeding):
        case static_cast<uint32_t>(SkillID::Bleeding):
            return SkillID::Bleeding;
        case static_cast<uint32_t>(EffectID::blind):
        case static_cast<uint32_t>(SkillID::Blind):
            return SkillID::Blind;
        case static_cast<uint32_t>(EffectID::burning):
        case static_cast<uint32_t>(SkillID::Burning):
            return SkillID::Burning;
        case static_cast<uint32_t>(SkillID::Crippled):
            return SkillID::Crippled;
        case static_cast<uint32_t>(SkillID::Deep_Wound):
            return SkillID::Deep_Wound;
        case static_cast<uint32_t>(EffectID::disease):
        case static_cast<uint32_t>(SkillID::Disease):
            return SkillID::Disease;
        case static_cast<uint32_t>(EffectID::poison):
        case static_cast<uint32_t>(SkillID::Poison):
            return SkillID::Poison;
        case static_cast<uint32_t>(EffectID::dazed):
        case static_cast<uint32_t>(SkillID::Dazed):
            return SkillID::Dazed;
        case static_cast<uint32_t>(EffectID::weakness):
        case static_cast<uint32_t>(SkillID::Weakness):
            return SkillID::Weakness;
        default:
            return SkillID::No_Skill;
        }
    }

    // A tracked skill whose sole effect is to inflict a condition (e.g. Weaken Armor -> Cracked Armor) is
    // displayed using the condition's own icon/name, since the condition is what actually matters to the
    // player and is shared by every skill that can cause it. Hexes keep their own icon even if they also
    // carry a condition.
    int GetDisplaySkillId(const int skill_id)
    {
        if (skill_id <= 0 || static_cast<uint32_t>(skill_id) >= GW::SkillbarMgr::GetSkillCount()) {
            return skill_id;
        }
        const auto* skill = GW::SkillbarMgr::GetSkillConstantData(static_cast<GW::Constants::SkillID>(skill_id));
        if (!skill || skill->type == GW::Constants::SkillType::Hex || skill->condition == 0) {
            return skill_id;
        }
        const auto condition_skill_id = GetConditionIconSkill(skill->condition);
        return condition_skill_id != GW::Constants::SkillID::No_Skill ? static_cast<int>(condition_skill_id) : skill_id;
    }

    // The condition (as its pseudo-skill, e.g. SkillID::Bleeding) when skill_id *is* that condition, else No_Skill.
    GW::Constants::SkillID GetConditionFromPseudoSkill(const int skill_id)
    {
        using GW::Constants::SkillID;
        switch (static_cast<SkillID>(skill_id)) {
        case SkillID::Bleeding:
        case SkillID::Blind:
        case SkillID::Burning:
        case SkillID::Crippled:
        case SkillID::Deep_Wound:
        case SkillID::Disease:
        case SkillID::Poison:
        case SkillID::Dazed:
        case SkillID::Weakness:
            return static_cast<SkillID>(skill_id);
        default:
            return SkillID::No_Skill;
        }
    }

    // The condition a (non-hex) skill inflicts, as its pseudo-skill, or No_Skill.
    GW::Constants::SkillID GetInflictedCondition(const int skill_id)
    {
        if (skill_id <= 0 || static_cast<uint32_t>(skill_id) >= GW::SkillbarMgr::GetSkillCount()) {
            return GW::Constants::SkillID::No_Skill;
        }
        const auto* skill = GW::SkillbarMgr::GetSkillConstantData(static_cast<GW::Constants::SkillID>(skill_id));
        if (!skill || skill->type == GW::Constants::SkillType::Hex || skill->condition == 0) {
            return GW::Constants::SkillID::No_Skill;
        }
        return GetConditionIconSkill(skill->condition);
    }

    // Does an observed skill activation count as the tracked id? Exact match always does. If the tracked id is a
    // condition itself (Bleeding, Poison, ...), any skill that inflicts that condition does too: the game never
    // activates the condition "skill", only the skill that causes it.
    bool SkillMatchesTrackedId(const int tracked_id, const int observed_id)
    {
        if (tracked_id == observed_id) {
            return true;
        }
        const auto tracked_condition = GetConditionFromPseudoSkill(tracked_id);
        return tracked_condition != GW::Constants::SkillID::No_Skill
            && tracked_condition == GetInflictedCondition(observed_id);
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

    constexpr uint32_t recharge_factor_ttl_ms = 30000; // speed-up effects are short-lived; forget what we learned after this
    constexpr float min_recharge_factor = 0.5f;        // recharge reductions do not stack beyond -50%
    constexpr uint32_t interrupt_match_window_ms = 1500;
    constexpr uint32_t cast_in_progress_timeout_ms = 10000; // generous upper bound on any skill's cast time

    // Skill durations are whole seconds in game (the wiki progression tables are integers). Weapon spells round
    // to the nearest second with .5 rounding down; other skills use plain nearest rounding.
    uint32_t RoundDurationToWholeSecondsMs(const float seconds, const bool half_down)
    {
        if (!std::isfinite(seconds) || seconds <= 0.f) {
            return 0;
        }
        constexpr double epsilon = 1e-4;
        const double whole = half_down ? std::ceil(static_cast<double>(seconds) - 0.5 - epsilon)
                                       : std::floor(static_cast<double>(seconds) + 0.5 + epsilon);
        return static_cast<uint32_t>(std::max(1.0, whole) * 1000.0);
    }

    // agent_id == 0 means "the player". Attribute levels of other agents (heroes in particular) can still be
    // read through the same API; non-visible agents (e.g. other real players) report no attributes, which is
    // returned as nullopt ("unknown") rather than 0.
    std::optional<uint32_t> GetAttributeLevel(const GW::Constants::AttributeByte attribute, const uint32_t agent_id = 0)
    {
        const auto resolved_agent_id = agent_id ? agent_id : GW::Agents::GetControlledCharacterId();
        const auto* attributes = GW::PartyMgr::GetAgentAttributes(resolved_agent_id);
        const auto attribute_id = static_cast<uint32_t>(attribute);
        if (!attributes || attribute_id >= 54) {
            return std::nullopt;
        }
        return attributes[attribute_id].level;
    }

    // caster_agent_id == 0 means "the player".
    float GetSkillDuration(const GW::Skill& skill, const uint32_t caster_agent_id = 0)
    {
        const auto duration0 = static_cast<float>(skill.duration0);
        const auto duration_delta = static_cast<float>(skill.duration15) - duration0;
        const auto resolved_caster_id = caster_agent_id ? caster_agent_id : GW::Agents::GetControlledCharacterId();
        const auto is_player_caster = resolved_caster_id == GW::Agents::GetControlledCharacterId();
        if (skill.title != 0) {
            // Title-track rank is only known for the player; for other casters assume the maximum rank.
            if (!is_player_caster) {
                return static_cast<float>(skill.duration15);
            }
            const auto* title = GW::PlayerMgr::GetTitleTrack(static_cast<GW::Constants::TitleID>(skill.title));
            const auto* world = GW::GetWorldContext();
            if (!title || !world || !title->max_title_rank
                || title->current_title_tier_index >= world->title_tiers.size()) {
                return static_cast<float>(skill.duration15); // unknown: assume maximum rank
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
        const auto attribute_level = GetAttributeLevel(skill.attribute, resolved_caster_id);
        // Unknown attribute (agent not exposed by the client, e.g. another real player): assume the maximum rank.
        // For other agents a reported level of 0 is treated the same way; it is almost always "not exposed".
        if (!attribute_level || (!*attribute_level && !is_player_caster)) {
            return static_cast<float>(skill.duration15);
        }
        return duration0 + duration_delta * static_cast<float>(std::min(*attribute_level, 15u)) / 15.f;
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
        const auto* effects = GW::Effects::GetAgentEffects(agent_id);
        if (skill.type == GW::Constants::SkillType::Hex) {
            // Exact check for allies whose effect list the client exposes; foes only have the coarse flag.
            if (effects && living->allegiance != GW::Constants::Allegiance::Enemy) {
                return std::ranges::any_of(*effects, [&skill](const GW::Effect& effect) {
                    return effect.skill_id == skill.skill_id && effect.GetTimeRemaining() > 0;
                });
            }
            return living->GetIsHexed();
        }
        if (skill.condition != 0) {
            const auto condition = GetInflictedCondition(static_cast<int>(skill.skill_id));
            // Exact data where the client exposes it (own party).
            if (effects && living->allegiance != GW::Constants::Allegiance::Enemy
                && condition != GW::Constants::SkillID::No_Skill
                && std::ranges::any_of(*effects, [condition](const GW::Effect& effect) {
                    return effect.skill_id == static_cast<uint32_t>(condition) && effect.GetTimeRemaining() > 0;
                })) {
                return true;
            }
            // Otherwise use the per-condition agent flags that exist; conditions without a flag fall back to
            // "has any condition".
            switch (condition) {
            case GW::Constants::SkillID::Bleeding:
                return living->GetIsBleeding();
            case GW::Constants::SkillID::Crippled:
                return living->GetIsCrippled();
            case GW::Constants::SkillID::Deep_Wound:
                return living->GetIsDeepWounded();
            case GW::Constants::SkillID::Poison:
                return living->GetIsPoisoned();
            default:
                return living->GetIsConditioned();
            }
        }
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
        GW::Chat::WriteChat(GW::Chat::CHANNEL_GWCA2, L"/sa (no argument) / show / hide - Toggle the SlopAuras window.", L"SlopAuras");
        GW::Chat::WriteChat(GW::Chat::CHANNEL_GWCA2, L"/sa settings / s - Toggle the SlopAuras settings window.", L"SlopAuras");
        GW::Chat::WriteChat(GW::Chat::CHANNEL_GWCA2, L"/sa print - Print tracked effects and enemy cooldowns.", L"SlopAuras");
        GW::Chat::WriteChat(GW::Chat::CHANNEL_GWCA2, L"/sa mute / unmute - Toggle SlopAuras sound notifications.", L"SlopAuras");
    };

    auto* instance = static_cast<SlopAuras*>(ToolboxPluginInstance());

    // Bare "/sa" behaves the same as "/sa show".
    if (argc < 2 || !argv || !argv[1]) {
        auto* visible = instance->GetVisiblePtr();
        *visible = true;
        GW::Chat::WriteChat(GW::Chat::CHANNEL_GWCA2, L"SlopAuras: Window shown.", L"SlopAuras");
        return;
    }

    const auto subcommand = PluginUtils::ToLower(argv[1]);
    if (subcommand == L"hide" || subcommand == L"show") {
        auto* visible = instance->GetVisiblePtr();
        *visible = !*visible;
        GW::Chat::WriteChat(GW::Chat::CHANNEL_GWCA2, *visible ? L"SlopAuras: Window shown." : L"SlopAuras: Window hidden.", L"SlopAuras");
    }
    else if (subcommand == L"settings" || subcommand == L"s") {
        instance->settings_window_visible = !instance->settings_window_visible;
        GW::Chat::WriteChat(GW::Chat::CHANNEL_GWCA2, instance->settings_window_visible
            ? L"SlopAuras: Settings window shown."
            : L"SlopAuras: Settings window hidden.", L"SlopAuras");
    }
    else if (subcommand == L"print") {
        instance->PrintTrackedEffects();
    }
    else if (subcommand == L"mute" || subcommand == L"unmute") {
        const auto muted = !instance->notifications_muted.load(std::memory_order_acquire);
        instance->SetNotificationsMuted(muted);
        GW::Chat::WriteChat(GW::Chat::CHANNEL_GWCA2, muted
            ? L"SlopAuras: Sound notifications muted."
            : L"SlopAuras: Sound notifications unmuted.", L"SlopAuras");
    }
    else {
        print_help();
    }
}

const SlopAuras::EffectConfig* SlopAuras::FindEffectConfig(const int skill_id) const
{
    const auto it = std::ranges::find(effect_configs, skill_id, &EffectConfig::skill_id);
    return it != effect_configs.end() ? &*it : nullptr;
}

bool SlopAuras::IsCastByMe(const int skill_id) const
{
    return std::ranges::any_of(effect_configs, [skill_id](const EffectConfig& config) {
        return config.cast_by_me && config.skill_id == skill_id;
    });
}

bool SlopAuras::IsTrackedExternalEnemyTarget(const int skill_id) const
{
    return IsTrackedExternalTarget(skill_id, true);
}

bool SlopAuras::IsTrackedExternalTarget(const int skill_id, const bool target_is_enemy) const
{
    const auto wanted = target_is_enemy ? EffectTarget::Enemies : EffectTarget::Allies;
    return std::ranges::any_of(effect_configs, [skill_id, wanted](const EffectConfig& config) {
        return !config.cast_by_me && config.target == wanted && SkillMatchesTrackedId(config.skill_id, skill_id);
    });
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
            case NotificationType::EffectApplied:
                if (const auto* config = FindEffectConfig(static_cast<int>(skill_id))) {
                    override_path = non_empty(config->applied_sound);
                }
                break;
            case NotificationType::EffectExpiring:
                if (const auto* config = FindEffectConfig(static_cast<int>(skill_id))) {
                    override_path = non_empty(config->expiring_sound);
                }
                break;
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

std::vector<SlopAuras::ActiveEffect> SlopAuras::CollectActiveEffects(const uint32_t now, const bool validate_casts)
{
    std::vector<ActiveEffect> active_effects;
    const auto generation = map_generation.load(std::memory_order_acquire);
    std::vector<EffectConfig> configs;
    std::vector<TrackedCast> casts;
    {
        std::lock_guard lock(tracking_mutex);
        configs = effect_configs;
        casts = tracked_casts;
    }
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
    const auto* player_effects = GW::Effects::GetPlayerEffects();
    for (const auto& config : configs) {
        if (generation != map_generation.load(std::memory_order_acquire) || !IsMapReady()) {
            return {};
        }
        const auto skill_id = config.skill_id;
        if (skill_id <= 0 || !config.show_in_window) {
            continue;
        }
        // Conditions are displayed as themselves, not as the (possibly several different) skills that can
        // cause them.
        const auto display_skill_id = GetDisplaySkillId(skill_id);

        // Effects not cast by the player come straight from the game's effect list(s); where to look for
        // them depends on the configured target.
        if (!config.cast_by_me) {
            switch (config.target) {
                case EffectTarget::Me: {
                    if (player_effects) {
                        DWORD remaining = 0;
                        for (const auto& effect : *player_effects) {
                            if (static_cast<int>(effect.skill_id) == skill_id) {
                                remaining = std::max(remaining, effect.GetTimeRemaining());
                            }
                        }
                        if (remaining > 0) {
                            add_active_effect(display_skill_id, GW::Agents::GetControlledCharacterId(), remaining);
                        }
                    }
                    break;
                }
                case EffectTarget::Allies: {
                    const auto* party_effects = GW::Effects::GetPartyEffectsArray();
                    if (party_effects && party_effects->valid()) {
                        for (const auto& agent_effects : *party_effects) {
                            if (!agent_effects.effects.valid()) {
                                continue;
                            }
                            DWORD remaining = 0;
                            for (const auto& effect : agent_effects.effects) {
                                if (static_cast<int>(effect.skill_id) == skill_id) {
                                    remaining = std::max(remaining, effect.GetTimeRemaining());
                                }
                            }
                            if (remaining > 0) {
                                add_active_effect(display_skill_id, agent_effects.agent_id, remaining);
                            }
                        }
                    }
                    break;
                }
                case EffectTarget::Enemies:
                    // Populated via tracked_casts by the ally-cast-on-enemy packet hook; handled generically below.
                    break;
            }
        }

        for (const auto& cast : casts) {
            if (!SkillMatchesTrackedId(skill_id, static_cast<int>(cast.skill_id))) {
                continue;
            }
            const auto elapsed = now - cast.timestamp;
            const auto predicted_remaining = elapsed < cast.duration_ms ? cast.duration_ms - elapsed : 0;
            const auto remaining = std::max<uint32_t>(predicted_remaining, cast.observed_remaining_ms);
            if (!remaining) {
                continue;
            }
            if (validate_casts && !cast.multi_ally && elapsed >= 1500) {
                const auto* skill = GW::SkillbarMgr::GetSkillConstantData(static_cast<GW::Constants::SkillID>(cast.skill_id));
                if (skill && (skill->type != GW::Constants::SkillType::Ritual || skill->condition != 0)
                    && !IsAgentEffectActive(cast.target_agent_id, *skill)) {
                    continue;
                }
            }
            add_active_effect(display_skill_id, cast.multi_ally ? 0 : cast.target_agent_id, remaining);
        }
    }
    return active_effects;
}

void SlopAuras::PrintTrackedEffects()
{
    if (!IsMapReady()) {
        GW::Chat::WriteChat(GW::Chat::CHANNEL_GWCA2, L"SlopAuras: Map is not ready.", L"SlopAuras");
        return;
    }

    const auto generation = map_generation.load(std::memory_order_acquire);
    const auto now = GW::MemoryMgr::GetSkillTimer();
    std::vector<std::string> natural_resistance_names;
    std::vector<TrackedCooldown> tracked_cooldowns_snapshot;
    {
        std::lock_guard lock(tracking_mutex);
        natural_resistance_names = natural_resistance_agent_names;
        for (const auto& cooldown : tracked_cooldowns) {
            if (FindCooldownConfig(static_cast<int>(cooldown.skill_id))) {
                tracked_cooldowns_snapshot.push_back(cooldown);
            }
        }
    }
    const auto active_effects = CollectActiveEffects(now, true);

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
        if (is_natural_resistance_enemy(cooldown.agent_id)) {
            continue;
        }
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
        tracked_knockdowns.clear();
        pending_casts.clear();
        in_progress_casts.clear();
        confirmed_interrupts.clear();
        agent_recharge_factors.clear();
        player_effect_notifications.clear();
        player_effect_snapshot_initialized = false;
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
    // caster/target field names are swapped for the cast-activation value_ids: `caster` holds the agent the
    // effect lands on and `target` holds the agent who actually cast the skill.
    const auto generic_value_target_callback_registered = GW::StoC::RegisterPacketCallback<GW::Packet::StoC::GenericValueTarget>(&generic_value_target_hook, [this](GW::HookStatus*, GW::Packet::StoC::GenericValueTarget* packet) {
        if (!packet || !IsMapReady()) {
            return;
        }
        switch (packet->Value_id) {
            case GW::Packet::StoC::GenericValueID::instant_skill_activated:
            case GW::Packet::StoC::GenericValueID::skill_activated:
                break;
            default:
                return;
        }
        const auto caster_agent_id = packet->target;
        const auto target_agent_id = packet->caster;
        const auto target_is_enemy = IsEnemyAgent(target_agent_id);
        if (target_is_enemy) {
            // Foe-on-foe casts are not interesting.
            if (IsEnemyAgent(caster_agent_id)) {
                return;
            }
        }
        else {
            // Allies outside the local party (other players' heroes, henchmen, allied NPCs) have no entry in the
            // client's party effect list, so the only way to see their effects is to watch the casts landing on
            // them. Party members are covered exactly by that list.
            const auto* target_agent = GW::Agents::GetAgentByID(target_agent_id);
            const auto* target_living = target_agent ? target_agent->GetAsAgentLiving() : nullptr;
            if (!target_living || target_living->allegiance != GW::Constants::Allegiance::Ally_NonAttackable
                || IsPartyAgent(target_agent_id)) {
                return;
            }
        }
        const auto skill_id = static_cast<int>(packet->value);
        {
            std::lock_guard lock(tracking_mutex);
            if (!IsTrackedExternalTarget(skill_id, target_is_enemy)) {
                if (skill_id > 0 && static_cast<uint32_t>(skill_id) < GW::SkillbarMgr::GetSkillCount()) {
                    const auto* debug_skill = GW::SkillbarMgr::GetSkillConstantData(static_cast<GW::Constants::SkillID>(skill_id));
                    if (debug_skill && debug_skill->condition != 0) {
                        OutputDebugStringA(std::format("SlopAuras: condition skill {} (condition {}) on {} agent {} by {} is not tracked\n",
                            skill_id, static_cast<uint32_t>(debug_skill->condition), target_is_enemy ? "enemy" : "ally",
                            target_agent_id, caster_agent_id).c_str());
                    }
                }
                return;
            }
        }
        TrackCast(skill_id, target_agent_id, caster_agent_id);
    });
    if (!generic_value_target_callback_registered) {
        OutputDebugStringW(L"SlopAuras: could not register the ally-cast skill callback; \"effect on enemies, not cast by me\" tracking will not work.\n");
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
        {
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
        }
        if (packet->agent_id != GW::Agents::GetControlledCharacterId()) {
            return;
        }
        const auto* skill = GW::SkillbarMgr::GetSkillConstantData(packet->skill_id);
        if (!IsTrackedCastType(skill)) {
            return;
        }
        std::lock_guard lock(tracking_mutex);
        if (generation != map_generation.load(std::memory_order_acquire) || !IsMapReady()) {
            return;
        }
        // The cast target is needed both for "cast by me" tracking and for "not cast by me" tracking on
        // enemies/allies: without it the cast falls back to the player as target and is dropped.
        if (!IsCastByMe(static_cast<int>(skill_id))
            && !IsTrackedExternalTarget(static_cast<int>(skill_id), true)
            && !IsTrackedExternalTarget(static_cast<int>(skill_id), false)) {
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
            TrackAgentCooldown(packet->agent_id, static_cast<uint32_t>(packet->skill_id));
            {
                std::lock_guard lock(tracking_mutex);
                std::erase_if(in_progress_casts, [agent_id = packet->agent_id](const InProgressCast& cast) {
                    return cast.agent_id == agent_id;
                });
            }
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
            TrackAgentCooldown(packet->agent_id, static_cast<uint32_t>(packet->skill_id));
            {
                std::lock_guard lock(tracking_mutex);
                std::erase_if(in_progress_casts, [agent_id = packet->agent_id](const InProgressCast& cast) {
                    return cast.agent_id == agent_id;
                });
            }
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
    GW::StoC::RemoveCallback<GW::Packet::StoC::GenericFloat>(&knockdown_hook);
    GW::StoC::RemoveCallback<GW::Packet::StoC::GenericValue>(&generic_value_hook);
    GW::StoC::RemoveCallback<GW::Packet::StoC::GenericValueTarget>(&generic_value_target_hook);
    {
        std::lock_guard lock(tracking_mutex);
        tracked_casts.clear();
        tracked_cooldowns.clear();
        tracked_knockdowns.clear();
        pending_casts.clear();
        in_progress_casts.clear();
        confirmed_interrupts.clear();
        agent_recharge_factors.clear();
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
        std::vector<std::pair<int, bool>> current_tracking;
        current_tracking.reserve(effect_configs.size());
        for (const auto& config : effect_configs) {
            current_tracking.emplace_back(config.skill_id, config.cast_by_me);
        }
        const auto tracked_effects_changed = player_effect_config_snapshot != current_tracking;
        player_effect_config_snapshot = std::move(current_tracking);

        if (player_effects) {
            std::vector<PlayerEffectNotification> current_player_effects;
            for (const auto& effect : *player_effects) {
                const auto skill_id = static_cast<uint32_t>(effect.skill_id);
                if (!effect.duration || !effect.GetTimeRemaining()
                    || !IsTrackedEffect(static_cast<int>(skill_id))
                    || IsCastByMe(static_cast<int>(skill_id))) {
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
                    pending_notifications.push_back({NotificationType::EffectApplied, skill_id});
                }
                auto& tracked = current_player_effects.back();
                const auto remaining = effect.GetTimeRemaining();
                if (!tracked.expiration_notified
                    && remaining <= static_cast<DWORD>(notification_lead_seconds * 1000.f)
                    && notification_enabled[static_cast<size_t>(NotificationType::EffectExpiring)]) {
                    tracked.expiration_notified = true;
                    pending_notifications.push_back({NotificationType::EffectExpiring, skill_id});
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
            if (cast->resistance_pending && !cast->duration_exact) {
                // The target name decodes asynchronously; retry every frame until it is available.
                const auto target_name = NormalizeAgentName(GetAgentName(cast->target_agent_id));
                if (target_name != "loading name...") {
                    cast->resistance_pending = false;
                    if (!target_name.empty() && target_name != "unknown target"
                        && std::ranges::any_of(natural_resistance_agent_names, [&target_name](const std::string& name) {
                            return NormalizeAgentName(name) == target_name;
                        })) {
                        cast->duration_ms = static_cast<uint32_t>(std::floor(cast->duration_ms / 2000.0 + 0.5) * 1000.0);
                    }
                }
            }
            if (!cast->multi_ally && !cast->duration_exact) {
                // Self/party targets expose real effect data: replace the prediction with it.
                if (const auto* target_effects = GW::Effects::GetAgentEffects(cast->target_agent_id)) {
                    for (const auto& effect : *target_effects) {
                        if (static_cast<uint32_t>(effect.skill_id) != cast->skill_id
                            || !IsEffectTimestampNearCast(effect.timestamp, cast->timestamp)) {
                            continue;
                        }
                        // For maintained effects agent_id is the caster; ignore another player's copy of the same skill.
                        if (effect.agent_id != 0 && effect.agent_id != GW::Agents::GetControlledCharacterId()) {
                            continue;
                        }
                        const auto remaining = effect.GetTimeRemaining();
                        if (!remaining) {
                            continue;
                        }
                        const auto exact_duration_ms = elapsed + remaining;
                        const auto drift_ms = exact_duration_ms > cast->duration_ms
                            ? exact_duration_ms - cast->duration_ms : cast->duration_ms - exact_duration_ms;
                        if (drift_ms >= 250) {
                            // Calibration aid: shows where the duration formula/rounding disagrees with the game.
                            OutputDebugStringA(std::format("SlopAuras: skill {} predicted {}ms, game reports {}ms\n",
                                cast->skill_id, cast->duration_ms, exact_duration_ms).c_str());
                        }
                        cast->duration_ms = exact_duration_ms;
                        cast->duration_exact = true;
                        cast->resistance_pending = false;
                        break;
                    }
                }
            }
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
            const auto remaining = std::max<uint32_t>(predicted_remaining, cast->observed_remaining_ms);
            if (!cast->expiration_notified
                && remaining <= static_cast<uint32_t>(notification_lead_seconds * 1000.f)
                && notification_enabled[static_cast<size_t>(NotificationType::EffectExpiring)]) {
                cast->expiration_notified = true;
                pending_notifications.push_back({NotificationType::EffectExpiring, cast->skill_id});
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
    std::unordered_set<int> tracked_effect_ids;
    std::unordered_set<int> tracked_cooldown_ids;
    std::vector<TrackedCast> casts;
    std::vector<TrackedCooldown> cooldowns;
    std::vector<TrackedKnockdown> knockdowns;
    {
        std::lock_guard lock(tracking_mutex);
        for (const auto& config : effect_configs) {
            if (config.show_on_nameplate) {
                tracked_effect_ids.insert(config.skill_id);
            }
        }
        for (const auto& config : cooldown_configs) {
            tracked_cooldown_ids.insert(config.skill_id);
        }
        casts = tracked_casts;
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
        else if (ally_nameplates_enabled
            && (IsPartyAgent(agent->agent_id) || living->allegiance == GW::Constants::Allegiance::Ally_NonAttackable)) {
            // Allowed through below.
        }
        else {
            continue;
        }

        struct NameplateLine {
            std::string text;
            ImU32 color;
            IDirect3DTexture9* icon = nullptr;
        };
        std::vector<NameplateLine> lines;
        if (nameplate_show_knockdown && living->GetIsKnockedDown()) {
            const auto timer = std::ranges::find_if(knockdowns, [agent](const TrackedKnockdown& knockdown) {
                return knockdown.agent_id == agent->agent_id;
            });
            if (timer != knockdowns.end() && now - timer->timestamp < timer->duration_ms) {
                const auto remaining = timer->duration_ms - (now - timer->timestamp);
                lines.emplace_back(std::format("DOWN  {:.1f}s", static_cast<double>(remaining) / 1000.0),
                    IM_COL32(255, 90, 80, 255));
            }
            else {
                lines.emplace_back("KNOCKED DOWN", IM_COL32(255, 90, 80, 255));
            }
        }

        if (nameplate_show_effects) {
            for (const auto& cast : casts) {
                if (cast.target_agent_id != agent->agent_id
                    || !std::ranges::any_of(tracked_effect_ids, [&cast](const int tracked_id) {
                        return SkillMatchesTrackedId(tracked_id, static_cast<int>(cast.skill_id));
                    })) {
                    continue;
                }
                const auto elapsed = now - cast.timestamp;
                if (elapsed >= cast.duration_ms) {
                    continue;
                }
                const auto* skill = GW::SkillbarMgr::GetSkillConstantData(static_cast<GW::Constants::SkillID>(cast.skill_id));
                if (!skill) {
                    continue;
                }
                const auto display_skill_id = GetDisplaySkillId(static_cast<int>(cast.skill_id));
                IDirect3DTexture9* icon_texture = nullptr;
                if (display_skill_id > 0) {
                    const auto icon = GetSkillImage(static_cast<GW::Constants::SkillID>(display_skill_id));
                    icon_texture = icon && *icon ? *icon : nullptr;
                }
                const auto remaining = std::format("{:.1f}s",
                    static_cast<double>(cast.duration_ms - elapsed) / 1000.0);
                if (skill->type == GW::Constants::SkillType::Hex || skill->condition != 0) {
                    lines.push_back({remaining, IM_COL32(190, 225, 255, 255), icon_texture});
                }
                else {
                    lines.push_back({std::format("{}  {}", GetSkillName(display_skill_id), remaining),
                        IM_COL32(190, 225, 255, 255)});
                }
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
            const auto line_width = text_size.x + (line.icon ? effect_icon_size + 4.f : 0.f);
            max_width = std::max(max_width, line_width);
            total_height += std::max(text_size.y, line.icon ? effect_icon_size : 0.f);
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
            const auto row_height = std::max(text_sizes[i].y, line.icon ? effect_icon_size : 0.f);
            const auto row_width = text_sizes[i].x + (line.icon ? effect_icon_size + 4.f : 0.f);
            auto x = name_position.x - row_width * 0.5f;
            if (line.icon) {
                const auto icon_y = y + (row_height - effect_icon_size) * 0.5f;
                draw_list->AddImage((ImTextureID)(intptr_t)line.icon, ImVec2(x, icon_y),
                    ImVec2(x + effect_icon_size, icon_y + effect_icon_size));
                x += effect_icon_size + 4.f;
            }
            if (!line.text.empty()) {
                draw_list->AddText(ImVec2(x, y + (row_height - text_sizes[i].y) * 0.5f), line.color, line.text.c_str());
            }
            y += row_height + vertical_spacing;
        }
    }
}

void SlopAuras::TrackCast(const int skill_id, const uint32_t target_agent_id, const uint32_t caster_agent_id)
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

    const auto player_agent_id = GW::Agents::GetControlledCharacterId();
    const auto resolved_caster_id = caster_agent_id ? caster_agent_id : player_agent_id;
    const auto is_player_caster = resolved_caster_id == player_agent_id;

    auto duration = GetSkillDuration(*skill, resolved_caster_id);
    // Gear/title-derived bonuses can only be read for the player; other casters use the base prediction.
    if (is_player_caster && skill->type == GW::Constants::SkillType::WeaponSpell) {
        duration *= 1.f + GetAttributeLevel(GW::Constants::AttributeByte::SpawningPower).value_or(0) * 0.04f;
    }
    if (is_player_caster && skill->type == GW::Constants::SkillType::Enchantment) {
        duration *= 1.f + GetEnchantingWeaponBonus();
    }
    if (duration <= 0.f || duration >= 0x20000) {
        return;
    }

    auto duration_ms = RoundDurationToWholeSecondsMs(duration, skill->type == GW::Constants::SkillType::WeaponSpell);
    if (duration_ms == 0) {
        return;
    }

    const auto multi_ally = IsMultiAllyEffect(*skill);
    const auto target_id = skill->type == GW::Constants::SkillType::Ritual
        ? player_agent_id
        : target_agent_id ? target_agent_id : player_agent_id;
    auto resolved_target_id = target_id;
    if (multi_ally) {
        resolved_target_id = 0;
    }
    else if (is_player_caster && (skill->type == GW::Constants::SkillType::WeaponSpell
        || skill->type == GW::Constants::SkillType::Enchantment) && IsEnemyAgent(target_id)) {
        resolved_target_id = player_agent_id;
    }
    const auto resistance_pending = !multi_ally
        && (skill->type == GW::Constants::SkillType::Hex || skill->condition != 0)
        && IsEnemyAgent(resolved_target_id);
    std::unique_lock lock(tracking_mutex);
    if (generation != map_generation.load(std::memory_order_acquire) || !IsMapReady()) {
        return;
    }
    if (is_player_caster && IsCastByMe(skill_id)) {
        // Tracked as "cast by me".
    }
    else if (multi_ally || !IsTrackedExternalTarget(skill_id, IsEnemyAgent(resolved_target_id))) {
        // Otherwise the cast only matters for the explicit "not cast by me" tracking on enemies/allies. That
        // includes the player's own casts: "regardless of who cast them" must not exclude the player.
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
    // A condition is one effect no matter which skill applied it, so match on the condition, not the skill.
    const auto is_condition_skill = skill->type != GW::Constants::SkillType::Hex && skill->condition != 0;
    const auto existing = std::ranges::find_if(tracked_casts, [&](const TrackedCast& cast) {
        if (cast.target_agent_id != resolved_target_id || cast.multi_ally != multi_ally) {
            return false;
        }
        if (cast.skill_id == static_cast<uint32_t>(skill_id)) {
            return true;
        }
        if (!is_condition_skill) {
            return false;
        }
        const auto* other = GW::SkillbarMgr::GetSkillConstantData(static_cast<GW::Constants::SkillID>(cast.skill_id));
        return other && other->type != GW::Constants::SkillType::Hex && other->condition == skill->condition;
    });
    if (existing != tracked_casts.end()) {
        if (is_condition_skill) {
            // A reapplied condition keeps its original timer unless the new duration outlasts what remains.
            const auto elapsed = now - existing->timestamp;
            const auto predicted_remaining = elapsed < existing->duration_ms ? existing->duration_ms - elapsed : 0;
            if (std::max<uint32_t>(predicted_remaining, existing->observed_remaining_ms) >= duration_ms) {
                return;
            }
        }
        existing->skill_id = static_cast<uint32_t>(skill_id);
        existing->timestamp = now;
        existing->duration_ms = duration_ms;
        existing->observed_remaining_ms = 0;
        existing->observed_updated_timestamp = now;
        existing->expiration_notified = false;
        existing->resistance_pending = resistance_pending;
        existing->duration_exact = false;
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
            now,
            resistance_pending
        });
    }
    const auto notify_applied = notification_enabled[static_cast<size_t>(NotificationType::EffectApplied)];
    lock.unlock();
    if (notify_applied) {
        PlayNotification(NotificationType::EffectApplied, static_cast<uint32_t>(skill_id));
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
        auto active_effects = CollectActiveEffects(now, false);
        if (!map_is_current()) {
            end_window();
            return;
        }

        bool has_active_effect = false;
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
            ImGui::Text("%s%s: %.1fs", cooldown.from_interrupt ? "~" : "", target_name.c_str(),
                static_cast<double>(cooldown.remaining) / 1000.0);
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
    ImGui::Separator();
    ImGui::TextUnformatted("Enemy nameplates");
    settings_changed |= ImGui::Checkbox("Show enemy nameplate overlays", &enemy_nameplates_enabled);
    settings_changed |= ImGui::Checkbox("Show ally nameplate overlays", &ally_nameplates_enabled);
    settings_changed |= ImGui::Checkbox("Show knockdown and timer", &nameplate_show_knockdown);
    settings_changed |= ImGui::Checkbox("Show tracked effects", &nameplate_show_effects);
    settings_changed |= ImGui::Checkbox("Show tracked cooldowns", &nameplate_show_cooldowns);
    settings_changed |= ImGui::Checkbox("Learn recharge speed-ups (heuristic)", &adaptive_recharge_enabled);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("When a tracked agent recasts a skill sooner than predicted, shorten predictions for that agent for a while.\n"
            "Cooldowns started by an interrupt are shown with a ~ because interrupt skills may disable the skill for longer.");
    }
    ImGui::TextWrapped("Only tracked effects and cooldowns are shown. Hexes and conditions use their icons and estimated remaining timers.");
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

    if (ImGui::BeginTable("effect_ids_table", 9,
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Skill", ImGuiTableColumnFlags_WidthStretch, 0.2f);
        ImGui::TableSetupColumn("Skill ID", ImGuiTableColumnFlags_WidthFixed, 90.f);
        ImGui::TableSetupColumn("Cast by me", ImGuiTableColumnFlags_WidthFixed, 90.f);
        ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthFixed, 90.f);
        ImGui::TableSetupColumn("In window", ImGuiTableColumnFlags_WidthFixed, 70.f);
        ImGui::TableSetupColumn("On nameplate", ImGuiTableColumnFlags_WidthFixed, 90.f);
        ImGui::TableSetupColumn("Applied sound", ImGuiTableColumnFlags_WidthStretch, 0.15f);
        ImGui::TableSetupColumn("Expiring sound", ImGuiTableColumnFlags_WidthStretch, 0.15f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 70.f);
        ImGui::TableHeadersRow();

        for (size_t entry_index = 0; entry_index < effect_configs.size(); ++entry_index) {
            auto& config = effect_configs[entry_index];
            ImGui::TableNextRow();
            ImGui::PushID(input_id++);

            ImGui::TableNextColumn();
            const auto name = IsMapReady() ? GetSkillName(config.skill_id) : std::string("Unknown skill");
            ImGui::TextUnformatted(name.c_str());

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            settings_changed |= ImGui::InputInt("##effect_id", &config.skill_id, 0);
            const int previous_id = config.skill_id;
            config.skill_id = std::max(0, config.skill_id);
            settings_changed |= config.skill_id != previous_id;

            const auto* skill = IsMapReady()
                && config.skill_id > 0 && static_cast<uint32_t>(config.skill_id) < GW::SkillbarMgr::GetSkillCount()
                ? GW::SkillbarMgr::GetSkillConstantData(static_cast<GW::Constants::SkillID>(config.skill_id))
                : nullptr;

            ImGui::TableNextColumn();
            if (IsTrackedCastType(skill)) {
                settings_changed |= ImGui::Checkbox("##cast_by_me", &config.cast_by_me);
            }

            ImGui::TableNextColumn();
            {
                static constexpr std::array<const char*, 3> target_labels{"Me", "Allies", "Enemies"};
                auto target_index = static_cast<int>(config.target);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::Combo("##effect_target", &target_index, target_labels.data(), static_cast<int>(target_labels.size()))) {
                    config.target = static_cast<EffectTarget>(target_index);
                    settings_changed = true;
                }
                if (config.cast_by_me && ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Ignored while \"Cast by me\" is checked; the actual cast target is used instead.");
                }
            }

            ImGui::TableNextColumn();
            settings_changed |= ImGui::Checkbox("##show_in_window", &config.show_in_window);

            ImGui::TableNextColumn();
            settings_changed |= ImGui::Checkbox("##show_on_nameplate", &config.show_on_nameplate);

            ImGui::TableNextColumn();
            settings_changed |= draw_wav_override_button("applied_wav", config.applied_sound,
                notification_sound_paths[static_cast<size_t>(NotificationType::EffectApplied)]);

            ImGui::TableNextColumn();
            settings_changed |= draw_wav_override_button("expiring_wav", config.expiring_sound,
                notification_sound_paths[static_cast<size_t>(NotificationType::EffectExpiring)]);

            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Remove")) {
                effect_configs.erase(effect_configs.begin() + static_cast<std::ptrdiff_t>(entry_index));
                settings_changed = true;
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (ImGui::Button("Add effect ID")) {
        effect_configs.emplace_back();
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
    ImGui::Separator();
    ImGui::TextUnformatted("Natural Resistance agents (names):");
    ImGui::TextUnformatted("Hex and condition duration is halved for casts targeting agents with these names, rounded half up to a whole second.");
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
        std::erase_if(tracked_casts, [this](const TrackedCast& cast) {
            const auto skill_id = static_cast<int>(cast.skill_id);
            return !IsCastByMe(skill_id) && !IsTrackedExternalTarget(skill_id, true)
                && !IsTrackedExternalTarget(skill_id, false);
        });
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
        std::vector<int> saved_cast_by_me_entries;
        std::vector<int> saved_cooldown_ids;
        std::vector<std::string> saved_applied_sounds;
        std::vector<std::string> saved_expiring_sounds;
        std::vector<std::string> saved_cooldown_sounds;
        // Indices listed here have the corresponding true-by-default flag turned off. An empty/missing list
        // (as in any settings file saved before these toggles existed) means every entry keeps showing
        // everywhere, exactly as before.
        std::vector<int> saved_hide_in_window_entries;
        std::vector<int> saved_hide_on_nameplate_entries;
        // Per-entry target (EffectTarget) value when cast_by_me is false. Missing/short entries default to
        // Me, matching the only lookup behavior that existed before this field.
        std::vector<int> saved_target_values;
        // Per-cooldown-entry target (CooldownTarget). Missing/short entries default to Enemy, matching the
        // only behavior that existed before ally cooldown tracking.
        std::vector<int> saved_cooldown_target_values;
        LoadSetting("effect_ids", saved_effect_ids);
        LoadSetting("cast_by_me_entries", saved_cast_by_me_entries);
        LoadSetting("cooldown_ids", saved_cooldown_ids);
        LoadSetting("natural_resistance_agent_names", natural_resistance_agent_names);
        LoadSetting("effect_applied_sound_overrides", saved_applied_sounds);
        LoadSetting("effect_expiring_sound_overrides", saved_expiring_sounds);
        LoadSetting("cooldown_sound_overrides", saved_cooldown_sounds);
        LoadSetting("effect_hide_in_window_entries", saved_hide_in_window_entries);
        LoadSetting("effect_hide_on_nameplate_entries", saved_hide_on_nameplate_entries);
        LoadSetting("effect_target_values", saved_target_values);
        LoadSetting("cooldown_target_values", saved_cooldown_target_values);
        effect_configs.clear();
        for (size_t i = 0; i < saved_effect_ids.size(); ++i) {
            EffectConfig config;
            config.skill_id = std::max(0, saved_effect_ids[i]);
            config.cast_by_me = std::ranges::find(saved_cast_by_me_entries, static_cast<int>(i)) != saved_cast_by_me_entries.end();
            config.applied_sound = i < saved_applied_sounds.size() ? saved_applied_sounds[i] : std::string{};
            config.expiring_sound = i < saved_expiring_sounds.size() ? saved_expiring_sounds[i] : std::string{};
            config.show_in_window = std::ranges::find(saved_hide_in_window_entries, static_cast<int>(i)) == saved_hide_in_window_entries.end();
            config.show_on_nameplate = std::ranges::find(saved_hide_on_nameplate_entries, static_cast<int>(i)) == saved_hide_on_nameplate_entries.end();
            config.target = i < saved_target_values.size()
                ? static_cast<EffectTarget>(std::clamp(saved_target_values[i], 0, 2))
                : EffectTarget::Me;
            effect_configs.push_back(std::move(config));
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
        LoadSetting("effect_icon_size", effect_icon_size);
        LoadSetting("cooldown_icon_size", cooldown_icon_size);
        LoadSetting("enemy_nameplates_enabled", enemy_nameplates_enabled);
        LoadSetting("ally_nameplates_enabled", ally_nameplates_enabled);
        LoadSetting("nameplate_show_knockdown", nameplate_show_knockdown);
        LoadSetting("nameplate_show_effects", nameplate_show_effects);
        LoadSetting("nameplate_show_cooldowns", nameplate_show_cooldowns);
        LoadSetting("notification_enabled", notification_enabled);
        LoadSetting("notification_sound_paths", notification_sound_paths);
        LoadSetting("notification_lead_seconds", notification_lead_seconds);
    }
    {
        std::lock_guard lock(tracking_mutex);
        effect_icon_size = std::clamp(effect_icon_size, 12.f, 64.f);
        cooldown_icon_size = std::clamp(cooldown_icon_size, 12.f, 64.f);
        notification_lead_seconds = std::clamp(notification_lead_seconds, 0.5f, 30.f);
        player_effect_config_snapshot.clear();
        for (const auto& config : effect_configs) {
            player_effect_config_snapshot.emplace_back(config.skill_id, config.cast_by_me);
        }
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
        std::vector<int> effect_ids;
        std::vector<int> cast_by_me_entries;
        std::vector<int> cooldown_ids;
        std::vector<std::string> effect_applied_sound_overrides;
        std::vector<std::string> effect_expiring_sound_overrides;
        std::vector<std::string> cooldown_sound_overrides;
        std::vector<int> effect_hide_in_window_entries;
        std::vector<int> effect_hide_on_nameplate_entries;
        std::vector<int> effect_target_values;
        std::vector<int> cooldown_target_values;
        for (size_t i = 0; i < effect_configs.size(); ++i) {
            effect_ids.push_back(effect_configs[i].skill_id);
            if (effect_configs[i].cast_by_me) {
                cast_by_me_entries.push_back(static_cast<int>(i));
            }
            effect_applied_sound_overrides.push_back(effect_configs[i].applied_sound);
            effect_expiring_sound_overrides.push_back(effect_configs[i].expiring_sound);
            if (!effect_configs[i].show_in_window) {
                effect_hide_in_window_entries.push_back(static_cast<int>(i));
            }
            if (!effect_configs[i].show_on_nameplate) {
                effect_hide_on_nameplate_entries.push_back(static_cast<int>(i));
            }
            effect_target_values.push_back(static_cast<int>(effect_configs[i].target));
        }
        for (const auto& config : cooldown_configs) {
            cooldown_ids.push_back(config.skill_id);
            cooldown_sound_overrides.push_back(config.ready_sound);
            cooldown_target_values.push_back(static_cast<int>(config.target));
        }
        SaveSetting("effect_ids", effect_ids);
        SaveSetting("cast_by_me_entries", cast_by_me_entries);
        SaveSetting("cooldown_ids", cooldown_ids);
        SaveSetting("natural_resistance_agent_names", natural_resistance_agent_names);
        SaveSetting("effect_applied_sound_overrides", effect_applied_sound_overrides);
        SaveSetting("effect_expiring_sound_overrides", effect_expiring_sound_overrides);
        SaveSetting("cooldown_sound_overrides", cooldown_sound_overrides);
        SaveSetting("effect_hide_in_window_entries", effect_hide_in_window_entries);
        SaveSetting("effect_hide_on_nameplate_entries", effect_hide_on_nameplate_entries);
        SaveSetting("effect_target_values", effect_target_values);
        SaveSetting("cooldown_target_values", cooldown_target_values);
        SaveSetting("adaptive_recharge_enabled", adaptive_recharge_enabled);
        SaveSetting("widget_mode", widget_mode);
        SaveSetting("effect_icon_size", effect_icon_size);
        SaveSetting("cooldown_icon_size", cooldown_icon_size);
        SaveSetting("enemy_nameplates_enabled", enemy_nameplates_enabled);
        SaveSetting("ally_nameplates_enabled", ally_nameplates_enabled);
        SaveSetting("nameplate_show_knockdown", nameplate_show_knockdown);
        SaveSetting("nameplate_show_effects", nameplate_show_effects);
        SaveSetting("nameplate_show_cooldowns", nameplate_show_cooldowns);
        SaveSetting("notification_enabled", notification_enabled);
        SaveSetting("notification_sound_paths", notification_sound_paths);
        SaveSetting("notification_lead_seconds", notification_lead_seconds);
    }
    ToolboxUIPlugin::SaveSettings(folder);
}
