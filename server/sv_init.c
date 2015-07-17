/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "server.h"

server_static_t	svs;				// persistant server info
server_t		sv;					// local server

/*
================
SV_FindIndex

================
*/
int32_t SV_FindIndex (char *name, int32_t start, int32_t max, qboolean create)
{
	int32_t		i;
    hash32_t    hash;
	if (!name || !name[0])
		return 0;

    hash = Q_Hash32(name, strlen(name));
	for (i = 1; i < max && sv.configstrings[start+i][0]; i++)
		if (!Q_HashEquals32(hash, sv.confighashes[start+i]) && !strcmp(sv.configstrings[start+i], name))
			return i;

	if (!create)
		return 0;

	// Knightmare 12/23/2001
	// Output a more useful error message to tell user what overflowed
	// And don't bomb out, either- instead, return last possible index
	if (i == max)
	//	Com_Error (ERR_DROP, "*Index: overflow");
	{
		if (start == CS_MODELS)
			Com_Printf (S_COLOR_YELLOW"Warning: Index overflow for models\n");
		else if (start == CS_SOUNDS)
			Com_Printf (S_COLOR_YELLOW"Warning: Index overflow for sounds\n");
		else if (start == CS_IMAGES)
			Com_Printf (S_COLOR_YELLOW"Warning: Index overflow for images\n");
		return (max-1);	// return the last possible index
	}
	// end Knightmare

	strncpy (sv.configstrings[start+i], name, sizeof(sv.configstrings[i]));
    sv.confighashes[start + i] = Q_Hash32(name, strlen(name));
    
	if (sv.state != ss_loading)
	{	// send the update to everyone
		SZ_Clear (&sv.multicast);
		MSG_WriteChar (&sv.multicast, svc_configstring);
		MSG_WriteShort (&sv.multicast, start+i);
		MSG_WriteString (&sv.multicast, name);
		SV_Multicast (vec3_origin, MULTICAST_ALL_R);
	}

	return i;
}


int32_t SV_ModelIndex (char *name)
{
	return SV_FindIndex (name, CS_MODELS, MAX_MODELS, true);
}

int32_t SV_SoundIndex (char *name)
{
	return SV_FindIndex (name, CS_SOUNDS, MAX_SOUNDS, true);
}

int32_t SV_ImageIndex (char *name)
{
	return SV_FindIndex (name, CS_IMAGES, MAX_IMAGES, true);
}


/*
================
SV_CreateBaseline

Entity baselines are used to compress the update messages
to the clients -- only the fields that differ from the
baseline will be transmitted
================
*/
void SV_CreateBaseline (void)
{
	edict_t			*svent;
	int32_t				entnum;	

	for (entnum = 1; entnum < ge->num_edicts ; entnum++)
	{
		svent = EDICT_NUM(entnum);
		if (!svent->inuse)
			continue;
		if (!svent->s.modelindex && !svent->s.sound && !svent->s.effects)
			continue;
		svent->s.number = entnum;

		//
		// take current state as baseline
		//
		VectorCopy (svent->s.origin, svent->s.old_origin);
		sv.baselines[entnum] = svent->s;
	}
}


/*
=================
SV_CheckForSavegame
=================
*/
void SV_CheckForSavegame (void)
{
	char		name[MAX_OSPATH];
	FILE		*f;
	int32_t			i;

	if (sv_noreload->value)
		return;

	if (Cvar_VariableValue ("deathmatch"))
		return;

	Com_sprintf (name, sizeof(name), "%s/save/current/%s.sav", FS_Gamedir(), sv.name);
	f = fopen (name, "rb");
	if (!f)
		return;		// no savegame

	fclose (f);

	SV_ClearWorld ();

	// get configstrings and areaportals
	SV_ReadLevelFile ();

	if (!sv.loadgame)
	{	// coming back to a level after being in a different
		// level, so run it for ten seconds

		// rlava2 was sending too many lightstyles, and overflowing the
		// reliable data. temporarily changing the server state to loading
		// prevents these from being passed down.
		server_state_t		previousState;		// PGM

		previousState = sv.state;				// PGM
		sv.state = ss_loading;					// PGM
		for (i=0 ; i<100 ; i++)
			ge->RunFrame ();

		sv.state = previousState;				// PGM
	}
}


