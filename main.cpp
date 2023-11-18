#include <cstdlib>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "plugin.hpp"
#include "memory.hpp"

Plugin pl;

std::filesystem::path user_preferences_path;

void engine_server_playback_temp_entity(mss::IVEngineServer*, mss::IRecipientFilter&, float, const void*, const mss::SendTable*, int);
void server_game_ents_check_transmit(mss::IServerGameEnts*, mss::CheckTransmitInfo*, const unsigned short*, int);
void base_server_write_delta_entities(mss::BaseServer*, mss::BaseClient*, mss::ClientFrame*, mss::ClientFrame*, mss::BfWrite&);
void cs_player_player_run_command(mss::CSPlayer*, mss::UserCmd*, mss::IMoveHelper*);
void cs_player_play_step_sound(mss::CSPlayer*, mss::Vector&, mss::surfacedata_t*, float, bool);
bool game_client_send_net_msg(mss::GameClient*, mss::INetMessage&, bool);
void engine_sound_server_emit_sound(mss::IEngineSound*, mss::IRecipientFilter&, int, int, const char*, float, mss::SoundLevel, int, int, int, const mss::Vector*, const mss::Vector*, mss::UtlVector<mss::Vector>*, bool, float, int);

mss::IVEngineServer* engine_server;
mss::ICvar* engine_cvar;
mss::BaseServer* server;
mss::IServerGameEnts* server_game_ents;
mss::GlobalVars* global_vars;

mss::TEFireBullets* te_fire_bullets;

mss::IPredictionSystem* recipient_filter_prediction_system;

mss::UtlDict<mss::UserMessage*>* user_messages;
mss::UtlDict<mss::FileWeaponInfo*, unsigned short>* weapon_info_database;

mss::ConVar* sv_stressbots;
mss::ConVar* sv_client_min_interp_ratio;
mss::ConVar* sv_client_max_interp_ratio;

int kill_cam_user_message_id;
int damage_user_message_id;
int text_msg_user_message_id;
int hud_msg_user_message_id;
int hint_text_user_message_id;
int key_hint_text_user_message_id;
int reset_hud_user_message_id;

bool is_step_sound = false;
bool is_weapon_sound = false;
bool is_peek_message = false;

constexpr std::byte nop_opcode{0x90};

constexpr const char* radio_menu_sounds[]{
    "buttons/button14.wav",
    "buttons/combine_button7.wav"
};

constexpr int hud_msg_hold_time_bits_offset = 26 * 8;
constexpr float text_msg_center_duration = 3.0f;
constexpr float hint_text_duration = 4.0f;
constexpr float key_hint_text_duration = 7.0f;

constexpr const char* base_player_spawn_symbol = "_ZN11CBasePlayer5SpawnEv";
constexpr const char* game_client_vtable_symbol = "_ZTV11CGameClient";
constexpr const char* cs_player_vtable_symbol = "_ZTV9CCSPlayer";
constexpr const char* cs_bot_vtable_symbol = "_ZTV6CCSBot";
constexpr const char* te_fire_bullets_symbol = "_ZL15g_TEFireBullets";
constexpr const char* weapon_info_database_symbol = "_ZL20m_WeaponInfoDatabase";
constexpr const char* base_entity_emit_sound_symbol = "_ZN11CBaseEntity9EmitSoundER16IRecipientFilteriPKcPK6VectorfPf";
constexpr const char* recipient_filter_prediction_system_symbol = "_ZL33g_RecipientFilterPredictionSystem";
constexpr const char* user_messages_symbol = "_ZL14g_UserMessages";

constexpr int server_game_ents_check_transmit_vtable_index = 7;
constexpr int base_server_write_delta_entities_vtable_index = 38;
constexpr int base_client_send_net_msg_vtable_index = 23;
constexpr int base_player_player_run_command_vtable_index = 420;
constexpr int base_player_play_step_sound_vtable_index = 360;
constexpr int engine_sound_emit_sound_vtable_index = 5;
constexpr int engine_server_playback_temp_entity_vtable_index = 61;

constexpr int base_player_spawn_stop_replay_mode_call = 0x5be;

constexpr int smooth_peeking_interp_ratio = 2;

decltype(base_server_write_delta_entities)* original_base_server_write_delta_entities;
decltype(server_game_ents_check_transmit)* original_server_game_ents_check_transmit;
decltype(game_client_send_net_msg)* original_game_client_send_net_msg;
decltype(cs_player_player_run_command)* original_cs_player_run_command;
decltype(cs_player_play_step_sound)* original_cs_player_play_step_sound;
decltype(engine_sound_server_emit_sound)* original_engine_sound_server_emit_sound;
decltype(engine_server_playback_temp_entity)* original_engine_server_playback_temp_entity;

void (*base_entity_emit_sound)(mss::IRecipientFilter& f, int ent_index, const char* sound_name, const mss::Vector* origin, float sound_time, float* duration);

class PlayerDisconnectListener : public mss::IGameEventListener2 {
    virtual void fire_game_event(mss::IGameEvent*);
} player_disconnect_listener;

struct UserPreferences {
    auto operator<=>(const UserPreferences&) const = default;

    unsigned my_target;
    bool turning;
    bool smooth;
    bool velocity;
};

constexpr UserPreferences default_preferences{
    .my_target = 0,
    .turning = false,
    .smooth = false,
    .velocity = false,
};

inline
bool load_user_preferences(unsigned account_id, UserPreferences& pref) {
    const auto s = user_preferences_path / std::to_string(account_id);

    if (!std::filesystem::exists(s))
        return false;

    std::ifstream f{s, std::ios::binary};
    f.read(reinterpret_cast<char*>(&pref), sizeof(pref));

    return true;
}

inline
void store_user_preferences(const std::filesystem::path& path, const UserPreferences& pref) {
    std::ofstream f{path, std::ios::binary | std::ios::trunc};
    f.write(reinterpret_cast<const char*>(&pref), sizeof(pref));
}

struct PlayerData {
    // index based, 0 - self
    int new_target;
    int old_target;
    int peek_target;
    std::vector<int> targets;

    // index based, starts from 1
    int last_target;

    // account_id, otherwise - player index (starts from 1)
    struct MyTarget {
        unsigned id;
        bool is_account_id;
    } my_target;

    int my_target_index();

    bool turning;
    bool smooth;
    bool velocity;

    bool init_message;

    struct Recreate {
        int first;
        int second;
        bool is_swapped;
    } recreate;

    float start_time;
    float hud_update_time;

    struct Message {
        std::byte data[256];
        int length;
        float end_time;
    } hud_msg[mss::max_net_message - 1], hint_text, key_hint_text, text_msg_center;

    int old_buttons;

} inline player_data[mss::max_players]{};

