// [DOS2] Divinity: Original Sin 2 - Definitive Edition (Switch, Larian; SDK 9.3.0 sdk build 230994ac): nn:: entry points its main imports
// that neither the TL2 / D3 set nor the DOOM / D2R HLE serve. Found statically (main imports minus every "_Z..." the host takes, minus
// TL2's own imports): without them the native SDK client opens a service session the host does not have (fsp-srv save size / extend,
// the error applet, hid vibration readback, the keyboard layout from settings, ngc, nifm, account profile image).
// The bodies follow nxnative (MIT, https://github.com/seedds/nxnative: dm/host/hle_fs.c GetSaveDataSize / ExtendSaveData,
// hle_hid.c GetKeyCodeMap / GetActualVibrationValue, hle_account.c LoadProfileImage, hle_ldn.c SetLocalNetworkMode).
#ifdef DM_GAME_DOS2
#include "hle.h"
#include <stdint.h>
#include <string.h>
#include <unistd.h>

// ---- fs: the save directory on the host has no size limit
HLE_FORCE("_ZN2nn2fs15GetSaveDataSizeEPlS1_RKNS_7account3UidE", "[DOS2] fsp-srv; save data size: 256 MB data + 256 MB journal",
          Result, h_dos2_SaveSize, int64_t *size, int64_t *journal, const void *uid) {
    (void)uid; if (size) *size = 256 << 20; if (journal) *journal = 256 << 20; return RES_OK;
}
HLE_FORCE("_ZN2nn2fs14ExtendSaveDataERKNS_7account3UidEll", "[DOS2] fsp-srv; extend save data: the host directory has no size limit",
          Result, h_dos2_ExtendSave, const void *uid, int64_t size, int64_t journal) {
    (void)uid; LOG("[dos2] fs extend save data to %lld bytes (journal %lld): nothing to do", (long long)size, (long long)journal); return RES_OK;
}

// ---- err: the error applet; logged, the game continues as after the player closed it
HLE_FORCE("_ZN2nn3err9ShowErrorENS0_9ErrorCodeE", "[DOS2] error applet: logged, closed at once", void, h_dos2_ShowError, uint64_t code) {
    LOG("[dos2] err::ShowError %04u-%04u (category / number, raw %#llx): no error applet on the host",
        (unsigned)(code & 0xffffffff), (unsigned)(code >> 32), (unsigned long long)code);
}

// ---- hid: the last vibration value the device accepted. The host does not read it back from the motor: neutral (amplitudes 0,
// frequencies 160 / 320 Hz, the SDK's VibrationValue::Make() default), as nxnative answers for a handle without a target
typedef struct { float amp_low, freq_low, amp_high, freq_high; } dos2_vib_value;
HLE_FORCE("_ZN2nn3hid23GetActualVibrationValueEPNS0_14VibrationValueERKNS0_21VibrationDeviceHandleE", "[DOS2] vibration: neutral value",
          void, h_dos2_VibActual, dos2_vib_value *v, const uint32_t *h) {
    (void)h; if (v) *v = (dos2_vib_value){0.f, 160.f, 0.f, 320.f};
}

// ---- kpr: the system keyboard layout (settings::detail::GetKeyCodeMap aborts without the settings session); no keyboard on the host:
// the caller's map is left as it made it
HLE_FORCE("_ZN2nn3kpr13GetKeyCodeMapEPNS0_10KeyCodeMapE", "[DOS2] keyboard layout key code map: none (no keyboard)", void, h_dos2_KeyMap, void *map) { (void)map; }

// ---- ngc: the profanity word list is a system data archive; offline there is no text other players see: nothing masked
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPcj", "[DOS2] profanity filter: nothing masked",
          Result, h_dos2_NgcMask, void *self, int *count, char *text, uint32_t pattern) {
    (void)self; (void)text; (void)pattern; if (count) *count = 0; return RES_OK;
}

// ---- nifm: local network mode (only for local wireless play, offline here): remembered, nothing else
static unsigned g_dos2_nifm_local;
HLE_FORCE("_ZN2nn4nifm19SetLocalNetworkModeEb", "[DOS2] nifm local network mode: remembered", void, h_dos2_NifmLocal, unsigned on) {
    g_dos2_nifm_local = on & 1; LOG("[dos2] nifm local network mode %s", g_dos2_nifm_local ? "on" : "off");
}

// ---- account: the host user has no profile image (the game falls back to its own portrait)
HLE_FORCE("_ZN2nn7account16LoadProfileImageEPmPvmRKNS0_3UidE", "[DOS2] user profile image: none (fails)",
          Result, h_dos2_LoadImage, uint64_t *n, void *buf, uint64_t sz, const void *uid) {
    (void)buf; (void)sz; (void)uid; if (n) *n = 0; return RES_FAIL;
}

// ---- socket: the one-argument resolver (docs/dos2/HLE_AUDIT.md). The native sdk body connects to sfdnsres (ConnectToHipcService "sfdnsres" at
// sdk 0xf9a7c), which the host refuses (hle_sf.c: a red line and sm's 0xE15; what the resolver does with that was not traced). The only
// caller (main 0x789338, Larian's network layer) checks for NULL and gives up; hle_net.c answers the three-argument overload and nnsocketGetHostByNameWithOptions the same way.
HLE_FORCE("_ZN2nn6socket16GetHostEntByNameEPKc", "[DOS2] name resolution offline: NULL", void *, h_dos2_HostEnt, const char *name) {
    LOG("[dos2] socket::GetHostEntByName(%s): offline, NULL", name ? name : "(null)"); return NULL;
}

// ---- os: process exit. exit / _Exit / a return from nnMain end in __nnmusl_ProcessExit -> nnosExitProcess (sdk 0xce7b0, reached through
// the sdk PLT) -> ThreadManager::ExitProcess -> svc 0x07, which the host has no case for (placeholder success): the call returns into the `udf`
// after it (nnosExitProcess sdk 0xce7c8) -> SIGILL and a false crash report. DOS2 reaches it from main 0xc03c64 (_Exit after tearing its subsystems down) and 0x7ce8f4
// (exit(-1) when OpenPreselectedUser fails). On a Switch the software closes: here the host's graceful exit (hle_lifecycle.c).
HLE_FORCE("nnosExitProcess", "[DOS2] process exit -> the host's graceful exit (dm_host_shutdown)", void, h_dos2_ExitProcess, void) {
    LOG("[dos2] the game exits (nnosExitProcess): graceful host exit");
    dm_host_shutdown("DOS2 exit");
    for (;;) pause();   // another thread is already shutting down (dm_host_shutdown runs once): wait for its _exit
}
#endif
