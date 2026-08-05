#include "GryceRenderer/render_api.h"
#include "GryceRenderer/viewport_api.h"

extern "C" {

int GRender_Init(const GRenderInitDesc* desc) { (void)desc; return 0; }
void GRender_Shutdown(void) {}
bool GRender_IsInitialized(void) { return false; }

void GRender_BeginFrame(void) {}
void GRender_RenderWorld(void) {}
void GRender_RenderGizmo(void) {}
void GRender_EndFrame(void) {}

GTextureHandle GRender_GetViewportTexture(void) { return nullptr; }
GTextureHandle GRender_GetGameViewTexture(void) { return nullptr; }
int GRender_GetViewportSize(int* out_w, int* out_h) { (void)out_w; (void)out_h; return -1; }
int GRender_GetGameViewSize(int* out_w, int* out_h) { (void)out_w; (void)out_h; return -1; }

void GRender_SetVSync(bool enabled) { (void)enabled; }

void GViewport_SetSize(int w, int h) { (void)w; (void)h; }
void GViewport_GetSize(int* out_w, int* out_h) { (void)out_w; (void)out_h; }
void GViewport_SetCamera(GEntityHandle camera_entity) { (void)camera_entity; }
GEntityHandle GViewport_GetCamera(void) { return 0; }

void GGameView_SetSize(int w, int h) { (void)w; (void)h; }
void GGameView_GetSize(int* out_w, int* out_h) { (void)out_w; (void)out_h; }
void GGameView_SetCamera(GEntityHandle camera_entity) { (void)camera_entity; }

} // extern "C"
