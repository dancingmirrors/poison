/* © 2026 dancingmirrors */

#include "poison.h"

#define POISON_PADDING 5

void poison_config_init(struct poison_config *config) {
    config->padding = POISON_PADDING;
    config->idle_timeout_ms = 300000;
    config->terminal_command = NULL;
    config->volume_up_command =
        strdup("wpctl set-volume -l 1.0 @DEFAULT_AUDIO_SINK@ 5%+");
    config->volume_down_command =
        strdup("wpctl set-volume -l 1.0 @DEFAULT_AUDIO_SINK@ 5%-");
    config->mute_command =
        strdup("wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle");
    config->hacks_enabled = 0;
    config->notifications_enabled = 0;
    config->drm_devices = NULL;
    config->render_device = NULL;
    config->renderer_name = NULL;
    config->hot_corner_enabled = 1;
    config->hot_corner_threshold = HOT_CORNER_THRESHOLD;
    config->exec_command_count = 0;
    for (int i = 0; i < MAX_EXEC_COMMANDS; i++) {
        config->exec_commands[i] = NULL;
    }
}

void poison_config_load(struct poison_config *config) {
    const char *home = getenv("HOME");
    if (!home) {
        return;
    }

    char config_path[512];
    int path_len = snprintf(config_path, sizeof(config_path), "%s/.poison.rc", home);
    if (path_len < 0 || (size_t)path_len >= sizeof(config_path)) {
        wlr_log(WLR_ERROR, ".poison.rc path is too long!");
        return;
    }

    FILE *file = fopen(config_path, "r");
    if (!file) {
        return;
    }

    char line[1024];
    while (fgets(line, sizeof(line), file)) {
        if (!strchr(line, '\n') && !feof(file)) {
            wlr_log(WLR_ERROR, "Config: line too long, skipping.");
            int c;
            while ((c = fgetc(file)) != '\n' && c != EOF) {
            }
            if (c == EOF) {
                break;
            }
            continue;
        }
        line[strcspn(line, "\n")] = 0;

        bool in_quote = false;
        for (char *p = line; *p; p++) {
            if (*p == '"') {
                in_quote = !in_quote;
            } else if (*p == '#' && !in_quote) {
                *p = '\0';
                break;
            }
        }

        char *start = line;
        while (*start == ' ' || *start == '\t') {
            start++;
        }

        if (*start == '\0' || *start == '#') {
            continue;
        }

        if (strncmp(start, "exec", 4) == 0 && (start[4] == ' ' || start[4] == '\t' || start[4] == '=' || start[4] == '\0')) {
            char *cmd = start + 4;
            while (*cmd == ' ' || *cmd == '\t') {
                cmd++;
            }
            if (*cmd == '=') {
                cmd++;
                while (*cmd == ' ' || *cmd == '\t') {
                    cmd++;
                }
            }

            if (*cmd == '\0') {
                wlr_log(WLR_ERROR, "Config: exec command is empty!");
                continue;
            }

            char command[512];
            const char *cmd_start = cmd;
            size_t cmd_len;

            if (*cmd == '"') {
                cmd_start = cmd + 1;
                const char *end_quote = strchr(cmd_start, '"');
                if (!end_quote) {
                    wlr_log(WLR_ERROR, "Config: exec command is missing a closing quote?");
                    continue;
                }
                cmd_len = end_quote - cmd_start;
            } else {
                cmd_len = strlen(cmd_start);
            }

            if (cmd_len >= sizeof(command)) {
                wlr_log(WLR_ERROR, "Config: exec command is too long!");
                continue;
            }
            snprintf(command, sizeof(command), "%.*s", (int)cmd_len, cmd_start);

            if (config->exec_command_count < MAX_EXEC_COMMANDS) {
                config->exec_commands[config->exec_command_count] = strdup(command);
                if (config->exec_commands[config->exec_command_count]) {
                    config->exec_command_count++;
                } else {
                    wlr_log(WLR_ERROR, "Config: failed to allocate memory for exec command!");
                }
            } else {
                wlr_log(WLR_ERROR, "Config: too many exec commands (max %d)!", MAX_EXEC_COMMANDS);
            }

            continue;
        }

        if (strncmp(start, "terminal", 8) == 0 && (start[8] == ' ' || start[8] == '\t' || start[8] == '=' || start[8] == '\0')) {
            char *cmd = start + 8;
            while (*cmd == ' ' || *cmd == '\t') {
                cmd++;
            }

            if (*cmd == '=') {
                cmd++;
                while (*cmd == ' ' || *cmd == '\t') {
                    cmd++;
                }
            }

            if (*cmd == '\0') {
                wlr_log(WLR_ERROR, "Config: terminal command is empty!");
                continue;
            }

            char command[512];
            const char *cmd_start = cmd;
            size_t cmd_len;

            if (*cmd == '"') {
                cmd_start = cmd + 1;
                const char *end_quote = strchr(cmd_start, '"');
                if (!end_quote) {
                    wlr_log(WLR_ERROR, "Config: terminal command is missing a closing quote?");
                    continue;
                }
                cmd_len = end_quote - cmd_start;
            } else {
                cmd_len = strlen(cmd_start);
                while (cmd_len > 0 && (cmd_start[cmd_len - 1] == ' ' || cmd_start[cmd_len - 1] == '\t')) {
                    cmd_len--;
                }
            }

            if (cmd_len >= sizeof(command)) {
                wlr_log(WLR_ERROR, "Config: terminal command is too long!");
                continue;
            }
            snprintf(command, sizeof(command), "%.*s", (int)cmd_len, cmd_start);

            if (config->terminal_command) {
                free(config->terminal_command);
            }
            config->terminal_command = strdup(command);
            if (config->terminal_command) {
                wlr_log(WLR_INFO, "Config: terminal set to '%s'.", config->terminal_command);
            } else {
                wlr_log(WLR_ERROR, "Config: failed to allocate memory for terminal!");
            }

            continue;
        }

        char *sep = strchr(start, '=');
        if (!sep) {
            sep = strpbrk(start, " \t");
        }
        if (sep) {
            *sep = '\0';
            char *key = start;
            char *value = sep + 1;

            char *key_end = key + strlen(key) - 1;
            while (key_end >= key && (*key_end == ' ' || *key_end == '\t')) {
                *key_end = '\0';
                key_end--;
            }

            if (*key == '\0') {
                continue;
            }

            while (*value == ' ' || *value == '\t') {
                value++;
            }

            if (*value == '"') {
                value++;
                char *end_quote = strchr(value, '"');
                if (end_quote) {
                    *end_quote = '\0';
                } else {
                    wlr_log(WLR_ERROR, "Config: value for '%s' is missing a closing quote?", key);
                }
            } else {
                char *value_end = value + strlen(value) - 1;
                while (value_end >= value && (*value_end == ' ' || *value_end == '\t')) {
                    *value_end = '\0';
                    value_end--;
                }
            }

            if (*value == '\0') {
                continue;
            }

            if (strcmp(key, "padding") == 0) {
                char *endptr;
                long val = strtol(value, &endptr, 10);
                if (*endptr == '\0' && val >= 0 && val <= 1000) {
                    config->padding = (int)val;
                    wlr_log(WLR_INFO, "Config: padding set to %d.", config->padding);
                }
            } else if (strcmp(key, "idle_timeout") == 0) {
                char *endptr;
                unsigned long val = strtoul(value, &endptr, 10);
                if (*endptr == '\0' && val <= UINT32_MAX) {
                    config->idle_timeout_ms = (uint32_t)val;
                    wlr_log(WLR_INFO, "Config: idle_timeout set to %u ms.", config->idle_timeout_ms);
                }
            } else if (strcmp(key, "volume_up") == 0 ||
                       strcmp(key, "volume_down") == 0 ||
                       strcmp(key, "mute") == 0) {
                char **target = (strcmp(key, "volume_up") == 0)
                    ? &config->volume_up_command
                    : (strcmp(key, "volume_down") == 0)
                    ? &config->volume_down_command
                    : &config->mute_command;
                char *dup = strdup(value);
                if (dup) {
                    if (*target) {
                        free(*target);
                    }
                    *target = dup;
                    wlr_log(WLR_INFO, "Config: %s set to '%s'.", key, *target);
                } else {
                    wlr_log(WLR_ERROR, "Config: failed to allocate memory for %s!", key);
                }
            } else if (strcmp(key, "hacks") == 0) {
                char *endptr;
                long val = strtol(value, &endptr, 10);
                if (*endptr == '\0' && (val == 0 || val == 1)) {
                    config->hacks_enabled = (int)val;
                    wlr_log(WLR_INFO, "Config: hacks set to %d.", config->hacks_enabled);
                }
            } else if (strcmp(key, "notifications") == 0) {
                char *endptr;
                long val = strtol(value, &endptr, 10);
                if (*endptr == '\0' && (val == 0 || val == 1)) {
                    config->notifications_enabled = (int)val;
                    wlr_log(WLR_INFO, "Config: notifications set to %d.", config->notifications_enabled);
                }
            } else if (strcmp(key, "hot_corner") == 0) {
                char *endptr;
                long val = strtol(value, &endptr, 10);
                if (*endptr == '\0' && (val == 0 || val == 1)) {
                    config->hot_corner_enabled = (int)val;
                    wlr_log(WLR_INFO, "Config: hot_corner set to %d.", config->hot_corner_enabled);
                }
            } else if (strcmp(key, "hot_corner_threshold") == 0) {
                char *endptr;
                long val = strtol(value, &endptr, 10);
                if (*endptr == '\0' && val >= 1 && val <= 10000) {
                    config->hot_corner_threshold = (int)val;
                    wlr_log(WLR_INFO, "Config: hot_corner_threshold set to %d.", config->hot_corner_threshold);
                }
            } else if (strcmp(key, "drm_devices") == 0 ||
                       strcmp(key, "render_device") == 0) {
                char **target = (strcmp(key, "drm_devices") == 0)
                    ? &config->drm_devices
                    : &config->render_device;

                bool valid = value[0] == '/';
                for (const char *p = value; valid && *p != '\0'; p++) {
                    if (*p == ':' && p[1] != '/') {
                        valid = false;
                    }
                }

                if (!valid) {
                    wlr_log(WLR_ERROR, "Config: %s wants absolute device paths.", key);
                } else {
                    char *dup = strdup(value);
                    if (dup) {
                        if (*target) {
                            free(*target);
                        }
                        *target = dup;
                        wlr_log(WLR_INFO, "Config: %s set to '%s'.", key, *target);
                    } else {
                        wlr_log(WLR_ERROR, "Config: failed to allocate memory for %s!", key);
                    }
                }
            } else if (strcmp(key, "renderer") == 0) {
                if (strcmp(value, "gles2") != 0 &&
                    strcmp(value, "vulkan") != 0 &&
                    strcmp(value, "pixman") != 0) {
                    wlr_log(WLR_ERROR, "Config: renderer must be one of "
                                       "gles2, vulkan or pixman, not '%s'.",
                            value);
                } else {
                    char *dup = strdup(value);
                    if (dup) {
                        if (config->renderer_name) {
                            free(config->renderer_name);
                        }
                        config->renderer_name = dup;
                        wlr_log(WLR_INFO, "Config: renderer set to '%s'.",
                                config->renderer_name);
                    } else {
                        wlr_log(WLR_ERROR, "Config: failed to allocate memory for renderer!");
                    }
                }
            }
        }
    }

    fclose(file);
}