extern "C" __attribute__((visibility("default")))
void* CreateInterface(const char* name, mss::InterfaceReturnStatus* rc) {
    Dl_info info;
    dladdr(reinterpret_cast<void*>(CreateInterface), &info);
    user_preferences_path = std::filesystem::path{info.dli_fname}.remove_filename() / "user_preferences";

    void* p = (std::strcmp(name, mss::interface_version_server_plugin_callbacks) == 0) ? &pl : nullptr;

    if (rc)
        *rc = (p ? mss::InterfaceReturnStatus::OK : mss::InterfaceReturnStatus::FAILED);

    return p;
}

inline
int index_from_user_id(int user_id) {
    for (int i = 0; i < server->clients.count(); ++i)
        if (server->clients[i]->user_id == user_id)
            return i;

    return -1;
}

inline
int index_from_account_id(unsigned account_id) {
    for (int i = 0; i < server->clients.count(); ++i) {
        const auto cl = server->clients[i];

        if ((cl->steam_id.component.account_id * cl->fully_authenticated * cl->is_active()) == account_id)
            return i;
    }

    return -1;
}

float client_lerp(mss::IClient* cl, float update_rate) {
    float cl_interp = std::atof(cl->get_user_setting("cl_interp"));
    float cl_interp_ratio = std::atof(cl->get_user_setting("cl_interp_ratio"));

    if (sv_client_min_interp_ratio->get_float() != -1.0)
        cl_interp_ratio = std::clamp(cl_interp_ratio, sv_client_min_interp_ratio->get_float(), sv_client_max_interp_ratio->get_float());

    float ratio_interp = cl_interp_ratio / update_rate;

    return cl_interp > ratio_interp ? cl_interp : ratio_interp;
}

void send_interp_cvars(mss::BaseClient* cl, const char* min, const char* max) {
    char data[256];

    mss::BfWrite bf{data, sizeof(data)};

    bf.write_u_bit_long(static_cast<unsigned>(mss::NetMessageType::SET_CONVAR), mss::net_msg_type_bits);
    bf.write_byte(2);

    bf.write_string("sv_client_min_interp_ratio");
    bf.write_string(min);

    bf.write_string("sv_client_max_interp_ratio");
    bf.write_string(max);

    cl->net_channel->send_data(bf);
}

inline
int PlayerData::my_target_index() {
    return this->my_target.is_account_id ? index_from_account_id(this->my_target.id) : static_cast<int>(this->my_target.id) - 1;
}

void PlayerDisconnectListener::fire_game_event(mss::IGameEvent* event) {
    auto user_id = event->get_int("userid");
    auto index = index_from_user_id(user_id);

    auto& pd = player_data[index];
    const auto cl = server->clients[index];

    if (cl->fully_authenticated) {
        const auto s = user_preferences_path / std::to_string(cl->steam_id.component.account_id);
        const UserPreferences pref{
            .my_target = pd.my_target.id * pd.my_target.is_account_id,
            .turning = pd.turning,
            .smooth = pd.smooth,
            .velocity = pd.velocity
        };

        if (std::filesystem::exists(s) || pref != default_preferences)
            store_user_preferences(s, pref);
    }

    pd.my_target.id = 0;
    pd.last_target = 0;
    pd.init_message = false;

    for (int i = 0; i < server->clients.count(); ++i) {
        if (player_data[i].last_target == (index + 1))
            player_data[i].last_target = 0;

        if (!player_data[i].my_target.is_account_id && (static_cast<int>(player_data[i].my_target.id) == (index + 1)))
            player_data[i].my_target.id = 0;
    }
}

inline
mss::BaseEntity* base_entity_from_edict(mss::Edict* e) {
    return e->unknown->get_base_entity();
}

inline
mss::BaseEntity* base_entity_from_client(mss::GameClient* cl) {
    return base_entity_from_edict(cl->edict);
}

inline
bool recipient_filter_contains(const mss::IRecipientFilter& f, int index) {
    int c = f.get_recipient_count();

    for (int i = 0; i < c; ++i)
        if (f.get_recipient_index(i) == index)
            return true;

    return false;
}

mss::WeaponSound determine_sound_type(mss::CSWeaponID weapon_id, mss::CSWeaponMode weapon_mode) {
    if (weapon_mode == mss::CSWeaponMode::PRIMARY)
        return mss::WeaponSound::SINGLE;
    
    switch (weapon_id) {
        case mss::CSWeaponID::M4A1:
            return mss::WeaponSound::SPECIAL1;
        
        case mss::CSWeaponID::USP:
            return mss::WeaponSound::SPECIAL1;

        default:
            return mss::WeaponSound::SINGLE;
    }
}

inline
bool should_borrow_user_message(int msg_type) {
    return msg_type == damage_user_message_id || msg_type == hud_msg_user_message_id || msg_type == hint_text_user_message_id || msg_type == key_hint_text_user_message_id || msg_type == text_msg_user_message_id || msg_type == reset_hud_user_message_id;
}

void text_msg_message(const mss::IRecipientFilter& f, int dest, const char* s) {
    auto bf = engine_server->begin_user_message(&f, text_msg_user_message_id);

    bf->write_byte(static_cast<int>(dest));
    bf->write_string(s);

    engine_server->end_user_message();
}

void kill_cam_message(const mss::IRecipientFilter& f, int mode, int first, int second) {
    auto bf = engine_server->begin_user_message(&f, kill_cam_user_message_id);

    bf->write_byte(mode);
    bf->write_byte(first);
    bf->write_byte(second);

    engine_server->end_user_message();
}

void hud_msg_message(const mss::IRecipientFilter& f, const mss::HudTextParms& params, const char* s) {
    auto bf = engine_server->begin_user_message(&f, hud_msg_user_message_id);

    bf->write_byte(params.channel);
    bf->write_float(params.x);
    bf->write_float(params.y);
    bf->write_byte(params.color1.r);
    bf->write_byte(params.color1.g);
    bf->write_byte(params.color1.b);
    bf->write_byte(params.color1.a);
    bf->write_byte(params.color2.r);
    bf->write_byte(params.color2.g);
    bf->write_byte(params.color2.b);
    bf->write_byte(params.color2.a);
    bf->write_byte(params.effect);
    bf->write_float(params.fade_in_time);
    bf->write_float(params.fade_out_time);
    bf->write_float(params.hold_time);
    bf->write_float(params.fx_time);
    bf->write_string(s);

    engine_server->end_user_message();
}

void substitute_user_message(const mss::IRecipientFilter& f, int msg_type, std::byte* data, int bits) {
    auto bf = engine_server->begin_user_message(&f, msg_type);
    const auto p = bf->data;

    bf->data = reinterpret_cast<unsigned long*>(data);
    bf->cur_bit = bits;

    engine_server->end_user_message();

    bf->data = p;
}

void substitute_hud_msg_user_message(const mss::IRecipientFilter& f, int msg_type, std::byte* data, int bits, float hold_time) {
    auto bf = engine_server->begin_user_message(&f, msg_type);
    const auto p = bf->data;

    bf->data = reinterpret_cast<unsigned long*>(data);

    bf->cur_bit = hud_msg_hold_time_bits_offset;
    bf->write_float(hold_time);

    bf->cur_bit = bits;

    engine_server->end_user_message();

    bf->data = p;
}

