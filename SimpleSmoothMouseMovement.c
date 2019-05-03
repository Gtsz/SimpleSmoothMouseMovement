// Reference:
// MouseRaw.cpp https://gist.github.com/luluco250/ac79d72a734295f167851ffdb36d77ee
// Microsoft Dev Center https://docs.microsoft.com/en-us/windows/desktop/api/winuser

// Make Windows.h slightly less of a headache.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define VC_EXTRALEAN
#include <Windows.h>
//#include <math.h>

// Only for redirecting stdout to the console.
#ifdef DEBUG
#include <stdio.h>
#endif

POINT mouVelocity = { 0 };

struct Config {
  double damper;
  double accelerator;
  double vel_threshold;
} config = { 5, 5, 0.5 };

BOOL CalcMouseTrace(POINT* out, POINT in, double dt);
UINT MouseMove(int dx, int dy);
LRESULT CALLBACK EventHandler(HWND, UINT, WPARAM, LPARAM);
BOOL RegisterRawInputMouse(HWND);

// For a float f, f != f will be true only if f is NaN.
#define NoNaN(f) (((f) != (f)) ? 0.f : (f))
#define fabs(f) (((f) < 0) ? -(f) : (f))

int __stdcall WinMain(
    HINSTANCE hInstance,
    HINSTANCE hPrevInstance,
    LPSTR     lpCmdLine,
    int       nShowCmd
) {
  // Why even bother with WinMain?
  HINSTANCE instance = GetModuleHandle(0);
  
#ifdef DEBUG
  // Get console window:
  FILE* console_output;
  FILE* console_error;
  
  if (AllocConsole()) {
    freopen_s(&console_output, "CONOUT$", "w", stdout);
    freopen_s(&console_error, "CONERR$", "w", stderr);
  }
  else {
    return -1;
  }
#endif
  
  // Create message-only window:

  const char* class_name = "SimpleMouseSmoother Class";

  // "{ 0 }" is necessary, otherwise we have to use ZeroMemory() (which is just memset).
  WNDCLASS window_class = { 0 };
  HWND window;

  window_class.lpfnWndProc = EventHandler;
  window_class.hInstance = instance;
  window_class.lpszClassName = class_name;

  if (!RegisterClass(&window_class))
    return -1;

  window = CreateWindow(class_name,
      "SimpleMouseSmoother", 0, 0, 0, 0, 0,
    HWND_MESSAGE, 0, 0, 0);

  if (window == NULL)
    return -1;

  RegisterRawInputMouse(window);

  // End of creating window.
  

  // Main loop:
  {
    MSG event;
    BOOL quit = FALSE;
    BOOL done = FALSE;
    BOOL received = FALSE;

    __int64 freq;
    __int64 t1, t2;
    double dt;

    POINT idS;

    QueryPerformanceFrequency((LARGE_INTEGER*)&freq);

    while (GetMessage(&event, NULL, WM_QUIT, WM_INPUT) && !quit) {
      QueryPerformanceCounter((LARGE_INTEGER*)&t1);
      received = TRUE;
      while (!done || received) {
        if (received) {
          if (event.message == WM_QUIT) {
            quit = TRUE;
            break;
          }
          TranslateMessage(&event);
          DispatchMessage(&event);
        }

        QueryPerformanceCounter((LARGE_INTEGER*)&t2);
        dt = (double)(t2 - t1) / freq;
        t1 = t2;

        done = CalcMouseTrace(&idS, mouVelocity, dt);
        MouseMove(idS.x - mouVelocity.x, idS.y - mouVelocity.y);

#ifdef DEBUG
        if (idS.x != 0 || idS.y != 0)
            printf("%d %d\n", idS.x, idS.y);
#endif

        // Reset mouse data in case WM_INPUT isn't called:
        mouVelocity.x = 0;
        mouVelocity.y = 0;

        received = PeekMessage(&event, NULL, WM_QUIT, WM_INPUT, PM_REMOVE);
      }
      done = FALSE;
    }
  }
  
#ifdef DEBUG
  fclose(console_output);
  fclose(console_error);
#endif

  return 0;
}

