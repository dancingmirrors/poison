#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#define MAIN_COLOR RGB(0xc0, 0x30, 0x30)
#define POPUP_COLOR RGB(0x20, 0xa0, 0xa0)
#define DIALOG_COLOR RGB(0x30, 0x40, 0xc0)
#define SPLASH_COLOR RGB(0xd0, 0x80, 0x20)
#define PROMOTED_COLOR RGB(0x80, 0x40, 0xb0)
#define APP_COLOR RGB(0xe0, 0x80, 0xa0)
#define TRAY_PIXEL 0xff30b040

#define WM_TRAY_ICON (WM_APP + 1)
#define TRAY_MENU_HELLO 1

static HINSTANCE instance;
static HWND main_window;

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam,
                                    LPARAM lparam) {
    if (msg == WM_CLOSE) {
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static void register_class(const wchar_t *name, COLORREF color) {
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(color);
    wc.lpszClassName = name;
    RegisterClassW(&wc);
}

static void pump(DWORD ms) {
    DWORD end = GetTickCount() + ms;
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        LONG left = (LONG)(end - GetTickCount());
        if (left <= 0) {
            break;
        }
        MsgWaitForMultipleObjects(0, NULL, FALSE, (DWORD)left, QS_ALLINPUT);
    }
}

static void say_line(const char *line) {
    printf("%s\n", line);
    fflush(stdout);
    const char *path = getenv("POISON_WINE_LOG");
    FILE *file = path ? fopen(path, "a") : NULL;
    if (file) {
        fprintf(file, "%s\n", line);
        fclose(file);
    }
}

static void say(const char *what) {
    say_line(what);
}

static void report(const char *name, HWND hwnd) {
    RECT window, client;
    GetWindowRect(hwnd, &window);
    GetClientRect(hwnd, &client);
    char line[256];
    snprintf(line, sizeof(line),
             "%s: window %ld,%ld %ldx%ld, client %ldx%ld, %s%s, max track %dx%d",
             name, window.left, window.top, window.right - window.left,
             window.bottom - window.top, client.right, client.bottom,
             IsWindowVisible(hwnd) ? "visible" : "hidden",
             IsIconic(hwnd) ? ", minimized" : "", GetSystemMetrics(SM_CXMAXTRACK),
             GetSystemMetrics(SM_CYMAXTRACK));
    say_line(line);
}

static HWND create_main(HWND owner) {
    HWND hwnd = CreateWindowExW(0, L"PoisonMain", L"Poison Wine test",
                                WS_OVERLAPPEDWINDOW, 100, 100, 640, 480, owner,
                                NULL, instance, NULL);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    return hwnd;
}

static void popups(void) {
    HWND reused = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"PoisonPopup",
                                  L"", WS_POPUP, 700, 120, 200, 300, main_window,
                                  NULL, instance, NULL);
    for (int i = 0; i < 30; i++) {
        HWND popup = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                                     L"PoisonPopup", L"", WS_POPUP,
                                     150 + (i * 37) % 500, 150 + (i * 53) % 300,
                                     160, 90, main_window, NULL, instance, NULL);
        ShowWindow(popup, SW_SHOWNOACTIVATE);
        ShowWindow(reused, SW_SHOWNOACTIVATE);
        pump(i % 3 == 0 ? 60 : 15);
        if (i % 4 == 0) {
            SetWindowPos(popup, NULL, 200 + i * 5, 220, 180, 100,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            pump(15);
        }
        if (i % 2) {
            ShowWindow(popup, SW_HIDE);
            pump(10);
        }
        DestroyWindow(popup);
        ShowWindow(reused, SW_HIDE);
        pump(i % 5 == 0 ? 50 : 10);
    }
    DestroyWindow(reused);
}

static void dialogs(void) {
    for (int i = 0; i < 8; i++) {
        HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, L"PoisonDialog",
                                      L"Dialog", WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                      200, 150, 400, 250, main_window, NULL,
                                      instance, NULL);
        EnableWindow(main_window, FALSE);
        ShowWindow(dialog, SW_SHOW);
        pump(i % 2 ? 300 : 80);
        EnableWindow(main_window, TRUE);
        if (i % 3 == 0) {
            ShowWindow(dialog, SW_HIDE);
            pump(30);
        }
        DestroyWindow(dialog);
        pump(i % 2 ? 50 : 0);
    }
}