bool is_new_target(const PlayerData& pd, int target) {
    return std::find(pd.targets.begin(), pd.targets.end(), target) == pd.targets.end();
}

bool is_valid_target(mss::GameClient* cl) {
    if (!cl->is_active() || (cl->is_fake_client() && !sv_stressbots->get_int()))
        return false;

    const auto ent = base_entity_from_client(cl);

    return ent->is_alive() && !(ent->effects & mss::EntityEffects::NO_DRAW);
}

int cycle_target(PlayerData& pd, int target, bool reverse=false) {
    int step = 1 - 2 * reverse;
    auto start_it = std::find(pd.targets.begin(), pd.targets.end(), target);

    for (auto it = start_it + step; it != start_it;) {
        if (it == pd.targets.end()) {
            it = pd.targets.begin();
            continue;
        }

        if (it < pd.targets.begin()) {
            it = pd.targets.end() - 1;
            continue;
        }

        int index = *it;

        if (is_valid_target(server->clients[index - 1]))
            return index;

        it += step;
    }

    return 0;
}

int find_new_target(int index, mss::Vector origin, bool nearest=true) {
    float best_distance = std::numeric_limits<float>::max() * nearest;
    int target = -1;

    for (int i = 0; i < server->clients.count(); ++i) {
        if (index == i)
            continue;
        
        const auto cl = server->clients[i];

        if (!is_valid_target(cl) || !is_new_target(player_data[index], i + 1))
            continue;

        const auto ent = base_entity_from_client(cl);

        float dist = ent->abs_origin.dist_to(origin) * (2 * nearest - 1);

        if (dist < best_distance) {
            best_distance = dist;
            target = i;
        }
    }

    return target + 1;
}

void engine_server_playback_temp_entity(mss::IVEngineServer* p, mss::IRecipientFilter& f, float delay, const void* sender, const mss::SendTable* pst, int class_id) {
    mss::MRecipientFilter filter;
    int predicted_entity = 0;

    filter.init_message = f.is_init_message();
    filter.reliable = f.is_reliable();

    if (recipient_filter_prediction_system->suppress_host)
        predicted_entity = engine_server->get_index_from_edict(recipient_filter_prediction_system->suppress_host->network.edict);

    for (int i = 0; i < server->clients.count(); ++i) {
        const auto cl = server->clients[i];

        if (!cl->is_active())
            continue;

        const auto& pd = player_data[i];

        if (pd.peek_target > 0) {
            if (recipient_filter_contains(f, pd.peek_target) || predicted_entity == pd.peek_target)
                filter.add_recipient(i);
        }
        else if (recipient_filter_contains(f, i + 1))
            filter.add_recipient(i);
    }

    original_engine_server_playback_temp_entity(p, filter, delay, sender, pst, class_id);

    if (sender == te_fire_bullets) {        
        int c = filter.get_recipient_count();

        filter.count = 0;

        for (int i = 0; i < c; ++i) {
            int index = filter.get_recipient_index(i) - 1;

            if (player_data[index].peek_target > 0)
                filter.add_recipient(index);
        }

        if (filter.count > 0) {
            char name[mss::max_weapon_string];
            std::sprintf(name, "weapon_%s", mss::weapon_alias_info[te_fire_bullets->weapon_id]);

            auto info = weapon_info_database->element(weapon_info_database->find(name));
            auto sound_type = determine_sound_type(static_cast<mss::CSWeaponID>(te_fire_bullets->weapon_id), static_cast<mss::CSWeaponMode>(te_fire_bullets->mode));

            auto s = info->shoot_sounds[static_cast<int>(sound_type)];

            is_weapon_sound = true;
            base_entity_emit_sound(filter, te_fire_bullets->player + 1, s, &te_fire_bullets->origin, 0.0f, nullptr);
            is_weapon_sound = false;
        }
    }
}

void engine_sound_server_emit_sound(mss::IEngineSound* p, mss::IRecipientFilter& f, int ent_index, int channel, const char* sample, float volume, mss::SoundLevel sound_level, int flags, int pitch, int special_dsp, const mss::Vector* origin, const mss::Vector* direction, mss::UtlVector<mss::Vector>* vec_origins, bool update_positions, float sound_time, int speaker_entity) {
    if (is_weapon_sound || (std::strcmp(sample, radio_menu_sounds[0]) == 0) || (std::strcmp(sample, radio_menu_sounds[1]) == 0))
        return original_engine_sound_server_emit_sound(p, f, ent_index, channel, sample, volume, sound_level, flags, pitch, special_dsp, origin, direction, vec_origins, update_positions, sound_time, speaker_entity);

    mss::MRecipientFilter filter;

    filter.init_message = f.is_init_message();
    filter.reliable = f.is_reliable();

    int predicted_entity = 0;

    if (recipient_filter_prediction_system->suppress_host)
        predicted_entity = engine_server->get_index_from_edict(recipient_filter_prediction_system->suppress_host->network.edict);

    if (is_step_sound) {
        mss::BitVec<mss::absolute_player_limit> v;
        engine_server->determine_multicast_recipients_for_message(false, *origin, v);

        for (int i = 0; i < server->clients.count(); ++i) {
            const auto cl = server->clients[i];

            if (!cl->is_active())
                continue;

            const auto& pd = player_data[i];

            if (pd.peek_target > 0) {
                if (recipient_filter_contains(f, pd.peek_target) || v.get(pd.peek_target - 1))
                    filter.add_recipient(i);
            }
            else if (recipient_filter_contains(f, i + 1))
                filter.add_recipient(i);
        }
    }
    else {
        for (int i = 0; i < server->clients.count(); ++i) {
            const auto cl = server->clients[i];

            if (!cl->is_active())
                continue;

            const auto& pd = player_data[i];

            if (pd.peek_target > 0) {
                if (recipient_filter_contains(f, pd.peek_target) || predicted_entity == pd.peek_target)
                    filter.add_recipient(i);
            }
            else if (recipient_filter_contains(f, i + 1))
                filter.add_recipient(i);
        }
    }

    original_engine_sound_server_emit_sound(p, filter, ent_index, channel, sample, volume, sound_level, flags, pitch, special_dsp, origin, direction, vec_origins, update_positions, sound_time, speaker_entity);   
}

