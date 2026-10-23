#include <assert.h>
#include <string.h>
#include "grok_protocol.h"

int main(void) {
    assert(grok_device_id_valid("passport-01"));
    assert(!grok_device_id_valid("../device"));
    assert(!grok_device_id_valid("-device"));
    assert(!grok_device_id_valid(""));
    assert(!grok_device_id_valid(NULL));
    assert(grok_message_valid("Hello\nArthur"));
    assert(!grok_message_valid("\xe4\xbd\xa0\xe5\xa5\xbd"));
    assert(!grok_message_valid("bad\rtext"));
    assert(!grok_message_valid("\x1b[2J"));
    char text[GROK_MESSAGE_MAX + 2];
    memset(text, 'a', sizeof(text));
    text[GROK_MESSAGE_MAX] = '\0';
    assert(grok_message_valid(text));
    text[GROK_MESSAGE_MAX] = 'a';
    text[GROK_MESSAGE_MAX + 1] = '\0';
    assert(!grok_message_valid(text));
    assert(!grok_brightness_valid(9));
    assert(grok_brightness_valid(10));
    assert(grok_brightness_valid(100));
    assert(!grok_brightness_valid(101));
    assert(grok_json_depth_valid("{\"command\":{\"text\":\"[[[[[\"}}"));
    assert(grok_json_depth_valid("{\"text\":\"an escaped \\\" quote\"}"));
    assert(!grok_json_depth_valid("[[[[[0]]]]]"));
    assert(!grok_json_depth_valid("{\"text\":\"unterminated}"));
    assert(!grok_json_depth_valid(NULL));
    grok_command_t command = {
        .id = "01234567-89ab-cdef-0123-456789abcdef",
        .kind = GROK_COMMAND_SHOW_MESSAGE,
        .text = "Hello",
    };
    assert(grok_command_valid(&command));
    assert(grok_command_is_duplicate(&command, command.id));
    assert(!grok_command_is_duplicate(&command, ""));
    assert(!grok_command_id_valid("0123456789ab-cdef-0123-456789abcdef"));
    command.kind = GROK_COMMAND_SET_BACKLIGHT;
    command.brightness = 0;
    assert(!grok_command_valid(&command));
    command.brightness = 40;
    assert(grok_command_valid(&command));
    command.id[0] = 'Z';
    assert(!grok_command_valid(&command));
    return 0;
}
