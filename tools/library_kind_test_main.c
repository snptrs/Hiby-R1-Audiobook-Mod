/* Host regression test for the books.kind column: schema bootstrap, the
 * additive migration from a pre-kind (v2-shaped) DB, repeated opens, and the
 * kind-filtered list queries.
 *
 * The repeated-open case is the important one. The kind column is added by an
 * ALTER TABLE that must live OUTSIDE SCHEMA_SQL, because audiobook_db_open
 * treats a schema-exec failure as fatal and ALTER TABLE ADD COLUMN reports
 * "duplicate column name" on every run after the first. Getting that wrong
 * makes the app stop opening its library on the second boot.
 *
 * Build (host, macOS or Linux; the .ps1 scripts are Windows/Zig only):
 *
 *   clang -O1 -DSQLITE_THREADSAFE=2 -DSQLITE_DEFAULT_MEMSTATUS=0 \
 *     -DSQLITE_OMIT_LOAD_EXTENSION=1 -DSQLITE_ENABLE_FTS5=1 \
 *     -DSQLITE_OMIT_DEPRECATED=1 -DSQLITE_TEMP_STORE=2 \
 *     -o library_kind_test -Iaudiobook_app \
 *     tools/library_kind_test_main.c audiobook_app/library.c \
 *     audiobook_app/bookmark_sd.c audiobook_app/sqlite3.c -lpthread
 *
 * Run: library_kind_test /tmp/kind.db   (the DB must not already exist)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "library.h"

static int fail;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); fail = 1; } \
    else printf("ok: %s\n", msg); \
} while (0)

static int name_count;
static int name_cb(const char *n, void *ctx) {
    (void)ctx; printf("    %s\n", n); name_count++; return 0;
}
static int book_count;
static int book_cb(const audiobook_book_t *b, void *ctx) {
    (void)ctx;
    printf("    kind=%d series_number=%.1f title=%s\n",
           b->kind, b->series_number, b->title);
    book_count++;
    return 0;
}

/* books as an older build left it: no kind column. */
static const char *PRE_MIGRATION_BOOKS =
    "CREATE TABLE books(book_id INTEGER PRIMARY KEY,"
    "book_key TEXT NOT NULL UNIQUE,title TEXT NOT NULL,sort_title TEXT NOT NULL,"
    "author_id INTEGER,narrator TEXT,series_id INTEGER,series_number REAL,"
    "root_path TEXT NOT NULL,cover_path TEXT,cover_cache_path TEXT,"
    "total_duration_ms INTEGER NOT NULL DEFAULT 0,"
    "track_count INTEGER NOT NULL DEFAULT 0,fingerprint TEXT,date_added INTEGER,"
    "date_modified INTEGER,last_played_at INTEGER,"
    "completed INTEGER NOT NULL DEFAULT 0,completed_at INTEGER,"
    "playback_speed REAL NOT NULL DEFAULT 1.0);";

static const char *FIXTURE =
    "INSERT INTO series(series_id,sort_name,display_name) VALUES(1,'a show','A Show');"
    "INSERT INTO books(book_key,title,sort_title,root_path,total_duration_ms,"
    "track_count,playback_speed,series_id,series_number,kind) VALUES"
    "('bk1','A Book','a book','/r/Audiobooks/A',1000,1,1.0,NULL,0,0),"
    "('ep1','Ep One','ep one','/r/Podcasts/A Show',100,1,1.0,1,1,1),"
    "('ep2','Ep Two','ep two','/r/Podcasts/A Show',200,1,1.0,1,2,1),"
    "('ep3','Ep Three','ep three','/r/Podcasts/A Show',300,1,1.0,1,3,1);";

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: library_kind_test DBPATH\n");
        return 2;
    }
    const char *path = argv[1];

    sqlite3 *raw = NULL;
    if (sqlite3_open(path, &raw) != SQLITE_OK) {
        printf("FAIL: could not create %s\n", path);
        return 1;
    }
    sqlite3_exec(raw, PRE_MIGRATION_BOOKS, NULL, NULL, NULL);
    sqlite3_close(raw);
    printf("-- seeded a pre-migration (v2-shaped) books table\n");

    sqlite3 *db = NULL;
    CHECK(audiobook_db_open(path, &db) == 0, "first open migrates and succeeds");
    if (!db) return 1;

    /* The brick case: ALTER TABLE now errors, which must not be fatal. */
    audiobook_db_close(db); db = NULL;
    CHECK(audiobook_db_open(path, &db) == 0, "second open succeeds");
    audiobook_db_close(db); db = NULL;
    CHECK(audiobook_db_open(path, &db) == 0, "third open succeeds");
    if (!db) return 1;

    sqlite3_exec(db, FIXTURE, NULL, NULL, NULL);

    printf("-- Titles excludes episodes\n");
    book_count = 0;
    audiobook_list_books(db, book_cb, NULL);
    CHECK(book_count == 1, "Titles lists only the audiobook");

    printf("-- Shows\n");
    name_count = 0;
    audiobook_list_shows(db, name_cb, NULL);
    CHECK(name_count == 1, "Shows lists exactly one show");

    printf("-- Series excludes podcast shows\n");
    name_count = 0;
    audiobook_list_series(db, name_cb, NULL);
    CHECK(name_count == 0, "Series excludes podcast shows");

    printf("-- Episodes of a show, reverse episode index\n");
    book_count = 0;
    audiobook_list_episodes_by_show(db, "A Show", book_cb, NULL);
    CHECK(book_count == 3, "all three episodes listed");

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT b.title FROM books b JOIN series s ON s.series_id=b.series_id "
        "WHERE s.display_name='A Show' AND b.kind=1 "
        "ORDER BY b.series_number DESC LIMIT 1", -1, &st, NULL);
    const char *first = NULL;
    if (sqlite3_step(st) == SQLITE_ROW)
        first = (const char *)sqlite3_column_text(st, 0);
    CHECK(first && strcmp(first, "Ep Three") == 0,
          "highest episode index sorts first");
    sqlite3_finalize(st);

    /* Guards the positional read at column 20 in fill_book_from_stmt. */
    printf("-- kind round-trips through the book struct\n");
    audiobook_book_t b;
    CHECK(audiobook_get_book_by_key(db, "ep2", &b) == 1
          && b.kind == LIB_KIND_PODCAST, "episode reads kind=1");
    CHECK(audiobook_get_book_by_key(db, "bk1", &b) == 1
          && b.kind == LIB_KIND_BOOK, "book reads kind=0");

    printf("-- Folders excludes podcast roots\n");
    name_count = 0;
    audiobook_list_folders(db, name_cb, NULL);
    CHECK(name_count == 1, "Folders lists only the audiobook root");

    audiobook_db_close(db);
    printf(fail ? "\nRESULT: FAIL\n" : "\nRESULT: PASS\n");
    return fail;
}