/*
================
SV_SpawnServer

Change the server to a new map, taking all connected
clients along with it.

================
*/
void SV_SpawnServer (char *server, char *spawnpoint, server_state_t serverstate, qboolean attractloop, qboolean loadgame)
{
	int32_t			i;
	uint32_t	checksum;
	fileHandle_t	f;

	if (attractloop)
		Cvar_Set ("paused", "0");

	Com_Printf ("\n------- Server Initialization -------\n");

	Com_DPrintf ("SpawnServer: %s\n",server);
	if (sv.demofile)
		FS_FCloseFile (sv.demofile);

	svs.spawncount++;		// any partially connected client will be
							// restarted
	sv.state = ss_dead;
	Com_SetServerState (sv.state);

	// wipe the entire per-level structure
	memset (&sv, 0, sizeof(sv));
	svs.realtime = 0;
	sv.loadgame = loadgame;
	sv.attractloop = attractloop;

	// save name for levels that don't set message
	strcpy (sv.configstrings[CS_NAME], server);
    sv.confighashes[CS_NAME] = Q_Hash32(server, strlen(server));
    
	if (Cvar_VariableValue ("deathmatch"))
	{
		sprintf(sv.configstrings[CS_AIRACCEL], "%g", sv_airaccelerate->value);
		pm_airaccelerate = sv_airaccelerate->value;
	}
	else
	{
		strcpy(sv.configstrings[CS_AIRACCEL], "0");
		pm_airaccelerate = 0;
	}
    sv.confighashes[CS_AIRACCEL] = Q_Hash32(sv.configstrings[CS_AIRACCEL], strlen(sv.configstrings[CS_AIRACCEL]));


	SZ_Init (&sv.multicast, sv.multicast_buf, sizeof(sv.multicast_buf));

	strcpy (sv.name, server);

	// leave slots at start for clients only
	for (i=0 ; i<maxclients->value ; i++)
	{
		// needs to reconnect
		if (svs.clients[i].state > cs_connected)
			svs.clients[i].state = cs_connected;
		svs.clients[i].lastframe = -1;
	}

	sv.time = 1000;

    // this has already been done?
    
    
	if (serverstate != ss_game)
	{
		sv.models[1] = CM_LoadMap ("", false, &checksum);	// no real map
	}
	else
	{
		Com_sprintf (sv.configstrings[CS_MODELS+1],sizeof(sv.configstrings[CS_MODELS+1]),
			"maps/%s.bsp", server);
        sv.confighashes[CS_MODELS+1] = Q_Hash32(sv.configstrings[CS_MODELS+1], strlen(sv.configstrings[CS_MODELS+1]));

		// resolve CS_PAKFILE, hack by Jay Dolan
		FS_FOpenFile(sv.configstrings[CS_MODELS + 1], &f, FS_READ);
		strcpy(sv.configstrings[CS_PAKFILE], (last_pk3_name ? last_pk3_name : ""));
		FS_FCloseFile(f);
        sv.confighashes[CS_PAKFILE] = Q_Hash32(sv.configstrings[CS_PAKFILE], strlen(sv.configstrings[CS_PAKFILE]));
		sv.models[1] = CM_LoadMap (sv.configstrings[CS_MODELS+1], false, &checksum);
	}
	Com_sprintf (sv.configstrings[CS_MAPCHECKSUM],sizeof(sv.configstrings[CS_MAPCHECKSUM]),
		"%i", checksum);
    sv.confighashes[CS_MAPCHECKSUM] = Q_Hash32(sv.configstrings[CS_MAPCHECKSUM], strlen(sv.configstrings[CS_MAPCHECKSUM]));
	//
	// clear physics interaction links
	//
	SV_ClearWorld ();
	
	for (i=1 ; i< CM_NumInlineModels() ; i++)
	{
		Com_sprintf (sv.configstrings[CS_MODELS+1+i], sizeof(sv.configstrings[CS_MODELS+1+i]),
			"*%i", i);
        sv.confighashes[CS_MODELS+1+i] = Q_Hash32(sv.configstrings[CS_MODELS+1+i], strlen(sv.configstrings[CS_MODELS+1+i]));
		sv.models[i+1] = CM_InlineModel (sv.configstrings[CS_MODELS+1+i]);
	}

	//
	// spawn the rest of the entities on the map
	//	

	// precache and static commands can be issued during
	// map initialization
	sv.state = ss_loading;
	Com_SetServerState (sv.state);

	// load and spawn all other entities
	ge->SpawnEntities ( sv.name, CM_EntityString(), spawnpoint );

	// run two frames to allow everything to settle
	ge->RunFrame ();
	ge->RunFrame ();

	// all precaches are complete
	sv.state = serverstate;
	Com_SetServerState (sv.state);
	
	// create a baseline for more efficient communications
	SV_CreateBaseline ();

	// check for a savegame
	SV_CheckForSavegame ();

	// set serverinfo variable
	Cvar_FullSet ("mapname", sv.name, CVAR_SERVERINFO | CVAR_NOSET);

	Com_Printf ("-------------------------------------\n");
}

