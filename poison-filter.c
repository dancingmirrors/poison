/* © 2026 dancingmirrors */

#define _GNU_SOURCE

#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define LOG_BUFFER_SIZE 4096

typedef void (*wl_log_func_t)(const char *fmt, va_list args);

static void (*original_set_handler)(wl_log_func_t) = NULL;
static wl_log_func_t user_handler = NULL;

static void filtered_log_handler(const char *fmt, va_list args) {
    char buffer[LOG_BUFFER_SIZE];

    va_list args_copy;
    va_copy(args_copy, args);

    vsnprintf(buffer, sizeof(buffer), fmt, args_copy);
    va_end(args_copy);

    const char *queue_pos = strstr(buffer, "queue");
    if (queue_pos != NULL && strstr(queue_pos, "destroyed while proxies") != NULL) {
        return;
    }

    if (strstr(buffer, "still attached") != NULL) {
        return;
    }

    if (user_handler) {
        user_handler(fmt, args);
    } else {
        vfprintf(stderr, fmt, args);
    }
}

void wl_log_set_handler_client(wl_log_func_t handler) {
    user_handler = handler;

    if (!original_set_handler) {
        original_set_handler = dlsym(RTLD_NEXT, "wl_log_set_handler_client");
    }

    if (original_set_handler) {
        original_set_handler(filtered_log_handler);
    }
}

__attribute__((constructor)) static void init_filter(void) {
    wl_log_set_handler_client(NULL);
}
