/* © 2026 dancingmirrors */

#include <xf86drm.h>

#include "poison.h"

static const char *pci_vendor_name(uint16_t vendor) {
    switch (vendor) {
    case 0x1002:
        return "AMD";
    case 0x10de:
        return "NVIDIA";
    case 0x1af4:
        return "virtio";
    case 0x8086:
        return "Intel";
    default:
        return NULL;
    }
}

static const char *device_describe(int fd, char *buf, size_t size) {
    if (fd < 0) {
        snprintf(buf, size, "unavailable");
        return buf;
    }

    drmVersionPtr version = drmGetVersion(fd);
    const char *driver =
        (version && version->name) ? version->name : "unknown driver";

    char pci[64] = "";
    drmDevicePtr dev = NULL;
    if (drmGetDevice2(fd, 0, &dev) == 0) {
        if (dev->bustype == DRM_BUS_PCI && dev->deviceinfo.pci) {
            uint16_t vendor = dev->deviceinfo.pci->vendor_id;
            const char *name = pci_vendor_name(vendor);
            snprintf(pci, sizeof(pci), "%s%s%04x:%04x, ",
                     name ? name : "", name ? " " : "", vendor,
                     dev->deviceinfo.pci->device_id);
        }
        drmFreeDevice(&dev);
    }

    char *node = drmGetDeviceNameFromFd2(fd);
    snprintf(buf, size, "%s (%s%s)", driver, pci, node ? node : "unknown node");
    free(node);

    if (version) {
        drmFreeVersion(version);
    }
    return buf;
}

static int device_same(int fd_a, int fd_b) {
    if (fd_a < 0 || fd_b < 0) {
        return -1;
    }

    drmDevicePtr a = NULL, b = NULL;
    int result = -1;
    if (drmGetDevice2(fd_a, 0, &a) == 0 && drmGetDevice2(fd_b, 0, &b) == 0) {
        result = drmDevicesEqual(a, b) ? 1 : 0;
    }
    if (a) {
        drmFreeDevice(&a);
    }
    if (b) {
        drmFreeDevice(&b);
    }
    return result;
}

static const char *const device_env_names[] = {
    "WLR_DRM_DEVICES",
    "WLR_RENDER_DRM_DEVICE",
    "WLR_RENDERER",
};

static bool device_env_ours[sizeof(device_env_names) /
                            sizeof(device_env_names[0])];

static void device_setenv(size_t index, const char *value) {
    const char *name = device_env_names[index];
    if (!value) {
        return;
    }

    const char *existing = getenv(name);
    if (existing && *existing) {
        wlr_log(WLR_INFO, "Config: %s is already '%s' in the environment: "
                          "keeping it over the configured '%s'.",
                name, existing, value);
        return;
    }

    setenv(name, value, 1);
    device_env_ours[index] = true;
    wlr_log(WLR_INFO, "Config: %s=%s.", name, value);
}

void poison_device_apply_env(const struct poison_config *config) {
    device_setenv(0, config->drm_devices);
    device_setenv(1, config->render_device);
    device_setenv(2, config->renderer_name);
}

void poison_device_clear_env(void) {
    for (size_t i = 0; i < sizeof(device_env_names) / sizeof(device_env_names[0]);
         i++) {
        if (device_env_ours[i]) {
            unsetenv(device_env_names[i]);
            device_env_ours[i] = false;
        }
    }
}

void poison_device_log_topology(struct poison_server *server) {
    if (!server->renderer || !server->backend) {
        return;
    }

    int render_fd = wlr_renderer_get_drm_fd(server->renderer);
    int scanout_fd = wlr_backend_get_drm_fd(server->backend);

    char buf[192];
    wlr_log(WLR_INFO, "Render device: %s.",
            device_describe(render_fd, buf, sizeof(buf)));
    wlr_log(WLR_INFO, "Primary scanout device: %s.",
            device_describe(scanout_fd, buf, sizeof(buf)));
}

void poison_device_log_output(struct poison_server *server,
                              struct wlr_output *wlr_output) {
    if (!wlr_output->backend || !server->renderer) {
        return;
    }

    int output_fd = wlr_backend_get_drm_fd(wlr_output->backend);
    if (output_fd < 0) {
        return;
    }

    char buf[192];
    const char *desc = device_describe(output_fd, buf, sizeof(buf));

    if (device_same(output_fd, wlr_renderer_get_drm_fd(server->renderer)) == 0) {
    } else {
        wlr_log(WLR_INFO, "Output %s scans out on %s.", wlr_output->name, desc);
    }
}
