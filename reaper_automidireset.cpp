// macOS REAPER extension to reinit MIDI when the configuration changes
//
// 1. Grab the reaper-sdk (https://github.com/justinfrankel/reaper-sdk)
// 2. Grab WDL: git clone https://github.com/justinfrankel/WDL.git
// 3. Put this folder into the 'reaper-plugins' folder of the sdk
// 4. Build then copy or link the binary file into <REAPER resource directory>/UserPlugins
//
// macOS
// =====
//
// clang++ -fPIC -O2 -std=c++14 -I../../WDL/WDL -I../../sdk \
//         -mmacosx-version-min=10.11 -arch x86_64 -arch arm64 \
//         -framework CoreFoundation -framework CoreMIDI \
//         -dynamiclib reaper_automidireset.cpp -o reaper_automidireset.dylib
//
// Linux (not supported)
// =====
//
// c++ -fPIC -O2 -std=c++14 -IWDL/WDL -shared main.cpp -o reaper_barebone.so
//
// Windows (not supported)
// =======
//
// (Use the VS Command Prompt matching your REAPER architecture, eg. x64 to use the 64-bit compiler)
// cl /nologo /O2 /Z7 /Zo /DUNICODE main.cpp /link /DEBUG /OPT:REF /PDBALTPATH:%_PDB% /DLL /OUT:reaper_barebone.dll

#define REAPERAPI_IMPLEMENT
#include "reaper_plugin_functions.h"
#include <cstdio>
#include <CoreMIDI/CoreMIDI.h>

static bool loadAPI(void *(*getFunc)(const char *));
static void notifyProc(const MIDINotification *message, void *refCon);

static MIDIClientRef g_MIDIClient = 0;

extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(
    REAPER_PLUGIN_HINSTANCE instance, reaper_plugin_info_t *rec)
{
  OSStatus err;

  if (!rec) {
    if (g_MIDIClient) {
      MIDIClientDispose(g_MIDIClient);
      g_MIDIClient = 0;
    }
    return 0;
  }
  if (rec->caller_version != REAPER_PLUGIN_VERSION
      || !loadAPI(rec->GetFunc)) 
  {
    return 0;
  }

  // set up MIDI Client for this instance
  err = MIDIClientCreate(CFSTR("reaper_automidireset"), (MIDINotifyProc)notifyProc, NULL, &g_MIDIClient);
  if (err || !g_MIDIClient) {
    return 0;
  }

  // ShowConsoleMsg("Hello World!\n");

  return 1;
}

static void notifyProc(const MIDINotification *message, void *refCon)
{
  if (message && message->messageID == 1) {
    // ShowConsoleMsg("change!\n");
    midi_reinit();
  }
}

#define REQUIRED_API(name) {(void **)&name, #name, true}
#define OPTIONAL_API(name) {(void **)&name, #name, false}

static bool loadAPI(void *(*getFunc)(const char *))
{
  if (!getFunc) {
    return false;
  }

  struct ApiFunc {
    void **ptr;
    const char *name;
    bool required;
  };

  const ApiFunc funcs[] {
      REQUIRED_API(ShowConsoleMsg),
      REQUIRED_API(midi_reinit),
  };

  for (const ApiFunc &func : funcs) {
    *func.ptr = getFunc(func.name);

    if (func.required && !*func.ptr) {
      fprintf(stderr, "[reaper_automidireset] Unable to import the following API function: %s\n", func.name);
      return false;
    }
  }

  return true;
}