void cs_player_player_run_command(mss::CSPlayer* p, mss::UserCmd* cmd, mss::IMoveHelper* mh) {
    int index = engine_server->get_index_from_edict(p->network.edict) - 1;
    auto& pd = player_data[index];

    if (pd.peek_target <= 0)
        return original_cs_player_run_command(p, cmd, mh);

    if (cmd->buttons & mss::PlayerButtons::ATTACK && ((pd.old_buttons & mss::PlayerButtons::ATTACK) == 0)) {
        const auto start_it = std::find(pd.targets.begin(), pd.targets.end(), pd.peek_target);

        auto it = std::find_if(start_it + 1, pd.targets.end(), [](auto elem) {return is_valid_target(server->clients[elem - 1]);});

        if (it == pd.targets.end()) {
            int target = find_new_target(index, p->abs_origin);

            if (!target)
                it = std::find_if(pd.targets.begin(), start_it, [](auto elem) {return is_valid_target(server->clients[elem - 1]);});
            else
                it = pd.targets.insert(pd.targets.end(), target);
        }

        if (it != pd.targets.end())
            pd.new_target = *it;
    }
    else if (cmd->buttons & mss::PlayerButtons::ATTACK2 && ((pd.old_buttons & mss::PlayerButtons::ATTACK2) == 0)) {
        const auto start_it = std::find(pd.targets.rbegin(), pd.targets.rend(), pd.peek_target);

        auto it = std::find_if(start_it + 1, pd.targets.rend(), [](auto elem) {return is_valid_target(server->clients[elem - 1]);});

        if (it == pd.targets.rend()) {
            int target = find_new_target(index, p->abs_origin, false);

            if (!target)
                it = std::find_if(pd.targets.rbegin(), start_it, [](auto elem) {return is_valid_target(server->clients[elem - 1]);});
            else {
                pd.targets.insert(pd.targets.begin(), target);
                it = pd.targets.rend() - 1;
            }
        }

        if (it != pd.targets.rend())
            pd.new_target = *it;
    }
    else if (cmd->buttons & mss::PlayerButtons::USE && ((pd.old_buttons & mss::PlayerButtons::USE) == 0)) {
        const auto cl = server->clients[pd.peek_target - 1];

        int my_idx = pd.my_target_index();

        if (my_idx != cl->client_slot) {
            pd.my_target.is_account_id = cl->fully_authenticated;
            pd.my_target.id = pd.my_target.is_account_id ? cl->steam_id.component.account_id : pd.peek_target;

            // update hud of our new target if he was peeking us or he wasn't peeking at all
            if (player_data[cl->client_slot].my_target_index() == index && ((player_data[cl->client_slot].peek_target == (index + 1)) || !player_data[cl->client_slot].peek_target))
                player_data[cl->client_slot].hud_update_time = 0.0f;
        }
        else
            pd.my_target.id = 0;

        // update hud of our old target if he was peeking us or we were peeking him while he wasn't peeking at all
        if (my_idx != -1 && (player_data[my_idx].my_target_index() == index) && ((player_data[my_idx].peek_target == index + 1) || (!player_data[my_idx].peek_target && cl->client_slot == my_idx)))
            player_data[my_idx].hud_update_time = 0.0f;

        pd.hud_update_time = 0.0f;
    }

    pd.old_buttons = cmd->buttons;

    cmd->buttons &= ~mss::PlayerButtons::ATTACK;
    cmd->buttons &= ~mss::PlayerButtons::ATTACK2;
    cmd->weapon_select = 0;

    if (!pd.turning)
        p->player_state.fix_angle = static_cast<int>(mss::FixAngle::ABSOLUTE);

    return original_cs_player_run_command(p, cmd, mh);
}

void cs_player_play_step_sound(mss::CSPlayer* p, mss::Vector& origin, mss::surfacedata_t* surface, float vol, bool force) {
    is_step_sound = true;
    original_cs_player_play_step_sound(p, origin, surface, vol, force);
    is_step_sound = false;
}

bool game_client_send_net_msg(mss::GameClient* cl, mss::INetMessage& msg, bool force_reliable) {
    if (is_peek_message)
        return original_game_client_send_net_msg(cl, msg, force_reliable);

    if (msg.get_group() == static_cast<int>(mss::NetMessageGroup::USER_MESSAGES)) {
        auto& user_message = static_cast<mss::SVC_UserMessage&>(msg);

        if (!should_borrow_user_message(user_message.msg_type))
            return original_game_client_send_net_msg(cl, msg, force_reliable);

        auto& pd = player_data[cl->client_slot];

        if (user_message.msg_type == hud_msg_user_message_id) {
            mss::BfRead bf{user_message.data_out.data, user_message.data_out.data_bytes};
            int channel = bf.read_byte() % (mss::max_net_message - 1);

            bf.cur_bit = hud_msg_hold_time_bits_offset;
            float hold_time = bf.read_float();

            std::memcpy(pd.hud_msg[channel].data, user_message.data_out.data, user_message.data_out.data_bytes);
            pd.hud_msg[channel].end_time = global_vars->current_time + hold_time;
            pd.hud_msg[channel].length = user_message.data_out.cur_bit;
        }
        else if (user_message.msg_type == hint_text_user_message_id) {
            std::memcpy(pd.hint_text.data, user_message.data_out.data, user_message.data_out.data_bytes);
            pd.hint_text.end_time = global_vars->current_time + hint_text_duration;
            pd.hint_text.length = user_message.data_out.cur_bit;
        }
        else if (user_message.msg_type == key_hint_text_user_message_id) {
            std::memcpy(pd.key_hint_text.data, user_message.data_out.data, user_message.data_out.data_bytes);
            pd.key_hint_text.end_time = global_vars->current_time + key_hint_text_duration;
            pd.key_hint_text.length = user_message.data_out.cur_bit;
        }
        else if (user_message.msg_type == text_msg_user_message_id) {
            mss::BfRead bf{user_message.data_out.data, user_message.data_out.data_bytes};

            auto dest = static_cast<mss::HudDestination>(bf.read_byte());

            if (dest != mss::HudDestination::CENTER)
                return original_game_client_send_net_msg(cl, msg, force_reliable);

            std::memcpy(pd.text_msg_center.data, user_message.data_out.data, user_message.data_out.data_bytes);
            pd.text_msg_center.end_time = global_vars->current_time + text_msg_center_duration;
            pd.text_msg_center.length = user_message.data_out.cur_bit;
        }
        else if (user_message.msg_type == reset_hud_user_message_id) {
            msg.set_reliable(false);

            pd.hud_update_time = -1.0f;

            for (int i = 0; i < server->clients.count(); ++i) {
                if (server->clients[i]->is_active() && player_data[i].peek_target == (cl->client_slot + 1))
                    player_data[i].hud_update_time = -1.0f;
            }

            pd.hint_text.end_time = 0.0f;
            pd.key_hint_text.end_time = 0.0f;
            pd.text_msg_center.end_time = 0.0f;
            
            for (int i = 0; i < mss::max_net_message - 1; ++i)
                pd.hud_msg[i].end_time = 0.0f;
        }

        for (int i = 0; i < server->clients.count(); ++i) {
            const auto target = server->clients[i];

            if (!target->is_active())
                continue;

            if (player_data[i].peek_target == (cl->client_slot + 1))
                original_game_client_send_net_msg(target, msg, force_reliable);
        }

        if (pd.peek_target)
            return true;
    }

    return original_game_client_send_net_msg(cl, msg, force_reliable);
}

