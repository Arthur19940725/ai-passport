#include "grok_protocol.h"

#include <string.h>

static size_t bounded_length(const char *value, size_t limit) {
    if (!value) return limit;
    size_t length = 0;
    while (length < limit && value[length]) length++;
    return length;
}

bool grok_device_id_valid(const char *value) {
    const size_t length = bounded_length(value, GROK_DEVICE_ID_MAX + 1);
    if (length == 0 || length > GROK_DEVICE_ID_MAX) return false;
    if (value[0] == '-' || value[0] == '_') return false;
    for (size_t i = 0; i < length; i++) {
        const unsigned char c = (unsigned char)value[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    }
    return true;
}

bool grok_command_id_valid(const char *value) {
    if (bounded_length(value, GROK_COMMAND_ID_LEN + 1) != GROK_COMMAND_ID_LEN) return false;
    for (size_t i = 0; i < GROK_COMMAND_ID_LEN; i++) {
        const char c = value[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool grok_message_valid(const char *value) {
    const size_t length = bounded_length(value, GROK_MESSAGE_MAX + 1);
    if (length == 0 || length > GROK_MESSAGE_MAX) return false;
    for (size_t i = 0; i < length; i++) {
        const unsigned char c = (unsigned char)value[i];
        if (c != '\n' && (c < 32 || c > 126)) return false;
    }
    return true;
}

bool grok_brightness_valid(int value) {
    return value >= GROK_BRIGHTNESS_MIN && value <= GROK_BRIGHTNESS_MAX;
}

bool grok_command_valid(const grok_command_t *command) {
    if (!command || !grok_command_id_valid(command->id)) return false;
    switch (command->kind) {
    case GROK_COMMAND_SHOW_MESSAGE: return grok_message_valid(command->text);
    case GROK_COMMAND_CLEAR_MESSAGE: return true;
    case GROK_COMMAND_SET_BACKLIGHT: return grok_brightness_valid(command->brightness);
    default: return false;
    }
}

bool grok_command_is_duplicate(const grok_command_t *command, const char *last_id) {
    return command && grok_command_id_valid(command->id) &&
           grok_command_id_valid(last_id) && strcmp(command->id, last_id) == 0;
}

bool grok_json_depth_valid(const char *json) {
    if (!json) return false;
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (size_t i = 0; i < 1024; i++) {
        const char c = json[i];
        if (!c) return depth == 0 && !in_string;
        if (in_string) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') in_string = false;
        } else if (c == '"') {
            in_string = true;
        } else if (c == '{' || c == '[') {
            if (++depth > 4) return false;
        } else if (c == '}' || c == ']') {
            if (--depth < 0) return false;
        }
    }
    return false;
}
