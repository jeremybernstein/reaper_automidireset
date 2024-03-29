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
// Windows
// =======
//
// (Use the VS Command Prompt matching your REAPER architecture, eg. x64 to use the 64-bit compiler)
// cl /nologo /O2 /Z7 /Zo /DUNICODE /I..\..\WDL\WDL /I..\..\sdk reaper_automidireset.cpp user32.lib /link /DEBUG /OPT:REF /PDBALTPATH:%_PDB% /DLL /OUT:reaper_automidireset.dll
//
// MinGW64 appears to work, as well:
//  c++ -fPIC -O2 -std=c++14 -DUNICODE -I../../WDL/WDL -I../../sdk -shared reaper_automidireset.cpp -o reaper_automidireset.dll
//
// Linux (not supported)
// =====
//
// c++ -fPIC -O2 -std=c++14 -IWDL/WDL -shared main.cpp -o reaper_barebone.so

#define REAPERAPI_IMPLEMENT
#include "reaper_plugin_functions.h"
#include <cstdio>

#define VERSION_STRING "1.3"

static int commandId = 0;

#ifdef WIN32

// Windows refresh code adapted from http://faudio.github.io/faudio/
#include <windows.h>
#include <dbt.h>
#include <MMSystem.h>
#include <cassert>

/* This is the same as KSCATEGORY_AUDIO */
static const GUID GUID_AUDIO_DEVIFACE = {0x6994AD04L, 0x93EF, 0x11D0, {0xA3, 0xCC, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96}};
static WCHAR WND_CLASS_MIDI_NAME[] = L"midiDummyWindow";
#define kMidiDeviceType ((void *)1)
void CALLBACK ScheduleMidiCheck(HWND hwnd, UINT uMsg, UINT timerId, DWORD dwTime);
bool RegisterDeviceInterfaceToHwnd(HWND hwnd, HDEVNOTIFY *hDeviceNotify);
DWORD WINAPI window_thread(LPVOID params);
HWND hDummyWindow;
#define WM_MIDI_REINIT (WM_USER + 1)
#define WM_MIDI_INIT (WM_USER + 2)

#elif __linux__

#include <algorithm>
#include <thread>
#include <chrono>
#include <libusb.h>

using std::thread;

static libusb_hotplug_callback_handle g_hp[2];
static thread g_usbServiceThread;
static bool g_usbInited = false;
typedef void (*timer_function)();

static bool is_midi_device(libusb_device *dev, struct libusb_device_descriptor *desc);
static int hotplug_callback(libusb_context *ctx, libusb_device *dev, libusb_hotplug_event event, void *user_data);

#else // __APPLE__

#include <CoreMIDI/CoreMIDI.h>
static void notifyProc(const MIDINotification *message, void *refCon);
static MIDIClientRef g_MIDIClient = 0;

#endif

#ifndef WIN32 // __linux__ or __APPLE__

#include <atomic>

using std::atomic;
static atomic<bool> g_eventReceived;
static atomic<bool> g_listsInited;

using namespace std::literals;
static void reaperTimer();

#endif

#include <vector>
std::vector<bool> inputsList;
std::vector<bool> outputsList;

static void initLists();
static void updateLists();
static bool loadAPI(void *(*getFunc)(const char *));
static void registerCustomAction();
static bool showInfo(KbdSectionInfo *sec, int command, int val, int val2, int relmode, HWND hwnd);

extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(
  REAPER_PLUGIN_HINSTANCE instance, reaper_plugin_info_t *rec)
{
#ifdef WIN32

  if (!rec) {
    return 0;
  }
  if (rec->caller_version != REAPER_PLUGIN_VERSION
      || !loadAPI(rec->GetFunc))
  {
    return 0;
  }

  // initLists called in the window_thread on Windows
  HANDLE wt = CreateThread(NULL, 0, window_thread, kMidiDeviceType, 0, 0);
  if (wt == INVALID_HANDLE_VALUE) {
    return 0;
  }
  CloseHandle(wt);

#elif __linux__

  if (!rec) {
    if (g_usbInited) {
      g_usbInited = false;
      plugin_register("-timer", NULL);
      g_usbServiceThread.join();
      libusb_exit(NULL);
    }
    return 0;
  }
  if (rec->caller_version != REAPER_PLUGIN_VERSION
      || !loadAPI(rec->GetFunc))
  {
    return 0;
  }

  g_usbInited = true;
  libusb_init(NULL);

  if (!libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)) {
    ShowConsoleMsg("automidireset: Hotplug not supported by this build of libusb\n");
    libusb_exit (NULL);
    g_usbInited = false;
  }

  int rc;
  if (g_usbInited) {
    rc = libusb_hotplug_register_callback (NULL, LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED, (libusb_hotplug_flag)0, LIBUSB_HOTPLUG_MATCH_ANY,
                                          LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY, hotplug_callback, NULL, &g_hp[0]);
    if (LIBUSB_SUCCESS != rc) {
      ShowConsoleMsg("Error registering callback 0\n");
      libusb_exit (NULL);
      g_usbInited = false;
    }
  }

  if (g_usbInited) {
    rc = libusb_hotplug_register_callback (NULL, LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT, (libusb_hotplug_flag)0, LIBUSB_HOTPLUG_MATCH_ANY,
                                          LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY, hotplug_callback, NULL, &g_hp[1]);
    if (LIBUSB_SUCCESS != rc) {
      ShowConsoleMsg("Error registering callback 1\n");
      libusb_exit (NULL);
      g_usbInited = false;
    }
  }

  if (g_usbInited) {
    g_usbServiceThread = thread([&]() {
      timeval tv;
      while (g_usbInited) {
        tv.tv_sec = 0;
        tv.tv_usec = 500;
        libusb_handle_events_timeout(NULL, &tv);
        std::this_thread::sleep_for(1ms);
      }
    });
  }

