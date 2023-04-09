//
// HPB bot - botman's High Ping Bastard bot
//
// (http://planethalflife.com/botman/)
//
// h_export.cpp
//

#ifndef _WIN32
#include <string.h>
#endif

#include <extdll.h>
#include <dllapi.h>
#include <h_export.h>
#include <meta_api.h>

#include "bot.h"

char g_argv[1024];

DLL_FUNCTIONS gFunctionTable;
enginefuncs_t g_engfuncs;
globalvars_t  *gpGlobals;

static FILE *fp;

#ifndef __linux__

// Required DLL entry point
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
   return TRUE;
}

#endif

void WINAPI GiveFnptrsToDll( enginefuncs_t* pengfuncsFromEngine, globalvars_t *pGlobals )
{
   char game_dir[256];
   char mod_name[32];
   char game_dll_filename[256];

   // get the engine functions from the engine...

   memcpy(&g_engfuncs, pengfuncsFromEngine, sizeof(enginefuncs_t));
   gpGlobals = pGlobals;

   // find the directory name of the currently running MOD...
   GetGameDir (game_dir);

   strcpy(mod_name, game_dir);

   game_dll_filename[0] = 0;

#ifndef __linux__
      strcpy(game_dll_filename, "cstrike\\dlls\\mp.dll");
#else
      strcpy(game_dll_filename, "cstrike/dlls/cs_i386.so");
#endif
}
