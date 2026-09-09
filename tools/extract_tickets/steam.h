// steam.h - minimal Steam client interface subset for extract_tickets.

#pragma once

// Steam-specific scalar types (steamtypes.h, Win32 branch).
typedef unsigned char uint8;
typedef unsigned __int16 uint16;
typedef __int32 int32;
typedef unsigned __int32 uint32;
typedef unsigned __int64 uint64;

typedef int32 HSteamPipe;
typedef int32 HSteamUser;
typedef uint32 AppId_t;
typedef uint32 DepotId_t;
typedef uint64 SteamAPICall_t;

// Steam universes (steamuniverse.h).
enum EUniverse {
    k_EUniverseInvalid = 0,
    k_EUniversePublic = 1,
    k_EUniverseBeta = 2,
    k_EUniverseInternal = 3,
    k_EUniverseDev = 4,
    k_EUniverseMax
};

// EResult subset (steamclientpublic.h); only success is checked by name.
enum EResult {
    k_EResultOK = 1,
};

// Result delivered for ISteamUser::RequestEncryptedAppTicket (isteamuser.h).
// k_iSteamUserCallbacks == 100.
struct EncryptedAppTicketResponse_t {
    enum { k_iCallback = 100 + 54 };
    EResult m_eResult;
};

// Interface versions requested via CreateInterface / GetISteamGenericInterface
inline constexpr const char* kSteamClientInterfaceVersion = "SteamClient023";
inline constexpr const char* kSteamUserInterfaceVersion = "SteamUser023";
inline constexpr const char* kSteamUtilsInterfaceVersion = "SteamUtils010";
inline constexpr const char* kSteamAppTicketInterfaceVersion = "STEAMAPPTICKET_INTERFACE_VERSION001";
inline constexpr const char* kSteamAppsInterfaceVersion = "STEAMAPPS_INTERFACE_VERSION008";

// Interfaces returned by ISteamClient getters we never dereference; declared
// opaque so the vtable slots keep their SDK signatures.
class ISteamGameServer;
class ISteamFriends;
class ISteamMatchmaking;
class ISteamMatchmakingServers;
class ISteamUser;

// isteamutils.h
class ISteamUtils {
public:
    virtual uint32 GetSecondsSinceAppActive() = 0;
    virtual uint32 GetSecondsSinceComputerActive() = 0;
    virtual EUniverse GetConnectedUniverse() = 0;
    virtual uint32 GetServerRealTime() = 0;
    virtual const char* GetIPCountry() = 0;
    virtual bool GetImageSize(int iImage, uint32* pnWidth, uint32* pnHeight) = 0;
    virtual bool GetImageRGBA(int iImage, uint8* pubDest, int nDestBufferSize) = 0;
    virtual bool GetCSERIPPort(uint32* unIP, uint16* usPort) = 0;
    virtual uint8 GetCurrentBatteryPower() = 0;
    virtual uint32 GetAppID() = 0;
    virtual void SetOverlayNotificationPosition(int eNotificationPosition) = 0;
    virtual bool IsAPICallCompleted(SteamAPICall_t hSteamAPICall, bool* pbFailed) = 0;
    virtual int GetAPICallFailureReason(SteamAPICall_t hSteamAPICall) = 0;
    virtual bool GetAPICallResult(SteamAPICall_t hSteamAPICall, void* pCallback, int cubCallback, int iCallbackExpected, bool* pbFailed) = 0;
};

// isteamclient.h
class ISteamClient {
public:
    virtual HSteamPipe CreateSteamPipe() = 0;
    virtual bool BReleaseSteamPipe(HSteamPipe hSteamPipe) = 0;
    virtual HSteamUser ConnectToGlobalUser(HSteamPipe hSteamPipe) = 0;
    virtual HSteamUser CreateLocalUser(HSteamPipe* phSteamPipe, int eAccountType) = 0;
    virtual void ReleaseUser(HSteamPipe hSteamPipe, HSteamUser hUser) = 0;
    virtual ISteamUser* GetISteamUser(HSteamUser hSteamUser, HSteamPipe hSteamPipe, const char* pchVersion) = 0;
    virtual ISteamGameServer* GetISteamGameServer(HSteamUser hSteamUser, HSteamPipe hSteamPipe, const char* pchVersion) = 0;
    virtual void SetLocalIPBinding(const void* unIP, uint16 usPort) = 0;
    virtual ISteamFriends* GetISteamFriends(HSteamUser hSteamUser, HSteamPipe hSteamPipe, const char* pchVersion) = 0;
    virtual ISteamUtils* GetISteamUtils(HSteamPipe hSteamPipe, const char* pchVersion) = 0;
    virtual ISteamMatchmaking* GetISteamMatchmaking(HSteamUser hSteamUser, HSteamPipe hSteamPipe, const char* pchVersion) = 0;
    virtual ISteamMatchmakingServers* GetISteamMatchmakingServers(HSteamUser hSteamUser, HSteamPipe hSteamPipe, const char* pchVersion) = 0;
    virtual void* GetISteamGenericInterface(HSteamUser hSteamUser, HSteamPipe hSteamPipe, const char* pchVersion) = 0;
};