/*
==============
SV_InitGame

A brand new game has been started
==============
*/
void PF_Configstring (int32_t index, const char *val);

void SV_InitGame (void)
{
	int32_t		i;
	edict_t	*ent;
	char	idmaster[32];

	if (svs.initialized)
	{
		// cause any connected clients to reconnect
		SV_Shutdown ("Server restarted\n", true);
	}
	else
	{
		// make sure the client is down
		CL_Drop ();
		SCR_BeginLoadingPlaque ();
	}

	// get any latched variable changes (maxclients, etc)
	Cvar_GetLatchedVars ();

	svs.initialized = true;

	if (Cvar_VariableValue ("coop") && Cvar_VariableValue ("deathmatch"))
	{
		Com_Printf("Deathmatch and Coop both set, disabling Coop\n");
		Cvar_FullSet ("coop", "0",  CVAR_SERVERINFO | CVAR_LATCH);
	}

	// dedicated servers can't be single player and are usually DM
	// so unless they explicity set coop, force it to deathmatch
	if (dedicated->value)
	{
		if (!Cvar_VariableValue ("coop"))
			Cvar_FullSet ("deathmatch", "1",  CVAR_SERVERINFO | CVAR_LATCH);
	}

	// init clients
	if (Cvar_VariableValue ("deathmatch"))
	{
		if (maxclients->value <= 1)
			Cvar_FullSet ("maxclients", "8", CVAR_SERVERINFO | CVAR_LATCH);
		else if (maxclients->value > MAX_CLIENTS)
			Cvar_FullSet ("maxclients", va("%i", MAX_CLIENTS), CVAR_SERVERINFO | CVAR_LATCH);
	}
	else if (Cvar_VariableValue ("coop"))
	{
		if (maxclients->value <= 1 || maxclients->value > 4)
			Cvar_FullSet ("maxclients", "4", CVAR_SERVERINFO | CVAR_LATCH);
	}
	else	// non-deathmatch, non-coop is one player
	{
		Cvar_FullSet ("maxclients", "1", CVAR_SERVERINFO | CVAR_LATCH);
	}

	svs.spawncount = rand();
	svs.clients = (client_t*)Z_TagMalloc (sizeof(client_t)*maxclients->value, TAG_SERVER);
	svs.num_client_entities = maxclients->value*UPDATE_BACKUP*64;
	svs.client_entities = (entity_state_t*)Z_TagMalloc (sizeof(entity_state_t)*svs.num_client_entities, TAG_SERVER);

	// init network stuff
	NET_Config ( (maxclients->value > 1) );

	// heartbeats will always be sent to the id master
	svs.last_heartbeat = -99999;		// send immediately
	Com_sprintf(idmaster, sizeof(idmaster), "192.246.40.37:%i", PORT_MASTER);
	NET_StringToAdr (idmaster, &master_adr[0]);

	// init game
	SV_InitGameProgs ();
	for (i = 0; i < maxclients->value; i++)
	{
		ent = EDICT_NUM(i+1);
		ent->s.number = i+1;
		svs.clients[i].edict = ent;
		memset (&svs.clients[i].lastcmd, 0, sizeof(svs.clients[i].lastcmd));
	}
}