void poison_config_apply_filter(struct poison_config *config) {
    if (!config->hacks_enabled) {
        return;
    }

    const char *lib_paths[] = {
        "/usr/local/lib/poison-filter.so",
        "/usr/lib/poison-filter.so",
        NULL};

    for (int i = 0; lib_paths[i] != NULL; i++) {
        if (access(lib_paths[i], F_OK) == 0) {
            setenv("LIBDECOR_PLUGIN_DIR", "/", 1);
            setenv("LD_PRELOAD", lib_paths[i], 1);
            wlr_log(WLR_INFO, "LD_PRELOAD hack: %s.", lib_paths[i]);
            return;
        }
    }

#if defined(__linux__)
    char exe_path[4096];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len > 0) {
        exe_path[len] = '\0';
        char *last_slash = strrchr(exe_path, '/');
        if (last_slash) {
            size_t remaining = sizeof(exe_path) - (last_slash - exe_path + 1);
            snprintf(last_slash + 1, remaining, "poison-filter.so");
            if (access(exe_path, F_OK) == 0) {
                setenv("LD_PRELOAD", exe_path, 1);
                wlr_log(WLR_INFO, "LD_PRELOAD hack: %s.", exe_path);
                return;
            }
        }
    }
#endif
}

void poison_config_execute_commands(struct poison_config *config) {
    for (int i = 0; i < config->exec_command_count; i++) {
        if (!config->exec_commands[i]) {
            continue;
        }

        pid_t pid = fork();
        if (pid == 0) {
            setsid();

            int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                if (devnull > 2) {
                    close(devnull);
                }
            }

            int max_fds = sysconf(_SC_OPEN_MAX);
            if (max_fds < 0) {
                max_fds = 1024;
            }

            for (int fd_iter = 3; fd_iter < max_fds; fd_iter++) {
                close(fd_iter);
            }

            execl("/bin/sh", "/bin/sh", "-c", config->exec_commands[i], (void *)NULL);
            _exit(1);
        } else if (pid < 0) {
            wlr_log(WLR_ERROR, "Failed to fork for exec command '%s'!", config->exec_commands[i]);
        } else {
            wlr_log(WLR_INFO, "Executed config command '%s' (pid %d).", config->exec_commands[i], pid);
        }

        free(config->exec_commands[i]);
        config->exec_commands[i] = NULL;
    }
    config->exec_command_count = 0;
}

void poison_config_cleanup(struct poison_config *config) {
    if (config->drm_devices) {
        free(config->drm_devices);
        config->drm_devices = NULL;
    }

    if (config->render_device) {
        free(config->render_device);
        config->render_device = NULL;
    }

    if (config->renderer_name) {
        free(config->renderer_name);
        config->renderer_name = NULL;
    }

    if (config->terminal_command) {
        free(config->terminal_command);
        config->terminal_command = NULL;
    }

    if (config->volume_up_command) {
        free(config->volume_up_command);
        config->volume_up_command = NULL;
    }

    if (config->volume_down_command) {
        free(config->volume_down_command);
        config->volume_down_command = NULL;
    }

    if (config->mute_command) {
        free(config->mute_command);
        config->mute_command = NULL;
    }

    for (int i = 0; i < MAX_EXEC_COMMANDS; i++) {
        if (config->exec_commands[i]) {
            free(config->exec_commands[i]);
            config->exec_commands[i] = NULL;
        }
    }
}
