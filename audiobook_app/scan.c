/* scan.c — audiobook library scanner.
 *
 * Walks the Audiobooks directory tree. Each leaf folder containing audio
 * files is a "book". Files are sorted by natural-numeric order (track2
 * before track10). Tags are read for metadata. Books are upserted into
 * the DB with ON CONFLICT(book_key) DO UPDATE — idempotent re-scans.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>
#include "scan.h"
#include "tags.h"
#include "cover.h"         /* cover_precache: pre-decode covers to .r565 at scan */
#include "posstore.h"      /* pos_remove_sd: drop stale SD .pos for pruned books */
#include "bookmark_sd.h"   /* bookmark_remove_book_sd: drop stale SD .bm */

/* ---- Limits ------------------------------------------------------------- */

#define MAX_FILES_PER_BOOK 1024
#define MAX_PATH_LEN 512
#define MAX_BOOKS 4096
/* Cap on synthesized placeholder chapters for a single file with no embedded
 * chapters. Guards against a bogus embedded_chapters (trak-count heuristic)
 * creating thousands of rows -> OOM on this 56MB device. Real single-file
 * audiobooks have at most a few hundred chapters. */
#define SYNTH_CHAPTER_CAP 1024

/* Minimum free space required on the DB's partition (/usr/data, the ~36 MB
 * UBIFS user-data partition) before we start a scan. sqlite's rollback journal
 * can transiently reach ~DB size, and writing into a full partition fails
 * mid-scan and leaves a half-written library.db — the exact "scan doesn't
 * complete / tile won't open" failure we hit. 1 MB is ample headroom for our
 * <1 MB library.db plus its journal; cover art is cached on the SD card next
 * to each source JPEG, so the scan itself only writes small DB rows. Note: the
 * stock music DB keeps /usr/data chronically near-full (~1.8 MB free observed),
 * so a 2 MB threshold would false-block the audiobook scan almost always; 1 MB
 * still catches a genuinely full partition while letting the scan run. */
#define SCAN_MIN_FREE_BYTES (1 * 1024 * 1024)

/* ---- SQL (local to scanner) -------------------------------------------- */

#define SQL_GET_BOOK_BY_KEY_ONLY "SELECT book_id FROM books WHERE book_key=?"

/* ---- File entry for sorting --------------------------------------------- */

typedef struct {
    char path[MAX_PATH_LEN];
    char name[256];
    int type;  /* audio_file_type() result */
} file_entry_t;

typedef struct {
    char path[MAX_PATH_LEN];
    file_entry_t *files;
    int file_count;
} book_dir_t;

/* ---- Natural sort comparison -------------------------------------------- */

static int file_entry_cmp(const void *a, const void *b) {
    const file_entry_t *fa = (const file_entry_t *)a;
    const file_entry_t *fb = (const file_entry_t *)b;
    return audiobook_natural_cmp(fa->name, fb->name);
}

/* ---- Directory walking -------------------------------------------------- */

static int is_audio_file(const char *name) {
    return audio_file_type(name) != 0;
}

static int has_audio_files(const char *dir_path) {
    DIR *d = opendir(dir_path);
    if (!d) return 0;
    struct dirent *ent;
    int found = 0;
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;
        if (is_audio_file(ent->d_name)) {
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

/* Collect audio files from a single directory (non-recursive). Returns
 * a sorted array of file_entry_t. Caller frees *out_files. */
static int collect_audio_files(const char *dir_path,
                                file_entry_t **out_files, int *out_count) {
    DIR *d = opendir(dir_path);
    if (!d) return -1;

    file_entry_t *files = calloc(MAX_FILES_PER_BOOK, sizeof(file_entry_t));
    if (!files) { closedir(d); return -1; }
    int count = 0;

    struct dirent *ent;
    while ((ent = readdir(d)) && count < MAX_FILES_PER_BOOK) {
        if (ent->d_name[0] == '.') continue;
        int type = audio_file_type(ent->d_name);
        if (type == 0) continue;

        snprintf(files[count].path, sizeof(files[count].path),
                 "%s/%s", dir_path, ent->d_name);
        strncpy(files[count].name, ent->d_name,
                sizeof(files[count].name) - 1);
        files[count].name[sizeof(files[count].name) - 1] = '\0';
        files[count].type = type;
        count++;
    }
    closedir(d);

    /* Sort by natural-numeric order */
    qsort(files, count, sizeof(file_entry_t), file_entry_cmp);

    *out_files = files;
    *out_count = count;
    return 0;
}

/* Recursively find book directories (leaf folders with audio files).
 * If a directory has audio files, it's a book. If it has subdirectories,
 * recurse into them (subdirectories may contain more books). A directory
 * can be both (audio files at this level + subdirs with more books). */
static int find_book_dirs(const char *root_path,
                          const char *parent_path,
                          book_dir_t **out_books, int *out_book_count,
                          int *out_capacity) {
    DIR *d = opendir(parent_path);
    if (!d) return -1;

    /* Check if this directory has audio files */
    int has_audio = has_audio_files(parent_path);

    if (has_audio) {
        /* This is a book directory */
        if (*out_book_count >= *out_capacity) {
            int new_cap = *out_capacity * 2;
            book_dir_t *new_books = realloc(*out_books,
                                           new_cap * sizeof(book_dir_t));
            if (!new_books) { closedir(d); return -1; }
            *out_books = new_books;
            *out_capacity = new_cap;
        }
        book_dir_t *bd = &(*out_books)[*out_book_count];
        strncpy(bd->path, parent_path, sizeof(bd->path) - 1);
        bd->path[sizeof(bd->path) - 1] = '\0';
        bd->files = NULL;
        bd->file_count = 0;
        (*out_book_count)++;
    }

    /* Recurse into subdirectories */
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;

        char child_path[MAX_PATH_LEN];
        snprintf(child_path, sizeof(child_path), "%s/%s",
                 parent_path, ent->d_name);

        struct stat st;
        if (stat(child_path, &st) < 0) continue;
        if (!S_ISDIR(st.st_mode)) continue;

        find_book_dirs(root_path, child_path, out_books, out_book_count,
                       out_capacity);
    }
    closedir(d);
    return 0;
}

/* ---- Upsert functions --------------------------------------------------- */

static int upsert_book(sqlite3 *db, const char *book_key, const char *title,
                       const char *sort_title, const char *root_path,
                       const char *cover_path, const char *cover_cache_path,
                       int64_t total_duration_ms, int track_count,
                       int date_modified, int author_id, int series_id,
                       const char *narrator, double series_number, int kind) {
    /* kind is in the DO UPDATE list so a folder moved between the Audiobooks
     * and Podcasts roots is re-kinded rather than keeping its stale value. */
    const char *sql =
        "INSERT INTO books(book_key,title,sort_title,root_path,cover_path,"
        "cover_cache_path,total_duration_ms,track_count,fingerprint,"
        "date_added,date_modified,last_played_at,completed,playback_speed,"
        "author_id,series_id,narrator,series_number,kind) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(book_key) DO UPDATE SET "
        "title=excluded.title,sort_title=excluded.sort_title,"
        "root_path=excluded.root_path,cover_path=excluded.cover_path,"
        "cover_cache_path=excluded.cover_cache_path,"
        "total_duration_ms=excluded.total_duration_ms,"
        "track_count=excluded.track_count,"
        "author_id=excluded.author_id,series_id=excluded.series_id,"
        "narrator=excluded.narrator,series_number=excluded.series_number,"
        "kind=excluded.kind";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;

    sqlite3_bind_text(stmt, 1, book_key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, sort_title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, root_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, cover_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, cover_cache_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 7, total_duration_ms);
    sqlite3_bind_int(stmt, 8, track_count);
    sqlite3_bind_text(stmt, 9, "", -1, SQLITE_TRANSIENT); /* fingerprint */
    /* date_added: keep existing or set now */
    int now = (int)time(NULL);
    sqlite3_bind_int(stmt, 10, now);
    sqlite3_bind_int(stmt, 11, date_modified);
    sqlite3_bind_int(stmt, 12, 0); /* last_played_at */
    sqlite3_bind_int(stmt, 13, 0); /* completed */
    sqlite3_bind_double(stmt, 14, 1.0); /* playback_speed */
    if (author_id > 0) sqlite3_bind_int(stmt, 15, author_id);
    else sqlite3_bind_null(stmt, 15);
    if (series_id > 0) sqlite3_bind_int(stmt, 16, series_id);
    else sqlite3_bind_null(stmt, 16);
    sqlite3_bind_text(stmt, 17, narrator ? narrator : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 18, series_number);
    sqlite3_bind_int(stmt, 19, kind);

    int ret = (sqlite3_step(stmt) == SQLITE_DONE) ? 0 : -1;
    sqlite3_finalize(stmt);

    if (ret == 0) {
        /* If this was an INSERT (not UPDATE), date_added should be set.
         * If UPDATE, preserve original date_added. The ON CONFLICT above
         * overwrites date_added — fix by reading back and setting if needed. */
    }

    return ret;
}

static int get_book_id_by_key(sqlite3 *db, const char *book_key) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, SQL_GET_BOOK_BY_KEY_ONLY, -1, &stmt, NULL)
        != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, book_key, -1, SQLITE_TRANSIENT);
    int book_id = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        book_id = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return book_id;
}

