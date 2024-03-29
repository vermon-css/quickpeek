# QuickPeek
This plugin allows you to spectate other players while being alive so your gameplay will not be interrupted.  
It can be useful for gamemodes where it's not against the rules to have info on other players' movement.  
It can make it more fun in turn-based gamemodes like trikz: oftentimes your gameplay depends on your partner's actions, so it's not that fun having to just wait for the partner to complete some sub-level in an obscured area where you cannot see their progress - and this is exactly what this plugin is to address, i.e. by enabling you to observe their actions first-person.

[You can take a look at the video to get an idea of the plugin.](https://www.youtube.com/watch?v=ZoUhiFdZ-2g)

In addition, the plugin tries to borrow HUD elements intended for other players you are peeking on. These HUD elements, for example, can be generated by other plugins.

The plugin has been focused on Counter-Strike: Source and Linux target platform.

# Installation
It's a [VSP](https://developer.valvesoftware.com/wiki/Server_plugins) plugin so we have to put all the files in the ``cstrike/addons`` folder.

It is recommended to set high network rates (especially ``sv_minupdaterate`` and ``sv_maxupdaterate`` cvars) to improve the usability of the plugin.


## File structure
* addons/  
    * quickpeek.vdf
    * quickpeek/  
        * main.so
        * user_preferences/ (it will be created automatically, it stores user prefereces per account id)

If we have the *.vdf* file, the plugin will be automatically loaded by the engine. It should look like below.
```
Plugin {
    "file"  "addons/quickpeek/main"
}
```
# Usage
To start peeking, we use ``+qpeek`` console command. It is intended to bind it.

If you are too fast in releasing the command, this will switch to the toggle mode, where it will wait for the press to turn off. So, if you hold the key for a long time, then releasing it will turn off the peeking, otherwise, it will wait for pressing again to turn it off. Sometimes every mode can be useful.

While peeking, you retain the ability to move, you can switch targets using mouse buttons ``(+attack/+attack2)`` and you can see the current target displayed at the bottom half of the screen. It remembers the last used target when peeking again.

You can select your favorite target with the ``+use`` button. With this selection, peeking always will be started from the player. If your target chooses you as well, it will give some convenience in the form of what you can see when this target is peeking at you. Your target will also see when you peek at him.

We can set additional preferences for peeking using ``qpeek`` console command. These are listed below:
* ``qpeek turning`` - enable rotation of your own player, that is, changing the view angle
* ``qpeek velocity`` - display the velocity of your current target
* ``qpeek smooth`` - ensure smooth peeking (it ensures your interp ratio is 2 at least)

By default, all these preferences are disabled.

While peeking, sometimes you can face annoying weapon history on the right
To get rid of that you can try to use client ``hud_drawhistory_time`` cvar set to ``0``.

## Compilation
You can compile it with ``g++`` or ``clang++``.  
There are ``build_gcc.sh`` and ``buld_clang.sh`` respectively.

If you're on Windows, you may use WSL to compile it for Linux.

# For developers
Watching another player is carried out by changing the local player on a client. This is achieved by ``KillCam`` user message. But client logic sets the local player only on player entity creation. Often the player entity has already been created, so simply sending this message will not do anything. Therefore, it must first be deleted, and then created again, and this must be local. This is achieved by replacing the entity's serial number personally for a client in ``CBaseServer::WriteDeltaEntities``, as a result of which a client recreates the entity at once.

The original use of ``KillCam`` was associated with a complete update of information on the client (sort of ``cl_fullupdate``), but this can be an overkill and could lead to player disconnects (e.g., ``Host_Error: CL_CopyExistingEntity``). Now such spectating should be even faster, where only significant entities would be recreated, and more safe, because the client recreates entities in one tick-moment.

## Assumptions
* A player entity should only be deleted when a client disconnects, if for some reason the entity is deleted differently, this will lead to a server crash (e.g. ``ent_remove``).
* Channel 5 of ``HudMsg`` user message is reserved.
* ``ResetHUD`` user message becomes unreliable.
* All memory becomes unprotected.
