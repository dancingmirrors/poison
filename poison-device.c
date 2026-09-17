/* © 2026 dancingmirrors */

#include <dirent.h>
#include <limits.h>
#include <unistd.h>

#include <xf86drm.h>

#include <wlr/backend/multi.h>

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

static char *device_resolve_one(const char *selector, const char *prefix) {
    if (selector[0] == '/') {
        return strdup(selector);
    }

    const size_t prefix_len = strlen(prefix);

    DIR *dir = opendir("/sys/class/drm");
    if (!dir) {
        wlr_log(WLR_ERROR, "Config: cannot read /sys/class/drm to resolve "
                           "device '%s'.",
                selector);
        return NULL;
    }

    long best = LONG_MAX;
    char *result = NULL;
    const struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, prefix, prefix_len) != 0) {
            continue;
        }

        const char *digits = entry->d_name + prefix_len;
        char *end = NULL;
        long index = strtol(digits, &end, 10);
        if (end == digits || *end != '\0' || index >= best) {
            continue;
        }

        char link[PATH_MAX];
        snprintf(link, sizeof(link), "/sys/class/drm/%s/device/driver",
                 entry->d_name);

        char target[PATH_MAX];
        ssize_t len = readlink(link, target, sizeof(target) - 1);
        if (len < 0) {
            continue;
        }
        target[len] = '\0';

        const char *driver = strrchr(target, '/');
        driver = driver ? driver + 1 : target;
        if (strcmp(driver, selector) != 0) {
            continue;
        }

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "/dev/dri/%s", entry->d_name);
        char *dup = strdup(path);
        if (dup) {
            free(result);
            result = dup;
            best = index;
        }
    }
    closedir(dir);

    if (!result) {
        wlr_log(WLR_ERROR, "Config: no %s* node is driven by '%s'.", prefix,
                selector);
    }
    return result;
}

static char *device_resolve_list(const char *selectors) {
    char *work = strdup(selectors);
    if (!work) {
        return NULL;
    }

    char *out = NULL;
    size_t out_len = 0;
    for (char *save = NULL, *tok = strtok_r(work, ":", &save); tok != NULL;
         tok = strtok_r(NULL, ":", &save)) {
        char *path = device_resolve_one(tok, "card");
        if (!path) {
            continue;
        }

        size_t add = strlen(path);
        char *grown = realloc(out, out_len + add + 2);
        if (!grown) {
            free(path);
            continue;
        }
        out = grown;
        if (out_len > 0) {
            out[out_len++] = ':';
        }
        memcpy(out + out_len, path, add + 1);
        out_len += add;
        free(path);
    }

    free(work);
    return out;
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
    if (config->drm_devices) {
        char *resolved = device_resolve_list(config->drm_devices);
        if (resolved) {
            device_setenv(0, resolved);
            free(resolved);
        }
    }

    if (config->render_device) {
        char *resolved = device_resolve_one(config->render_device, "renderD");
        if (resolved) {
            device_setenv(1, resolved);
            free(resolved);
        }
    }

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

    if (device_same(output_fd, wlr_renderer_get_drm_fd(server->renderer)) == 0) {
        char buf[192];
        wlr_log(WLR_INFO, "Output %s scans out on %s, which is not the render "
                          "device (cross-device buffer sharing is in use).",
                wlr_output->name, device_describe(output_fd, buf, sizeof(buf)));
    }
}

static void device_report_no_timeline(struct wlr_backend *backend, void *data) {
    if (backend->features.timeline) {
        return;
    }

    int fd = wlr_backend_get_drm_fd(backend);
    if (fd < 0) {
        return;
    }

    unsigned int *blockers = data;
    (*blockers)++;

    char buf[192];
    wlr_log(WLR_INFO, "  %s does not support timeline synchronization.",
            device_describe(fd, buf, sizeof(buf)));
}

void poison_device_log_timeline_blockers(struct poison_server *server) {
    if (!server->backend || server->backend->features.timeline ||
        !wlr_backend_is_multi(server->backend)) {
        return;
    }

    unsigned int blockers = 0;
    wlr_multi_for_each_backend(server->backend, device_report_no_timeline,
                               &blockers);
    if (blockers > 0) {
        wlr_log(WLR_INFO, "Explicit synchronization is off for every output "
                          "because of the device(s) above. Set drm_devices to "
                          "the remaining ones to turn it back on, at the cost "
                          "of the outputs they drive.");
    }
}
