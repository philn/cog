#include <cog.h>

#include <assert.h>
#include <fcntl.h>
#include <gbm.h>
#include <wpe/fdo.h>
#include <wpe/fdo-egl.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>


#if !defined(EGL_EXT_platform_base)
typedef EGLDisplay (EGLAPIENTRYP PFNEGLGETPLATFORMDISPLAYEXTPROC) (EGLenum platform, void *native_display, const EGLint *attrib_list);
#endif

#if !defined(EGL_MESA_platform_gbm) || !defined(EGL_KHR_platform_gbm)
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif


struct buffer_object {
    uint32_t fb_id;
    struct gbm_bo* bo;
    struct wl_resource* buffer_resource;
};

static struct {
    int fd;

    drmModeConnector *connector;
    drmModeModeInfo *mode;
    drmModeEncoder *encoder;

    uint32_t connector_id;
    uint32_t crtc_id;
    int crtc_index;

    uint32_t width;
    uint32_t height;

    bool mode_set;
    struct buffer_object* committed_buffer;
} drm_data = {
    .fd = -1,
    .connector = NULL,
    .mode = NULL,
    .encoder = NULL,
    .connector_id = 0,
    .crtc_id = 0,
    .crtc_index = -1,
    .width = 0,
    .height = 0,
    .mode_set = false,
    .committed_buffer = NULL,
};

static struct {
    struct gbm_device *device;
} gbm_data = {
    .device = NULL,
};

static struct {
    EGLDisplay display;
} egl_data = {
    .display = EGL_NO_DISPLAY,
};

static struct {
    GSource* source;
} glib_data = {
    .source = NULL,
};

static struct {
    struct wpe_view_backend_exportable_fdo *exportable;
} wpe_host_data;

static struct {
    struct wpe_view_backend* backend;
} wpe_view_data;


static void destroy_buffer (struct buffer_object *buffer)
{
    drmModeRmFB (drm_data.fd, buffer->fb_id);
    gbm_bo_destroy (buffer->bo);

    wpe_view_backend_exportable_fdo_dispatch_release_buffer (wpe_host_data.exportable, buffer->buffer_resource);
    g_free (buffer);
}

static void
clear_drm ()
{
    g_clear_pointer (&drm_data.encoder, drmModeFreeEncoder);
    g_clear_pointer (&drm_data.connector, drmModeFreeConnector);
    if (drm_data.fd != -1) {
        close (drm_data.fd);
        drm_data.fd = -1;
    }
}

static gboolean
init_drm ()
{
    drm_data.fd = open ("/dev/dri/card0", O_RDWR);
    if (drm_data.fd < 0)
        return FALSE;

    drmModeRes *resources = drmModeGetResources (drm_data.fd);
    if (!resources)
        return FALSE;

    for (int i = 0; i < resources->count_connectors; ++i) {
        drm_data.connector = drmModeGetConnector (drm_data.fd, resources->connectors[i]);
        if (drm_data.connector->connection == DRM_MODE_CONNECTED)
            break;

        g_clear_pointer (&drm_data.connector, drmModeFreeConnector);
    }
    if (!drm_data.connector) {
        g_clear_pointer (&resources, drmModeFreeResources);
        return FALSE;
    }

    for (int i = 0, area = 0; i < drm_data.connector->count_modes; ++i) {
        drmModeModeInfo *current_mode = &drm_data.connector->modes[i];
        if (current_mode->type & DRM_MODE_TYPE_PREFERRED) {
            drm_data.mode = current_mode;
            break;
        }

        int current_area = current_mode->hdisplay * current_mode->vdisplay;
        if (current_area > area) {
            drm_data.mode = current_mode;
            area = current_area;
        }
    }
    if (!drm_data.mode) {
        g_clear_pointer (&drm_data.connector, drmModeFreeConnector);
        g_clear_pointer (&resources, drmModeFreeResources);
        return FALSE;
    }

    for (int i = 0; i < resources->count_encoders; ++i) {
        drm_data.encoder = drmModeGetEncoder (drm_data.fd, resources->encoders[i]);
        if (drm_data.encoder->encoder_id == drm_data.connector->encoder_id)
            break;

        g_clear_pointer (&drm_data.encoder, drmModeFreeEncoder);
    }
    if (!drm_data.encoder) {
        g_clear_pointer (&drm_data.connector, drmModeFreeConnector);
        g_clear_pointer (&resources, drmModeFreeResources);
        return FALSE;
    }

    drm_data.connector_id = drm_data.connector->connector_id;
    drm_data.crtc_id = drm_data.encoder->crtc_id;
    for (int i = 0; i < resources->count_crtcs; ++i) {
        if (resources->crtcs[i] == drm_data.crtc_id) {
            drm_data.crtc_index = i;
            break;
        }
    }

    drm_data.width = drm_data.mode->hdisplay;
    drm_data.height = drm_data.mode->vdisplay;

    g_clear_pointer (&resources, drmModeFreeResources);
    return TRUE;
}