static void upsert_book_description(sqlite3 *db, int book_id,
                                    const char *description) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT INTO book_metadata(book_id,description) VALUES(?,?) "
        "ON CONFLICT(book_id) DO UPDATE SET "
        "description=excluded.description";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_int(stmt, 1, book_id);
    sqlite3_bind_text(stmt, 2, description ? description : "",
                      -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

static int upsert_track(sqlite3 *db, int book_id, int ordinal,
                        int disc_number, int track_number,
                        const char *path, const char *title,
                        const char *sort_title, int64_t duration_ms,
                        int embedded_chapters, int64_t file_size,
                        int file_mtime) {
    const char *sql =
        "INSERT INTO tracks(book_id,ordinal,disc_number,track_number,path,"
        "title,sort_title,duration_ms,embedded_chapters,file_size,file_mtime,"
        "fingerprint) VALUES(?,?,?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(path) DO UPDATE SET "
        "book_id=excluded.book_id,ordinal=excluded.ordinal,"
        "disc_number=excluded.disc_number,track_number=excluded.track_number,"
        "title=excluded.title,sort_title=excluded.sort_title,"
        "duration_ms=excluded.duration_ms,"
        "embedded_chapters=excluded.embedded_chapters,"
        "file_size=excluded.file_size,file_mtime=excluded.file_mtime";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;

    sqlite3_bind_int(stmt, 1, book_id);
    sqlite3_bind_int(stmt, 2, ordinal);
    sqlite3_bind_int(stmt, 3, disc_number);
    sqlite3_bind_int(stmt, 4, track_number);
    sqlite3_bind_text(stmt, 5, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, sort_title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 8, duration_ms);
    sqlite3_bind_int(stmt, 9, embedded_chapters);
    sqlite3_bind_int64(stmt, 10, file_size);
    sqlite3_bind_int(stmt, 11, file_mtime);
    sqlite3_bind_text(stmt, 12, "", -1, SQLITE_TRANSIENT);

    int ret = (sqlite3_step(stmt) == SQLITE_DONE) ? 0 : -1;
    sqlite3_finalize(stmt);
    return ret;
}

static int delete_chapters_for_track(sqlite3 *db, int track_id) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM chapters WHERE track_id=?",
                          -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int(stmt, 1, track_id);
    int ret = (sqlite3_step(stmt) == SQLITE_DONE) ? 0 : -1;
    sqlite3_finalize(stmt);
    return ret;
}

static int get_track_id_by_path(sqlite3 *db, const char *path) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT track_id FROM tracks WHERE path=?", -1, &stmt, NULL)
        != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
    int track_id = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        track_id = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return track_id;
}

static int upsert_chapter(sqlite3 *db, int track_id, int ordinal,
                          const char *title, int64_t start_ms, int64_t end_ms) {
    const char *sql =
        "INSERT INTO chapters(track_id,ordinal,title,start_ms,end_ms,"
        "bookmarkable) VALUES(?,?,?,?,?,1) "
        "ON CONFLICT(track_id,ordinal) DO UPDATE SET "
        "title=excluded.title,start_ms=excluded.start_ms,"
        "end_ms=excluded.end_ms";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, track_id);
    sqlite3_bind_int(stmt, 2, ordinal);
    sqlite3_bind_text(stmt, 3, title ? title : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, start_ms);
    sqlite3_bind_int64(stmt, 5, end_ms);
    int ret = (sqlite3_step(stmt) == SQLITE_DONE) ? 0 : -1;
    sqlite3_finalize(stmt);
    return ret;
}

/* Append a chapter title to the FTS chapter_titles buffer (space-separated,
 * truncated at capacity). */
static void append_chapter_title(char *buf, int buf_sz, const char *title) {
    if (!buf || buf_sz <= 0 || !title || !title[0]) return;
    int len = (int)strlen(buf);
    if (len >= buf_sz - 1) return;
    if (len > 0) buf[len++] = ' ';
    strncpy(buf + len, title, buf_sz - len - 1);
    buf[buf_sz - 1] = '\0';
}

/* Context + callback for audio_read_chapters: upsert each chapter into the DB
 * and accumulate titles for FTS. */
typedef struct {
    sqlite3 *db;
    int track_id;
    char *chapter_titles;
    int chapter_titles_sz;
    int count;
    /* Offset to add to each embedded chapter's start_ms/end_ms so chapters in
     * a multi-file book are stored BOOK-relative (cumulative across files), not
     * file-relative. 0 for single-file books. Lets chapter-tap seek resolve to
     * the right track via cmd_seek's book-ms -> track-idx accumulation. */
    int64_t book_offset_ms;
} scan_chapter_ctx_t;

static int scan_chapter_cb(int ordinal, const char *title,
                           int64_t start_ms, int64_t end_ms, void *ctx) {
    scan_chapter_ctx_t *c = (scan_chapter_ctx_t *)ctx;
    if (!c || c->track_id <= 0) return 1;
    upsert_chapter(c->db, c->track_id, ordinal, title,
                   start_ms + c->book_offset_ms, end_ms + c->book_offset_ms);
    append_chapter_title(c->chapter_titles, c->chapter_titles_sz, title);
    c->count++;
    return 0;
}