// isteamuser.h
class ISteamUser {
public:
    virtual HSteamUser GetHSteamUser() = 0;
    // In Valve's Steamworks SDK, GetSteamID() returns CSteamID by value. Under the
    // MSVC x64 ABI for member functions, returning a class with user-defined constructors
    // requires the caller to allocate storage and pass a pointer to it in RDX (with 'this'
    // in RCX), returning that pointer in RAX. Declaring it as GetSteamID(uint64* pOutSteamID)
    // guarantees that MSVC x64 puts 'this' in RCX and pOutSteamID in RDX, perfectly matching
    // steamclient64.dll's vtable wrapper and avoiding access violation crashes (0xc0000005).
    virtual uint64* GetSteamID(uint64* pOutSteamID) = 0;
    virtual int InitiateGameConnection_DEPRECATED(void*, int, uint64, uint32, uint16, bool) = 0;
    virtual void TerminateGameConnection_DEPRECATED(uint32, uint16) = 0;
    virtual void TrackAppUsageEvent(uint64, int, const char*) = 0;
    virtual bool GetUserDataFolder(char*, int) = 0;
    virtual void StartVoiceRecording() = 0;
    virtual void StopVoiceRecording() = 0;
    virtual int GetAvailableVoice(uint32*, uint32*, uint32) = 0;
    virtual int GetVoice(bool, void*, uint32, uint32*, bool, void*, uint32, uint32*, uint32) = 0;
    virtual int DecompressVoice(const void*, uint32, void*, uint32, uint32*, uint32) = 0;
    virtual uint32 GetVoiceOptimalSampleRate() = 0;
    virtual uint32 GetAuthSessionTicket(void*, int, uint32*, const void*) = 0;
    virtual uint32 GetAuthTicketForWebApi(const char*) = 0;
    virtual int BeginAuthSession(const void*, int, uint64) = 0;
    virtual void EndAuthSession(uint64) = 0;
    virtual void CancelAuthTicket(uint32) = 0;
    virtual int UserHasLicenseForApp(uint64, AppId_t) = 0;
    virtual bool BIsBehindNAT() = 0;
    virtual void AdvertiseGame(uint64, uint32, uint16) = 0;
    virtual SteamAPICall_t RequestEncryptedAppTicket(void* pDataToInclude, int cbDataToInclude) = 0;
    virtual bool GetEncryptedAppTicket(void* pTicket, int cbMaxTicket, uint32* pcbTicket) = 0;
};

// isteamappticket.h
class ISteamAppTicket
{
public:
    virtual uint32 GetAppOwnershipTicketData( uint32 nAppID, void *pvBuffer, uint32 cbBufferLength, uint32 *piAppId, uint32 *piSteamId, uint32 *piSignature, uint32 *pcbSignature ) = 0;
};

// isteamapps.h
class ISteamApps {
public:
    virtual bool BIsSubscribed() = 0;
    virtual bool BIsLowViolence() = 0;
    virtual bool BIsCybercafe() = 0;
    virtual bool BIsVACBanned() = 0;
    virtual const char* GetCurrentGameLanguage() = 0;
    virtual const char* GetAvailableGameLanguages() = 0;
    virtual bool BIsSubscribedApp(AppId_t appID) = 0;
    virtual bool BIsDlcInstalled(AppId_t appID) = 0;
    virtual uint32 GetEarliestPurchaseUnixTime(AppId_t nAppID) = 0;
    virtual bool BIsSubscribedFromFreeWeekend() = 0;
    virtual int GetDLCCount() = 0;
    virtual bool BGetDLCDataByIndex(int iDLC, AppId_t* pAppID, bool* pbAvailable, char* pchName, int cchNameBufferSize) = 0;
    virtual void InstallDLC(AppId_t nAppID) = 0;
    virtual void UninstallDLC(AppId_t nAppID) = 0;
    virtual void RequestAppProofOfPurchaseKey(AppId_t nAppID) = 0;
    virtual bool GetCurrentBetaName(char* pchName, int cchNameBufferSize) = 0;
    virtual bool MarkContentCorrupt(bool bMissingFilesOnly) = 0;
    virtual uint32 GetInstalledDepots(AppId_t appID, DepotId_t* pvecDepots, uint32 cMaxDepots) = 0;
    virtual uint32 GetAppInstallDir(AppId_t appID, char* pchFolder, uint32 cchFolderBufferSize) = 0;
    virtual bool BIsAppInstalled(AppId_t appID) = 0;
};

typedef void* (*CreateInterfaceFn)(const char* pName, int* pReturnCode);
