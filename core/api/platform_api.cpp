#include "GrycePlatform/window_api.h"
#include "GrycePlatform/input_api.h"

extern "C" {

int GWindow_InitExternal(GWindowHandle hwnd, int w, int h) { (void)hwnd; (void)w; (void)h; return 0; }
int GWindow_Create(const char* title, int w, int h, GWindowMode mode) { (void)title; (void)w; (void)h; (void)mode; return 0; }
void GWindow_Destroy(void) {}
bool GWindow_IsValid(void) { return false; }

void GWindow_GetSize(int* out_w, int* out_h) { (void)out_w; (void)out_h; }
void GWindow_SetSize(int w, int h) { (void)w; (void)h; }
GWindowHandle GWindow_GetNativeHandle(void) { return nullptr; }
bool GWindow_ShouldClose(void) { return true; }
void GWindow_PollEvents(void) {}
void GWindow_SwapBuffers(void) {}

void GInput_InjectKey(int key_code, GInputAction action) { (void)key_code; (void)action; }
void GInput_InjectMouseMove(float x, float y) { (void)x; (void)y; }
void GInput_InjectMouseButton(int button, GInputAction action, float x, float y) { (void)button; (void)action; (void)x; (void)y; }
void GInput_InjectMouseScroll(float delta_x, float delta_y) { (void)delta_x; (void)delta_y; }

bool GInput_IsKeyPressed(int key_code) { (void)key_code; return false; }
bool GInput_IsKeyHeld(int key_code) { (void)key_code; return false; }
bool GInput_IsMouseButtonPressed(int button) { (void)button; return false; }
void GInput_GetMousePosition(float* out_x, float* out_y) { (void)out_x; (void)out_y; }

} // extern "C"