static void promote(void) {
    HWND window = CreateWindowExW(0, L"PoisonPromoted", L"Promoted", WS_POPUP,
                                  300, 200, 420, 260, NULL, NULL, instance, NULL);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    pump(500);
    ShowWindow(window, SW_HIDE);
    pump(300);
    SetWindowLongW(window, GWL_STYLE, WS_POPUP | WS_CAPTION | WS_SYSMENU);
    SetWindowPos(window, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    pump(1000);
    report("promoted", window);
}

static HWND create_application_window(void) {
    HWND hwnd = CreateWindowExW(0, L"PoisonApp", L"Poison Wine test",
                                WS_POPUP | WS_CAPTION | WS_CLIPSIBLINGS |
                                    WS_SYSMENU | WS_MINIMIZEBOX,
                                GetSystemMetrics(SM_CXSCREEN) / 2,
                                GetSystemMetrics(SM_CYSCREEN) / 2, 0, 0, NULL,
                                NULL, instance, NULL);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    return hwnd;
}

static void show_per_pixel_alpha(HWND hwnd, int x, int y, int width,
                                 int height) {
    HDC screen = GetDC(NULL);
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO info = {0};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, NULL, 0);
    uint32_t *pixels = bits;
    for (int i = 0; i < width * height; i++) {
        pixels[i] = 0xff000000 | 0x20a0a0;
    }
    HGDIOBJ old = SelectObject(dc, bitmap);
    POINT pos = {x, y}, origin = {0, 0};
    SIZE size = {width, height};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd, screen, &pos, &size, dc, &origin, 0, &blend,
                        ULW_ALPHA);
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(NULL, screen);
}

static void layered(void) {
    for (int i = 0; i < 12; i++) {
        int x = 200 + i * 20, y = 150, width = 220, height = 120;
        HWND popup = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW |
                                         WS_EX_TOPMOST,
                                     L"PoisonPopup", L"", WS_POPUP, x, y, width,
                                     height, main_window, NULL, instance, NULL);
        if (i % 2 == 0) {
            SetLayeredWindowAttributes(popup, 0, 0, LWA_ALPHA);
            ShowWindow(popup, SW_SHOWNOACTIVATE);
            for (int alpha = 0; alpha <= 255; alpha += 51) {
                SetLayeredWindowAttributes(popup, 0, (BYTE)alpha, LWA_ALPHA);
                pump(15);
            }
        } else {
            show_per_pixel_alpha(popup, x, y, width, height);
            ShowWindow(popup, SW_SHOWNOACTIVATE);
        }
        pump(i % 3 ? 30 : 120);
        ShowWindow(popup, SW_HIDE);
        pump(20);
        DestroyWindow(popup);
        pump(20);
    }
}

static void splash(HWND splash_window) {
    pump(300);
    DestroyWindow(splash_window);
}

static bool tray_custom_menu;
static HWND tray_menu;

static LRESULT CALLBACK tray_menu_proc(HWND hwnd, UINT msg, WPARAM wparam,
                                       LPARAM lparam) {
    switch (msg) {
    case WM_LBUTTONUP:
        say("TRAY MENU HELLO");
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE) {
            ShowWindow(hwnd, SW_HIDE);
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static void show_tray_menu(void) {
    const int width = 180, height = 60;
    if (!tray_menu) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = tray_menu_proc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = CreateSolidBrush(POPUP_COLOR);
        wc.lpszClassName = L"PoisonTrayMenu";
        RegisterClassW(&wc);
        tray_menu = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                                    L"PoisonTrayMenu", L"", WS_POPUP, 0, 0,
                                    width, height, NULL, NULL, instance, NULL);
    }
    POINT pt;
    GetCursorPos(&pt);
    SetWindowPos(tray_menu, HWND_TOPMOST, pt.x - width, pt.y - height, width,
                 height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetForegroundWindow(tray_menu);
    SetWindowPos(tray_menu, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    RECT rect;
    GetWindowRect(tray_menu, &rect);
    char line[128];
    snprintf(line, sizeof(line), "TRAY MENU SHOWN %ld,%ld %ldx%ld", rect.left,
             rect.top, rect.right - rect.left, rect.bottom - rect.top);
    say_line(line);
}

static LRESULT CALLBACK tray_proc(HWND hwnd, UINT msg, WPARAM wparam,
                                  LPARAM lparam) {
    if (msg != WM_TRAY_ICON) {
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    switch (LOWORD(lparam)) {
    case WM_LBUTTONDOWN:
        say("TRAY LBUTTONDOWN");
        break;
    case WM_LBUTTONUP:
        say("TRAY LBUTTONUP");
        break;
    case WM_RBUTTONUP: {
        say("TRAY RBUTTONUP");
        if (tray_custom_menu) {
            show_tray_menu();
            break;
        }
        POINT pt;
        GetCursorPos(&pt);
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, TRAY_MENU_HELLO, L"Hello from the tray");
        SetForegroundWindow(hwnd);
        UINT id = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                 pt.x, pt.y, 0, hwnd, NULL);
        DestroyMenu(menu);
        say(id == TRAY_MENU_HELLO ? "TRAY MENU HELLO" : "TRAY MENU NONE");
        break;
    }
    }
    return 0;
}

static HICON create_tray_icon(void) {
    enum { SIZE = 16 };
    BITMAPINFO info = {0};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = SIZE;
    info.bmiHeader.biHeight = -SIZE;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HDC screen = GetDC(NULL);
    HBITMAP color = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits,
                                     NULL, 0);
    ReleaseDC(NULL, screen);
    uint32_t *pixels = bits;
    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            int dx = 2 * x + 1 - SIZE, dy = 2 * y + 1 - SIZE;
            pixels[y * SIZE + x] =
                dx * dx + dy * dy <= SIZE * SIZE ? TRAY_PIXEL : 0;
        }
    }
    HBITMAP mask = CreateBitmap(SIZE, SIZE, 1, 1, NULL);
    ICONINFO icon_info = {TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&icon_info);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