static void
drm_page_flip_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, void *data)
{
    g_clear_pointer (&drm_data.committed_buffer, destroy_buffer);

    wpe_view_backend_exportable_fdo_dispatch_frame_complete (wpe_host_data.exportable);
    drm_data.committed_buffer = (struct buffer_object *) data;
}


static void
clear_gbm ()
{
    g_clear_pointer (&gbm_data.device, gbm_device_destroy);
}

static gboolean
init_gbm ()
{
    gbm_data.device = gbm_create_device (drm_data.fd);
    if (!gbm_data.device)
        return FALSE;

    return TRUE;
}


static void
clear_egl ()
{
    if (egl_data.display != EGL_NO_DISPLAY)
        eglTerminate (egl_data.display);
    eglReleaseThread ();
}

static gboolean
init_egl ()
{
    static PFNEGLGETPLATFORMDISPLAYEXTPROC s_eglGetPlatformDisplay = NULL;
    if (!s_eglGetPlatformDisplay)
        s_eglGetPlatformDisplay = (PFNEGLGETPLATFORMDISPLAYEXTPROC) eglGetProcAddress ("eglGetPlatformDisplayEXT");

    if (s_eglGetPlatformDisplay)
        egl_data.display = s_eglGetPlatformDisplay (EGL_PLATFORM_GBM_KHR, gbm_data.device, NULL);
    else
        egl_data.display = eglGetDisplay (gbm_data.device);

    if (!egl_data.display) {
        clear_egl ();
        return FALSE;
    }

    if (!eglInitialize (egl_data.display, NULL, NULL)) {
        clear_egl ();
        return FALSE;
    }

    return TRUE;
}


struct drm_source {
    GSource source;
    GPollFD pfd;
    drmEventContext event_context;
};

static gboolean
drm_source_check (GSource *base)
{
    struct drm_source *source = (struct drm_source *) base;
    return !!source->pfd.revents;
}

static gboolean
drm_source_dispatch (GSource* base, GSourceFunc callback, gpointer user_data)
{
    struct drm_source *source = (struct drm_source *) base;
    if (source->pfd.revents & (G_IO_ERR | G_IO_HUP))
        return FALSE;

    if (source->pfd.revents & G_IO_IN)
        drmHandleEvent (drm_data.fd, &source->event_context);
    source->pfd.revents = 0;
    return TRUE;
}


static void
clear_glib ()
{
    if (glib_data.source)
        g_source_destroy (glib_data.source);
    g_clear_pointer (&glib_data.source, g_source_unref);
}

static gboolean
init_glib ()
{
    static GSourceFuncs source_funcs = {
        NULL,
        drm_source_check,
        drm_source_dispatch,
        NULL,
        NULL,
        NULL,
    };

    glib_data.source = g_source_new (&source_funcs, sizeof (struct drm_source));
    struct drm_source* source = (struct drm_source *) glib_data.source;
    memset (&source->event_context, 0, sizeof (drmEventContext));
    source->event_context.version = DRM_EVENT_CONTEXT_VERSION;
    source->event_context.page_flip_handler = drm_page_flip_handler;

    source->pfd.fd = drm_data.fd;
    source->pfd.events = G_IO_IN | G_IO_ERR | G_IO_HUP;
    source->pfd.revents = 0;
    g_source_add_poll (glib_data.source, &source->pfd);

    g_source_set_name (glib_data.source, "cog: drm");
    g_source_set_can_recurse (glib_data.source, TRUE);
    g_source_attach (glib_data.source, g_main_context_get_thread_default ());

    return TRUE;
}


static void
on_export_buffer_resource (void* data, struct wl_resource* buffer_resource)
{
    assert (!"should not be reached");
}

