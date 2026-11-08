#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <gdiplus.h>
#include <uxtheme.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr UINT kStatusMessage = WM_APP + 1;
constexpr UINT kCaptureHotkeyMessage = WM_APP + 2;
constexpr UINT kAutoIntervalEdit = 1105;
constexpr UINT kCalibrationButton = 1001;
constexpr UINT kStartButton = 1002;
constexpr UINT kStopButton = 1003;
constexpr UINT kBrowseButton = 1004;
constexpr UINT kStaminaButton = 1005;
constexpr UINT kAfkButton = 1006;
constexpr UINT kAutoButton = 1007;
constexpr UINT kManualHotkeyButton = 1008;
constexpr UINT kAutoHotkeyButton = 1009;
constexpr UINT kCalibrationHotkeyButton = 1010;
constexpr UINT kNameEdit = 1101;
constexpr UINT kSurnameEdit = 1102;
constexpr UINT kLogEdit = 1103;
constexpr UINT kRestEdit = 1104;
constexpr std::size_t kMaxSamples = 500000;
constexpr unsigned int kKeyW = 0x01;
constexpr unsigned int kKeyA = 0x02;
constexpr unsigned int kKeyS = 0x04;
constexpr unsigned int kKeyD = 0x08;
constexpr unsigned int kKeySpace = 0x10;
constexpr int kHotkeyManual = 1;
constexpr int kHotkeyAuto = 2;
constexpr int kHotkeyCalibration = 3;
constexpr COLORREF kBackground = RGB(12, 12, 15);
constexpr COLORREF kPanel = RGB(22, 22, 27);
constexpr COLORREF kPanelLight = RGB(29, 28, 34);
constexpr COLORREF kHeader = RGB(19, 18, 22);
constexpr COLORREF kAccent = RGB(205, 24, 48);
constexpr COLORREF kText = RGB(243, 240, 242);
constexpr COLORREF kMuted = RGB(158, 151, 158);

using Clock = std::chrono::steady_clock;

struct MouseEvent {
    long long elapsedMs = 0;
    LONG dx = 0;
    LONG dy = 0;
    unsigned int keyMask = 0;
};

std::filesystem::path GetAppDirectory() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        throw std::runtime_error("Не удалось определить папку программы.");
    }
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

std::wstring Lower(std::wstring value) {
    if (!value.empty()) {
        CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
    }
    return value;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}

class SystemRpmApp {
public:
    int Run(HINSTANCE instance) {
        instance_ = instance;
        appDirectory_ = GetAppDirectory();
        Gdiplus::GdiplusStartupInput startupInput;
        if (Gdiplus::GdiplusStartup(
                &gdiplusToken_, &startupInput, nullptr) != Gdiplus::Ok) {
            throw std::runtime_error("Не удалось запустить графическую подсистему.");
        }
        const auto embeddedIcon = static_cast<HICON>(LoadImageW(
            instance_, MAKEINTRESOURCEW(1), IMAGE_ICON, 64, 64, 0));
        if (embeddedIcon) {
            appIcon_ = embeddedIcon;
        }
        const auto avatarPath = appDirectory_ / L"assets"
            / L"SystemRPM_avatar.png";
        if (std::filesystem::exists(avatarPath)) {
            avatar_.reset(Gdiplus::Bitmap::FromFile(
                avatarPath.c_str(), FALSE));
            if (!avatar_ || avatar_->GetLastStatus() != Gdiplus::Ok) {
                throw std::runtime_error("Не удалось загрузить аватар System RPM.");
            }
        } else if (appIcon_) {
            avatar_.reset(Gdiplus::Bitmap::FromHICON(appIcon_));
            if (!avatar_ || avatar_->GetLastStatus() != Gdiplus::Ok) {
                throw std::runtime_error(
                    "Не удалось загрузить встроенный аватар программы.");
            }
        }
        staminaCalibrationPath_ = appDirectory_ / L"stamina_calibration.csv";
        afkCalibrationPath_ = appDirectory_ / L"anti_afk_mouse_calibration.csv";
        settingsPath_ = appDirectory_ / L"stamina_settings.ini";
        LoadSettings();
        LoadCalibration();

        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = instance_;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = nullptr;
        windowClass.lpszClassName = L"SystemRPMWindow";
        windowClass.hIcon = nullptr;
        if (!appIcon_ && avatar_) {
            Gdiplus::Bitmap iconBitmap(64, 64, PixelFormat32bppARGB);
            Gdiplus::Graphics iconGraphics(&iconBitmap);
            iconGraphics.SetInterpolationMode(
                Gdiplus::InterpolationModeHighQualityBicubic);
            iconGraphics.DrawImage(avatar_.get(), 0, 0, 64, 64);
            if (iconBitmap.GetHICON(&appIcon_) == Gdiplus::Ok) {
                appIconOwned_ = true;
            }
        }
        windowClass.hIcon = appIcon_;
        windowClass.hIconSm = appIcon_;
        if (!RegisterClassExW(&windowClass)) {
            throw std::runtime_error("Не удалось создать класс окна.");
        }

        window_ = CreateWindowExW(
            0, windowClass.lpszClassName, L"System RPM 1.6v",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX
                | WS_CLIPCHILDREN,
            CW_USEDEFAULT, CW_USEDEFAULT, 960, 740,
            nullptr, nullptr, instance_, this);
        if (!window_) {
            throw std::runtime_error("Не удалось открыть окно программы.");
        }
        if (appIcon_) {
            SendMessageW(window_, WM_SETICON, ICON_BIG,
                reinterpret_cast<LPARAM>(appIcon_));
            SendMessageW(window_, WM_SETICON, ICON_SMALL,
                reinterpret_cast<LPARAM>(appIcon_));
        }

        RAWINPUTDEVICE mouseDevice{};
        mouseDevice.usUsagePage = 0x01;
        mouseDevice.usUsage = 0x02;
        mouseDevice.dwFlags = RIDEV_INPUTSINK;
        mouseDevice.hwndTarget = window_;
        if (!RegisterRawInputDevices(
                &mouseDevice, 1, sizeof(mouseDevice))) {
            throw std::runtime_error(
                "Не удалось включить точную запись движения мыши.");
        }

        ShowWindow(window_, SW_SHOW);
        UpdateWindow(window_);
        RegisterHotkeys();
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }

    ~SystemRpmApp() {
        EndHotkeyCapture();
        StopCalibration();
        StopPlayback();
        UnregisterHotKey(window_, kHotkeyManual);
        UnregisterHotKey(window_, kHotkeyAuto);
        UnregisterHotKey(window_, kHotkeyCalibration);
        avatar_.reset();
        if (appIcon_) {
            if (appIconOwned_) {
                DestroyIcon(appIcon_);
            }
        }
        if (editBrush_) {
            DeleteObject(editBrush_);
        }
        if (uiFont_) {
            DeleteObject(uiFont_);
        }
        if (titleFont_) {
            DeleteObject(titleFont_);
        }
        if (panelBrush_) {
            DeleteObject(panelBrush_);
        }
        if (headerControlBrush_) {
            DeleteObject(headerControlBrush_);
        }
        if (gdiplusToken_ != 0) {
            Gdiplus::GdiplusShutdown(gdiplusToken_);
        }
    }

private:
    enum class Module {
        Stamina,
        Afk
    };

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    HWND nameEdit_ = nullptr;
    HWND surnameEdit_ = nullptr;
    HWND logEdit_ = nullptr;
    HWND restEdit_ = nullptr;
    HWND autoIntervalEdit_ = nullptr;
    HWND calibrationButton_ = nullptr;
    HWND startButton_ = nullptr;
    HWND autoButton_ = nullptr;
    HWND stopButton_ = nullptr;
    HWND restLabel_ = nullptr;
    HWND autoIntervalLabel_ = nullptr;
    HWND manualHotkeyButton_ = nullptr;
    HWND autoHotkeyButton_ = nullptr;
    HWND calibrationHotkeyButton_ = nullptr;
    HWND statusLabel_ = nullptr;
    HWND staminaControls_[4]{};
    HWND moduleButtons_[2]{};
    HFONT uiFont_ = nullptr;
    HFONT titleFont_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    HBRUSH panelBrush_ = nullptr;
    HBRUSH headerControlBrush_ = nullptr;
    HICON appIcon_ = nullptr;
    bool appIconOwned_ = false;
    ULONG_PTR gdiplusToken_ = 0;
    HHOOK keyboardHook_ = nullptr;
    std::unique_ptr<Gdiplus::Bitmap> avatar_;
    std::filesystem::path appDirectory_;
    std::filesystem::path staminaCalibrationPath_;
    std::filesystem::path afkCalibrationPath_;
    std::filesystem::path settingsPath_;
    Module module_ = Module::Stamina;
    std::vector<MouseEvent> events_;
    bool calibrationLoadError_ = false;
    bool restoringSettings_ = false;
    std::mutex eventsMutex_;
    std::thread calibrationThread_;
    std::thread playbackThread_;
    std::atomic<bool> calibrationActive_{false};
    std::atomic<bool> calibrationRecording_{false};
    std::atomic<bool> playbackActive_{false};
    std::atomic<bool> automaticPlayback_{false};
    std::atomic<bool> stopRequested_{false};
    int manualHotkey_ = VK_F6;
    int autoHotkey_ = VK_F7;
    int calibrationHotkey_ = VK_F8;
    int capturingHotkey_ = 0;
    std::mutex statusMutex_;
    std::wstring statusText_;
    std::mutex calibrationSamplesMutex_;
    std::vector<MouseEvent> calibrationSamples_;
    Clock::time_point calibrationStartedAt_{};
    inline static SystemRpmApp* hotkeyCaptureApp_ = nullptr;