/* ---- Author / series lookup-or-create ----------------------------------- */

/* Get-or-create a row in `authors` or `series` by display_name, returning its
 * id. display_name="" yields -1 (NULL FK). */
static int get_or_create_named(sqlite3 *db, const char *table,
                               const char *id_col, const char *display_name) {
    if (!display_name || !display_name[0]) return -1;
    char sql[256];
    sqlite3_stmt *stmt = NULL;

    snprintf(sql, sizeof(sql), "SELECT %s FROM %s WHERE display_name=?",
             id_col, table);
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, display_name, -1, SQLITE_TRANSIENT);
        int id = -1;
        if (sqlite3_step(stmt) == SQLITE_ROW) id = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        if (id > 0) return id;
    }

    char sort_name[256];
    audiobook_derive_sort_title(display_name, sort_name, sizeof(sort_name));
    snprintf(sql, sizeof(sql),
             "INSERT INTO %s(sort_name,display_name) VALUES(?,?) "
             "ON CONFLICT(display_name) DO NOTHING", table);
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, sort_name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, display_name, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    snprintf(sql, sizeof(sql), "SELECT %s FROM %s WHERE display_name=?",
             id_col, table);
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, display_name, -1, SQLITE_TRANSIENT);
        int id = -1;
        if (sqlite3_step(stmt) == SQLITE_ROW) id = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        return id;
    }
    return -1;
}

static int get_or_create_author(sqlite3 *db, const char *display_name) {
    return get_or_create_named(db, "authors", "author_id", display_name);
}

static int get_or_create_series(sqlite3 *db, const char *display_name) {
    return get_or_create_named(db, "series", "series_id", display_name);
}

/* ---- Path-derived metadata --------------------------------------------- */

/* Path of `path` relative to `root`, with leading slashes stripped. Returns
 * NULL when path is not under root, or is root itself. Callers must handle
 * NULL: passing a non-relative absolute path into the component split below
 * yields an empty first component (everything before the leading '/'), which
 * previously produced an empty author and a junk series named "usr". */
static const char *path_relative_to(const char *root, const char *path) {
    if (!root || !path) return NULL;
    size_t rlen = strlen(root);
    if (strncmp(path, root, rlen) != 0) return NULL;
    const char *rel = path + rlen;
    while (*rel == '/') rel++;
    return *rel ? rel : NULL;
}

/* The library convention is /Audiobooks/<Author>/<Series>/<leaf book dir>/.
 * From the book's root_path relative to the active library root we derive the
 * author (depth-1 ancestor) and series (depth-2 ancestor, if present). The root
 * is a parameter because the scanner now walks more than one. */

