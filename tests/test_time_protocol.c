#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "time_protocol.h"

static void parse(const char *line, const char *expected, bool set, int64_t seconds)
{
    roundie_time_frame_t frame = {0};
    for (size_t i = 0; line[i] != '\0'; i++) {
        assert(roundie_time_feed(&frame, line[i]) == (line[i] == '\n'));
    }
    roundie_time_command_t command;
    assert(strcmp(roundie_time_parse(&frame, &command), expected) == 0);
    if (strcmp(expected, "OK") == 0) {
        assert(command.set == set);
        assert(command.seconds == seconds);
        assert(strcmp(command.id, "1234abcd") == 0);
    }
}

int main(void)
{
    parse("!RT1 1234abcd GET\n", "OK", false, 0);
    parse("!RT1 1234abcd RESTART\n", "OK", false, 0);
    parse("!RT1 1234abcd SET 946684800\n", "OK", true, 946684800);
    parse("!RT1 1234abcd SET 4102444799\n", "OK", true, 4102444799);
    const char *bad[] = {
        "", "log line", "!RT2 1234abcd GET", "!RT1 1234ABCD GET",
        "!RT1 1234abcd GET ", "!RT1 1234abcd SET -1", "!RT1 1234abcd SET +946684800",
        "!RT1 1234abcd SET 946684800x", "!RT1 1234abcd SET 946684800 extra",
        "!RT1 1234abcd GET SET", "!RT1 1234abc GET",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        parse(bad[i], "BAD_COMMAND", false, 0);
    }
    parse("!RT1 1234abcd SET 946684799\n", "RANGE", false, 0);
    parse("!RT1 1234abcd SET 4102444800\n", "RANGE", false, 0);
    parse("!RT1 1234abcd SET 99999999999999999\n", "RANGE", false, 0);
    parse("!RT1 1234abcd SET \n", "RANGE", false, 0);
    parse("!RT1 1234abcd GET\r\n", "FRAME", false, 0);
    roundie_time_frame_t frame = {0};
    const char *start = "!RT1 1234abcd SET ";
    for (size_t i = 0; start[i] != '\0'; i++) {
        assert(!roundie_time_feed(&frame, start[i]));
    }
    for (unsigned i = 0; i < 500; i++) {
        assert(!roundie_time_feed(&frame, '9'));
    }
    assert(frame.used == ROUNDIE_TIME_LINE_CAPACITY - 1);
    assert(frame.discard);
    assert(roundie_time_feed(&frame, '\n'));
    roundie_time_command_t command;
    assert(strcmp(roundie_time_parse(&frame, &command), "FRAME") == 0);
    frame = (roundie_time_frame_t){0};
    for (size_t i = 0; start[i] != '\0'; i++) {
        roundie_time_feed(&frame, start[i]);
    }
    frame.discard = true; /* Worker expires partial frames after two seconds. */
    const char *suffix = "946684800\n";
    for (size_t i = 0; suffix[i] != '\0'; i++) {
        roundie_time_feed(&frame, suffix[i]);
    }
    assert(strcmp(roundie_time_parse(&frame, &command), "FRAME") == 0);
    puts("time protocol tests passed");
    return 0;
}