void server_game_ents_check_transmit(mss::IServerGameEnts* self, mss::CheckTransmitInfo* info, const unsigned short* edict_indices, int num_edicts) {
    original_server_game_ents_check_transmit(self, info, edict_indices, num_edicts);

    int index = engine_server->get_index_from_edict(info->client_ent) - 1;
    const auto& pd = player_data[index];

    if (pd.peek_target <= 0)
        return;

    auto ent = static_cast<mss::BasePlayer*>(base_entity_from_edict(info->client_ent));
    auto target = static_cast<mss::BasePlayer*>(base_entity_from_client(server->clients[pd.peek_target - 1]));

    for (int i = 0; i < 2; ++i) {
        if (ent->view_model[i].is_valid())
            info->transmit_edict->clear(ent->view_model[i].get_entry_index());

        if (target->view_model[i].is_valid())
            info->transmit_edict->set(target->view_model[i].get_entry_index());
    }
}

void base_server_write_delta_entities(mss::BaseServer* sv, mss::BaseClient* cl, mss::ClientFrame* to, mss::ClientFrame* from, mss::BfWrite& bf) {
    if (!from)
        return original_base_server_write_delta_entities(sv, cl, to, from, bf);

    auto& pd = player_data[cl->client_slot];

    if (!pd.recreate.first && !pd.recreate.second)
        return original_base_server_write_delta_entities(sv, cl, to, from, bf);

    // if we don't do this, the engine can delete an entity before receiving killcam message and that leads to client stuck, so we prevent the entity deletion
    if (pd.old_target && !server->clients[pd.old_target - 1]->is_active())
        to->snapshot->entities[pd.old_target].cls = reinterpret_cast<mss::ServerClass*>(0x1);

    int target = pd.recreate.first ? pd.recreate.first : pd.recreate.second;

    if (!pd.recreate.is_swapped) {
        auto entry = &to->snapshot->entities[target];

        // we swap the serial number for player entity to truly recreate it on a client
        entry->serial_number = -entry->serial_number;
        original_base_server_write_delta_entities(sv, cl, to, from, bf);
        entry->serial_number = -entry->serial_number;
    }
    else {
        struct RecreateEntity {
            int index;
            mss::ServerClass* cls;
        };

        // entities to trigger SV_NeedsExplicitCreate for EnterPVS only
        std::vector<RecreateEntity> entities;

        auto ent = static_cast<mss::BasePlayer*>(base_entity_from_client(server->clients[target - 1]));

        for (int i = 0; i < mss::max_weapons; ++i)
            if (ent->my_weapons[i].is_valid()) {
                int index = ent->my_weapons[i].get_entry_index();
                entities.emplace_back(index, from->snapshot->entities[index].cls);
            }

        for (int i = 0; i < 2; ++i)
            if (ent->view_model[i].is_valid()) {
                int index = ent->view_model->get_entry_index();
                entities.emplace_back(index, from->snapshot->entities[index].cls);
            }

        for (const auto elem : entities)
            from->snapshot->entities[elem.index].cls = nullptr;

        auto entry = &from->snapshot->entities[target];

        // give proper serial number back to a client, it leads to recreate also
        entry->serial_number = -entry->serial_number;
        original_base_server_write_delta_entities(sv, cl, to, from, bf);
        entry->serial_number = -entry->serial_number;

        for (const auto elem : entities)
            from->snapshot->entities[elem.index].cls = elem.cls;

        if (pd.recreate.first)
            pd.recreate.first = 0;
        else
            pd.recreate.second = 0;
    }

    pd.recreate.is_swapped = !pd.recreate.is_swapped;

    if (pd.old_target && !server->clients[pd.old_target - 1]->is_active())
        to->snapshot->entities[pd.old_target].cls = nullptr;
}

bool Plugin::load(mss::CreateInterface ef, mss::CreateInterface sf) {
    init_memory_modules();
    unprotect_all_memory();

    std::filesystem::create_directory(user_preferences_path);

    engine_server = static_cast<mss::IVEngineServer*>(ef(mss::interface_version_engine_server, nullptr));
    engine_cvar = static_cast<mss::ICvar*>(ef(mss::interface_version_engine_cvar, nullptr));
    server = static_cast<mss::BaseServer*>(engine_server->get_i_server());
    
    auto player_info_manager = static_cast<mss::IPlayerInfoManager*>(sf(mss::interface_version_player_info_manager, nullptr));
    global_vars = player_info_manager->get_global_vars();

    auto game_event_mananger = static_cast<mss::IGameEventManager2*>(ef(mss::interface_version_game_event_manager, nullptr));
    game_event_mananger->add_listener(&player_disconnect_listener, "player_disconnect", true);

    auto vtable = reinterpret_cast<void**>(binary_symbols("bin/engine_srv.so", &game_client_vtable_symbol, 1).back()) + 2;

    original_game_client_send_net_msg = reinterpret_cast<decltype(game_client_send_net_msg)*>(vtable[base_client_send_net_msg_vtable_index]);
    vtable[base_client_send_net_msg_vtable_index] = reinterpret_cast<void*>(game_client_send_net_msg);

    constexpr const char* server_symbols[]{
        base_player_spawn_symbol,
        te_fire_bullets_symbol,
        weapon_info_database_symbol,
        base_entity_emit_sound_symbol,
        recipient_filter_prediction_system_symbol,
        user_messages_symbol,
        cs_player_vtable_symbol,
        cs_bot_vtable_symbol
    };

    const auto v = binary_symbols("cstrike/bin/server_srv.so", server_symbols, sizeof(server_symbols) / sizeof(const char*));

    std::memset(reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(v[0]) + base_player_spawn_stop_replay_mode_call), static_cast<int>(nop_opcode), 6);

    te_fire_bullets = reinterpret_cast<mss::TEFireBullets*>(v[1]);
    weapon_info_database = reinterpret_cast<decltype(weapon_info_database)>(v[2]);
    base_entity_emit_sound = reinterpret_cast<decltype(base_entity_emit_sound)>(v[3]);
    recipient_filter_prediction_system = reinterpret_cast<mss::IPredictionSystem*>(v[4]);
    user_messages = reinterpret_cast<decltype(user_messages)>(v[5]);

    vtable = reinterpret_cast<void**>(v[6]) + 2;
    original_cs_player_run_command = reinterpret_cast<decltype(cs_player_player_run_command)*>(vtable[base_player_player_run_command_vtable_index]);
    vtable[base_player_player_run_command_vtable_index] = reinterpret_cast<void*>(cs_player_player_run_command);

    for (int i = 0; i < 2; ++i) {
        vtable = reinterpret_cast<void**>(v[6 + i]) + 2;
        original_cs_player_play_step_sound = reinterpret_cast<decltype(cs_player_play_step_sound)*>(vtable[base_player_play_step_sound_vtable_index]);
        vtable[base_player_play_step_sound_vtable_index] = reinterpret_cast<void*>(cs_player_play_step_sound);
    }

    auto server_game_ents = static_cast<mss::IServerGameEnts*>(sf(mss::interface_version_server_game_ents, nullptr));

    original_server_game_ents_check_transmit = reinterpret_cast<decltype(server_game_ents_check_transmit)*>((*reinterpret_cast<void***>(server_game_ents))[server_game_ents_check_transmit_vtable_index]);
    (*reinterpret_cast<void***>(server_game_ents))[server_game_ents_check_transmit_vtable_index] = reinterpret_cast<void*>(server_game_ents_check_transmit);

    auto engine_sound = static_cast<mss::IEngineSound*>(ef(mss::interface_version_engine_sound, nullptr));

    original_engine_sound_server_emit_sound = reinterpret_cast<decltype(engine_sound_server_emit_sound)*>((*reinterpret_cast<void***>(engine_sound))[engine_sound_emit_sound_vtable_index]);
    (*reinterpret_cast<void***>(engine_sound))[engine_sound_emit_sound_vtable_index] = reinterpret_cast<void*>(engine_sound_server_emit_sound);

    original_base_server_write_delta_entities = reinterpret_cast<decltype(base_server_write_delta_entities)*>((*reinterpret_cast<void***>(server))[base_server_write_delta_entities_vtable_index]);
    (*reinterpret_cast<void***>(server))[base_server_write_delta_entities_vtable_index] = reinterpret_cast<void*>(base_server_write_delta_entities);

    original_engine_server_playback_temp_entity = reinterpret_cast<decltype(engine_server_playback_temp_entity)*>((*reinterpret_cast<void***>(engine_server))[engine_server_playback_temp_entity_vtable_index]);
    (*reinterpret_cast<void***>(engine_server))[engine_server_playback_temp_entity_vtable_index] = reinterpret_cast<void*>(engine_server_playback_temp_entity);

    sv_stressbots = engine_cvar->find_var("sv_stressbots");
    sv_client_min_interp_ratio = engine_cvar->find_var("sv_client_min_interp_ratio");
    sv_client_max_interp_ratio = engine_cvar->find_var("sv_client_max_interp_ratio");

    sv_stressbots->flags = 0;
    sv_stressbots->set_value(1);

    kill_cam_user_message_id = user_messages->find("KillCam");
    damage_user_message_id = user_messages->find("Damage");
    text_msg_user_message_id = user_messages->find("TextMsg");
    hud_msg_user_message_id = user_messages->find("HudMsg");
    hint_text_user_message_id = user_messages->find("HintText");
    key_hint_text_user_message_id = user_messages->find("KeyHintText");
    reset_hud_user_message_id = user_messages->find("ResetHUD");

    return true;
}