static void derive_path_metadata(const char *root, const char *root_path,
                                 char *author, int author_sz,
                                 char *series, int series_sz) {
    if (author && author_sz > 0) author[0] = '\0';
    if (series && series_sz > 0) series[0] = '\0';
    if (!root_path) return;

    const char *rel = path_relative_to(root, root_path);
    if (!rel) return;

    char buf[512];
    strncpy(buf, rel, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *comps[16];
    int n = 0;
    char *p = buf;
    while (*p && n < 16) {
        comps[n++] = p;
        char *slash = strchr(p, '/');
        if (!slash) break;
        *slash = '\0';
        p = slash + 1;
    }
    int ancestors = n - 1;  /* exclude the leaf book dir itself */
    if (author && author_sz > 0) {
        if (ancestors >= 1) {
            strncpy(author, comps[0], author_sz - 1);
            author[author_sz - 1] = '\0';
        } else {
            strncpy(author, "Unknown", author_sz - 1);
            author[author_sz - 1] = '\0';
        }
    }
    if (series && series_sz > 0 && ancestors >= 2) {
        strncpy(series, comps[1], series_sz - 1);
        series[series_sz - 1] = '\0';
    }
}

/* Parse a series number from the last "[... <number>]" group in the leaf
 * directory name. "Day by Day Armageddon 1" -> 1.0; "3-4" -> 3.0;
 * "Trilobyte 3.5" -> 3.5. Returns 0 if none found. */
static double parse_series_number(const char *leaf) {
    if (!leaf) return 0;
    const char *lb = NULL;
    for (const char *p = leaf; *p; p++) if (*p == '[') lb = p;
    if (!lb) return 0;
    const char *rb = strchr(lb, ']');
    if (!rb || rb == lb + 1) return 0;

    const char *p = rb - 1;
    while (p > lb && !((*p >= '0' && *p <= '9') || *p == '.')) p--;
    if (p <= lb) return 0;
    const char *end = p + 1;
    while (p > lb && ((*p >= '0' && *p <= '9') || *p == '.')) p--;
    p++;

    char numbuf[32];
    int ni = 0;
    for (const char *q = p; q < end && ni < 31; q++) {
        if (*q == '-') break;  /* "3-4" -> just "3" */
        numbuf[ni++] = *q;
    }
    numbuf[ni] = '\0';
    if (ni == 0) return 0;
    return atof(numbuf);
}

/* Clean a book title from its leaf directory name: strip a leading
 * "YYYY - " year prefix and a trailing " [Series N]" bracket group. */
static void clean_book_title(const char *leaf, char *out, int out_sz) {
    if (!out || out_sz <= 0) return;
    const char *src = leaf ? leaf : "";

    /* strip leading 4-digit year + separators */
    if (strlen(src) > 4 && src[0] >= '0' && src[0] <= '9' &&
        src[1] >= '0' && src[1] <= '9' && src[2] >= '0' && src[2] <= '9' &&
        src[3] >= '0' && src[3] <= '9') {
        const char *p = src + 4;
        const char *after = p;
        while (*p == ' ' || *p == '.' || *p == '-' || *p == '_') p++;
        if (*p && p != after) src = p;  /* require a separator after the year */
    }

    strncpy(out, src, out_sz - 1);
    out[out_sz - 1] = '\0';

    /* strip trailing " [...]" — the last bracketed group */
    int len = (int)strlen(out);
    while (len > 0 && (out[len - 1] == ' ' || out[len - 1] == '\t')) out[--len] = '\0';
    if (len > 0 && out[len - 1] == ']') {
        char *lb = NULL;
        for (char *q = out; *q; q++) if (*q == '[') lb = q;
        if (lb) {
            *lb = '\0';
            len = (int)strlen(out);
            while (len > 0 && out[len - 1] == ' ') out[--len] = '\0';
        }
    }
    if (out[0] == '\0') {  /* stripped too much — keep raw */
        strncpy(out, leaf ? leaf : "", out_sz - 1);
        out[out_sz - 1] = '\0';
    }
}

/* ---- FTS5 index update -------------------------------------------------- */

static int update_fts_index(sqlite3 *db, int book_id, const char *title,
                            const char *author, const char *narrator,
                            const char *series, const char *chapter_titles) {
    /* Delete old FTS entry, insert new one */
    const char *del_sql = "DELETE FROM book_search WHERE book_id=?";
    const char *ins_sql =
        "INSERT INTO book_search(book_id,title,author,narrator,series,"
        "chapter_titles) VALUES(?,?,?,?,?,?)";

    sqlite3_stmt *stmt = NULL;

    if (sqlite3_prepare_v2(db, del_sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, book_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    if (sqlite3_prepare_v2(db, ins_sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int(stmt, 1, book_id);
    sqlite3_bind_text(stmt, 2, title ? title : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, author ? author : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, narrator ? narrator : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, series ? series : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, chapter_titles ? chapter_titles : "",
                      -1, SQLITE_TRANSIENT);
    int ret = (sqlite3_step(stmt) == SQLITE_DONE) ? 0 : -1;
    sqlite3_finalize(stmt);
    return ret;
}

/* ---- Cover art detection ------------------------------------------------ */

static void find_cover_art(const char *dir_path, char *out, int out_len) {
    if (!dir_path || !out || out_len <= 0) return;
    out[0] = '\0';

    /* Check common cover filenames */
    const char *cover_names[] = {
        "cover.jpg", "cover.png", "cover.jpeg",
        "folder.jpg", "folder.png",
        "Cover.jpg", "Cover.png",
        NULL
    };

    for (int i = 0; cover_names[i]; i++) {
        char path[MAX_PATH_LEN];
        snprintf(path, sizeof(path), "%s/%s", dir_path, cover_names[i]);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            strncpy(out, path, out_len - 1);
            out[out_len - 1] = '\0';
            return;
        }
    }
}

/* Hash a book_key to a 16-char hex name for the embedded-cover cache file.
 * A long book_path can sanitize to a book_key longer than MAX_PATH_LEN once
 * prefixed with the cache dir; the hash keeps cache filenames short and
 * fixed-width regardless. FNV-1a 64-bit is deterministic, so a re-scan reuses
 * the same cache file (idempotent overwrites via "wb"). Collisions across a
 * realistic library are negligible. */
static uint64_t fnv1a64(const char *s) {
    uint64_t h = 1469598103934665603ULL;  /* FNV-1a offset basis */
    for (const char *p = s; *p; p++) {
        h ^= (uint8_t)*p;
        h *= 1099511628211ULL;
    }
    return h;
}

static void cover_cache_name(const char *book_key, char *out, int out_len) {
    snprintf(out, out_len, "%016llx", (unsigned long long)fnv1a64(book_key));
}

/* ---- Podcast episode helpers -------------------------------------------- */

/* Episode book_key. audiobook_derive_book_key collapses space, '-', '.', '_'
 * and brackets all to a single '_', so per-FILE keys collide readily:
 * "Ep 1.mp3", "Ep-1.mp3" and "Ep.1.mp3" all sanitize to the same string, and
 * long names truncate to a shared prefix. A collision is silently destructive
 * (the second upsert UPDATEs the first episode's row, then its track insert
 * fails on UNIQUE(book_id, ordinal) and the episode vanishes), so append a
 * hash of the full path to disambiguate. */
static void derive_episode_key(const char *file_path, char *out, int out_len) {
    if (!out || out_len <= 0) return;
    char sane[384];
    audiobook_derive_book_key(file_path, sane, sizeof(sane));
    snprintf(out, out_len, "%s_%016llx", sane,
             (unsigned long long)fnv1a64(file_path));
}

/* Episode display title from a filename: strip the extension only.
 * Deliberately NOT clean_book_title, which strips a leading 4-digit year and
 * would turn "2026-08-01 Interview.mp3" into "08-01 Interview" — eating the
 * part that makes date-prefixed episodes sort correctly. */
static void episode_title_from_name(const char *name, char *out, int out_sz) {
    if (!out || out_sz <= 0) return;
    const char *src = name ? name : "";
    const char *dot = strrchr(src, '.');
    int len = dot && dot != src ? (int)(dot - src) : (int)strlen(src);
    if (len > out_sz - 1) len = out_sz - 1;
    memcpy(out, src, (size_t)len);
    out[len] = '\0';
    if (out[0] == '\0') {
        strncpy(out, src, out_sz - 1);
        out[out_sz - 1] = '\0';
    }
}

/* Name shown for files sitting loose in the podcast root rather than in a show
 * folder. Not the root's own basename ("Podcasts"), which would render as
 * Podcasts -> Podcasts -> episodes. */
#define UNSORTED_SHOW_NAME "Unsorted"

/* Show name for an episode folder: the folder's path RELATIVE to the podcast
 * root, not its basename. series.display_name is UNIQUE, so basenames would
 * merge unrelated shows whose folders happen to share a name ("2024",
 * "Season 1", "Episodes"), interleaving two episode-index sequences into one
 * list. path_relative_to returns NULL for the root itself, which is the
 * loose-files case. */
static void derive_show_name(const char *root, const char *dir_path,
                             char *out, int out_sz) {
    if (!out || out_sz <= 0) return;
    const char *rel = path_relative_to(root, dir_path);
    if (!rel) rel = UNSORTED_SHOW_NAME;
    strncpy(out, rel, out_sz - 1);
    out[out_sz - 1] = '\0';
}

/* ---- Podcast show folders ---------------------------------------------- */

/* Upsert one show folder's files as one book row per file. Podcast episodes
 * cannot be tracks of a shared "show" row because progress is keyed
 * book_id PRIMARY KEY, so a show-as-book could hold only one resume point.
 *
 * root_path stays the SHOW FOLDER rather than the file: audiobook_cleanup_orphans
 * deletes any book whose root_path is not a stat-able directory, so a file path
 * there would be wiped on the next scan.
 *
 * Returns the number of episode rows upserted. cover_path is resolved once by
 * the caller and shared by every episode, so the on-SD .r565 cache is shared
 * rather than duplicated per episode. */
static int process_podcast_dir(sqlite3 *db, const char *root, book_dir_t *bd,
                               const char *cover_path) {
    char show[256];
    derive_show_name(root, bd->path, show, sizeof(show));
    int series_id = show[0] ? get_or_create_series(db, show) : -1;

    int upserted = 0;
    for (int j = 0; j < bd->file_count; j++) {
        audio_tags_t tags;
        memset(&tags, 0, sizeof(tags));
        audio_read_tags(bd->files[j].path, &tags);

        char title[512];
        if (tags.title[0]) {
            strncpy(title, tags.title, sizeof(title) - 1);
            title[sizeof(title) - 1] = '\0';
        } else {
            episode_title_from_name(bd->files[j].name, title, sizeof(title));
        }
        char sort_title[512];
        audiobook_derive_sort_title(title, sort_title, sizeof(sort_title));

        char book_key[512];
        derive_episode_key(bd->files[j].path, book_key, sizeof(book_key));

        /* Podcast files nearly always carry the show in the artist tag; fall
         * back to the folder-derived show name. */
        const char *author_name = tags.artist[0] ? tags.artist : show;
        int author_id = get_or_create_author(db, author_name);

        /* series_number is the episode's index in the natural filename sort
         * collect_audio_files already applied. The episode list orders by it
         * descending, so this is what makes "newest first" work for
         * date-prefixed and numbered filenames. */
        if (upsert_book(db, book_key, title, sort_title, bd->path,
                        cover_path, "", tags.duration_ms, 1,
                        tags.file_mtime, author_id, series_id, "",
                        (double)(j + 1), LIB_KIND_PODCAST) < 0) {
            fprintf(stderr, "[scan] upsert episode failed for %s\n",
                    bd->files[j].path);
            continue;
        }
        int book_id = get_book_id_by_key(db, book_key);
        if (book_id < 0) continue;
        if (tags.description[0])
            upsert_book_description(db, book_id, tags.description);

        char track_title[512];
        strncpy(track_title, title, sizeof(track_title) - 1);
        track_title[sizeof(track_title) - 1] = '\0';
        if (upsert_track(db, book_id, 1, 1, j + 1, bd->files[j].path,
                         track_title, sort_title, tags.duration_ms,
                         tags.embedded_chapters, tags.file_size,
                         tags.file_mtime) < 0)
            continue;

        /* Real embedded chapters are still useful (some shows ship them), but
         * nothing is synthesized: the single-file fallback would give every
         * episode one meaningless "Chapter 1". */
        char chapter_titles[1024];
        chapter_titles[0] = '\0';
        int track_id = get_track_id_by_path(db, bd->files[j].path);
        if (track_id > 0) {
            delete_chapters_for_track(db, track_id);
            int chapter_format = (bd->files[j].type == AUDIO_EXT_MP3 ||
                                  bd->files[j].type == AUDIO_EXT_M4B ||
                                  bd->files[j].type == AUDIO_EXT_M4A);
            if (chapter_format) {
                scan_chapter_ctx_t cctx;
                cctx.db = db;
                cctx.track_id = track_id;
                cctx.chapter_titles = chapter_titles;
                cctx.chapter_titles_sz = (int)sizeof(chapter_titles);
                cctx.count = 0;
                cctx.book_offset_ms = 0;
                audio_read_chapters(bd->files[j].path, scan_chapter_cb, &cctx);
            }
        }
        /* Outside the track_id block: an episode that failed its track lookup
         * still needs an FTS row, or it becomes unsearchable. */
        update_fts_index(db, book_id, title, author_name, "", show,
                         chapter_titles);
        upserted++;
    }
    return upserted;
}

/* ---- Main scan ---------------------------------------------------------- */

int audiobook_scan_root(sqlite3 *db, const char *root_path, int kind,
                        const char *label,
                        scan_progress_cb progress, void *ctx) {
    if (!db || !root_path) return -1;
    if (!label) label = (kind == LIB_KIND_PODCAST) ? "Podcasts" : "Audiobooks";

    struct stat root_st;
    if (stat(root_path, &root_st) < 0 || !S_ISDIR(root_st.st_mode)) {
        if (progress) progress(5, 0, 0, "library root not found", ctx);
        return -1;
    }

    /* Pre-scan free-space guard on the partition holding library.db (now on
     * SD). If the SD is full, sqlite writes fail mid-scan; abort loudly here
     * instead of silently producing a broken library. Best-effort: if statvfs
     * itself fails, fall through and let sqlite surface any real write error
     * rather than blocking the scan. */
    struct statvfs vfs;
    if (statvfs(AUDIOBOOK_DB_DIR, &vfs) == 0) {
        unsigned long long free_bytes = (unsigned long long)vfs.f_bavail
                                        * (unsigned long long)vfs.f_frsize;
        if (free_bytes < (unsigned long long)SCAN_MIN_FREE_BYTES) {
            if (progress) progress(5, 0, 0, "storage full", ctx);
            return -1;
        }
    }

    if (progress) progress(0, 0, 0, "starting scan", ctx);

    /* Find all book directories */
    int capacity = 64;
    int book_count = 0;
    book_dir_t *books = calloc(capacity, sizeof(book_dir_t));
    if (!books) return -1;

    find_book_dirs(root_path, root_path, &books, &book_count, &capacity);

    if (progress) progress(1, 0, book_count, "found books", ctx);

    /* Ensure the embedded-cover cache dir exists on the SD card (next to the
     * library root). Embedded covers extracted from audio metadata (M4B covr
     * / MP3 APIC) are written here as <hash>.jpg; the cover decoder then reads
     * them through its normal JPEG + .r565 cache path. Best-effort: ignore
     * EEXIST; if the dir can't be created, embedded-cover extraction below
     * simply fails to write and those books show no cover (non-fatal). */
    char covercache_dir[MAX_PATH_LEN];
    snprintf(covercache_dir, sizeof(covercache_dir), "%s/.covercache",
             root_path);
    mkdir(covercache_dir, 0755);

    /* Ensure library_roots entry exists */
    sqlite3_stmt *lr_stmt = NULL;
    if (sqlite3_prepare_v2(db,
        "INSERT INTO library_roots(path,label,enabled) VALUES(?,?,1) "
        "ON CONFLICT(path) DO UPDATE SET last_scan_started_at=?",
        -1, &lr_stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(lr_stmt, 1, root_path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(lr_stmt, 2, label, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(lr_stmt, 3, (int)time(NULL));
        sqlite3_step(lr_stmt);
        sqlite3_finalize(lr_stmt);
    }

    /* Wrap the entire scan (library_roots marker + per-book upserts + orphan
     * cleanup) in a single transaction. Without this, each INSERT/UPDATE runs
     * as its own autocommit transaction and appends to the WAL; if the storage
     * fills mid-scan (SQLITE_FULL) the scan aborts and leaves a multi-MB stale
     * WAL that persists across reboots and blocks library.db on next open —
     * the recurring "freeze" (see Hiby-R1-wal-scan-abort). One transaction
     * means an abort rolls back cleanly: ROLLBACK discards the WAL frames back
     * to the last committed state, so no stale large WAL survives the abort.
     * VACUUM below must stay OUTSIDE the transaction (it cannot run inside
     * one). Best-effort: if BEGIN itself fails, fall through to the old
     * autocommit behavior rather than blocking the scan.
     *
     * The write mutex serializes this scan (event thread) with save_progress
     * (player thread) so two writers never collide in the WAL — important now
     * that the DB lives on exFAT where fcntl locks may be no-ops. */
    audiobook_db_write_lock();
    int tx_active = (sqlite3_exec(db, "BEGIN", NULL, NULL, NULL) == SQLITE_OK);

    int changed_count = 0;

    /* Process each book directory */
    for (int i = 0; i < book_count; i++) {
        book_dir_t *bd = &books[i];

        if (progress) progress(2, i + 1, book_count, bd->path, ctx);

        /* Collect audio files. Every early exit below must go through
         * next_book: bd->files is a ~786 KB calloc and bare `continue`s used to
         * leak it on a 56 MB device. */
        if (collect_audio_files(bd->path, &bd->files, &bd->file_count) < 0)
            goto next_book;
        if (bd->file_count == 0) goto next_book;

        /* Derive book_key */
        char book_key[512];
        audiobook_derive_book_key(bd->path, book_key, sizeof(book_key));

        /* Derive title from directory name (last component), cleaned */
        const char *dir_name = strrchr(bd->path, '/');
        dir_name = dir_name ? dir_name + 1 : bd->path;
        char title[512];
        clean_book_title(dir_name, title, sizeof(title));

        char sort_title[512];
        audiobook_derive_sort_title(title, sort_title, sizeof(sort_title));

        /* Find cover art. First a separate cover image in the book folder
         * (cover.jpg/folder.jpg/...); if none, fall back to the embedded cover
         * art in the primary track's metadata (M4B covr atom / MP3 APIC frame),
         * extracted to the shared .covercache dir on the SD card as a .jpg or
         * .png depending on the embedded type. The cover decoder then reads
         * that image through its normal libjpeg/pngdec + .r565 cache path,
         * identical to a separate cover file. */
        char cover_path[MAX_PATH_LEN];
        find_cover_art(bd->path, cover_path, sizeof(cover_path));
        if (!cover_path[0] && bd->file_count > 0) {
            char cname[24];
            /* Keyed on the folder's book_key, so every episode in a show
             * shares one extracted image and one .r565 cache entry. */
            cover_cache_name(book_key, cname, sizeof(cname));
            char cache_base[MAX_PATH_LEN], cache_path[MAX_PATH_LEN];
            snprintf(cache_base, sizeof(cache_base), "%s/%s",
                     covercache_dir, cname);
            if (audio_extract_cover(bd->files[0].path, cache_base,
                                    cache_path, sizeof(cache_path)) == 1) {
                strncpy(cover_path, cache_path, sizeof(cover_path) - 1);
                cover_path[sizeof(cover_path) - 1] = '\0';
            }
        }

        /* Persist both .r565 sizes next to the source so the first on-screen
         * view is a cheap cache hit rather than a libjpeg decode stall. Runs on
         * the event thread (scan is invoked from handle_home_touch), so it is
         * off the render thread by construction, and each call has its own
         * jmp_buf so it is re-entrant with the thumbnail pre-warm. */
        if (cover_path[0]) {
            cover_precache(cover_path, COVER_PX);
            cover_precache(cover_path, COVER_THUMB_PX);
        }

        /* Podcast roots: one book row per FILE, not one book per folder. */
        if (kind == LIB_KIND_PODCAST) {
            changed_count += process_podcast_dir(db, root_path, bd, cover_path);
            goto next_book;
        }

        /* Derive author/series from the path convention
         * /Audiobooks/<Author>/<Series>/<leaf>/. */
        char path_author[256], path_series[256];
        derive_path_metadata(root_path, bd->path,
                             path_author, sizeof(path_author),
                             path_series, sizeof(path_series));
        double series_number = parse_series_number(dir_name);

        /* Compute total duration + read tags from all tracks. Capture the
         * first non-empty tag artist/composer as author/narrator candidates
         * (they override the path-derived author when present). */
        int64_t total_duration = 0;
        int latest_mtime = 0;
        char tag_artist[256] = "", tag_composer[256] = "";
        char description[2048] = "";

        for (int j = 0; j < bd->file_count; j++) {
            audio_tags_t tags;
            if (audio_read_tags(bd->files[j].path, &tags) == 0) {
                total_duration += tags.duration_ms;
                if (tags.file_mtime > latest_mtime)
                    latest_mtime = tags.file_mtime;
                if (!tag_artist[0] && tags.artist[0]) {
                    strncpy(tag_artist, tags.artist, sizeof(tag_artist) - 1);
                    tag_artist[sizeof(tag_artist) - 1] = '\0';
                }
                if (!tag_composer[0] && tags.composer[0]) {
                    strncpy(tag_composer, tags.composer,
                            sizeof(tag_composer) - 1);
                    tag_composer[sizeof(tag_composer) - 1] = '\0';
                }
                if (!description[0] && tags.description[0]) {
                    strncpy(description, tags.description,
                            sizeof(description) - 1);
                    description[sizeof(description) - 1] = '\0';
                }
            }
        }

        /* Choose final author/narrator/series display strings. */
        const char *author_name = tag_artist[0] ? tag_artist : path_author;
        const char *narrator = tag_composer[0] ? tag_composer : "";
        const char *series_name = path_series[0] ? path_series : "";

        int author_id = get_or_create_author(db, author_name);
        int series_id = series_name[0] ? get_or_create_series(db, series_name)
                                       : -1;

        /* Upsert book */
        if (upsert_book(db, book_key, title, sort_title, bd->path,
                        cover_path, "", total_duration, bd->file_count,
                        latest_mtime, author_id, series_id, narrator,
                        series_number, LIB_KIND_BOOK) < 0) {
            fprintf(stderr, "[scan] upsert_book failed for %s\n", bd->path);
            goto next_book;
        }

        int book_id = get_book_id_by_key(db, book_key);
        if (book_id < 0) goto next_book;
        upsert_book_description(db, book_id, description);

        changed_count++;

        /* Accumulate chapter titles across all tracks for the FTS index. */
        char chapter_titles[4096];
        chapter_titles[0] = '\0';

        /* Cumulative book-relative position across files. Each synthesized or
         * embedded chapter is stored with book-relative start_ms/end_ms so the
         * chapter list shows real positions and chapter-tap seek resolves to the
         * correct track (cmd_seek accumulates tracks[i].duration_ms the same way). */
        int64_t book_pos_ms = 0;

        /* Upsert tracks */
        for (int j = 0; j < bd->file_count; j++) {
            audio_tags_t tags;
            memset(&tags, 0, sizeof(tags));
            audio_read_tags(bd->files[j].path, &tags);

            /* Derive track title: use tag title or filename */
            char track_title[512];
            if (tags.title[0]) {
                strncpy(track_title, tags.title, sizeof(track_title) - 1);
                track_title[sizeof(track_title) - 1] = '\0';
            } else {
                strncpy(track_title, bd->files[j].name,
                        sizeof(track_title) - 1);
                track_title[sizeof(track_title) - 1] = '\0';
            }

            char track_sort_title[512];
            audiobook_derive_sort_title(track_title, track_sort_title,
                                        sizeof(track_sort_title));

            if (upsert_track(db, book_id, j + 1, tags.disc_number,
                            tags.track_number, bd->files[j].path,
                            track_title, track_sort_title,
                            tags.duration_ms, tags.embedded_chapters,
                            tags.file_size, tags.file_mtime) < 0) {
                continue;
            }

            /* Upsert chapters. For MP3/M4B/M4A, parse real embedded chapters
             * (ID3 CHAP/CTOC, Nero chpl, or QuickTime chapter track) via audio_read_chapters;
             * they are offset to book-relative by scan_chapter_cb. If none are
             * found, synthesize: multi-file books get one chapter per file
             * titled after the track (NOT "Chapter 1" placeholders — that was
             * the bug where a multi-file .m4a/.m4b book with no embedded chapters
             * showed every chapter as "Chapter 1"); single-file books get one
             * placeholder covering the file. Either way the window is
             * book-relative [book_pos_ms, book_pos_ms + duration]. */
            int track_id = get_track_id_by_path(db, bd->files[j].path);
            if (track_id > 0) {
                delete_chapters_for_track(db, track_id);
                int has_embedded_chapter_format =
                    (bd->files[j].type == AUDIO_EXT_MP3 ||
                     bd->files[j].type == AUDIO_EXT_M4B ||
                     bd->files[j].type == AUDIO_EXT_M4A);
                int parsed = 0;
                if (has_embedded_chapter_format) {
                    scan_chapter_ctx_t cctx;
                    cctx.db = db;
                    cctx.track_id = track_id;
                    cctx.chapter_titles = chapter_titles;
                    cctx.chapter_titles_sz = (int)sizeof(chapter_titles);
                    cctx.count = 0;
                    cctx.book_offset_ms = book_pos_ms;
                    parsed = audio_read_chapters(bd->files[j].path,
                                                 scan_chapter_cb, &cctx);
                }
                if (parsed <= 0) {
                    if (bd->file_count > 1) {
                        /* Multi-file book: one chapter per file, titled after
                         * the track, spanning its book-relative window. */
                        upsert_chapter(db, track_id, j + 1, track_title,
                                       book_pos_ms,
                                       book_pos_ms + tags.duration_ms);
                        append_chapter_title(chapter_titles,
                                             sizeof(chapter_titles),
                                             track_title);
                    } else {
                        /* Single file, no embedded chapters: evenly-spaced
                         * placeholders. nch is capped to guard against a bogus
                         * trak-count heuristic OOMing the device. */
                        int nch = tags.embedded_chapters > 0
                                      ? tags.embedded_chapters : 1;
                        if (nch > SYNTH_CHAPTER_CAP) nch = SYNTH_CHAPTER_CAP;
                        int64_t chapter_duration = tags.duration_ms / nch;
                        for (int c = 0; c < nch; c++) {
                            char ch_title[32];
                            snprintf(ch_title, sizeof(ch_title),
                                     "Chapter %d", c + 1);
                            upsert_chapter(db, track_id, c + 1, ch_title,
                                           c * chapter_duration,
                                           (c + 1) * chapter_duration);
                            append_chapter_title(chapter_titles,
                                                 sizeof(chapter_titles),
                                                 ch_title);
                        }
                    }
                }
            }
            book_pos_ms += tags.duration_ms;
        }

        /* Update FTS index with real metadata + collected chapter titles. */
        update_fts_index(db, book_id, title, author_name, narrator,
                         series_name, chapter_titles);

    next_book:
        free(bd->files);
        bd->files = NULL;
    }

    if (progress) progress(3, 0, 0, "indexing search", ctx);

    /* Update library_roots scan status */
    sqlite3_stmt *lr_done = NULL;
    if (sqlite3_prepare_v2(db,
        "UPDATE library_roots SET last_scan_completed_at=?,"
        "last_scan_status='ok',last_seen_mtime=?,last_seen_size=? "
        "WHERE path=?", -1, &lr_done, NULL) == SQLITE_OK) {
        sqlite3_bind_int(lr_done, 1, (int)time(NULL));
        sqlite3_bind_int(lr_done, 2, (int)root_st.st_mtime);
        sqlite3_bind_int64(lr_done, 3, (int64_t)root_st.st_size);
        sqlite3_bind_text(lr_done, 4, root_path, -1, SQLITE_TRANSIENT);
        sqlite3_step(lr_done);
        sqlite3_finalize(lr_done);
    }

    /* Orphan cleanup is NOT called here. It has no root filter, so running it
     * per-root would let one root's pass delete rows belonging to a root whose
     * files have not been re-upserted yet. audiobook_scan_all runs it once,
     * after every root, scoped to the roots that actually scanned. */

    /* Commit the scan transaction. If this fails (SQLITE_FULL on /usr/data
     * filling up while writing the commit frame), the transaction did not
     * commit — roll it back so the WAL resets to the pre-scan state instead
     * of leaving a stale large WAL behind (the freeze trigger). */
    if (tx_active) {
        int commit_rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
        if (commit_rc != SQLITE_OK) {
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            audiobook_db_write_unlock();
            if (progress) progress(5, 0, 0, "scan aborted (storage full)", ctx);
            free(books);
            return -1;
        }
    }
    audiobook_db_write_unlock();

    if (progress) progress(5, changed_count, book_count, "done", ctx);

    free(books);
    return 0;
}

/* The roots the app owns. Order matters only for progress reporting. Keep in
 * sync with APP_PATH_SQL in music_catalog.c, which hides these from HiBy's
 * stock Music catalog. */
static const struct {
    const char *path;
    int kind;
    const char *label;
} SCAN_ROOTS[] = {
    { AUDIOBOOK_LIBRARY_ROOT, LIB_KIND_BOOK,    "Audiobooks" },
    { AUDIOBOOK_PODCAST_ROOT, LIB_KIND_PODCAST, "Podcasts"   },
};
#define SCAN_ROOT_COUNT ((int)(sizeof(SCAN_ROOTS) / sizeof(SCAN_ROOTS[0])))

int audiobook_scan_library(sqlite3 *db, const char *root_path,
                           scan_progress_cb progress, void *ctx) {
    return audiobook_scan_root(db, root_path, LIB_KIND_BOOK, "Audiobooks",
                               progress, ctx);
}

int audiobook_scan_all(sqlite3 *db, scan_progress_cb progress, void *ctx) {
    if (!db) return -1;

    const char *scanned[SCAN_ROOT_COUNT];
    int scanned_n = 0;
    int failures = 0;

    for (int i = 0; i < SCAN_ROOT_COUNT; i++) {
        struct stat st;
        /* A root that is simply absent is not an error: plenty of users will
         * never create /Podcasts. Only a root that exists and then fails to
         * scan counts as a failure. */
        if (stat(SCAN_ROOTS[i].path, &st) < 0 || !S_ISDIR(st.st_mode))
            continue;
        if (audiobook_scan_root(db, SCAN_ROOTS[i].path, SCAN_ROOTS[i].kind,
                                SCAN_ROOTS[i].label, progress, ctx) < 0) {
            failures++;
            continue;
        }
        scanned[scanned_n++] = SCAN_ROOTS[i].path;
    }

    /* Nothing resolved (card unmounted, or neither folder exists): do NOT run
     * cleanup. Every book would fail its root_path stat and be deleted along
     * with its .pos and bookmark sidecars, which are the authoritative copies.
     * A missing card must never look like an empty library. */
    if (scanned_n == 0) {
        if (progress) progress(5, 0, 0, "no library roots found", ctx);
        return failures ? -1 : 0;
    }

    audiobook_db_write_lock();
    int tx = (sqlite3_exec(db, "BEGIN", NULL, NULL, NULL) == SQLITE_OK);
    audiobook_cleanup_orphans_scoped(db, scanned, scanned_n, progress, ctx);
    if (tx && sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK)
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    audiobook_db_write_unlock();

    /* Compact library.db once per refresh, not once per root: SQLite keeps
     * freed pages inside the file unless VACUUM'd, so over many card swaps it
     * would only grow. Must be outside any transaction. Best-effort — a
     * failure leaves the DB working, just not compact. */
    sqlite3_exec(db, "VACUUM", NULL, NULL, NULL);

    return failures ? -1 : 0;
}

/* ---- Orphan cleanup ----------------------------------------------------- */

/* Is path inside one of the roots that actually scanned this pass? Books under
 * a root that did not resolve are left strictly alone: otherwise a renamed or
 * unmounted /Audiobooks would look like "every book deleted", and the deletion
 * takes the .pos and bookmark sidecars with it — those are the authoritative
 * copies of resume positions, so the loss is unrecoverable. */
static int path_under_any_root(const char *path, const char *const *roots,
                               int n_roots) {
    if (!path) return 0;
    for (int i = 0; i < n_roots; i++) {
        size_t rlen = strlen(roots[i]);
        if (strncmp(path, roots[i], rlen) == 0
            && (path[rlen] == '\0' || path[rlen] == '/'))
            return 1;
    }
    return 0;
}

/* Delete one book plus everything keyed to it: the FK cascade covers tracks,
 * chapters, progress and bookmarks; book_search, the .pos file and the SD
 * bookmark file are not covered and must be removed explicitly. */
static void delete_book_row(sqlite3 *db, int book_id) {
    sqlite3_stmt *del = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM books WHERE book_id=?",
                          -1, &del, NULL) == SQLITE_OK) {
        sqlite3_bind_int(del, 1, book_id);
        sqlite3_step(del);
        sqlite3_finalize(del);
    }
    if (sqlite3_prepare_v2(db, "DELETE FROM book_search WHERE book_id=?",
                          -1, &del, NULL) == SQLITE_OK) {
        sqlite3_bind_int(del, 1, book_id);
        sqlite3_step(del);
        sqlite3_finalize(del);
    }
    pos_remove_sd(book_id);
    bookmark_remove_book_sd(book_id);
}

int audiobook_cleanup_orphans_scoped(sqlite3 *db, const char *const *roots,
                                     int n_roots, scan_progress_cb progress,
                                     void *ctx) {
    int removed = 0;

    if (!db || !roots || n_roots <= 0) return 0;

    if (progress) progress(4, 0, 0, "cleaning orphans", ctx);

    /* Find books whose root_path no longer exists */
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT book_id,root_path FROM books",
                          -1, &stmt, NULL) != SQLITE_OK)
        return 0;

    int *dead_ids = NULL;
    int dead_count = 0;
    int dead_cap = 64;
    dead_ids = malloc(dead_cap * sizeof(int));
    int dead_ok = (dead_ids != NULL);   /* OOM → skip recording (best-effort) */

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int book_id = sqlite3_column_int(stmt, 0);
        const char *rpath = (const char *)sqlite3_column_text(stmt, 1);
        if (!rpath) continue;
        if (!path_under_any_root(rpath, roots, n_roots)) continue;
        struct stat st;
        if (stat(rpath, &st) < 0 || !S_ISDIR(st.st_mode)) {
            if (!dead_ok) continue;
            if (dead_count >= dead_cap) {
                dead_cap *= 2;
                int *new_ids = realloc(dead_ids, dead_cap * sizeof(int));
                if (!new_ids) { dead_ok = 0; continue; }
                dead_ids = new_ids;
            }
            dead_ids[dead_count++] = book_id;
        }
    }
    sqlite3_finalize(stmt);

    /* Delete dead books (cascades to tracks, chapters, progress, bookmarks) */
    for (int i = 0; i < dead_count; i++) {
        delete_book_row(db, dead_ids[i]);
        removed++;
    }

    /* Find tracks whose file no longer exists */
    if (sqlite3_prepare_v2(db, "SELECT track_id,path FROM tracks", -1,
                          &stmt, NULL) == SQLITE_OK) {
        int *dead_tracks = NULL;
        int dt_count = 0, dt_cap = 64;
        dead_tracks = malloc(dt_cap * sizeof(int));
        int dt_ok = (dead_tracks != NULL);   /* OOM → skip recording */

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            int track_id = sqlite3_column_int(stmt, 0);
            const char *tpath = (const char *)sqlite3_column_text(stmt, 1);
            if (!tpath) continue;
            struct stat st;
            if (stat(tpath, &st) < 0) {
                if (!dt_ok) continue;
                if (dt_count >= dt_cap) {
                    dt_cap *= 2;
                    int *nt = realloc(dead_tracks, dt_cap * sizeof(int));
                    if (!nt) { dt_ok = 0; continue; }
                    dead_tracks = nt;
                }
                dead_tracks[dt_count++] = track_id;
            }
        }
        sqlite3_finalize(stmt);

        for (int i = 0; i < dt_count; i++) {
            sqlite3_stmt *del = NULL;
            if (sqlite3_prepare_v2(db, "DELETE FROM tracks WHERE track_id=?",
                                  -1, &del, NULL) == SQLITE_OK) {
                sqlite3_bind_int(del, 1, dead_tracks[i]);
                sqlite3_step(del);
                sqlite3_finalize(del);
            }
            removed++;
        }
        free(dead_tracks);
    }

    /* Books left with no tracks. Needed because a podcast episode's root_path
     * is its SHOW FOLDER, which survives deleting the episode file: the pass
     * above removes the track but the book row would linger forever.
     *
     * Must run AFTER the dead-track pass, or the tracks it looks for have not
     * been deleted yet. root_path must still stat as a directory, so a
     * transient SD read failure cannot cascade into destroying sidecars —
     * book_id is a rowid and gets reused, so a stale <id>.pos could otherwise
     * later attach to an unrelated episode. */
    if (sqlite3_prepare_v2(db,
            "SELECT b.book_id,b.root_path FROM books b "
            "WHERE NOT EXISTS (SELECT 1 FROM tracks t WHERE t.book_id=b.book_id)",
            -1, &stmt, NULL) == SQLITE_OK) {
        int *empty_ids = NULL;
        int e_count = 0, e_cap = 64;
        empty_ids = malloc(e_cap * sizeof(int));
        int e_ok = (empty_ids != NULL);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            int book_id = sqlite3_column_int(stmt, 0);
            const char *rpath = (const char *)sqlite3_column_text(stmt, 1);
            if (!rpath) continue;
            if (!path_under_any_root(rpath, roots, n_roots)) continue;
            struct stat st;
            if (stat(rpath, &st) < 0 || !S_ISDIR(st.st_mode)) continue;
            if (!e_ok) continue;
            if (e_count >= e_cap) {
                e_cap *= 2;
                int *ne = realloc(empty_ids, e_cap * sizeof(int));
                if (!ne) { e_ok = 0; continue; }
                empty_ids = ne;
            }
            empty_ids[e_count++] = book_id;
        }
        sqlite3_finalize(stmt);

        for (int i = 0; i < e_count; i++) {
            delete_book_row(db, empty_ids[i]);
            removed++;
        }
        free(empty_ids);
    }

    free(dead_ids);

    if (progress) progress(4, removed, 0, "orphans removed", ctx);

    return removed;
}

int audiobook_cleanup_orphans(sqlite3 *db, scan_progress_cb progress,
                              void *ctx) {
    static const char *const roots[] = {
        AUDIOBOOK_LIBRARY_ROOT, AUDIOBOOK_PODCAST_ROOT
    };
    return audiobook_cleanup_orphans_scoped(
        db, roots, (int)(sizeof(roots) / sizeof(roots[0])), progress, ctx);
}