// The extra information acted as the identifier in a WM_INPUT event
// It helps the receiver to classify whether the event is generate by MouseMove(dx,dy).
// This is just a magic number.
#ifndef IT_IS_SENT_BY_ME
#define IT_IS_SENT_BY_ME 3584750163
#endif

LRESULT CALLBACK EventHandler(
    HWND   hwnd,
    UINT   event,
    WPARAM wparam,
    LPARAM lparam
) {
  switch (event) {
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    case WM_INPUT: {
      // The official Microsoft examples are pretty terrible about this.
      // Size needs to be non-constant because GetRawInputData() can return the
      // size necessary for the RAWINPUT data, which is a weird feature.
      UINT size = sizeof(RAWINPUT);
      static RAWINPUT raw[sizeof(RAWINPUT)];
      GetRawInputData((HRAWINPUT)lparam, RID_INPUT, 
                      raw, &size, sizeof(RAWINPUTHEADER));

      if (raw->header.dwType == RIM_TYPEMOUSE) {
        if (raw->data.mouse.ulExtraInformation == IT_IS_SENT_BY_ME) {
          mouVelocity.x = 0;
          mouVelocity.y = 0;
        } else {
          mouVelocity.x = raw->data.mouse.lLastX;
          mouVelocity.y = raw->data.mouse.lLastY;
        }
      }
    } return 0;
  }

  // Run default message processor for any missed events:
  return DefWindowProc(hwnd, event, wparam, lparam);
}

BOOL RegisterRawInputMouse(HWND window) {
#ifndef HID_USAGE_PAGE_GENERIC
#define HID_USAGE_PAGE_GENERIC ((USHORT) 0x01)
#endif
#ifndef HID_USAGE_GENERIC_MOUSE
#define HID_USAGE_GENERIC_MOUSE ((USHORT) 0x02)
#endif
  
  // We're configuring just one RAWINPUTDEVICE, the mouse,
  // so it's a single-element array (a pointer).
  RAWINPUTDEVICE rid[1];
  rid[0].usUsagePage = HID_USAGE_PAGE_GENERIC;
  rid[0].usUsage = HID_USAGE_GENERIC_MOUSE;
  rid[0].dwFlags = RIDEV_INPUTSINK;
  rid[0].hwndTarget = window;
  return RegisterRawInputDevices(rid, 1, sizeof(rid[0]));
}

BOOL CalcMouseTrace(POINT* out, POINT in, double dt) {
  static double vx = 0, vy = 0;
  static double dx = 0, dy = 0;

  double mvx = NoNaN(in.x / dt);
  double mvy = NoNaN(in.y / dt);

  double ax = mvx * config.accelerator - vx * config.damper;
  double ay = mvy * config.accelerator - vy * config.damper;

  // Euler integration
  vx += ax * dt;
  vy += ay * dt;
  dx += vx * dt;
  dy += vy * dt;

  // Convert
  out->x = (int)dx;
  out->y = (int)dy;
  dx -= out->x;
  dy -= out->y;

  if (fabs(vx) < config.vel_threshold && 
      fabs(vy) < config.vel_threshold) {
    vx = 0.f;
    vy = 0.f;
    dx = 0.f;
    dy = 0.f;
    return TRUE;
  } else
    return FALSE;
}

UINT MouseMove(int dx, int dy) {
  static INPUT input = { INPUT_MOUSE, { 0, 0, 0,
    MOUSEEVENTF_MOVE, 0,
    IT_IS_SENT_BY_ME } };
  
  if (!dx && !dy) return 0;
  
  //input.type = INPUT_MOUSE;
  //input.mi.dwFlags = MOUSEEVENTF_MOVE;
  //input.mi.dwExtraInfo = IT_IS_SENT_BY_ME;
  input.mi.dx = dx;
  input.mi.dy = dy;
  return SendInput(1, &input, sizeof(input));
}