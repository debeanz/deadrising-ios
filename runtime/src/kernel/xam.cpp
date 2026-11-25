#include <stdafx.h>
#include "xam.h"
#include "xdm.h"
#include <dr_shims.h>
#include <cpu/guest_thread.h>
#include <fstream>
#include <mutex>
#include <ranges>
#include <unordered_set>
#include "xxHashMap.h"
#include <hid/input.h>
#include <patches/frame_rate.h>
void SignalEventHandle(uint32_t handle); // imports.cpp
#include <os/logger.h>

struct XamListener : KernelObject
{
    uint32_t id{};
    uint64_t areas{};
    std::vector<std::tuple<uint32_t, uint32_t>> notifications;

    XamListener(const XamListener&) = delete;
    XamListener& operator=(const XamListener&) = delete;

    XamListener();
    ~XamListener();
};

struct XamEnumeratorBase : KernelObject
{
    virtual uint32_t Next(void* buffer)
    {
        return -1;
    }
};

template<typename TIterator = std::vector<XHOSTCONTENT_DATA>::iterator>
struct XamEnumerator : XamEnumeratorBase
{
    uint32_t fetch;
    size_t size;
    TIterator position;
    TIterator begin;
    TIterator end;

    XamEnumerator() = default;
    XamEnumerator(uint32_t fetch, size_t size, TIterator begin, TIterator end) : fetch(fetch), size(size), position(begin), begin(begin), end(end)
    {

    }

    uint32_t Next(void* buffer) override
    {
        if (position == end)
        {
            return -1;
        }

        if (buffer == nullptr)
        {
            for (size_t i = 0; i < fetch; i++)
            {
                if (position == end)
                {
                    return i == 0 ? -1 : i;
                }

                ++position;
            }
        }

        for (size_t i = 0; i < fetch; i++)
        {
            if (position == end)
            {
                return i == 0 ? -1 : i;
            }

            memcpy(buffer, &*position, size);

            ++position;
            buffer = (void*)((size_t)buffer + size);
        }

        return fetch;
    }
};

std::array<xxHashMap<XHOSTCONTENT_DATA>, 3> gContentRegistry{};
std::unordered_set<XamListener*> gListeners{};
xxHashMap<std::string> gRootMap;

std::string_view XamGetRootPath(const std::string_view& root)
{
    const auto result = gRootMap.find(StringHash(root));

    if (result == gRootMap.end())
        return "";

    return result->second;
}

void XamRootCreate(const std::string_view& root, const std::string_view& path)
{
    // A root reopened on another package (the game's "save:") must point at the new folder.
    gRootMap.insert_or_assign(StringHash(root), std::string(path));
}

XamListener::XamListener()
{
    gListeners.insert(this);
}

XamListener::~XamListener()
{
    gListeners.erase(this);
}

XCONTENT_DATA XamMakeContent(uint32_t type, const std::string_view& name)
{
    XCONTENT_DATA data{ 1, type };

    strncpy(data.szFileName, name.data(), sizeof(data.szFileName));

    return data;
}

void XamRegisterContent(const XCONTENT_DATA& data, const std::string_view& root)
{
    const auto idx = data.dwContentType - 1;

    gContentRegistry[idx].emplace(StringHash(data.szFileName), XHOSTCONTENT_DATA{ data }).first->second.szRoot = root;
}

void XamRegisterContent(uint32_t type, const std::string_view name, const std::string_view& root)
{
    XCONTENT_DATA data{ 1, type, {}, "" };

    strncpy(data.szFileName, name.data(), sizeof(data.szFileName));

    XamRegisterContent(data, root);
}

uint32_t XamNotifyCreateListener(uint64_t qwAreas)
{
    auto* listener = CreateKernelObject<XamListener>();

    listener->areas = qwAreas;

    return GetKernelHandle(listener);
}

void XamNotifyEnqueueEvent(uint32_t dwId, uint32_t dwParam)
{
    for (const auto& listener : gListeners)
    {
        if (((1 << MSG_AREA(dwId)) & listener->areas) == 0)
            continue;

        listener->notifications.emplace_back(dwId, dwParam);
    }
}