static void tray(void) {
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = tray_proc;
    wc.hInstance = instance;
    wc.lpszClassName = L"PoisonTray";
    RegisterClassW(&wc);
    HWND owner = CreateWindowExW(0, L"PoisonTray", L"", WS_POPUP, 0, 0, 0, 0,
                                 NULL, NULL, instance, NULL);

    NOTIFYICONDATAW nid = {0};
    nid.cbSize = sizeof(nid);
    nid.hWnd = owner;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY_ICON;
    nid.hIcon = create_tray_icon();
    lstrcpynW(nid.szTip, L"Poison Wine test", ARRAYSIZE(nid.szTip));
    say(Shell_NotifyIconW(NIM_ADD, &nid) ? "TRAY ADDED" : "TRAY FAILED");
    pump(1500);

    nid.uFlags = NIF_STATE;
    nid.dwState = NIS_HIDDEN;
    nid.dwStateMask = NIS_HIDDEN;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    say("TRAY HIDDEN");
    pump(1500);

    nid.dwState = 0;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    say("TRAY SHOWN");
}

int main(int argc, char **argv) {
    const char *scenario = argc > 1 ? argv[1] : "main";
    instance = GetModuleHandleW(NULL);
    register_class(L"PoisonMain", MAIN_COLOR);
    register_class(L"PoisonPopup", POPUP_COLOR);
    register_class(L"PoisonDialog", DIALOG_COLOR);
    register_class(L"PoisonSplash", SPLASH_COLOR);
    register_class(L"PoisonPromoted", PROMOTED_COLOR);
    register_class(L"PoisonApp", APP_COLOR);

    HWND splash_window = NULL;
    if (strcmp(scenario, "splash") == 0) {
        int width = GetSystemMetrics(SM_CXSCREEN);
        int height = GetSystemMetrics(SM_CYSCREEN);
        splash_window = CreateWindowExW(WS_EX_TOOLWINDOW, L"PoisonSplash", L"",
                                        WS_POPUP, (width - 500) / 2,
                                        (height - 250) / 2, 500, 250, NULL,
                                        NULL, instance, NULL);
        ShowWindow(splash_window, SW_SHOW);
        pump(800);
    }

    HWND owner = NULL;
    if (strcmp(scenario, "delphi") == 0) {
        owner = create_application_window();
        pump(500);
        say("APPLICATION");
        pump(2000);
    }
    main_window = create_main(owner);
    pump(1000);
    report("main", main_window);
    say("READY");

    if (strcmp(scenario, "popups") == 0) {
        popups();
    } else if (strcmp(scenario, "dialogs") == 0) {
        dialogs();
    } else if (strcmp(scenario, "promote") == 0) {
        promote();
    } else if (strcmp(scenario, "layered") == 0) {
        layered();
    } else if (strcmp(scenario, "splash") == 0) {
        splash(splash_window);
    } else if (strcmp(scenario, "tray") == 0) {
        tray();
    } else if (strcmp(scenario, "tray-popup") == 0) {
        tray_custom_menu = true;
        tray();
        pump(1000);
        show_tray_menu();
    } else if (strcmp(scenario, "minimize") == 0) {
        ShowWindow(main_window, SW_MINIMIZE);
        pump(800);
        ShowWindow(main_window, SW_RESTORE);
    }

    pump(500);
    report("main", main_window);
    say("DONE");
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
