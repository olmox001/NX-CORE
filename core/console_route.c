/*
 * kernel/core/console_route.c
 *
 * Provider-neutral stdout/stderr destination selection.  A graphical process
 * may own a pixel surface without changing where its inherited console stream
 * goes: self-rendered output targets the controlling terminal, never the
 * process's own pixel window.  A detached graphical process has no ctty and
 * therefore remains UART-only through window_text_write(-1, ...).
 */
#include <kernel/object.h>

int object_console_output_window(int self_rendered, int own_window,
                                 int ctty_window) {
  if (self_rendered != 0)
    return ctty_window > 0 ? ctty_window : -1;
  if (own_window > 0)
    return own_window;
  return ctty_window > 0 ? ctty_window : -1;
}
