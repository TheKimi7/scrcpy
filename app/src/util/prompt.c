#include "prompt.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
# include <windows.h>
#else
# include <poll.h>
# include <unistd.h>
#endif

/**
 * Read one line from the terminal, interruptible
 *
 * Wait for the standard input to become readable, but wake up regularly to
 * check if the interruptor has been triggered, so that scrcpy does not hang on
 * exit while a question is pending.
 *
 * Anything typed after the first newline is discarded, so that a multi-line
 * paste does not answer several questions at once.
 *
 * Return false on interruption, end of file or error.
 */
static bool
sc_prompt_read_line(struct sc_intr *intr, char *buf, size_t size);

#ifdef _WIN32

bool
sc_prompt_is_available(void) {
    // GetConsoleMode() fails on anything which is not a console, so it also
    // covers redirections and a scrcpy started without any console attached
    DWORD mode;
    return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode)
        && GetConsoleMode(GetStdHandle(STD_ERROR_HANDLE), &mode);
}

/**
 * Discard the pending console events which are not a key press
 *
 * Waiting on the console input handle wakes up on any input record, including
 * key releases, mouse moves and focus changes. Remove them from the queue so
 * that waiting does not spin, and so that the blocking read below is only
 * started once the user actually typed something.
 *
 * Return true if a key press is pending.
 */
static bool
sc_prompt_skip_non_key_events(HANDLE input) {
    for (;;) {
        INPUT_RECORD record;
        DWORD count;
        if (!PeekConsoleInput(input, &record, 1, &count) || !count) {
            return false;
        }

        if (record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown) {
            return true;
        }

        if (!ReadConsoleInput(input, &record, 1, &count)) {
            return false;
        }
    }
}

static bool
sc_prompt_read_line(struct sc_intr *intr, char *buf, size_t size) {
    assert(size > 0);

    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    size_t len = 0;

    for (;;) {
        if (intr && sc_intr_is_interrupted(intr)) {
            return false;
        }

        DWORD result = WaitForSingleObject(input, 100);
        if (result == WAIT_TIMEOUT) {
            // Re-check the interruptor
            continue;
        }

        if (result != WAIT_OBJECT_0) {
            return false;
        }

        if (!sc_prompt_skip_non_key_events(input)) {
            continue;
        }

        // A key press is pending. The console echoes the characters and
        // handles line editing, and returns the whole line at once, so this
        // blocks until the user presses Enter.
        DWORD r;
        if (!ReadFile(input, buf + len, (DWORD) (size - len - 1), &r,
                      NULL)) {
            return false;
        }

        if (!r) {
            // EOF (Ctrl+Z then Enter)
            return false;
        }

        len += r;
        buf[len] = '\0';

        char *newline = memchr(buf, '\n', len);
        if (newline) {
            // Ignore anything typed after the end of the line
            *newline = '\0';
            return true;
        }

        if (len == size - 1) {
            // The line does not fit: return it truncated, it will be rejected
            // as an invalid answer
            return true;
        }
    }
}

#else

bool
sc_prompt_is_available(void) {
    if (!isatty(STDIN_FILENO) || !isatty(STDERR_FILENO)) {
        return false;
    }

    // Only prompt from the foreground process group: reading from a
    // background job would raise SIGTTIN
    return tcgetpgrp(STDIN_FILENO) == getpgrp();
}

static bool
sc_prompt_read_line(struct sc_intr *intr, char *buf, size_t size) {
    assert(size > 0);

    size_t len = 0;

    for (;;) {
        if (intr && sc_intr_is_interrupted(intr)) {
            return false;
        }

        struct pollfd pfd = {
            .fd = STDIN_FILENO,
            .events = POLLIN,
        };

        int r = poll(&pfd, 1, 100);
        if (r < 0) {
            if (errno == EINTR && intr) {
                // The interruptor is checked on the next iteration
                continue;
            }
            // Without an interruptor, nothing else would report that scrcpy
            // must quit, so give up on the first signal
            return false;
        }

        if (r == 0) {
            // Timeout, re-check the interruptor
            continue;
        }

        // Read from the file descriptor rather than from the FILE*: stdio
        // could buffer more than one line, and the extra bytes would be
        // invisible to poll()
        ssize_t n = read(STDIN_FILENO, buf + len, size - len - 1);
        if (n < 0) {
            if (errno == EINTR && intr) {
                continue;
            }
            return false;
        }

        if (n == 0) {
            // EOF (Ctrl+D)
            return false;
        }

        len += n;
        buf[len] = '\0';

        char *newline = memchr(buf, '\n', len);
        if (newline) {
            // Ignore anything typed after the end of the line
            *newline = '\0';
            return true;
        }

        if (len == size - 1) {
            // The line does not fit: return it truncated, it will be rejected
            // as an invalid answer
            return true;
        }
    }
}

#endif

bool
sc_prompt_select_index(struct sc_intr *intr, const char *label, size_t count,
                       size_t *idx_out) {
    assert(count > 1);

    char buf[256];

    for (;;) {
        fprintf(stderr, "Select a %s [1-%" SC_PRIsizet "] "
                        "(or 'q' to quit): ", label, count);
        fflush(stderr);

        if (!sc_prompt_read_line(intr, buf, sizeof(buf))) {
            // Interrupted, EOF (Ctrl+D) or error
            fprintf(stderr, "\n");
            return false;
        }

        char *line = buf;
        while (*line == ' ' || *line == '\t') {
            ++line;
        }

        if (*line == 'q' || *line == 'Q') {
            return false;
        }

        char *endptr;
        errno = 0;
        long value = strtol(line, &endptr, 10);

        // Accept only a whole number, optionally surrounded by blanks
        bool is_number = endptr != line && !errno;
        if (is_number) {
            while (*endptr == ' ' || *endptr == '\t' || *endptr == '\r'
                    || *endptr == '\n') {
                ++endptr;
            }
            is_number = *endptr == '\0';
        }

        if (!is_number || value < 1 || (size_t) value > count) {
            if (*line != '\r' && *line != '\n' && *line != '\0') {
                // Stay quiet if the user just pressed Enter
                fprintf(stderr, "Invalid selection, expected a number between "
                                "1 and %" SC_PRIsizet "\n", count);
            }
            continue;
        }

        *idx_out = value - 1;
        return true;
    }
}