    static LRESULT CALLBACK WindowProc(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        SystemRpmApp* app = reinterpret_cast<SystemRpmApp*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            app = static_cast<SystemRpmApp*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(app));
            app->window_ = window;
        }
        if (!app) {
            return DefWindowProcW(window, message, wParam, lParam);
        }
        return app->HandleMessage(message, wParam, lParam);
    }

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE:
            CreateControls();
            return 0;
        case WM_COMMAND:
            if (HIWORD(wParam) == BN_CLICKED) {
                HandleCommand(LOWORD(wParam));
            } else if (HIWORD(wParam) == EN_CHANGE
                && (LOWORD(wParam) == kNameEdit
                    || LOWORD(wParam) == kSurnameEdit
                    || LOWORD(wParam) == kLogEdit
                    || LOWORD(wParam) == kRestEdit
                    || LOWORD(wParam) == kAutoIntervalEdit)
                && !restoringSettings_) {
                SaveSettings();
            }
            return 0;
        case WM_HOTKEY:
            HandleHotkey(static_cast<int>(wParam));
            return 0;
        case WM_INPUT:
            HandleRawInput(reinterpret_cast<HRAWINPUT>(lParam));
            return DefWindowProcW(window_, message, wParam, lParam);
        case kCaptureHotkeyMessage:
            if (capturingHotkey_ != 0) {
                if (wParam == VK_ESCAPE) {
                    EndHotkeyCapture();
                    UpdateHotkeyLabels();
                    SetStatus(L"Настройка горячей клавиши отменена.");
                } else {
                    ApplyHotkey(
                        capturingHotkey_, static_cast<int>(wParam));
                }
            }
            return 0;
        case WM_PAINT:
            PaintWindow();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_DRAWITEM:
            return DrawButton(reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            HWND control = reinterpret_cast<HWND>(lParam);
            RECT rect{};
            GetWindowRect(control, &rect);
            MapWindowPoints(nullptr, window_,
                reinterpret_cast<POINT*>(&rect), 2);
            const bool inHeader = rect.top < 134;
            HBRUSH brush = inHeader ? headerControlBrush_ : panelBrush_;
            SetTextColor(dc, kText);
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, inHeader ? kHeader : kPanel);
            return reinterpret_cast<LRESULT>(brush);
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, kText);
            SetBkColor(dc, kPanelLight);
            return reinterpret_cast<LRESULT>(editBrush_);
        }
        case kStatusMessage:
            RefreshStatus();
            return 0;
        case WM_CLOSE:
            EndHotkeyCapture();
            StopCalibration();
            StopPlayback();
            SaveSettings();
            DestroyWindow(window_);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window_, message, wParam, lParam);
        }
    }

    HWND AddControl(
        const wchar_t* type, const wchar_t* text, DWORD style,
        int x, int y, int width, int height, int id) {
        HWND control = CreateWindowExW(
            0, type, text, WS_CHILD | WS_VISIBLE | style,
            x, y, width, height, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            instance_, nullptr);
        if (control && std::wstring(type) == L"BUTTON") {
            SetWindowTheme(control, L" ", L" ");
        }
        return control;
    }

    void HandleRawInput(HRAWINPUT inputHandle) {
        if (!calibrationRecording_.load()) {
            return;
        }
        RAWINPUT input{};
        UINT size = sizeof(input);
        if (GetRawInputData(
                inputHandle, RID_INPUT, &input, &size,
                sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1)) {
            SetStatus(L"Не удалось прочитать движение мыши.");
            return;
        }
        if (input.header.dwType != RIM_TYPEMOUSE
            || (input.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0) {
            return;
        }
        const LONG dx = input.data.mouse.lLastX;
        const LONG dy = input.data.mouse.lLastY;
        if (dx == 0 && dy == 0) {
            return;
        }
        AppendCalibrationEvent(
            dx, dy, module_ == Module::Stamina ? ReadKeyMask() : 0);
    }

    void AppendCalibrationEvent(
        LONG dx, LONG dy, unsigned int keyMask) {
        std::lock_guard<std::mutex> lock(calibrationSamplesMutex_);
        if (!calibrationRecording_.load()
            || calibrationSamples_.size() >= kMaxSamples) {
            return;
        }
        const auto elapsed = std::chrono::duration_cast<
            std::chrono::milliseconds>(
                Clock::now() - calibrationStartedAt_).count();
        calibrationSamples_.push_back({elapsed, dx, dy, keyMask});
    }

    static void AddRoundedRect(
        Gdiplus::GraphicsPath& path, float x, float y,
        float width, float height, float radius) {
        const float diameter = radius * 2.0f;
        path.AddArc(x, y, diameter, diameter, 180.0f, 90.0f);
        path.AddArc(x + width - diameter, y, diameter, diameter, 270.0f, 90.0f);
        path.AddArc(x + width - diameter, y + height - diameter,
            diameter, diameter, 0.0f, 90.0f);
        path.AddArc(x, y + height - diameter, diameter, diameter, 90.0f, 90.0f);
        path.CloseFigure();
    }

    void PaintWindow() {
        PAINTSTRUCT paint{};
        HDC target = BeginPaint(window_, &paint);
        RECT client{};
        GetClientRect(window_, &client);
        HDC buffer = CreateCompatibleDC(target);
        HBITMAP bitmap = CreateCompatibleBitmap(
            target, client.right, client.bottom);
        if (!buffer || !bitmap) {
            if (bitmap) {
                DeleteObject(bitmap);
            }
            if (buffer) {
                DeleteDC(buffer);
            }
            HBRUSH background = CreateSolidBrush(kBackground);
            FillRect(target, &client, background);
            DeleteObject(background);
            EndPaint(window_, &paint);
            return;
        }
        HGDIOBJ previousBitmap = SelectObject(buffer, bitmap);
        Gdiplus::Graphics graphics(buffer);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.Clear(Gdiplus::Color(
            255, GetRValue(kBackground), GetGValue(kBackground),
            GetBValue(kBackground)));

        Gdiplus::SolidBrush shadow(Gdiplus::Color(70, 0, 0, 0));
        Gdiplus::SolidBrush headerBrush(Gdiplus::Color(255, 19, 18, 22));
        Gdiplus::SolidBrush panelBrush(Gdiplus::Color(255, 22, 22, 27));
        Gdiplus::SolidBrush accentBrush(Gdiplus::Color(255, 205, 24, 48));
        Gdiplus::Pen outline(Gdiplus::Color(255, 48, 44, 50), 1.0f);
        Gdiplus::Pen divider(Gdiplus::Color(255, 42, 39, 45), 1.0f);
        Gdiplus::GraphicsPath headerShadow;
        AddRoundedRect(headerShadow, 18.0f, 20.0f,
            static_cast<float>(client.right - 36), 104.0f, 18.0f);
        graphics.FillPath(&shadow, &headerShadow);
        Gdiplus::GraphicsPath header;
        AddRoundedRect(header, 18.0f, 16.0f,
            static_cast<float>(client.right - 36), 104.0f, 18.0f);
        graphics.FillPath(&headerBrush, &header);
        graphics.DrawPath(&outline, &header);

        Gdiplus::GraphicsPath panelShadow;
        AddRoundedRect(panelShadow, 18.0f, 138.0f,
            static_cast<float>(client.right - 36),
            static_cast<float>(client.bottom - 154), 18.0f);
        graphics.FillPath(&shadow, &panelShadow);
        Gdiplus::GraphicsPath card;
        AddRoundedRect(card, 18.0f, 134.0f,
            static_cast<float>(client.right - 36),
            static_cast<float>(client.bottom - 154), 18.0f);
        graphics.FillPath(&panelBrush, &card);
        graphics.DrawPath(&outline, &card);

        Gdiplus::GraphicsPath accent;
        AddRoundedRect(accent, 18.0f, 16.0f, 5.0f, 104.0f, 2.5f);
        graphics.FillPath(&accentBrush, &accent);
        graphics.DrawLine(&divider, 44.0f, 146.0f,
            static_cast<float>(client.right - 44), 146.0f);

        const auto statusColor = playbackActive_.load()
            ? Gdiplus::Color(255, 81, 209, 137)
            : calibrationActive_.load()
                ? Gdiplus::Color(255, 242, 178, 76)
                : Gdiplus::Color(255, 141, 137, 147);
        Gdiplus::SolidBrush statusBrush(statusColor);
        graphics.FillEllipse(&statusBrush,
            48.0f, static_cast<float>(client.bottom - 46), 9.0f, 9.0f);

        if (avatar_) {
            Gdiplus::GraphicsPath clip;
            clip.AddEllipse(34.0f, 34.0f, 68.0f, 68.0f);
            graphics.SetClip(&clip);
            graphics.DrawImage(avatar_.get(), 34, 34, 68, 68);
            graphics.ResetClip();
            Gdiplus::Pen avatarBorder(Gdiplus::Color(255, 205, 24, 48), 2.0f);
            graphics.DrawEllipse(&avatarBorder, 34, 34, 68, 68);
        }
        BitBlt(target, 0, 0, client.right, client.bottom,
            buffer, 0, 0, SRCCOPY);
        SelectObject(buffer, previousBitmap);
        DeleteObject(bitmap);
        DeleteDC(buffer);
        EndPaint(window_, &paint);
    }

    LRESULT DrawButton(const DRAWITEMSTRUCT* item) {
        if (!item || item->CtlType != ODT_BUTTON) {
            return FALSE;
        }
        const int id = static_cast<int>(item->CtlID);
        COLORREF fill = kPanelLight;
        COLORREF border = RGB(67, 55, 62);
        COLORREF text = kText;
        if (id == kStaminaButton || id == kAfkButton) {
            const bool active = (module_ == Module::Stamina
                && id == kStaminaButton) || (module_ == Module::Afk
                && id == kAfkButton);
            fill = active ? kAccent : kPanelLight;
            border = active ? RGB(227, 53, 74) : RGB(67, 55, 62);
        } else if (id == kStartButton || id == kAutoButton
            || id == kBrowseButton) {
            fill = kAccent;
            border = RGB(227, 53, 74);
        } else if (id == kManualHotkeyButton
            || id == kAutoHotkeyButton
            || id == kCalibrationHotkeyButton) {
            fill = RGB(31, 29, 34);
            border = RGB(63, 56, 64);
        } else if (id == kStopButton) {
            fill = RGB(48, 43, 49);
        }
        if ((item->itemState & ODS_DISABLED) != 0) {
            fill = RGB(40, 38, 42);
            text = kMuted;
        } else if ((item->itemState & ODS_HOTLIGHT) != 0) {
            fill = RGB(224, 36, 61);
        }
        wchar_t caption[128]{};
        GetWindowTextW(item->hwndItem, caption, 128);
        Gdiplus::Graphics graphics(item->hDC);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetTextRenderingHint(
            Gdiplus::TextRenderingHintAntiAliasGridFit);
        const COLORREF outerFill = id == kStaminaButton
                || id == kAfkButton
            ? kHeader : kPanel;
        HBRUSH outerBrush = CreateSolidBrush(outerFill);
        FillRect(item->hDC, &item->rcItem, outerBrush);
        DeleteObject(outerBrush);
        const Gdiplus::RectF bounds(
            static_cast<Gdiplus::REAL>(item->rcItem.left + 0.5f),
            static_cast<Gdiplus::REAL>(item->rcItem.top + 0.5f),
            static_cast<Gdiplus::REAL>(
                item->rcItem.right - item->rcItem.left - 1),
            static_cast<Gdiplus::REAL>(
                item->rcItem.bottom - item->rcItem.top - 1));
        Gdiplus::GraphicsPath path;
        AddRoundedRect(path, bounds.X, bounds.Y,
            bounds.Width, bounds.Height, 9.0f);
        Gdiplus::SolidBrush backgroundBrush(Gdiplus::Color(
            255, GetRValue(fill), GetGValue(fill), GetBValue(fill)));
        Gdiplus::Pen borderPen(Gdiplus::Color(
            255, GetRValue(border), GetGValue(border), GetBValue(border)),
            1.0f);
        graphics.FillPath(&backgroundBrush, &path);
        graphics.DrawPath(&borderPen, &path);

        Gdiplus::FontFamily family(L"Segoe UI");
        Gdiplus::Font buttonFont(&family, 9.5f, Gdiplus::FontStyleBold,
            Gdiplus::UnitPoint);
        Gdiplus::SolidBrush foregroundBrush(Gdiplus::Color(
            255, GetRValue(text), GetGValue(text), GetBValue(text)));
        Gdiplus::StringFormat format;
        format.SetAlignment(Gdiplus::StringAlignmentCenter);
        format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        graphics.DrawString(caption, -1, &buttonFont, bounds,
            &format, &foregroundBrush);

        if ((item->itemState & ODS_FOCUS) != 0) {
            Gdiplus::Pen focusPen(Gdiplus::Color(210, 235, 65, 82), 1.0f);
            Gdiplus::GraphicsPath focus;
            AddRoundedRect(focus, bounds.X + 4.0f, bounds.Y + 4.0f,
                bounds.Width - 8.0f, bounds.Height - 8.0f, 5.0f);
            graphics.DrawPath(&focusPen, &focus);
        }
        return TRUE;
    }

    void CreateControls() {
        uiFont_ = CreateFontW(
            -16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        titleFont_ = CreateFontW(
            -27, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        editBrush_ = CreateSolidBrush(kPanelLight);
        panelBrush_ = CreateSolidBrush(kPanel);
        headerControlBrush_ = CreateSolidBrush(kHeader);
        auto applyFont = [this](HWND control) {
            SendMessageW(control, WM_SETFONT,
                reinterpret_cast<WPARAM>(uiFont_), TRUE);
        };
        HWND title = AddControl(
            L"STATIC", L"SYSTEM RPM", SS_LEFT,
            124, 32, 400, 38, 0);
        HWND stamina = AddControl(
            L"BUTTON", L"СТАМИНА", BS_OWNERDRAW | BS_NOTIFY,
            670, 48, 120, 46, kStaminaButton);
        HWND afk = AddControl(
            L"BUTTON", L"АНТИ-АФК", BS_OWNERDRAW | BS_NOTIFY,
            802, 48, 120, 46, kAfkButton);
        HWND subtitle = AddControl(
            L"STATIC", L"Версия 1.6  /  АВТОМАТИЗАЦИЯ МАРШРУТА",
            SS_LEFT, 126, 74, 460, 22, 0);
        HWND nameLabel = AddControl(
            L"STATIC", L"Имя персонажа", SS_LEFT,
            46, 160, 360, 24, 0);
        nameEdit_ = AddControl(
            L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL,
            46, 189, 360, 34, kNameEdit);
        HWND surnameLabel = AddControl(
            L"STATIC", L"Фамилия персонажа", SS_LEFT,
            456, 160, 360, 24, 0);
        surnameEdit_ = AddControl(
            L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL,
            456, 189, 360, 34, kSurnameEdit);
        HWND logLabel = AddControl(
            L"STATIC", L"Файл игрового лога", SS_LEFT,
            46, 246, 400, 24, 0);
        logEdit_ = AddControl(
            L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL,
            46, 275, 720, 34, kLogEdit);
        HWND browse = AddControl(
            L"BUTTON", L"ОБЗОР", BS_OWNERDRAW | BS_NOTIFY,
            782, 274, 134, 36, kBrowseButton);
        HWND restLabel = AddControl(
            L"STATIC", L"Отдых после триггера (сек.)", SS_LEFT,
            46, 332, 400, 24, 0);
        restLabel_ = restLabel;
        restEdit_ = AddControl(
            L"EDIT", L"10", WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER,
            46, 361, 150, 34, kRestEdit);
        autoIntervalLabel_ = AddControl(
            L"STATIC", L"Интервал повтора анти-АФК (сек.)", SS_LEFT,
            250, 332, 460, 24, 0);
        autoIntervalEdit_ = AddControl(
            L"EDIT", L"10", WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER,
            250, 361, 150, 34, kAutoIntervalEdit);
        HWND hint = AddControl(
            L"STATIC",
            L"Остановка по сообщению в логе с именем и фамилией персонажа. "
            L"После отдыха маршрут продолжится автоматически.",
            SS_LEFT, 46, 421, 836, 46, 0);
        HWND hotkeyLabel = AddControl(
            L"STATIC", L"ГОРЯЧИЕ КЛАВИШИ  ·  Нажмите кнопку и затем нужную клавишу",
            SS_LEFT, 46, 476, 836, 24, 0);
        manualHotkeyButton_ = AddControl(
            L"BUTTON", L"", BS_OWNERDRAW | BS_NOTIFY,
            46, 503, 240, 40, kManualHotkeyButton);
        autoHotkeyButton_ = AddControl(
            L"BUTTON", L"", BS_OWNERDRAW | BS_NOTIFY,
            336, 503, 240, 40, kAutoHotkeyButton);
        calibrationHotkeyButton_ = AddControl(
            L"BUTTON", L"", BS_OWNERDRAW | BS_NOTIFY,
            626, 503, 240, 40, kCalibrationHotkeyButton);
        calibrationButton_ = AddControl(
            L"BUTTON", L"ЗАПИСАТЬ МАРШРУТ", BS_OWNERDRAW | BS_NOTIFY,
            46, 578, 230, 48, kCalibrationButton);
        startButton_ = AddControl(
            L"BUTTON", L"РУЧНОЙ · 1 РАЗ", BS_OWNERDRAW | BS_NOTIFY,
            290, 578, 170, 48, kStartButton);
        autoButton_ = AddControl(
            L"BUTTON", L"АВТО · ВКЛ / ВЫКЛ", BS_OWNERDRAW | BS_NOTIFY,
            474, 578, 190, 48, kAutoButton);
        stopButton_ = AddControl(
            L"BUTTON", L"ОСТАНОВИТЬ", BS_OWNERDRAW | BS_NOTIFY,
            678, 578, 190, 48, kStopButton);
        statusLabel_ = AddControl(
            L"STATIC", L"Готово", SS_LEFT,
            62, 638, 820, 28, 0);
        HWND helper = AddControl(
            L"STATIC", L"Калибровка начнётся через 5 секунд",
            SS_LEFT, 296, 548, 360, 20, 0);

        staminaControls_[0] = nameLabel;
        staminaControls_[1] = nameEdit_;
        staminaControls_[2] = surnameLabel;
        staminaControls_[3] = surnameEdit_;
        moduleButtons_[0] = stamina;
        moduleButtons_[1] = afk;
        for (HWND control : {title, stamina, afk, subtitle, nameLabel, nameEdit_,
                surnameLabel, surnameEdit_, logLabel, logEdit_, browse,
                restLabel, restEdit_, autoIntervalLabel_, autoIntervalEdit_,
                hint, hotkeyLabel, manualHotkeyButton_,
                autoHotkeyButton_, calibrationHotkeyButton_,
                calibrationButton_, startButton_, autoButton_, stopButton_,
                statusLabel_, helper}) {
            applyFont(control);
        }
        SendMessageW(title, WM_SETFONT,
            reinterpret_cast<WPARAM>(titleFont_), TRUE);
        staminaExtras_ = {
            nameLabel, nameEdit_, surnameLabel, surnameEdit_,
            logLabel, logEdit_, browse, hint};
        calibrationHint_ = helper;
        ShowModuleControls();
        InvalidateRect(window_, nullptr, FALSE);
        RestoreSettingsToControls();
        UpdateHotkeyLabels();
        if (events_.empty()) {
            SetStatus(calibrationLoadError_
                ? L"Файл калибровки повреждён; запишите новую."
                : L"Первый запуск: выполните калибровку маршрута.");
        }
    }

    std::vector<HWND> staminaExtras_;
    HWND calibrationHint_ = nullptr;

    static bool IsSupportedHotkey(int key) {
        return (key >= 'A' && key <= 'Z')
            || (key >= '0' && key <= '9')
            || (key >= VK_F1 && key <= VK_F12)
            || key == VK_SPACE;
    }

    static std::wstring HotkeyName(int key) {
        if (key >= 'A' && key <= 'Z') {
            return std::wstring(1, static_cast<wchar_t>(key));
        }
        if (key >= '0' && key <= '9') {
            return std::wstring(1, static_cast<wchar_t>(key));
        }
        if (key >= VK_F1 && key <= VK_F12) {
            return L"F" + std::to_wstring(key - VK_F1 + 1);
        }
        return key == VK_SPACE ? L"SPACE" : L"?";
    }

    int& HotkeyValue(int id) {
        if (id == kManualHotkeyButton) {
            return manualHotkey_;
        }
        if (id == kAutoHotkeyButton) {
            return autoHotkey_;
        }
        if (id == kCalibrationHotkeyButton) {
            return calibrationHotkey_;
        }
        throw std::invalid_argument("Неизвестный идентификатор горячей клавиши.");
    }

    int HotkeyId(int buttonId) const {
        if (buttonId == kManualHotkeyButton) return kHotkeyManual;
        if (buttonId == kAutoHotkeyButton) return kHotkeyAuto;
        return kHotkeyCalibration;
    }

    void UpdateHotkeyLabels() {
        if (!manualHotkeyButton_) {
            return;
        }
        SetWindowTextW(manualHotkeyButton_,
            (L"РУЧНОЙ ЗАПУСК  ·  " + HotkeyName(manualHotkey_)).c_str());
        SetWindowTextW(autoHotkeyButton_,
            (L"АВТО ВКЛ / ВЫКЛ  ·  " + HotkeyName(autoHotkey_)).c_str());
        SetWindowTextW(calibrationHotkeyButton_,
            (L"КАЛИБРОВКА  ·  " + HotkeyName(calibrationHotkey_)).c_str());
        InvalidateRect(manualHotkeyButton_, nullptr, TRUE);
        InvalidateRect(autoHotkeyButton_, nullptr, TRUE);
        InvalidateRect(calibrationHotkeyButton_, nullptr, TRUE);
    }

    void RegisterHotkeys() {
        const struct HotkeyEntry {
            int id;
            int buttonId;
        } entries[] = {
            {kHotkeyManual, kManualHotkeyButton},
            {kHotkeyAuto, kAutoHotkeyButton},
            {kHotkeyCalibration, kCalibrationHotkeyButton}
        };
        bool adjusted = false;
        for (const auto& entry : entries) {
            int& key = HotkeyValue(entry.buttonId);
            if (RegisterHotKey(
                    window_, entry.id, MOD_NOREPEAT,
                    static_cast<UINT>(key))) {
                continue;
            }
            const int previous = key;
            const int alternatives[] = {
                VK_F9, VK_F10, VK_F11, VK_F12, VK_F5, VK_F4, VK_F3,
                VK_F2, VK_F1, '0', '9', '8', '7', '6', '5', '4'};
            for (const int candidate : alternatives) {
                if (candidate == manualHotkey_ || candidate == autoHotkey_
                    || candidate == calibrationHotkey_) {
                    continue;
                }
                if (RegisterHotKey(
                        window_, entry.id, MOD_NOREPEAT,
                        static_cast<UINT>(candidate))) {
                    key = candidate;
                    adjusted = true;
                    break;
                }
            }
            if (key == previous) {
                SetStatus(L"Не удалось зарегистрировать клавишу "
                    + HotkeyName(key) + L". Нажмите соответствующую "
                      L"кнопку и назначьте другую.");
            }
        }
        UpdateHotkeyLabels();
        if (adjusted) {
            if (!SaveSettings()) {
                return;
            }
            SetStatus(L"Занятая клавиша заменена свободной. "
                L"Назначения можно изменить ниже.");
        }
    }

    void BeginHotkeyCapture(int buttonId) {
        EndHotkeyCapture();
        capturingHotkey_ = buttonId;
        HWND button = GetDlgItem(window_, buttonId);
        SetWindowTextW(button, L"НАЖМИТЕ КЛАВИШУ  ·  ESC — ОТМЕНА");
        SetFocus(button);
        hotkeyCaptureApp_ = this;
        keyboardHook_ = SetWindowsHookExW(
            WH_KEYBOARD_LL, LowLevelKeyboardProc, instance_, 0);
        if (!keyboardHook_) {
            capturingHotkey_ = 0;
            SetStatus(L"Не удалось включить перехват клавиши. Код ошибки: "
                + std::to_wstring(GetLastError()));
            UpdateHotkeyLabels();
            return;
        }
        SetStatus(L"Нажмите F1–F12, A–Z, 0–9 или пробел.");
    }

    static LRESULT CALLBACK LowLevelKeyboardProc(
        int code, WPARAM wParam, LPARAM lParam) {
        SystemRpmApp* app = hotkeyCaptureApp_;
        if (code >= 0 && app && app->capturingHotkey_ != 0) {
            const auto* key = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
            const bool keyDown = wParam == WM_KEYDOWN
                || wParam == WM_SYSKEYDOWN;
            const bool keyUp = wParam == WM_KEYUP
                || wParam == WM_SYSKEYUP;
            const bool supportedKey = IsSupportedHotkey(
                static_cast<int>(key->vkCode)) || key->vkCode == VK_ESCAPE;
            if (keyDown && supportedKey) {
                PostMessageW(app->window_, kCaptureHotkeyMessage,
                    static_cast<WPARAM>(key->vkCode), 0);
            }
            if ((keyDown || keyUp) && supportedKey) {
                return 1;
            }
        }
        return CallNextHookEx(
            app ? app->keyboardHook_ : nullptr, code, wParam, lParam);
    }

    void EndHotkeyCapture() {
        capturingHotkey_ = 0;
        if (keyboardHook_) {
            if (!UnhookWindowsHookEx(keyboardHook_)) {
                SetStatus(L"Не удалось отключить захват клавиши. Код ошибки: "
                    + std::to_wstring(GetLastError()));
            }
            keyboardHook_ = nullptr;
        }
        if (hotkeyCaptureApp_ == this) {
            hotkeyCaptureApp_ = nullptr;
        }
    }

    void ApplyHotkey(int buttonId, int key) {
        if (!IsSupportedHotkey(key)) {
            SetStatus(L"Эта клавиша не поддерживается. Попробуйте F1–F12, "
                L"A–Z, 0–9 или пробел. Esc — отмена.");
            return;
        }
        if ((buttonId != kManualHotkeyButton && key == manualHotkey_)
            || (buttonId != kAutoHotkeyButton && key == autoHotkey_)
            || (buttonId != kCalibrationHotkeyButton
                && key == calibrationHotkey_)) {
            EndHotkeyCapture();
            UpdateHotkeyLabels();
            SetStatus(L"Эта клавиша уже назначена. Нажмите кнопку ещё раз "
                L"и выберите другую.");
            return;
        }
        int& current = HotkeyValue(buttonId);
        const int previous = current;
        const int id = HotkeyId(buttonId);
        UnregisterHotKey(window_, id);
        if (!RegisterHotKey(
                window_, id, MOD_NOREPEAT, static_cast<UINT>(key))) {
            if (!RegisterHotKey(
                    window_, id, MOD_NOREPEAT,
                    static_cast<UINT>(previous))) {
                SetStatus(L"Не удалось назначить клавишу или восстановить "
                    L"предыдущую. Перезапустите программу.");
            } else {
                SetStatus(L"Клавиша занята Windows или другой программой.");
            }
            return;
        }
        current = key;
        EndHotkeyCapture();
        UpdateHotkeyLabels();
        if (!SaveSettings()) {
            return;
        }
        SetStatus(L"Горячая клавиша сохранена.");
    }

    void TriggerManualPlayback() {
        if (playbackActive_.load()) {
            const bool wasAutomatic = automaticPlayback_.load();
            StopPlayback();
            if (wasAutomatic) {
                StartPlayback(false);
            }
        } else {
            StartPlayback(false);
        }
    }

    void ToggleAutomaticPlayback() {
        if (playbackActive_.load()) {
            const bool wasAutomatic = automaticPlayback_.load();
            StopPlayback();
            if (!wasAutomatic) {
                StartPlayback(true);
            }
        } else {
            StartPlayback(true);
        }
    }

    void HandleHotkey(int id) {
        if (capturingHotkey_ != 0) {
            if (id == kHotkeyManual) {
                ApplyHotkey(capturingHotkey_, manualHotkey_);
            } else if (id == kHotkeyAuto) {
                ApplyHotkey(capturingHotkey_, autoHotkey_);
            } else if (id == kHotkeyCalibration) {
                ApplyHotkey(capturingHotkey_, calibrationHotkey_);
            }
            return;
        }
        if (id == kHotkeyManual) {
            TriggerManualPlayback();
        } else if (id == kHotkeyAuto) {
            ToggleAutomaticPlayback();
        } else if (id == kHotkeyCalibration) {
            if (calibrationActive_.load()) {
                StopCalibration();
            } else {
                StartCalibration();
            }
        }
    }

    void ShowModuleControls() {
        const bool staminaMode = module_ == Module::Stamina;
        for (HWND control : staminaControls_) {
            ShowWindow(control, staminaMode ? SW_SHOW : SW_HIDE);
        }
        for (HWND control : staminaExtras_) {
            ShowWindow(control, staminaMode ? SW_SHOW : SW_HIDE);
        }
        SetWindowTextW(restLabel_,
            L"Отдых по стамине (сек.)");
        ShowWindow(restLabel_, staminaMode ? SW_SHOW : SW_HIDE);
        ShowWindow(restEdit_, staminaMode ? SW_SHOW : SW_HIDE);
        SetWindowTextW(autoIntervalLabel_,
            L"Интервал повтора анти-АФК (сек.)");
        ShowWindow(autoIntervalLabel_, staminaMode ? SW_HIDE : SW_SHOW);
        ShowWindow(autoIntervalEdit_, staminaMode ? SW_HIDE : SW_SHOW);
        SetWindowTextW(calibrationHint_, staminaMode
            ? L"Запись: W A S D  +  ПРОБЕЛ  +  МЫШЬ"
            : L"Горячие клавиши работают поверх игрового окна");
        SetWindowTextW(calibrationButton_, staminaMode
            ? L"Калибровка маршрута" : L"Калибровка мыши");
        SetWindowTextW(startButton_, L"РУЧНОЙ · 1 РАЗ");
        for (HWND button : moduleButtons_) {
            InvalidateRect(button, nullptr, TRUE);
        }
        RefreshStatus();
    }

    std::wstring GetText(HWND control) const {
        const int length = GetWindowTextLengthW(control);
        std::wstring text(static_cast<std::size_t>(std::max(0, length)) + 1, L'\0');
        GetWindowTextW(control, text.data(), length + 1);
        text.resize(static_cast<std::size_t>(std::max(0, length)));
        return text;
    }

    void HandleCommand(int id) {
        if (id == kStaminaButton || id == kAfkButton) {
            if (playbackActive_.load() || calibrationActive_.load()) {
                MessageBoxW(window_,
                    L"Остановите текущую операцию перед сменой режима.",
                    L"System RPM", MB_OK | MB_ICONINFORMATION);
                return;
            }
            module_ = id == kStaminaButton ? Module::Stamina : Module::Afk;
            LoadCalibration();
            ShowModuleControls();
            if (events_.empty()) {
                SetStatus(calibrationLoadError_
                    ? L"Файл калибровки повреждён; запишите новую."
                    : L"Для этого режима ещё нет калибровки.");
            }
        } else if (id == kCalibrationButton) {
            if (calibrationActive_.load()) {
                StopCalibration();
            } else {
                StartCalibration();
            }
        } else if (id == kStartButton) {
            TriggerManualPlayback();
        } else if (id == kAutoButton) {
            ToggleAutomaticPlayback();
        } else if (id == kStopButton) {
            StopPlayback();
        } else if (id == kBrowseButton) {
            BrowseForLog();
        } else if (id == kManualHotkeyButton
            || id == kAutoHotkeyButton
            || id == kCalibrationHotkeyButton) {
            BeginHotkeyCapture(id);
        }
    }

    std::filesystem::path CalibrationPath() const {
        return module_ == Module::Stamina
            ? staminaCalibrationPath_ : afkCalibrationPath_;
    }

    void LoadCalibration() {
        calibrationLoadError_ = false;
        std::ifstream file(CalibrationPath(), std::ios::binary);
        if (!file) {
            events_.clear();
            std::error_code error;
            calibrationLoadError_ =
                std::filesystem::exists(CalibrationPath(), error) || error;
            return;
        }
        std::string signature;
        unsigned int version = 0;
        std::size_t count = 0;
        if (!(file >> signature >> version >> count)
            || signature != (module_ == Module::Stamina
                ? "STAMINA" : "AAFMOUSE")
            || version != 1 || count > kMaxSamples) {
            events_.clear();
            calibrationLoadError_ = true;
            return;
        }
        std::string line;
        std::getline(file, line);
        std::vector<MouseEvent> loaded;
        loaded.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            MouseEvent event{};
            if (!std::getline(file, line)) {
                events_.clear();
                calibrationLoadError_ = true;
                return;
            }
            std::istringstream row(line);
            if (!(row >> event.elapsedMs >> event.dx >> event.dy)
                || (module_ == Module::Stamina
                    && !(row >> event.keyMask))
                || event.elapsedMs < 0
                || event.keyMask > 0x1F
                || (!loaded.empty()
                    && event.elapsedMs < loaded.back().elapsedMs)) {
                events_.clear();
                calibrationLoadError_ = true;
                return;
            }
            loaded.push_back(event);
        }
        if (module_ == Module::Stamina && loaded.empty()) {
            events_.clear();
            calibrationLoadError_ = true;
            return;
        }
        events_ = std::move(loaded);
    }

    void SaveCalibration(const std::vector<MouseEvent>& events) {
        const auto path = CalibrationPath();
        std::ofstream file(path, std::ios::trunc);
        if (!file) {
            throw std::runtime_error("Не удалось открыть файл калибровки.");
        }
        file << (module_ == Module::Stamina ? "STAMINA" : "AAFMOUSE")
             << " 1\n" << events.size() << '\n';
        for (const auto& event : events) {
            file << event.elapsedMs << ' ' << event.dx << ' '
                 << event.dy << ' ' << event.keyMask << '\n';
        }
        if (!file) {
            throw std::runtime_error("Не удалось записать калибровку.");
        }
        calibrationLoadError_ = false;
    }

    static unsigned int ReadKeyMask() {
        unsigned int mask = 0;
        if (GetAsyncKeyState('W') & 0x8000) mask |= kKeyW;
        if (GetAsyncKeyState('A') & 0x8000) mask |= kKeyA;
        if (GetAsyncKeyState('S') & 0x8000) mask |= kKeyS;
        if (GetAsyncKeyState('D') & 0x8000) mask |= kKeyD;
        if (GetAsyncKeyState(VK_SPACE) & 0x8000) mask |= kKeySpace;
        return mask;
    }

    struct MinecraftWindowSearch {
        HWND ignoredWindow;
        HWND result;
    };

    static BOOL CALLBACK FindMinecraftWindow(HWND window, LPARAM parameter) {
        auto* search = reinterpret_cast<MinecraftWindowSearch*>(parameter);
        if (window == search->ignoredWindow || !IsWindowVisible(window)) {
            return TRUE;
        }
        wchar_t title[512]{};
        const int length = GetWindowTextW(window, title, 512);
        if (length > 0
            && Lower(std::wstring(title, static_cast<std::size_t>(length)))
                .find(L"minecraft") != std::wstring::npos) {
            search->result = window;
            return FALSE;
        }
        return TRUE;
    }

    bool FocusMinecraftWindow() const {
        MinecraftWindowSearch search{window_, nullptr};
        EnumWindows(FindMinecraftWindow,
            reinterpret_cast<LPARAM>(&search));
        if (!search.result) {
            return false;
        }
        ShowWindow(search.result, SW_RESTORE);
        return SetForegroundWindow(search.result) != 0
            || GetForegroundWindow() == search.result;
    }

    void StartCalibration() {
        if (playbackActive_.load()) {
            SetStatus(L"Сначала остановите воспроизведение.");
            return;
        }
        if (calibrationThread_.joinable()) {
            calibrationThread_.join();
        }
        calibrationActive_.store(true);
        SetStatus(L"Калибровка начнётся через 5 секунд. Переключаюсь в Minecraft...");
        calibrationThread_ = std::thread([this] {
            try {
                const bool foundMinecraft = FocusMinecraftWindow();
                for (int seconds = 5; seconds > 0; --seconds) {
                    if (!calibrationActive_.load()) {
                        SetStatus(L"Калибровка отменена.");
                        return;
                    }
                    SetStatus(L"Калибровка начнётся через "
                        + std::to_wstring(seconds) + L" сек. "
                        + (foundMinecraft
                            ? L"Minecraft активирован."
                            : L"Переключитесь в Minecraft."));
                    std::this_thread::sleep_for(
                        std::chrono::seconds(1));
                }
                if (!calibrationActive_.load()) {
                    SetStatus(L"Калибровка отменена.");
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(
                        calibrationSamplesMutex_);
                    calibrationSamples_.clear();
                    calibrationStartedAt_ = Clock::now();
                }
                calibrationRecording_.store(true);
                SetStatus(module_ == Module::Stamina
                    ? L"Идёт запись: WASD, пробел и движения мыши. "
                      L"Нажмите «Остановить калибровку» для сохранения."
                    : L"Идёт запись движений мыши. "
                      L"Нажмите «Остановить калибровку» для сохранения.");
                unsigned int previousKeys = 0;
                while (calibrationActive_.load()
                    && calibrationRecording_.load()) {
                    const unsigned int keys = module_ == Module::Stamina
                        ? ReadKeyMask() : 0;
                    if (keys != previousKeys) {
                        AppendCalibrationEvent(0, 0, keys);
                    }
                    previousKeys = keys;
                    {
                        std::lock_guard<std::mutex> lock(
                            calibrationSamplesMutex_);
                        if (calibrationSamples_.size() >= kMaxSamples) {
                            calibrationRecording_.store(false);
                            break;
                        }
                    }
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(2));
                }
                calibrationRecording_.store(false);
                std::vector<MouseEvent> captured;
                {
                    std::lock_guard<std::mutex> lock(
                        calibrationSamplesMutex_);
                    captured = calibrationSamples_;
                }
                const bool sampleLimitReached =
                    captured.size() == kMaxSamples;
                if (captured.empty()) {
                    SetStatus(L"Движения мыши или клавиш не записаны.");
                } else {
                    SaveCalibration(captured);
                    {
                        std::lock_guard<std::mutex> lock(eventsMutex_);
                        events_ = std::move(captured);
                    }
                    SetStatus(sampleLimitReached
                        ? L"Достигнут лимит записи; калибровка сохранена."
                        : L"Калибровка сохранена.");
                }
                calibrationActive_.store(false);
            } catch (const std::exception& error) {
                calibrationActive_.store(false);
                SetStatus(Utf8ToWide(error.what()));
            }
            if (window_) {
                PostMessageW(window_, kStatusMessage, 0, 0);
            }
        });
    }

    void StopCalibration() {
        calibrationActive_.store(false);
        calibrationRecording_.store(false);
        if (calibrationThread_.joinable()) {
            calibrationThread_.join();
        }
        if (calibrationButton_) {
            SetWindowTextW(calibrationButton_, module_ == Module::Stamina
                ? L"Калибровка маршрута" : L"Калибровка мыши");
        }
    }

    static void ReleaseInputs() {
        INPUT inputs[5]{};
        const WORD keys[] = {'W', 'A', 'S', 'D', VK_SPACE};
        for (std::size_t index = 0; index < 5; ++index) {
            inputs[index].type = INPUT_KEYBOARD;
            inputs[index].ki.wVk = keys[index];
            inputs[index].ki.dwFlags = KEYEVENTF_KEYUP;
        }
        if (SendInput(5, inputs, sizeof(INPUT)) != 5) {
            throw std::runtime_error(
                "Не удалось отпустить клавиши WASD и пробел.");
        }
    }

    static void SetKeys(unsigned int previous, unsigned int next) {
        const WORD keys[] = {'W', 'A', 'S', 'D', VK_SPACE};
        const unsigned int masks[] = {
            kKeyW, kKeyA, kKeyS, kKeyD, kKeySpace};
        INPUT inputs[5]{};
        UINT count = 0;
        for (std::size_t index = 0; index < 5; ++index) {
            if ((previous & masks[index]) != (next & masks[index])) {
                inputs[count].type = INPUT_KEYBOARD;
                inputs[count].ki.wVk = keys[index];
                if ((next & masks[index]) == 0) {
                    inputs[count].ki.dwFlags = KEYEVENTF_KEYUP;
                }
                ++count;
            }
        }
        if (count != 0 && SendInput(count, inputs, sizeof(INPUT)) != count) {
            throw std::runtime_error("Не удалось отправить клавиатурный ввод.");
        }
    }

    bool IsStopped() const {
        return stopRequested_.load();
    }

    bool WaitUntil(Clock::time_point deadline, bool stopForRest = false) {
        while (!IsStopped()
            && !(stopForRest && restRequested_.load())
            && Clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return IsStopped() || (stopForRest && restRequested_.load());
    }

    bool PlayEventsOnce(
        const std::vector<MouseEvent>& events, bool stopForRest = false) {
        unsigned int previousKeys = 0;
        const auto startedAt = Clock::now();
        for (const auto& event : events) {
            if (WaitUntil(
                    startedAt + std::chrono::milliseconds(event.elapsedMs),
                    stopForRest)) {
                SetKeys(previousKeys, 0);
                return false;
            }
            if (event.dx != 0 || event.dy != 0) {
                INPUT input{};
                input.type = INPUT_MOUSE;
                input.mi.dx = event.dx;
                input.mi.dy = event.dy;
                input.mi.dwFlags = MOUSEEVENTF_MOVE;
                if (SendInput(1, &input, sizeof(input)) != 1) {
                    throw std::runtime_error("Не удалось переместить мышь.");
                }
            }
            SetKeys(previousKeys, event.keyMask);
            previousKeys = event.keyMask;
        }
        SetKeys(previousKeys, 0);
        return true;
    }

    bool ReadTrigger(
        std::ifstream& log, const std::filesystem::path& logPath,
        std::uintmax_t& offset, std::string& pending,
        const std::wstring& firstName, const std::wstring& surname) {
        std::error_code error;
        const auto size = std::filesystem::file_size(logPath, error);
        if (error) {
            throw std::runtime_error("Не удалось прочитать указанный файл лога.");
        }
        if (size < offset) {
            offset = 0;
            pending.clear();
        }
        log.clear();
        log.seekg(static_cast<std::streamoff>(offset));
        const auto bytesToRead = std::min<std::uintmax_t>(
            size - offset, 64 * 1024);
        std::string added(static_cast<std::size_t>(bytesToRead), '\0');
        if (!added.empty()) {
            log.read(added.data(), static_cast<std::streamsize>(added.size()));
            const auto bytesRead = static_cast<std::uintmax_t>(log.gcount());
            offset += bytesRead;
            added.resize(static_cast<std::size_t>(bytesRead));
            pending += added;
        }

        const auto matches = [&](const std::string& rawLine) {
            const std::wstring line = Lower(Utf8ToWide(rawLine));
            return !firstName.empty() && !surname.empty()
                && line.find(firstName) != std::wstring::npos
                && line.find(surname) != std::wstring::npos
                && line.find(L"останов") != std::wstring::npos
                && line.find(L"отдыш") != std::wstring::npos;
        };
        bool triggered = false;
        std::size_t lineStart = 0;
        for (std::size_t index = 0; index < pending.size(); ++index) {
            if (pending[index] == '\n') {
                triggered = triggered || matches(
                    pending.substr(lineStart, index - lineStart));
                lineStart = index + 1;
            }
        }
        if (lineStart != pending.size()) {
            triggered = triggered || matches(pending.substr(lineStart));
        }
        if (lineStart > 0) {
            pending.erase(0, lineStart);
        }
        return triggered;
    }

    std::atomic<bool> restRequested_{false};

    void RunStamina(
        const std::vector<MouseEvent>& events,
        const std::filesystem::path& logPath,
        const std::wstring& firstName, const std::wstring& surname,
        unsigned int restSeconds) {
        std::ifstream log(logPath, std::ios::binary);
        if (!log) {
            throw std::runtime_error("Не удалось открыть файл игрового лога.");
        }
        std::error_code error;
        std::uintmax_t offset = std::filesystem::file_size(logPath, error);
        if (error) {
            throw std::runtime_error("Не удалось определить размер файла лога.");
        }
        std::string pending;
        while (!IsStopped()) {
            restRequested_.store(false);
            std::atomic<bool> monitorRunning{true};
            std::string monitorError;
            std::thread logMonitor([&] {
                try {
                    while (monitorRunning.load() && !IsStopped()) {
                        if (ReadTrigger(
                                log, logPath, offset, pending,
                                firstName, surname)) {
                            pending.clear();
                            restRequested_.store(true);
                            return;
                        }
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(40));
                    }
                } catch (const std::exception& error) {
                    monitorError = error.what();
                    monitorRunning.store(false);
                    stopRequested_.store(true);
                }
            });
            try {
                while (!IsStopped() && !restRequested_.load()) {
                    if (!PlayEventsOnce(events, true)
                        && !restRequested_.load()) {
                        break;
                    }
                }
            } catch (...) {
                monitorRunning.store(false);
                logMonitor.join();
                throw;
            }
            monitorRunning.store(false);
            logMonitor.join();
            if (!monitorError.empty()) {
                throw std::runtime_error(monitorError);
            }
            if (IsStopped()) {
                break;
            }
            if (!restRequested_.load()) {
                break;
            }
            ReleaseInputs();
            SetStatus(L"Триггер для " + firstName + L" " + surname
                + L" найден — отдых.");
            const auto restUntil = Clock::now()
                + std::chrono::seconds(restSeconds);
            if (WaitUntil(restUntil)) {
                break;
            }
            SetStatus(L"Отдых завершён — бег продолжается.");
        }
        ReleaseInputs();
    }

    void StartPlayback(bool automatic) {
        if (calibrationActive_.load()) {
            SetStatus(L"Сначала завершите калибровку.");
            return;
        }
        if (playbackActive_.load()) {
            SetStatus(L"Воспроизведение уже запущено.");
            return;
        }
        std::vector<MouseEvent> events;
        {
            std::lock_guard<std::mutex> lock(eventsMutex_);
            events = events_;
        }
        if (events.empty()) {
            SetStatus(L"Сначала выполните калибровку.");
            return;
        }

        std::filesystem::path logPath;
        std::wstring firstName;
        std::wstring surname;
        unsigned int intervalSeconds = 10;
        unsigned int restSeconds = 10;
        const Module playbackModule = module_;
        if (automatic) {
            const std::wstring intervalText = GetText(autoIntervalEdit_);
            try {
                std::size_t parsedLength = 0;
                const unsigned long parsed = std::stoul(
                    intervalText, &parsedLength);
                if (parsedLength != intervalText.size()
                    || parsed == 0 || parsed > 86400) {
                    throw std::out_of_range("interval");
                }
                intervalSeconds = static_cast<unsigned int>(parsed);
            } catch (const std::exception&) {
                SetStatus(L"Интервал должен быть от 1 до 86400 секунд.");
                return;
            }
        }
        if (automatic && playbackModule == Module::Stamina) {
            firstName = Lower(GetText(nameEdit_));
            surname = Lower(GetText(surnameEdit_));
            logPath = GetText(logEdit_);
            if (firstName.empty() || surname.empty()) {
                SetStatus(L"Укажите имя и фамилию персонажа.");
                return;
            }
            const std::wstring restText = GetText(restEdit_);
            try {
                std::size_t parsedLength = 0;
                const unsigned long parsed = std::stoul(
                    restText, &parsedLength);
                if (parsedLength != restText.size()
                    || parsed == 0 || parsed > 86400) {
                    throw std::out_of_range("rest");
                }
                restSeconds = static_cast<unsigned int>(parsed);
            } catch (const std::exception&) {
                SetStatus(L"Время отдыха должно быть от 1 до 86400 секунд.");
                return;
            }
            std::error_code logError;
            if (logPath.empty()
                || !std::filesystem::is_regular_file(logPath, logError)) {
                SetStatus(L"Выберите существующий файл игрового лога.");
                return;
            }
            SaveSettings();
        }
        if (playbackThread_.joinable()) {
            playbackThread_.join();
        }
        stopRequested_.store(false);
        playbackActive_.store(true);
        automaticPlayback_.store(automatic);
        SetStatus(automatic && playbackModule == Module::Stamina
            ? L"Авто-стамина включена. Ожидание сообщения в логе."
            : automatic
                ? L"Автоматическое воспроизведение включено."
                : L"Ручное воспроизведение запущено.");
        playbackThread_ = std::thread(
            [this, events = std::move(events), logPath, firstName, surname,
                intervalSeconds, restSeconds, automatic, playbackModule] {
                try {
                    if (playbackModule == Module::Stamina && automatic) {
                        RunStamina(events, logPath,
                            firstName, surname, restSeconds);
                    } else if (automatic) {
                        while (!IsStopped()) {
                            if (!PlayEventsOnce(events)) {
                                break;
                            }
                            if (WaitUntil(Clock::now()
                                + std::chrono::seconds(intervalSeconds))) {
                                break;
                            }
                        }
                    } else {
                        PlayEventsOnce(events);
                    }
                    ReleaseInputs();
                    playbackActive_.store(false);
                    automaticPlayback_.store(false);
                    if (!IsStopped()) {
                        SetStatus(L"Воспроизведение завершено.");
                    } else {
                        SetStatus(L"Остановлено.");
                    }
                } catch (const std::exception& error) {
                    playbackActive_.store(false);
                    automaticPlayback_.store(false);
                    std::wstring message = L"Ошибка: "
                        + Utf8ToWide(error.what());
                    try {
                        ReleaseInputs();
                    } catch (const std::exception& releaseError) {
                        message += L" Ошибка отпускания клавиш: "
                            + Utf8ToWide(releaseError.what());
                    }
                    SetStatus(std::move(message));
                }
                if (window_) {
                    PostMessageW(window_, kStatusMessage, 0, 0);
                }
            });
    }

    void StopPlayback() {
        stopRequested_.store(true);
        if (playbackThread_.joinable()) {
            playbackThread_.join();
        }
        playbackActive_.store(false);
        automaticPlayback_.store(false);
        if (window_) {
            SetStatus(L"Остановлено.");
        }
    }

    void BrowseForLog() {
        wchar_t path[MAX_PATH]{};
        const std::wstring current = GetText(logEdit_);
        if (current.size() < MAX_PATH) {
            std::copy(current.begin(), current.end(), path);
        }
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = window_;
        dialog.lpstrFilter = L"Файлы логов (*.log;*.txt)\0*.log;*.txt\0Все файлы (*.*)\0*.*\0";
        dialog.lpstrFile = path;
        dialog.nMaxFile = MAX_PATH;
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST
            | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&dialog)) {
            SetWindowTextW(logEdit_, path);
            SaveSettings();
        } else if (CommDlgExtendedError() != 0) {
            SetStatus(L"Не удалось открыть диалог выбора файла.");
        }
    }

    void LoadSettings() {
        std::ifstream file(settingsPath_, std::ios::binary);
        if (!file) {
            defaultLogPath_ = appDirectory_ / L"latest.log";
            return;
        }
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(file, line)) {
            lines.push_back(line);
        }
        if (lines.size() < 4) {
            SetStatus(L"Файл настроек неполный; будут использованы значения по умолчанию.");
            defaultLogPath_ = appDirectory_ / L"latest.log";
            return;
        }
        savedFirstName_ = Utf8ToWide(lines[0]);
        savedSurname_ = Utf8ToWide(lines[1]);
        savedLogPath_ = Utf8ToWide(lines[2]);
        savedRestSeconds_ = Utf8ToWide(lines[3]);
        const bool hasSeparateInterval = lines.size() >= 8;
        savedAutoIntervalSeconds_ = hasSeparateInterval
            ? Utf8ToWide(lines[4]) : L"10";
        const std::size_t hotkeyOffset = hasSeparateInterval ? 5 : 4;
        const std::string manualKey = lines.size() > hotkeyOffset
            ? lines[hotkeyOffset] : "";
        const std::string autoKey = lines.size() > hotkeyOffset + 1
            ? lines[hotkeyOffset + 1] : "";
        const std::string calibrationKey = lines.size() > hotkeyOffset + 2
            ? lines[hotkeyOffset + 2] : "";
        const auto readNumber = [](const std::string& value,
            unsigned long minimum, unsigned long maximum) {
            try {
                std::size_t parsedLength = 0;
                const unsigned long parsed = std::stoul(value, &parsedLength);
                return parsedLength == value.size()
                    && parsed >= minimum && parsed <= maximum
                    ? static_cast<int>(parsed) : 0;
            } catch (const std::exception&) {
                return 0;
            }
        };
        const int manual = readNumber(manualKey, 1, 255);
        const int automatic = readNumber(autoKey, 1, 255);
        const int calibration = readNumber(calibrationKey, 1, 255);
        if (IsSupportedHotkey(manual)) manualHotkey_ = manual;
        if (IsSupportedHotkey(automatic)) autoHotkey_ = automatic;
        if (IsSupportedHotkey(calibration)) {
            calibrationHotkey_ = calibration;
        }
        defaultLogPath_ = appDirectory_ / L"latest.log";
    }

    bool SaveSettings() {
        if (!nameEdit_ || !surnameEdit_ || !logEdit_) {
            return false;
        }
        std::ofstream file(settingsPath_, std::ios::binary | std::ios::trunc);
        if (!file) {
            SetStatus(L"Не удалось сохранить настройки.");
            return false;
        }
        file << WideToUtf8(GetText(nameEdit_)) << '\n'
             << WideToUtf8(GetText(surnameEdit_)) << '\n'
             << WideToUtf8(GetText(logEdit_)) << '\n'
             << WideToUtf8(GetText(restEdit_)) << '\n'
             << WideToUtf8(GetText(autoIntervalEdit_)) << '\n'
             << manualHotkey_ << '\n'
             << autoHotkey_ << '\n'
             << calibrationHotkey_ << '\n';
        if (!file) {
            SetStatus(L"Ошибка записи настроек.");
            return false;
        }
        return true;
    }

    void RestoreSettingsToControls() {
        if (!nameEdit_) {
            return;
        }
        restoringSettings_ = true;
        SetWindowTextW(nameEdit_, savedFirstName_.c_str());
        SetWindowTextW(surnameEdit_, savedSurname_.c_str());
        const std::wstring path = savedLogPath_.empty()
            ? defaultLogPath_.wstring() : savedLogPath_;
        SetWindowTextW(logEdit_, path.c_str());
        if (!savedRestSeconds_.empty()) {
            SetWindowTextW(restEdit_, savedRestSeconds_.c_str());
        }
        if (!savedAutoIntervalSeconds_.empty()) {
            SetWindowTextW(
                autoIntervalEdit_, savedAutoIntervalSeconds_.c_str());
        }
        restoringSettings_ = false;
    }

    void SetStatus(std::wstring text) {
        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            statusText_ = std::move(text);
        }
        if (window_) {
            PostMessageW(window_, kStatusMessage, 0, 0);
        }
    }

    void RefreshStatus() {
        if (!statusLabel_) {
            return;
        }
        std::lock_guard<std::mutex> lock(statusMutex_);
        SetWindowTextW(statusLabel_,
            statusText_.empty() ? L"Готово" : statusText_.c_str());
        if (calibrationButton_) {
            SetWindowTextW(calibrationButton_, calibrationActive_.load()
                ? L"ОТМЕНИТЬ КАЛИБРОВКУ"
                : module_ == Module::Stamina
                    ? L"Калибровка маршрута" : L"Калибровка мыши");
            InvalidateRect(calibrationButton_, nullptr, TRUE);
        }
        if (autoButton_) {
            SetWindowTextW(autoButton_,
                automaticPlayback_.load() && playbackActive_.load()
                    ? L"АВТО · ВЫКЛЮЧИТЬ" : L"АВТО · ВКЛЮЧИТЬ");
            InvalidateRect(autoButton_, nullptr, TRUE);
        }
        InvalidateRect(window_, nullptr, FALSE);
    }

    std::wstring savedFirstName_;
    std::wstring savedSurname_;
    std::wstring savedLogPath_;
    std::wstring savedRestSeconds_;
    std::wstring savedAutoIntervalSeconds_ = L"10";
    std::filesystem::path defaultLogPath_;
};

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    try {
        SystemRpmApp app;
        return app.Run(instance);
    } catch (const std::exception& error) {
        const std::wstring message = Utf8ToWide(error.what());
        MessageBoxW(nullptr, message.c_str(),
            L"System RPM — ошибка", MB_OK | MB_ICONERROR);
        return 1;
    }
}
