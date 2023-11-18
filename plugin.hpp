#pragma once

#include "mini_source_sdk/mini_source_sdk.hpp"

class Plugin : mss::IServerPluginCallbacks {
    virtual bool load(mss::CreateInterface ef, mss::CreateInterface sf);
	virtual void unload();
	virtual void pause();
	virtual void unpause();
	virtual const char* get_plugin_description();
	virtual void level_init(const char* map_name);
	virtual void server_activate(mss::Edict* edict_list, int edict_count, int max_clients);
	virtual void game_frame(bool is_simulating);
	virtual void level_shutdown();
	virtual void client_active(mss::Edict* e);
	virtual void client_disconnect(mss::Edict* e);
	virtual void client_put_in_server(mss::Edict* e, const char* player_name);
	virtual void set_command_client(int index);
	virtual void client_settings_changed(mss::Edict* e);
	virtual mss::PluginResult client_connect(bool* is_allowed_to_connect, mss::Edict* e, const char* name, const char* address, char* reject, int max_reject_len);
	virtual mss::PluginResult client_command(mss::Edict* e, const mss::Command& c);
	virtual mss::PluginResult network_id_validated(const char* user_name, const char* network_id);
	virtual void on_query_cvar_value_finished(int cookie, mss::Edict* e, mss::QueryCvarValueStatus status, const char* cvar_name, const char* cvar_value);
	virtual void on_edict_allocated(mss::Edict* e);
	virtual void on_edict_freed(const mss::Edict* e);
};