void Plugin::unload() {

}

void Plugin::pause() {

}

void Plugin::unpause() {

}

const char* Plugin::get_plugin_description() {
    return "QuickPeek (v1.0.0)";
}

void Plugin::level_init(const char* map_name) {

}

void Plugin::server_activate(mss::Edict* edict_list, int edict_count, int max_clients) {
    for (int i = 0; i < server->clients.count(); ++i) {
        const auto cl = server->clients[i];

        if (!cl->is_connected() || !cl->fully_authenticated)
            continue;

        const auto& pd = player_data[i];

        const auto s = user_preferences_path / std::to_string(cl->steam_id.component.account_id);
        const UserPreferences pref{
            .my_target = pd.my_target.id * pd.my_target.is_account_id,
            .turning = pd.turning,
            .smooth = pd.smooth,
            .velocity = pd.velocity
        };

        if (std::filesystem::exists(s) || pref != default_preferences)
            store_user_preferences(s, pref);
    }
}

void Plugin::game_frame(bool is_simulating) {
    is_peek_message = true;

    for (int i = 0; i < server->clients.count(); ++i) {
        auto cl = server->clients[i];

        if (!cl->is_active())
            continue;

        auto& pd = player_data[i];
        const auto ent = static_cast<mss::CSPlayer*>(base_entity_from_client(cl));

        if (ent->is_alive()) {
            if (pd.new_target && !is_valid_target(server->clients[pd.new_target - 1])) {
                pd.new_target = cycle_target(pd, pd.new_target);

                if (!pd.new_target) {
                    pd.new_target = find_new_target(i, ent->abs_origin);
                    pd.targets = {pd.new_target};
                }
            }
        }
        else
            pd.new_target = 0;

        if (cl->should_send_messages() && (!pd.recreate.first && !pd.recreate.second) && pd.new_target != pd.peek_target) {
            int target_entity;
            mss::ObserverMode mode;

            if (pd.new_target) {
                target_entity = pd.new_target;
                mode = mss::ObserverMode::IN_EYE;

                if (!pd.peek_target) {
                    if (!pd.init_message) {
                        constexpr const char* msg = "[QuickPeek]: Try 'qpeek help' in the console for useful information\n";
                        text_msg_message(mss::SRecipientFilter{i}, static_cast<int>(mss::HudDestination::TALK), msg);

                        pd.init_message = true;
                    }

                    if (pd.smooth) {
                        float desired_lerp = cl->snapshot_interval * smooth_peeking_interp_ratio;

                        // doing ratio interpolation, client looks at unbounded cl_updaterate
                        float update_rate = std::atof(cl->get_user_setting("cl_updaterate"));

                        if (client_lerp(cl, update_rate) < desired_lerp) {
                            char buf[16];
                            std::snprintf(buf, sizeof(buf), "%f", desired_lerp * update_rate);

                            send_interp_cvars(cl, buf, buf);
                        }
                    }
                }
            }
            else {
                target_entity = i + 1;
                mode = mss::ObserverMode::NONE;

                // doing ratio interpolation, client looks at unbounded cl_updaterate
                if (pd.smooth && (client_lerp(cl, std::atof(cl->get_user_setting("cl_updaterate"))) < (cl->snapshot_interval * smooth_peeking_interp_ratio)))
                    send_interp_cvars(cl, sv_client_min_interp_ratio->string, sv_client_max_interp_ratio->string);
            }

            cl->entity_index = target_entity;
            cl->view_entity = engine_server->get_edict_from_index(target_entity);

            // send both reliable and unreliable
            for (int n = 0; n < 2; ++n)
                kill_cam_message(mss::SRecipientFilter{i, static_cast<bool>(n)}, static_cast<int>(mode), target_entity, i + 1);

            pd.recreate.first = target_entity;

            if (pd.peek_target != -1) {
                pd.old_target = pd.peek_target;
                pd.recreate.second = pd.peek_target ? pd.peek_target : i + 1;
            }
            else
                pd.recreate.second = 0;

            pd.recreate.is_swapped = false;

            pd.peek_target = pd.new_target;
            pd.hud_update_time = 0.0f;

            int my_idx = pd.my_target_index();

            if (my_idx != -1 && player_data[my_idx].my_target_index() == i && !player_data[my_idx].peek_target && (pd.new_target == (my_idx + 1) || pd.old_target == (my_idx + 1)))
                player_data[my_idx].hud_update_time = 0.0f;

            auto& target_pd = player_data[target_entity - 1];

            const auto filter = mss::SRecipientFilter{i};

            if (global_vars->current_time < target_pd.text_msg_center.end_time)
                substitute_user_message(filter, text_msg_user_message_id, target_pd.text_msg_center.data, target_pd.text_msg_center.length);
            else {
                auto bf = engine_server->begin_user_message(&filter, text_msg_user_message_id);
                bf->write_byte(static_cast<int>(mss::HudDestination::CENTER));
                bf->write_char('\0');
                engine_server->end_user_message();
            }

            if (global_vars->current_time < target_pd.hint_text.end_time)
                substitute_user_message(filter, hint_text_user_message_id, target_pd.hint_text.data, target_pd.hint_text.length);
            else {
                auto bf = engine_server->begin_user_message(&filter, hint_text_user_message_id);
                bf->write_char('\0');
                engine_server->end_user_message();
            }

            if (global_vars->current_time < target_pd.key_hint_text.end_time)
                substitute_user_message(filter, key_hint_text_user_message_id, target_pd.key_hint_text.data, target_pd.key_hint_text.length);
            else {
                auto bf = engine_server->begin_user_message(&filter, key_hint_text_user_message_id);

                bf->write_byte(1);
                bf->write_char('\0');

                engine_server->end_user_message();
            }

            for (int c = 0; c < mss::max_net_message - 1; ++c) {
                float diff_time = target_pd.hud_msg[c].end_time - global_vars->current_time;

                if (diff_time > 0)
                    substitute_hud_msg_user_message(filter, hud_msg_user_message_id, target_pd.hud_msg[c].data, target_pd.hud_msg[c].length, diff_time);
                else if (global_vars->current_time < pd.hud_msg[c].end_time) {
                    mss::HudTextParms params;
                    params.channel = c;

                    hud_msg_message(filter, params, "");
                }
            }
        }
    }

    for (int i = 0; i < server->clients.count(); ++i) {
        auto cl = server->clients[i];

        if (!cl->is_active() || !cl->should_send_messages())
            continue;

        auto& pd = player_data[i];

        if (pd.hud_update_time <= global_vars->current_time) {
            constexpr mss::Color32 match_color{0,191,255, 255};

            if (pd.peek_target > 0) {
                constexpr mss::Color32 default_color{255, 255, 255, 0};
                constexpr mss::Color32 my_target_color{255, 0, 0, 0};

                mss::Color32 clr{default_color};

                if ((pd.my_target_index() + 1) == pd.peek_target)
                    clr = ((player_data[pd.peek_target - 1].my_target_index()) == i ? match_color : my_target_color);

                mss::HudTextParms params{
                    .x = -1.0f,
                    .y = 0.65f,
                    .effect = 0,
                    .color1 = clr,
                    .color2 = clr,
                    .fade_in_time = 0.0f,
                    .fade_out_time = 0.0f,
                    .hold_time = 1.5f,
                    .fx_time = 0.0f,
                    .channel = 5
                };
                const auto ent = static_cast<mss::BasePlayer*>(base_entity_from_client(server->clients[pd.peek_target - 1]));

                char buf[256];

                if (pd.velocity)
                    std::sprintf(buf, "%s\n%.0f", ent->net_name, ent->abs_velocity.length_2d());
                else
                    std::strcpy(buf, ent->net_name);

                hud_msg_message(mss::SRecipientFilter{i}, params, buf);

                constexpr float velocity_dependent_next_update_time[]{1.0, 0.1};
                pd.hud_update_time = global_vars->current_time + velocity_dependent_next_update_time[pd.velocity];
            }
            else {
                constexpr mss::HudTextParms params{
                    .x = -1.0f,
                    .y = 0.65f,
                    .effect = 0,
                    .color1 = match_color,
                    .color2 = match_color,
                    .fade_in_time = 0.0f,
                    .fade_out_time = 0.0f,
                    .hold_time = 1.5f,
                    .fx_time = 0.0f,
                    .channel = 5
                };

                int my_idx = pd.my_target_index();

                if (my_idx != -1 && (player_data[my_idx].peek_target > 0 && (player_data[my_idx].peek_target == (player_data[my_idx].my_target_index() + 1)))) {
                    const auto ent = static_cast<mss::BasePlayer*>(base_entity_from_client(server->clients[my_idx]));
                    hud_msg_message(mss::SRecipientFilter{i}, params, ent->net_name);

                    pd.hud_update_time = global_vars->current_time + 1.0f;
                }
                else if (pd.hud_update_time == 0.0f) {
                    hud_msg_message(mss::SRecipientFilter{i}, params, "");
                    pd.hud_update_time = std::numeric_limits<float>::max();
                }
            }
        }
    }
    
    is_peek_message = false;
}

