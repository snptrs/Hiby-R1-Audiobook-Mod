/* scan.h — audiobook library scanner.
 *
 * Walks the Audiobooks directory, groups files into books by folder,
 * reads tags, computes durations, sorts tracks, and upserts everything
 * into the library database. Idempotent: re-scanning only updates
 * changed books.
 */

#ifndef AUDIOBOOK_SCAN_H
#define AUDIOBOOK_SCAN_H

#include "library.h"

/* Scan progress callback. stage: 0=starting, 1=scanning folder,
 * 2=processing book, 3=indexing search, 4=cleaning orphans, 5=done.
 * current/current_total for progress display. */
typedef void (*scan_progress_cb)(int stage, int current, int total,
                                 const char *info, void *ctx);

/* Scan one root and upsert its books/tracks/chapters in a single transaction.
 * Does NOT run orphan cleanup or VACUUM — audiobook_scan_all owns those.
 *
 * kind selects the grouping: LIB_KIND_BOOK treats each folder containing audio
 * as one book with those files as tracks; LIB_KIND_PODCAST treats each FILE as
 * its own row (an episode) with the folder as its show.
 * label is stored in library_roots; NULL picks a default from kind.
 * Returns 0 on success, -1 on error. */
int audiobook_scan_root(sqlite3 *db, const char *root_path, int kind,
                        const char *label,
                        scan_progress_cb progress, void *ctx);

/* audiobook_scan_root(..., LIB_KIND_BOOK, "Audiobooks", ...). */
int audiobook_scan_library(sqlite3 *db, const char *root_path,
                           scan_progress_cb progress, void *ctx);

/* Scan every configured root (Audiobooks, then Podcasts), then run orphan
 * cleanup once and compact the DB. Each root gets its own transaction; the
 * write lock is non-recursive, so one transaction cannot span both.
 *
 * A root that does not exist is skipped silently — many users will never
 * create /Podcasts. Returns -1 only when a root that DOES exist failed to
 * scan, so a missing folder never surfaces as a scan error. */
int audiobook_scan_all(sqlite3 *db, scan_progress_cb progress, void *ctx);

/* Clean up orphaned DB entries: books whose root_path is no longer a
 * directory, tracks whose file is gone, and books left with no tracks.
 *
 * Only books under one of `roots` are considered. That scoping is a safety
 * requirement, not an optimization: if a root is missing (renamed folder,
 * unmounted card) every book under it would look dead, and deletion also
 * removes the .pos and bookmark sidecars that hold the authoritative resume
 * positions. Pass only roots that actually scanned. Returns items removed. */
int audiobook_cleanup_orphans_scoped(sqlite3 *db, const char *const *roots,
                                     int n_roots, scan_progress_cb progress,
                                     void *ctx);

/* Convenience wrapper scoping cleanup to all configured roots. */
int audiobook_cleanup_orphans(sqlite3 *db, scan_progress_cb progress,
                              void *ctx);

#endif /* AUDIOBOOK_SCAN_H */