#ifndef SC_PROMPT_H
#define SC_PROMPT_H

#include "common.h"

#include <stdbool.h>
#include <stddef.h>

#include "util/intr.h"

/**
 * Return true if a question may be asked on the terminal
 *
 * The question is printed on stderr (like the logs) and the answer is read
 * from stdin, so both must be attached to a terminal.
 *
 * On Unix, scrcpy must also run in the foreground process group: a background
 * job (scrcpy &) still has a terminal on stdin, but reading from it would
 * raise SIGTTIN.
 */
bool
sc_prompt_is_available(void);

/**
 * Ask the user to pick one item out of `count`, and write the index of the
 * answer (between 0 and count-1) to `*idx_out`
 *
 * The caller must have printed the items beforehand, numbered from 1 to
 * `count`. The `label` names what is being selected, as in "Select a device".
 *
 * An invalid answer is rejected and the question is asked again. Typing 'q' or
 * pressing Ctrl+D gives up.
 *
 * If `intr` is not NULL, the question is aborted once the interruptor is
 * triggered (typically on Ctrl+C). If it is NULL, the question is aborted as
 * soon as a signal is received instead, since nothing else would notice.
 *
 * Return false if no item was selected.
 */
bool
sc_prompt_select_index(struct sc_intr *intr, const char *label, size_t count,
                       size_t *idx_out);

#endif