void Plugin::level_shutdown() {

}

void Plugin::client_active(mss::Edict* e) {

}

void Plugin::client_disconnect(mss::Edict* e) {
    int index = engine_server->get_index_from_edict(e);

    server->clients[index - 1]->entity_index = index;

    for (int i = 0; i < server->clients.count(); ++i) {
        const auto cl = server->clients[i];

        if (!cl->is_active())
            continue;
        
        auto& pd = player_data[i];

        if (pd.peek_target == index) {
            pd.old_target = index;
            pd.peek_target = -1;

            cl->entity_index = i + 1;
            cl->view_entity = cl->edict;
        }

        if (pd.recreate.first == index) {
            pd.recreate.first = 0;
            pd.recreate.is_swapped = false;
        }
        else if (pd.recreate.second == index) {
            pd.recreate.second = 0;
            pd.recreate.is_swapped = false;
        }
    }
}

void Plugin::client_put_in_server(mss::Edict* e, const char* player_name) {

}

void Plugin::set_command_client(int index) {

}

void Plugin::client_settings_changed(mss::Edict* e) {

}

mss::PluginResult Plugin::client_connect(bool* is_allowed_to_connect, mss::Edict* e, const char* name, const char* address, char* reject, int max_reject_len) {
    int index = engine_server->get_index_from_edict(e) - 1;
    auto& pd = player_data[index];

    pd.targets.clear();
    pd.new_target = 0;
    pd.peek_target = 0;
    pd.old_target = 0;
    pd.recreate.first = 0;
    pd.recreate.second = 0;
    pd.old_buttons = 0;
    pd.start_time = 0.0f;
    pd.hud_update_time = std::numeric_limits<float>::max();

    pd.turning = false;
    pd.smooth = false;
    pd.velocity = false;

    pd.hint_text.end_time = 0.0f;
    pd.key_hint_text.end_time = 0.0f;

    for (int i = 0; i < mss::max_net_message - 1; ++i)
        pd.hud_msg[i].end_time = 0.0f;
        
    return mss::PluginResult::CONTINUE;
}