static void
on_export_dmabuf_resource (void* data, struct wpe_view_backend_exportable_fdo_dmabuf_resource* dmabuf_resource)
{
    struct gbm_import_fd_modifier_data modifier_data;
    modifier_data.width = dmabuf_resource->width;
    modifier_data.height = dmabuf_resource->height;
    modifier_data.format = dmabuf_resource->format;
    modifier_data.num_fds = dmabuf_resource->n_planes;
    for (uint32_t i = 0; i < modifier_data.num_fds; ++i) {
        modifier_data.fds[i] = dmabuf_resource->fds[i];
        modifier_data.strides[i] = dmabuf_resource->strides[i];
        modifier_data.offsets[i] = dmabuf_resource->offsets[i];
    }
    modifier_data.modifier = dmabuf_resource->modifiers[0];

    struct gbm_bo* bo = gbm_bo_import (gbm_data.device, GBM_BO_IMPORT_FD_MODIFIER,
                                       (void *)(&modifier_data), GBM_BO_USE_SCANOUT);
    if (!bo)
        return;

    uint32_t in_handles[4] = { 0, };
    uint32_t in_strides[4] = { 0, };
    uint32_t in_offsets[4] = { 0, };
    uint64_t in_modifiers[4] = { 0, };
    in_modifiers[0] = gbm_bo_get_modifier (bo);

    int plane_count = gbm_bo_get_plane_count (bo);
    for (int i = 0; i < plane_count; ++i) {
        in_handles[i] = gbm_bo_get_handle_for_plane (bo, i).u32;
        in_strides[i] = gbm_bo_get_stride_for_plane (bo, i);
        in_offsets[i] = gbm_bo_get_offset (bo, i);
        in_modifiers[i] = in_modifiers[0];
    }

    int flags = 0;
    if (in_modifiers[0])
        flags = DRM_MODE_FB_MODIFIERS;

    uint32_t fb_id = 0;
    int ret = drmModeAddFB2WithModifiers (drm_data.fd, dmabuf_resource->width, dmabuf_resource->height, dmabuf_resource->format,
                                          in_handles, in_strides, in_offsets,
                                          in_modifiers, &fb_id, flags);
    if (ret)
        return;

    if (!drm_data.mode_set) {
        ret = drmModeSetCrtc (drm_data.fd, drm_data.crtc_id, fb_id, 0, 0,
                              &drm_data.connector_id, 1, drm_data.mode);
        if (ret)
            return;
        drm_data.mode_set = true;
    }

    struct buffer_object* buffer = g_new0 (struct buffer_object, 1);
    buffer->fb_id = fb_id;
    buffer->bo = bo;
    buffer->buffer_resource = dmabuf_resource->buffer_resource;
    ret = drmModePageFlip (drm_data.fd, drm_data.crtc_id, fb_id,
                           DRM_MODE_PAGE_FLIP_EVENT, buffer);
}

gboolean
cog_platform_setup (CogPlatform *platform,
                    CogShell    *shell G_GNUC_UNUSED,
                    const char  *params,
                    GError     **error)
{
    g_assert (platform);
    g_return_val_if_fail (COG_IS_SHELL (shell), FALSE);

    if (!wpe_loader_init ("libWPEBackend-fdo-1.0.so")) {
        g_set_error_literal (error,
                             COG_PLATFORM_WPE_ERROR,
                             COG_PLATFORM_WPE_ERROR_INIT,
                             "Failed to set backend library name");
        return FALSE;
    }

    if (!init_drm ()) {
        g_set_error_literal (error,
                             COG_PLATFORM_WPE_ERROR,
                             COG_PLATFORM_WPE_ERROR_INIT,
                             "Failed to initialize DRM");
        return FALSE;
    }

    if (!init_gbm ()) {
        g_set_error_literal (error,
                             COG_PLATFORM_WPE_ERROR,
                             COG_PLATFORM_WPE_ERROR_INIT,
                             "Failed to initialize GBM");
        return FALSE;
    }

    if (!init_egl ()) {
        g_set_error_literal (error,
                             COG_PLATFORM_WPE_ERROR,
                             COG_PLATFORM_WPE_ERROR_INIT,
                             "Failed to initialize EGL");
        return FALSE;
    }

    if (!init_glib ()) {
        g_set_error_literal (error,
                             COG_PLATFORM_WPE_ERROR,
                             COG_PLATFORM_WPE_ERROR_INIT,
                             "Failed to initialize GBM");
        return FALSE;
    }

    wpe_fdo_initialize_for_egl_display (egl_data.display);

    return TRUE;
}

void
cog_platform_teardown (CogPlatform *platform)
{
    g_assert (platform);

    g_clear_pointer (&drm_data.committed_buffer, destroy_buffer);

    clear_glib ();
    clear_egl ();
    clear_gbm ();
    clear_drm ();
}

WebKitWebViewBackend*
cog_platform_get_view_backend (CogPlatform   *platform,
                               WebKitWebView *related_view,
                               GError       **error)
{
    static struct wpe_view_backend_exportable_fdo_client exportable_client = {
        .export_buffer_resource = on_export_buffer_resource,
        .export_dmabuf_resource = on_export_dmabuf_resource,
    };

    wpe_host_data.exportable = wpe_view_backend_exportable_fdo_create (&exportable_client,
                                                                       NULL,
                                                                       drm_data.width,
                                                                       drm_data.height);
    g_assert (wpe_host_data.exportable);

    wpe_view_data.backend = wpe_view_backend_exportable_fdo_get_view_backend (wpe_host_data.exportable);
    g_assert (wpe_view_data.backend);

    WebKitWebViewBackend *wk_view_backend =
        webkit_web_view_backend_new (wpe_view_data.backend,
                                     (GDestroyNotify) wpe_view_backend_exportable_fdo_destroy,
                                     wpe_host_data.exportable);
    g_assert (wk_view_backend);

    return wk_view_backend;
}
