/* Host regression test for orphan cleanup scoping and the zero-tracks pass.
 *
 * The scoping is a data-loss guard, not an optimization. audiobook_cleanup_orphans
 * decides a book is dead when its root_path does not stat as a directory, and
 * deleting a book also removes its .pos and SD bookmark files, which hold the
 * authoritative resume position. So an unscoped cleanup run while /Audiobooks is
 * missing (renamed folder, unmounted card, or a card holding only podcasts)
 * would silently destroy every audiobook's progress. Before the roots parameter
 * existed, the only thing preventing that was the incidental early return when
 * the single root failed to stat.
 *
 * Build (host; the .ps1 scripts are Windows/Zig only):
 *
 *   clang -O1 -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 \
 *     -DSQLITE_OMIT_LOAD_EXTENSION=1 -DSQLITE_ENABLE_FTS5=1 \
 *     -DSQLITE_OMIT_DEPRECATED=1 -DSQLITE_TEMP_STORE=2 \
 *     -o scan_orphan_test -Iaudiobook_app -Ivendor \
 *     tools/scan_orphan_test_main.c audiobook_app/library.c \
 *     audiobook_app/scan.c audiobook_app/tags.c audiobook_app/bookmark_sd.c \
 *     audiobook_app/library_test_stubs.c audiobook_app/sqlite3.c -lpthread
 *
 * Run: scan_orphan_test <tree_dir> <db_path>
 * where <tree_dir> holds Audiobooks/ and Podcasts/ subtrees (the caller builds
 * them; see the shell driver in the plan's verification section). The test
 * renames the Audiobooks subtree aside partway through and restores it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "library.h"
#include "scan.h"

static int fail;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); fail = 1; } \
    else printf("ok: %s\n", msg); \
} while (0)

static int count_kind(sqlite3 *db, int kind) {
    sqlite3_stmt *st = NULL;
    int n = -1;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM books WHERE kind=?",
                           -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int(st, 1, kind);
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

static int scalar(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    int n = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: scan_orphan_test TREE_DIR DB_PATH\n");
        return 2;
    }
    char ab[1024], pc[1024], ab_moved[1024], cmd[2400];
    snprintf(ab, sizeof(ab), "%s/Audiobooks", argv[1]);
    snprintf(pc, sizeof(pc), "%s/Podcasts", argv[1]);
    snprintf(ab_moved, sizeof(ab_moved), "%s/Audiobooks_moved", argv[1]);

    sqlite3 *db = NULL;
    if (audiobook_db_open(argv[2], &db) < 0) {
        printf("FAIL: could not open %s\n", argv[2]);
        return 1;
    }

    CHECK(audiobook_scan_root(db, ab, LIB_KIND_BOOK, NULL, NULL, NULL) == 0,
          "audiobook root scans");
    CHECK(audiobook_scan_root(db, pc, LIB_KIND_PODCAST, NULL, NULL, NULL) == 0,
          "podcast root scans");

    int books = count_kind(db, LIB_KIND_BOOK);
    int eps = count_kind(db, LIB_KIND_PODCAST);
    printf("   seeded: %d books, %d episodes\n", books, eps);
    CHECK(books > 0 && eps > 0, "both kinds present");

    /* Both roots present: cleanup must delete nothing. */
    const char *both[] = { ab, pc };
    audiobook_cleanup_orphans_scoped(db, both, 2, NULL, NULL);
    CHECK(count_kind(db, LIB_KIND_BOOK) == books
          && count_kind(db, LIB_KIND_PODCAST) == eps,
          "cleanup with both roots present deletes nothing");

    /* THE GUARD. Audiobooks root disappears; only the podcast root scanned, so
     * only it is passed in. Every audiobook row must survive. */
    snprintf(cmd, sizeof(cmd), "mv '%s' '%s'", ab, ab_moved);
    if (system(cmd) != 0) { printf("FAIL: could not move tree aside\n"); return 1; }

    const char *podcast_only[] = { pc };
    audiobook_cleanup_orphans_scoped(db, podcast_only, 1, NULL, NULL);
    CHECK(count_kind(db, LIB_KIND_BOOK) == books,
          "audiobooks survive cleanup when their root is missing and unscanned");
    CHECK(scalar(db, "SELECT COUNT(*) FROM progress") >= 0,
          "progress table intact");

    /* Same missing root, but now wrongly claimed as scanned: this is what the
     * scoping protects against, and it should wipe them. Asserting the
     * destructive behaviour documents exactly why callers must pass only roots
     * that actually scanned. */
    audiobook_cleanup_orphans_scoped(db, both, 2, NULL, NULL);
    CHECK(count_kind(db, LIB_KIND_BOOK) == 0,
          "audiobooks ARE deleted if a missing root is claimed as scanned");
    CHECK(count_kind(db, LIB_KIND_PODCAST) == eps,
          "podcast episodes unaffected by the audiobook root going away");

    snprintf(cmd, sizeof(cmd), "mv '%s' '%s'", ab_moved, ab);
    if (system(cmd) != 0) printf("warning: could not restore tree\n");

    /* Zero-tracks pass: delete one episode FILE. Its show folder still exists,
     * so the book row passes the S_ISDIR test and only this pass removes it. */
    sqlite3_stmt *st = NULL;
    char victim[1024] = "";
    int victim_id = -1;
    if (sqlite3_prepare_v2(db,
            "SELECT b.book_id,t.path FROM books b JOIN tracks t "
            "ON t.book_id=b.book_id WHERE b.kind=1 LIMIT 1",
            -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
        victim_id = sqlite3_column_int(st, 0);
        snprintf(victim, sizeof(victim), "%s",
                 (const char *)sqlite3_column_text(st, 1));
    }
    sqlite3_finalize(st);
    CHECK(victim_id > 0 && victim[0], "picked an episode to delete");

    if (remove(victim) != 0) { printf("FAIL: could not delete %s\n", victim); return 1; }
    printf("   deleted file: %s\n", victim);

    audiobook_cleanup_orphans_scoped(db, podcast_only, 1, NULL, NULL);
    CHECK(count_kind(db, LIB_KIND_PODCAST) == eps - 1,
          "exactly one episode row removed after its file was deleted");

    char q[256];
    snprintf(q, sizeof(q),
             "SELECT COUNT(*) FROM books WHERE book_id=%d", victim_id);
    CHECK(scalar(db, q) == 0, "the deleted episode's row is gone");
    snprintf(q, sizeof(q),
             "SELECT COUNT(*) FROM book_search WHERE book_id=%d", victim_id);
    CHECK(scalar(db, q) == 0, "its FTS row is gone too");
    CHECK(scalar(db,
        "SELECT COUNT(*) FROM books b WHERE NOT EXISTS "
        "(SELECT 1 FROM tracks t WHERE t.book_id=b.book_id)") == 0,
        "no book rows left without tracks");

    audiobook_db_close(db);
    printf(fail ? "\nRESULT: FAIL\n" : "\nRESULT: PASS\n");
    return fail;
}