mss::PluginResult Plugin::client_command(mss::Edict* e, const mss::Command& c) {
    if (std::strcmp(c[0], "+qpeek") == 0) {
        int index = engine_server->get_index_from_edict(e) - 1;
        auto cl = server->clients[index];

        if (!cl->is_active())
            return mss::PluginResult::STOP;

        auto ent = base_entity_from_client(cl);

        if (!ent->is_alive())
            return mss::PluginResult::STOP;

        auto& pd = player_data[index];

        pd.targets.clear();

        if (!pd.peek_target) {
            if (auto idx = pd.my_target_index(); idx != -1 && is_valid_target(server->clients[idx]))
                pd.new_target = idx + 1;
            else if (pd.last_target && is_valid_target(server->clients[pd.last_target - 1]))
                pd.new_target = pd.last_target;
            else
                pd.new_target = find_new_target(index, ent->abs_origin);

            pd.targets = {pd.new_target};
            pd.start_time = global_vars->current_time;
        }
        else {
            pd.new_target = 0;

            if (pd.peek_target > 0)
                pd.last_target = pd.peek_target;
        }

        return mss::PluginResult::STOP;
    }
    else if (std::strcmp(c[0], "-qpeek") == 0) {
        int index = engine_server->get_index_from_edict(e) - 1;
        auto cl = server->clients[index];

        if (!cl->is_active())
            return mss::PluginResult::STOP;

        auto& pd = player_data[index];

        if ((global_vars->current_time - pd.start_time - cl->net_channel->get_avg_latency(static_cast<int>(mss::Flow::OUTGOING))) >= 0.15f)
            pd.new_target = 0;

        if (pd.peek_target > 0)
            pd.last_target = pd.peek_target;
    
        return mss::PluginResult::STOP;
    }
    else if (std::strcmp(c[0], "qpeek") == 0) {
        if (c.count() < 2) {
            engine_server->client_printf(e, "no subcommand specified\ntry 'qpeek help' for more information\n");
            return mss::PluginResult::STOP;
        }

        int index = engine_server->get_index_from_edict(e) - 1;
        auto& pd = player_data[index];

        if (std::strcmp(c[1], "turning") == 0) {
            pd.turning = !pd.turning;

            constexpr const char* print[]{
                "turning is now off\n",
                "turning is now on\n"
            };

            engine_server->client_printf(e, print[pd.turning]);
        }
        else if (std::strcmp(c[1], "smooth") == 0) {
            if (pd.peek_target) {
                engine_server->client_printf(e, "you can't change it while peeking\n");
                return mss::PluginResult::STOP;
            }

            pd.smooth = !pd.smooth;

            constexpr const char* print[]{
                "smooth peeking is now off\n",
                "smooth peeking is now on\n"
            };

            engine_server->client_printf(e, print[pd.smooth]);
        }
        else if (std::strcmp(c[1], "velocity") == 0) {
            pd.velocity = !pd.velocity;

            if (pd.peek_target > 0)
                pd.hud_update_time = 0.0f;

            constexpr const char* print[]{
                "velocity display is now off\n",
                "velocity display is now on\n"
            };

            engine_server->client_printf(e, print[pd.velocity]);
        }
        else if (std::strcmp(c[1], "help") == 0) {
            constexpr const char* print[]{
                "Using this, you can peek at other players while being alive so your gameplay will not be interrupted.\n\n"
                "To start peeking, use '+qpeek'. It is intended to bind it.\n"
                "If you are too fast in releasing the key, this will switch to the toggle mode, where it will wait for the press to turn off.\n"
                "So, if you hold the key for a long time, then releasing it will turn off the peeking, otherwise it will wait for pressing again to turn it off. Sometimes every mode can be useful.\n\n"
                "Control:\n"
                "\t+attack: move to a next player\n"
                "\t+attack2: move to a previous player\n"
                "\t+use: select my target\n\n"
                "With my target selection, you can choose your one favorite target from which the peeking will start.\n"
                "If your target chooses you as well, it will give some convenience in the form of what you can see when this target is peeking at you. Your target will also see when you peek him.\n\n",

                "We can set additional preferences for peeking using 'qpeek {preference}' which are listed below:\n"
                "\tturning: enable rotation of your own player, that is, changing the view angle\n"
                "\tvelocity: display velocity of your current target\n"
                "\tsmooth: ensure smooth peeking (it ensures your interp ratio is 2 at least)\n"
                "By default, all these preferences are disabled.\n\n"
                "While peeking, sometimes you can face annoying weapon history on the right\n"
                "To get rid of that you can try to use client 'hud_drawhistory_time' cvar set to '0'.\n"
            };

            // it will be splitted due to large buffer
            engine_server->client_printf(e, print[0]);
            engine_server->client_printf(e, print[1]);
        }
        else
            engine_server->client_printf(e, "unknown subcommand\ntry 'qpeek help' for more information\n");

        return mss::PluginResult::STOP;
    }

    return mss::PluginResult::CONTINUE;
}

mss::PluginResult Plugin::network_id_validated(const char* user_name, const char* network_id) {
    for (int i = 0; i < server->clients.count(); ++i) {
        const auto cl = server->clients[i];

        if (cl->is_connected() && (std::strcmp(cl->get_network_id_string(), network_id) == 0) && cl->steam_id.all_bits) {
            auto& pd = player_data[i];

            UserPreferences pref{
                .my_target = pd.my_target.id,
                .turning = pd.turning,
                .smooth = pd.smooth,
                .velocity = pd.velocity
            };

            if (pref == default_preferences && load_user_preferences(cl->steam_id.component.account_id, pref)) {
                pd.my_target.id = pref.my_target;
                pd.my_target.is_account_id = pref.my_target != 0;

                pd.turning = pref.turning;
                pd.smooth = pref.smooth;
                pd.velocity = pref.velocity;
            }

            for (int n = 0; n < server->clients.count(); ++n) {
                if (server->clients[n]->is_active() && !player_data[n].my_target.is_account_id && player_data[n].my_target.id == static_cast<unsigned>(cl->user_id)) {
                    player_data[n].my_target.id = cl->steam_id.component.account_id;
                    player_data[n].my_target.is_account_id = true;
                }
            }
        }
    }
    
    return mss::PluginResult::CONTINUE;
}

void Plugin::on_query_cvar_value_finished(int cookie, mss::Edict* e, mss::QueryCvarValueStatus status, const char* cvar_name, const char* cvar_value) {

}

void Plugin::on_edict_allocated(mss::Edict* e) {

}

void Plugin::on_edict_freed(const mss::Edict* e) {

}