#else // __APPLE__

  OSStatus err;

  if (!rec) {
    if (g_MIDIClient) {
      plugin_register("-timer", NULL);
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

#endif

#ifndef WIN32 // __linux__ or __APPLE__

  g_listsInited = false;
  g_eventReceived = false;
  plugin_register("timer", (void *)reaperTimer);

#endif

  registerCustomAction();
  return 1;
}

bool showInfo(KbdSectionInfo *sec, int command, int val, int val2, int relmode, HWND hwnd)
{
  if (command != commandId) return false;

  char infoString[512];
  snprintf(infoString, 512, "automidireset // sockmonkey72\nPlug-and-play MIDI devices\n\nVersion %s\n%s\n\nCopyright (c) 2022 Jeremy Bernstein\njeremy.d.bernstein@googlemail.com%s",
           VERSION_STRING, __DATE__,
           !midi_init ? "\n\nPlease update to REAPER 6.47+ for the most reliable experience." : "");
  ShowConsoleMsg(infoString);
  return true;
}

void registerCustomAction()
{
  custom_action_register_t action {
    0,
    "SM72_AMSINFO",
    "sockmonkey72_automidireset: Plug-and-play MIDI devices",
    nullptr
  };

  commandId = plugin_register("custom_action", &action);
  plugin_register("hookcommand2", (void *)&showInfo);
}

static void initLists()
{
  if (!midi_init) return;

  char portName[512];
  inputsList.clear();
  int numMIDIInputs = GetNumMIDIInputs();
  for (int i = 0; i < numMIDIInputs; i++) {
    portName[0] = '\0';
    bool inputAttached = GetMIDIInputName(i, portName, 512);
    inputsList.push_back(inputAttached);
    // char cslMsg[512];
    // snprintf(cslMsg, 512, "MIDI Init INPUT %d %s (%d)\n", i, portName, inputAttached);
    // ShowConsoleMsg(cslMsg);
  }
  int numMIDIOutputs = GetNumMIDIOutputs();
  for (int i = 0; i < numMIDIOutputs; i++) {
    portName[0] = '\0';
    bool outputAttached = GetMIDIOutputName(i, portName, 512);
    outputsList.push_back(outputAttached);
    // char cslMsg[512];
    // snprintf(cslMsg, 512, "MIDI Init OUTPUT %d %s (%d)\n", i, portName, outputAttached);
    // ShowConsoleMsg(cslMsg);
  }
}

static void updateLists()
{
  if (!midi_init) return;

  int numMIDIInputs = GetNumMIDIInputs();
  for (int i = 0; i < numMIDIInputs; i++) {
    char inputName[512] = "";
    bool inputAttached = GetMIDIInputName(i, inputName, 512);
    if (*inputName && inputsList[i] != inputAttached) {
      // char cslMsg[512];
      // snprintf(cslMsg, 512, "MIDI Init INPUT %d %s (was %d, now %d)\n", i, inputName, inputsList[i] ? 1 : 0, inputAttached);
      // ShowConsoleMsg(cslMsg);
      midi_init(i, -1);
      inputsList[i] = inputAttached;
    }
  }
  int numMIDIOutputs = GetNumMIDIOutputs();
  for (int i = 0; i < numMIDIOutputs; i++) {
    char outputName[512] = "";
    bool outputAttached = GetMIDIOutputName(i, outputName, 512);
    if (*outputName && outputsList[i] != outputAttached) {
      // char cslMsg[512];
      // snprintf(cslMsg, 512, "MIDI Init OUTPUT %d %s (was %d, now %d)\n", i, outputName, outputsList[i] ? 1 : 0, outputAttached);
      // ShowConsoleMsg(cslMsg);
      midi_init(-1, i);
      outputsList[i] = outputAttached;
    }
  }
}

#ifndef WIN32 // __linux__ or __APPLE__

static atomic<bool> g_inDelayTimer;
static std::chrono::time_point<std::chrono::steady_clock> start;

void reaperTimer()
{
  if (!g_listsInited) {
    initLists();
    g_listsInited = true;
  }
  if (g_eventReceived) {
    start = std::chrono::steady_clock::now();
    g_inDelayTimer = true;
    g_eventReceived = false;
  }
  else if (g_inDelayTimer) {
    const auto end = std::chrono::steady_clock::now();
    if (((end - start) / 1ms) > 1500) { // 1.5s delay is safe
      midi_reinit();
      updateLists();
      g_inDelayTimer = false;
    }
  }
}

#endif

#ifdef WIN32

bool RegisterDeviceInterfaceToHwnd(HWND hwnd, HDEVNOTIFY *hDeviceNotify)
{

  DEV_BROADCAST_DEVICEINTERFACE NotificationFilter;

  ZeroMemory(&NotificationFilter, sizeof(NotificationFilter));

  NotificationFilter.dbcc_size = sizeof(DEV_BROADCAST_DEVICEINTERFACE);
  NotificationFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
  NotificationFilter.dbcc_classguid = GUID_AUDIO_DEVIFACE;

  HDEVNOTIFY hDevNotify = RegisterDeviceNotification(hwnd, &NotificationFilter, DEVICE_NOTIFY_WINDOW_HANDLE);

  if (!hDevNotify) {
    return false;
  }

  return true;
}

void CALLBACK ScheduleMidiCheck(HWND hwnd, UINT uMsg, UINT timerId, DWORD dwTime)
{
  PostMessage(hDummyWindow, WM_MIDI_REINIT, 0, 0);
  KillTimer(hwnd, 0);
}

INT_PTR WINAPI midi_hardware_status_callback(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  LRESULT lRet = 0;
  PDEV_BROADCAST_HDR pbdi;
  PDEV_BROADCAST_DEVICEINTERFACE pdi;
  static HDEVNOTIFY hDeviceNotify;

  switch (msg) {

  case WM_MIDI_INIT:
    initLists();
    break;

  case WM_MIDI_REINIT:
    //ShowConsoleMsg("MIDI Reinit\n");
    midi_reinit(); // this looks like overkill, but appears to be necessary on some systems
    updateLists();
    break;

  case WM_CREATE:
    if (!RegisterDeviceInterfaceToHwnd(hwnd, &hDeviceNotify)) {
      assert(false && "failed to register device interface");
    }

    break;

  case WM_CLOSE:
    if (!UnregisterDeviceNotification(hDeviceNotify)) {
      assert(false && "failed to unregister device interface");
    }

    DestroyWindow(hwnd);
    break;

  case WM_DESTROY:
    PostQuitMessage(0);
    break;

  case WM_DEVICECHANGE: {
    switch (wParam) {
    case DBT_DEVICEARRIVAL:

      pbdi = (PDEV_BROADCAST_HDR)lParam;

      if (pbdi->dbch_devicetype != DBT_DEVTYP_DEVICEINTERFACE) {
        break;
      }

      pdi = (PDEV_BROADCAST_DEVICEINTERFACE) pbdi;

      if (!IsEqualGUID(pdi->dbcc_classguid, GUID_AUDIO_DEVIFACE)) {
        break;
      }

      /*
        midiXXXGetNumDevs() / midiXXXGetDevCaps() does not update until after the
        WM_DEVICECHANGE message has been dispatched.

        The timer waits for the device list to be updated and then handles the change.

        It works but it's not pretty.
      */
      SetTimer(hwnd, 0, midi_init ? 1500 : 500, (TIMERPROC)&ScheduleMidiCheck);
      break;

    case DBT_DEVICEREMOVECOMPLETE:
      pbdi = (PDEV_BROADCAST_HDR)lParam;

      if (pbdi->dbch_devicetype != DBT_DEVTYP_DEVICEINTERFACE) {
        break;
      }

      pdi = (PDEV_BROADCAST_DEVICEINTERFACE) pbdi;

      if (!IsEqualGUID(pdi->dbcc_classguid, GUID_AUDIO_DEVIFACE)) {
        break;
      }

      SetTimer(hwnd, 0, midi_init ? 1500 : 500, (TIMERPROC)&ScheduleMidiCheck);
      break;

    case DBT_DEVNODES_CHANGED:
      SetTimer(hwnd, 0, midi_init ? 1500 : 500, (TIMERPROC)&ScheduleMidiCheck);
      break;
    }

    break;
  }

  default:
    return DefWindowProc(hwnd, msg, wParam, lParam);
  }

  return lRet;
}

DWORD WINAPI window_thread(LPVOID params)
{
  /*
  WM_DEVICECHANGE messages are only sent to windows and services.
  A dummy receiving window is therefore created in hidden mode.
  */

  WNDCLASSEX wndClass = {0};
  wndClass.cbSize = sizeof(WNDCLASSEX);
  wndClass.hInstance = (HINSTANCE) GetModuleHandle(NULL);

  if (params == kMidiDeviceType) {
    wndClass.lpfnWndProc = (WNDPROC)midi_hardware_status_callback;
    wndClass.lpszClassName = WND_CLASS_MIDI_NAME;
    ATOM registered = RegisterClassEx(&wndClass);
    if (registered) {
      hDummyWindow = CreateWindow(WND_CLASS_MIDI_NAME, L"midi window", WS_ICONIC,
                                  0, 0, CW_USEDEFAULT, 0, NULL, NULL, wndClass.hInstance, NULL);
      if (hDummyWindow) {
        ShowWindow(hDummyWindow, SW_HIDE);
      }
    }
  }

  PostMessage(hDummyWindow, WM_MIDI_INIT, 0, 0); // call initLists();

  MSG msg;

  while (GetMessage(&msg, NULL, 0, 0) != 0) {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }

  return 0;
}

#elif __linux__

static bool is_midi_device(libusb_device *dev, struct libusb_device_descriptor *desc)
{
  bool rv = false;

  if (desc->bNumConfigurations) {
    struct libusb_config_descriptor *config;

    libusb_device_handle *udev;
    int ret = libusb_open(dev, &udev);
    if (ret) {
      // fprintf(stderr, "Couldn't open device, some information will be missing\n");
      udev = NULL;
    }

    for (int i = 0; i < desc->bNumConfigurations; ++i) {
      ret = libusb_get_config_descriptor(dev, i, &config);
      if (ret) {
        // fprintf(stderr, "Couldn't get configuration descriptor %d, some information will be missing\n", i);
      }
      else {
        for (int j = 0; j < config->bNumInterfaces; ++j) {
          const struct libusb_interface *interface = &config->interface[j];
          for (int k = 0; k < interface->num_altsetting; ++k) {
            const struct libusb_interface_descriptor *altsetting = &interface->altsetting[k];
            if (altsetting->bInterfaceClass == LIBUSB_CLASS_AUDIO
                && altsetting->bInterfaceSubClass == 3)
            {
              rv = true;
              break;
            }
          }
          if (rv) break;
        }
        libusb_free_config_descriptor(config);
        if (rv) break;
      }
    }
    if (udev) libusb_close(udev);
  }
  return rv;
}

static int hotplug_callback(libusb_context *ctx, libusb_device *dev, libusb_hotplug_event event, void *user_data)
{
  struct libusb_device_descriptor desc;
  int rc;

  rc = libusb_get_device_descriptor(dev, &desc);
  if (LIBUSB_SUCCESS != rc) {
    // ShowConsoleMsg("Error getting device descriptor\n");
    return 0;
  }

  if (is_midi_device(dev, &desc)) {
    g_eventReceived = true;
  }

  return 0;
}

#else // __APPLE__

static void notifyProc(const MIDINotification *message, void *refCon)
{
  if (message && message->messageID == 1) {
    g_eventReceived = true;
  }
}

#endif

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
    REQUIRED_API(GetNumMIDIInputs),
    REQUIRED_API(GetNumMIDIOutputs),
    REQUIRED_API(GetMIDIInputName),
    REQUIRED_API(GetMIDIOutputName),
    OPTIONAL_API(midi_init),
    REQUIRED_API(midi_reinit),
    REQUIRED_API(plugin_register),
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