bool XNotifyGetNext(uint32_t hNotification, uint32_t dwMsgFilter, be<uint32_t>* pdwId, be<uint32_t>* pParam)
{
    auto& listener = *GetKernelObject<XamListener>(hNotification);

    if (dwMsgFilter)
    {
        for (size_t i = 0; i < listener.notifications.size(); i++)
        {
            if (std::get<0>(listener.notifications[i]) == dwMsgFilter)
            {
                if (pdwId)
                    *pdwId = std::get<0>(listener.notifications[i]);

                if (pParam)
                    *pParam = std::get<1>(listener.notifications[i]);

                listener.notifications.erase(listener.notifications.begin() + i);

                return true;
            }
        }
    }
    else
    {
        if (listener.notifications.empty())
            return false;

        if (pdwId)
            *pdwId = std::get<0>(listener.notifications[0]);

        if (pParam)
            *pParam = std::get<1>(listener.notifications[0]);

        listener.notifications.erase(listener.notifications.begin());

        return true;
    }

    return false;
}

uint32_t XamShowMessageBoxUI(uint32_t dwUserIndex, be<uint16_t>* wszTitle, be<uint16_t>* wszText, uint32_t cButtons,
    xpointer<be<uint16_t>>* pwszButtons, uint32_t dwFocusButton, uint32_t dwFlags, be<uint32_t>* pResult, XXOVERLAPPED* pOverlapped)
{
    // Dead Rising: answer with the first button ("Yes" / "OK" / "Continue": the prompts after
    // Start ask to create save data). DR_MSGBOX_BUTTON=<n> picks another one.
    static const int forcedButton = getenv("DR_MSGBOX_BUTTON") ? atoi(getenv("DR_MSGBOX_BUTTON")) : -1;
    *pResult = forcedButton >= 0 && uint32_t(forcedButton) < cButtons ? uint32_t(forcedButton) : 0;
    {
        auto narrow = [](const be<uint16_t>* text)
        {
            std::string out;
            for (size_t i = 0; text != nullptr && i < 400 && text[i] != 0; i++)
                out += char(uint16_t(text[i]) < 128 ? uint16_t(text[i]) : '?');
            return out;
        };
        std::string buttons;
        for (uint32_t i = 0; i < cButtons; i++)
            buttons += (i ? " | " : "") + narrow(pwszButtons[i].get());
        LOGFN_WARNING("[xam] message box \"{}\": \"{}\" [{}] -> button {}", narrow(wszTitle), narrow(wszText), buttons, pResult->get());
    }

#if 0
    assert("XamShowMessageBoxUI encountered!" && false);
#elif _WIN32
    // This code is Win32-only as it'll most likely crash, misbehave or
    // cause corruption due to using a different type of memory than what
    // wchar_t is on Linux. Windows uses 2 bytes while Linux uses 4 bytes.
    std::vector<std::wstring> texts{};

    texts.emplace_back(reinterpret_cast<wchar_t*>(wszTitle));
    texts.emplace_back(reinterpret_cast<wchar_t*>(wszText));

    for (size_t i = 0; i < cButtons; i++)
        texts.emplace_back(reinterpret_cast<wchar_t*>(pwszButtons[i].get()));

    for (auto& text : texts)
    {
        for (size_t i = 0; i < text.size(); i++)
            ByteSwapInplace(text[i]);
    }

    wprintf(L"[XamShowMessageBoxUI] !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    wprintf(L"[XamShowMessageBoxUI] If you are encountering this message and the game has ceased functioning,\n");
    wprintf(L"[XamShowMessageBoxUI] please create an issue at https://github.com/hedge-dev/UnleashedRecomp/issues.\n");
    wprintf(L"[XamShowMessageBoxUI] !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    wprintf(L"[XamShowMessageBoxUI] %ls\n", texts[0].c_str());
    wprintf(L"[XamShowMessageBoxUI] %ls\n", texts[1].c_str());
    wprintf(L"[XamShowMessageBoxUI] ");

    for (size_t i = 0; i < cButtons; i++)
    {
        wprintf(L"%ls", texts[2 + i].c_str());

        if (i != cButtons - 1)
            wprintf(L" | ");
    }

    wprintf(L"\n");
    wprintf(L"[XamShowMessageBoxUI] Defaulted to button: %d\n", pResult->get());
    wprintf(L"[XamShowMessageBoxUI] !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
#endif

    // XN_SYS_UI opened, then closed: Dead Rising takes one notification per frame and waits for
    // "closed" after the overlapped completes.
    XamNotifyEnqueueEvent(9, 1);
    XamNotifyEnqueueEvent(9, 0);

    // Dead Rising waits on the overlapped: complete it (button = the default) and signal its event.
    if (pOverlapped)
    {
        pOverlapped->dwCompletionContext = GuestThread::GetCurrentThreadId();
        pOverlapped->Error = 0;
        pOverlapped->Length = 0;
        if (pOverlapped->hEvent)
            SignalEventHandle(pOverlapped->hEvent);
        return 997; // ERROR_IO_PENDING: the result is in the overlapped
    }

    return 0;
}

// ---- Dead Rising: saves ----
// Each save package (XCONTENTTYPE_SAVEDATA) is a folder _recomp_save/<file name>/ holding the
// files the game writes through its root ("save:"), and <file name>.xcontent beside it keeps the
// package's XCONTENT_DATA (display name), so the saves are found again after a restart (the
// registry was memory only: every save was gone on the next start).
namespace
{
    std::string ContentFolderName(const XCONTENT_DATA& data)
    {
        std::string name(data.szFileName, strnlen(data.szFileName, sizeof(data.szFileName)));
        for (char& c : name)
            if (strchr("<>:\"/\\|?*", c) != nullptr || c < 32)
                c = '_';
        return name.empty() ? std::string("_") : name;
    }

    std::filesystem::path ContentRoot(const XCONTENT_DATA& data)
    {
        if (data.dwContentType == XCONTENTTYPE_SAVEDATA)
            return GetSavePath(true) / ContentFolderName(data);
        if (data.dwContentType == XCONTENTTYPE_DLC)
            return GetGamePath() / "dlc";
        return GetGamePath();
    }

    std::string PathString(const std::filesystem::path& path)
    {
        const auto u8 = path.u8string();
        return std::string(reinterpret_cast<const char*>(u8.c_str()), u8.size());
    }

    void LoadSavedContent()
    {
        static std::once_flag once;
        std::call_once(once, []
        {
            std::error_code ec;
            const auto folder = GetSavePath(true);
            for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
            {
                if (entry.path().extension() != ".xcontent")
                    continue;
                XCONTENT_DATA data{};
                std::ifstream file(entry.path(), std::ios::binary);
                if (!file.read(reinterpret_cast<char*>(&data), sizeof(data)) || data.dwContentType != XCONTENTTYPE_SAVEDATA)
                    continue;
                XamRegisterContent(data, PathString(ContentRoot(data)));
                LOGFN("[save] found save \"{}\"", ContentFolderName(data));
            }
        });
    }

    void WriteContentData(const XCONTENT_DATA& data)
    {
        if (data.dwContentType != XCONTENTTYPE_SAVEDATA)
            return;
        std::ofstream file(GetSavePath(true) / (ContentFolderName(data) + ".xcontent"), std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(&data), sizeof(data));
    }

    void ClearFolder(const std::filesystem::path& folder)
    {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
            std::filesystem::remove_all(entry.path(), ec);
    }

    // Calls with an overlapped report their result there (the game waits on its event).
    uint32_t CompleteContentCall(XXOVERLAPPED* overlapped, uint32_t result, uint32_t length)
    {
        if (overlapped == nullptr)
            return result;
        overlapped->dwCompletionContext = GuestThread::GetCurrentThreadId();
        overlapped->Error = result;
        overlapped->Length = length;
        if (overlapped->hEvent)
            SignalEventHandle(overlapped->hEvent);
        return 997; // ERROR_IO_PENDING: the result is in the overlapped
    }
}

uint32_t XamContentGetDeviceState(uint32_t deviceId, XXOVERLAPPED* overlapped)
{
    return CompleteContentCall(overlapped, ERROR_SUCCESS, 0);
}

uint32_t XamContentGetCreator(uint32_t userIndex, const XCONTENT_DATA* contentData, be<uint32_t>* isCreator, be<uint64_t>* xuid, XXOVERLAPPED* overlapped)
{
    if (isCreator)
        *isCreator = true;
    if (xuid)
        *xuid = 0xB13EBABEBABEBABE;
    return CompleteContentCall(overlapped, ERROR_SUCCESS, 0);
}

uint32_t XamContentCreateEnumerator(uint32_t dwUserIndex, uint32_t DeviceID, uint32_t dwContentType,
    uint32_t dwContentFlags, uint32_t cItem, be<uint32_t>* pcbBuffer, be<uint32_t>* phEnum)
{
    if (dwUserIndex != 0)
    {
        GuestThread::SetLastError(ERROR_NO_SUCH_USER);
        return 0xFFFFFFFF;
    }
    if (dwContentType < 1 || dwContentType > gContentRegistry.size())
        return ERROR_INVALID_PARAMETER;
    LoadSavedContent();
    LOGFN("[save] enumerate content type {} (device {}): {} package(s)", dwContentType, DeviceID, gContentRegistry[dwContentType - 1].size());

    const auto& registry = gContentRegistry[dwContentType - 1];
    const auto& values = registry | std::views::values;
    auto* enumerator = CreateKernelObject<XamEnumerator<decltype(values.begin())>>(cItem, sizeof(_XCONTENT_DATA), values.begin(), values.end());

    if (pcbBuffer)
        *pcbBuffer = sizeof(_XCONTENT_DATA) * cItem;

    *phEnum = GetKernelHandle(enumerator);

    return 0;
}

uint32_t XamEnumerate(uint32_t hEnum, uint32_t dwFlags, void* pvBuffer, uint32_t cbBuffer, be<uint32_t>* pcItemsReturned, XXOVERLAPPED* pOverlapped)
{
    auto* enumerator = GetKernelObject<XamEnumeratorBase>(hEnum);
    const auto count = enumerator->Next(pvBuffer);

    if (count == -1)
        return CompleteContentCall(pOverlapped, ERROR_NO_MORE_FILES, 0);

    if (pcItemsReturned)
        *pcItemsReturned = count;

    return CompleteContentCall(pOverlapped, ERROR_SUCCESS, count);
}

uint32_t XamContentCreateEx(uint32_t dwUserIndex, const char* szRootName, const XCONTENT_DATA* pContentData,
    uint32_t dwContentFlags, be<uint32_t>* pdwDisposition, be<uint32_t>* pdwLicenseMask,
    uint32_t dwFileCacheSize, uint64_t uliContentSize, PXXOVERLAPPED pOverlapped)
{
    const uint32_t type = pContentData->dwContentType;
    if (type < 1 || type > gContentRegistry.size())
        return CompleteContentCall(pOverlapped, ERROR_INVALID_PARAMETER, 0);
    LoadSavedContent();

    auto& registry = gContentRegistry[type - 1];
    const auto key = StringHash(pContentData->szFileName);
    const bool exists = registry.contains(key);
    const uint32_t mode = dwContentFlags & 0xF;
    const std::filesystem::path folder = ContentRoot(*pContentData);

    // Dispositions as on the console (and Xenia): 1 CREATE_NEW, 2 CREATE_ALWAYS (replaces an
    // existing package's files), 3 OPEN_EXISTING, 4 OPEN_ALWAYS, 5 TRUNCATE_EXISTING.
    uint32_t result = ERROR_SUCCESS, disposition = XCONTENT_NEW;
    bool create = false, clear = false;
    switch (mode)
    {
    case CREATE_NEW:
        if (exists)
            result = 183; // ERROR_ALREADY_EXISTS
        else
            create = true;
        break;
    case CREATE_ALWAYS:
        create = !exists;
        clear = exists;
        break;
    case OPEN_EXISTING:
        if (exists)
            disposition = XCONTENT_EXISTING;
        else
            result = ERROR_PATH_NOT_FOUND;
        break;
    case 4: // OPEN_ALWAYS
        if (exists)
            disposition = XCONTENT_EXISTING;
        else
            create = true;
        break;
    case 5: // TRUNCATE_EXISTING
        if (exists)
        {
            disposition = XCONTENT_EXISTING;
            clear = true;
        }
        else
            result = ERROR_PATH_NOT_FOUND;
        break;
    default:
        result = ERROR_INVALID_PARAMETER;
        break;
    }

    if (result == ERROR_SUCCESS)
    {
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        if (clear && type == XCONTENTTYPE_SAVEDATA)
            ClearFolder(folder);
        if (create)
            XamRegisterContent(*pContentData, PathString(folder));
        if (create || clear)
            WriteContentData(*pContentData);
        XamRootCreate(szRootName, exists && !create ? registry.find(key)->second.szRoot : PathString(folder));
    }

    LOGFN("[save] content create \"{}\" type {} root \"{}\" mode {} -> {} (disposition {})", ContentFolderName(*pContentData), type,
        szRootName != nullptr ? szRootName : "", mode, result, disposition);
    if (pdwDisposition)
        *pdwDisposition = disposition;
    if (pdwLicenseMask)
        *pdwLicenseMask = 0;
    return CompleteContentCall(pOverlapped, result, disposition);
}

uint32_t XamContentClose(const char* szRootName, XXOVERLAPPED* pOverlapped)
{
    LOGFN("[save] content close root \"{}\"", szRootName != nullptr ? szRootName : "");
    gRootMap.erase(StringHash(szRootName));
    return CompleteContentCall(pOverlapped, ERROR_SUCCESS, 0);
}

uint32_t XamContentGetDeviceData(uint32_t DeviceID, XDEVICE_DATA* pDeviceData)
{
    pDeviceData->DeviceID = DeviceID;
    pDeviceData->DeviceType = XCONTENTDEVICETYPE_HDD;
    pDeviceData->ulDeviceBytes = 0x10000000;
    pDeviceData->ulDeviceFreeBytes = 0x10000000;
    pDeviceData->wszName[0] = 'H';
    pDeviceData->wszName[1] = 'D';
    pDeviceData->wszName[2] = 'D';
    pDeviceData->wszName[3] = '\0';

    return 0;
}

// ---- Dead Rising: input (hid/input.cpp) ----
// Report one connected controller with nothing pressed so the game never waits on "connect a controller".

// Dead Rising calls it as XInputGetCapabilities(userIndex, flags, caps) (Unleashed's import takes an extra
// first argument).
uint32_t XamInputGetCapabilities(uint32_t userIndex, uint32_t flags, XAMINPUT_CAPABILITIES* caps)
{
    if (userIndex != 0)
        return ERROR_DEVICE_NOT_CONNECTED;

    memset(caps, 0, sizeof(*caps));
    caps->Type = 1;    // XINPUT_DEVTYPE_GAMEPAD
    caps->SubType = 1; // XINPUT_DEVSUBTYPE_GAMEPAD
    return ERROR_SUCCESS;
}

uint32_t XamInputGetState(uint32_t userIndex, uint32_t flags, XAMINPUT_STATE* state)
{
    if (static const bool order = getenv("DR_ORDER_LOG") != nullptr; order)
    {
        static std::atomic<uint32_t> n;
        if (n++ < 40)
            LOGFN("[order] pad poll");
    }
    {
        static std::atomic<uint32_t> calls;
        static uint16_t lastLogged;
        uint32_t packetPeek = 0;
        const uint16_t buttons = Input::GetPad(packetPeek).buttons;
        const uint32_t n = calls++;
        if (n < 4 || n % 2000 == 0 || buttons != lastLogged)
            LOGFN("[input] XamInputGetState #{} user {} flags {:X} buttons {:04X}", n, userIndex, flags, buttons);
        lastLogged = buttons;
    }
    if (userIndex != 0)
        return ERROR_DEVICE_NOT_CONNECTED;

    // SDL controllers + keyboard (hid/input.cpp); the guest structure is big-endian. Above 30 fps the
    // state only changes on UI frames (patches/frame_rate.h): the UI updates every other frame, and a
    // press that first showed on a frame in between would never reach it (the game sees a press for
    // one frame, when the state changes).
    // Only buttons and triggers wait for UI frames: the sticks are read every frame, or Frank's
    // turning and the camera moved in 30 Hz steps at 60 fps.
    static uint32_t packet = 0;
    static Input::Pad pad;
    const Input::Pad fresh = Input::GetPad(packet);
    if (!FrameRate::Above30() || FrameRate::UiFrame())
        pad = fresh;
    else
    {
        pad.thumbLX = fresh.thumbLX;
        pad.thumbLY = fresh.thumbLY;
        pad.thumbRX = fresh.thumbRX;
        pad.thumbRY = fresh.thumbRY;
    }
    state->dwPacketNumber = ByteSwap(packet);
    state->Gamepad.wButtons = ByteSwap(pad.buttons);
    state->Gamepad.bLeftTrigger = pad.leftTrigger;
    state->Gamepad.bRightTrigger = pad.rightTrigger;
    state->Gamepad.sThumbLX = ByteSwap(pad.thumbLX);
    state->Gamepad.sThumbLY = ByteSwap(pad.thumbLY);
    state->Gamepad.sThumbRX = ByteSwap(pad.thumbRX);
    state->Gamepad.sThumbRY = ByteSwap(pad.thumbRY);
    return ERROR_SUCCESS;
}

uint32_t XamInputSetState(uint32_t userIndex, uint32_t flags, XAMINPUT_VIBRATION* vibration)
{
    if (userIndex == 0 && vibration != nullptr)
        Input::SetVibration(ByteSwap(vibration->wLeftMotorSpeed), ByteSwap(vibration->wRightMotorSpeed));
    return userIndex == 0 ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}
