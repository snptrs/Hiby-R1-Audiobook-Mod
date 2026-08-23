/* Stubs for the standalone library_test harness.
 *
 * scan.c calls two functions that live in translation units the harness has no
 * business linking: cover_precache (cover.c, which needs dlopen'd libjpeg and
 * libz) and pos_remove_sd (player.c, which pulls in ALSA, the decoders and the
 * player thread). Both are side-effect-only from the scanner's point of view:
 * a cover pre-decode that does not happen just means a later lazy decode, and
 * the .pos file the harness would delete belongs to a device, not the host.
 *
 * Only ever linked into library_test, never into the shipped hook library.
 */

#include <stdio.h>

int cover_precache(const char *cover_path, int px) {
    (void)cover_path;
    (void)px;
    return 0;
}

void pos_remove_sd(int book_id) {
    (void)book_id;
}
