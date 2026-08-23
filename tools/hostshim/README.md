# Host typecheck shim

`ui.c`, `hook.c` and `render.c` include `<linux/input.h>` and `<linux/fb.h>`,
so they cannot be compiled on macOS at all. The shipped build also uses no
`-Wall`/`-Werror` (see `build_r1_audiobook_hook.ps1`), which means a missed
`switch` case over `ui_screen_t` or `list_mode_t` produces no diagnostic
anywhere, and several of those switches fail silently at runtime rather than
loudly (`collect_list_books`'s `default:` lists the whole library;
`rebuild_screen` has no `default:` and just never builds the cache).

These headers exist only so `clang -fsyntax-only -Wall -Wextra` can be pointed
at those files on a host, to get that diagnostic back. Values match the Linux
uapi but the structs are not layout-compatible and nothing here is ever linked
or shipped. Do not add them to any build script's include path.

Used by `tools/test_host_library_scan.sh`.