/*
======================
SV_Map

  the full syntax is:

  map [*]<map>$<startspot>+<nextserver>

command from the console or progs.
Map can also be a.cin, .pcx, or .dm2 file
Nextserver is used to allow a cinematic to play, then proceed to
another level:

	map tram.cin+jail_e3
======================
*/
void SV_Map (qboolean attractloop, char *levelstring, qboolean loadgame)
{
	char	level[MAX_QPATH];
	char	*ch;
	int32_t		l;
	char	spawnpoint[MAX_QPATH];

	sv.loadgame = loadgame;
	sv.attractloop = attractloop;

	if (sv.state == ss_dead && !sv.loadgame)
		SV_InitGame ();	// the game is just starting
	memset(level,0,sizeof(level));
	// r1ch fix: buffer overflow
//	strcpy (level, levelstring);
	strncpy (level, levelstring, sizeof(level)-1);

	// if there is a + in the map, set nextserver to the remainder
	ch = strstr(level, "+");
	if (ch)
	{
		*ch = 0;
			Cvar_Set ("nextserver", va("gamemap \"%s\"", ch+1));
	}
	else
		Cvar_Set ("nextserver", "");

	//ZOID special hack for end game screen in coop mode
	if (Cvar_VariableValue ("coop") && !Q_strcasecmp(level, "victory.pcx"))
		Cvar_Set ("nextserver", "gamemap \"*base1\"");

	// if there is a $, use the remainder as a spawnpoint
	ch = strstr(level, "$");
	if (ch)
	{
		*ch = 0;
		strcpy (spawnpoint, ch+1);
	}
	else
		spawnpoint[0] = 0;

	// skip the end-of-unit flag if necessary
	if (level[0] == '*')
		memmove(level, level+1, strlen(level));

	l = strlen(level);
#ifdef	ROQ_SUPPORT
	if (l > 4 && (!strcmp (level+l-4, ".cin") || !strcmp (level+l-4, ".roq")) )
#else
	if (l > 4 && !strcmp (level+l-4, ".cin") )
#endif // ROQ_SUPPORT
	{
		if (!dedicated->value)
			SCR_BeginLoadingPlaque ();			// for local system
		SV_BroadcastCommand ("changing\n");
		SV_SpawnServer (level, spawnpoint, ss_cinematic, attractloop, loadgame);
	}
	else if (l > 4 && !strcmp (level+l-4, ".dm2") )
	{
		if (!dedicated->value)
			SCR_BeginLoadingPlaque ();			// for local system
		SV_BroadcastCommand ("changing\n");
		SV_SpawnServer (level, spawnpoint, ss_demo, attractloop, loadgame);
	}
	else if (l > 4 && !strcmp (level+l-4, ".pcx"))
	{
		if (!dedicated->value)
			SCR_BeginLoadingPlaque ();			// for local system
		SV_BroadcastCommand ("changing\n");
		SV_SpawnServer (level, spawnpoint, ss_pic, attractloop, loadgame);
	}
	else
	{
		if (!dedicated->value)
			SCR_BeginLoadingPlaque ();			// for local system
		SV_BroadcastCommand ("changing\n");
		SV_SendClientMessages ();
		SV_SpawnServer (level, spawnpoint, ss_game, attractloop, loadgame);
		Cbuf_CopyToDefer ();
	}

	SV_BroadcastCommand ("reconnect\n");
}
