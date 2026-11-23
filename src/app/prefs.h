#ifndef RECIF_PREFS_H
#define RECIF_PREFS_H

/* Flat `key: 'value'` settings files kept beside the executable. */

#include "stream.h"

#include <stdbool.h>

enum { PREFS_ENTRIES = 24, PREFS_VALUE = RC_HOST_PATH + 16 };

typedef struct {
    char key[32];
    char value[PREFS_VALUE];
} PrefsEntry;

typedef struct {
    char path[RC_HOST_PATH];
    PrefsEntry entries[PREFS_ENTRIES];
    unsigned count;
    bool found;                 /* the file existed */
} Prefs;

/* The 2.11BSD pack shipped beside the programs. */
#define PREFS_BSD_PACK "disks/unix211-rp06.dsk"

void prefs_open(Prefs *prefs, const char *file_name);
const char *prefs_get(const Prefs *prefs, const char *key, const char *fallback);
void prefs_set(Prefs *prefs, const char *key, const char *value);
bool prefs_save(const Prefs *prefs);
/* With no settings file yet and the 2.11BSD pack beside the program, the
 * key/value pairs (NULL-ended) that start it. */
void prefs_first_start(Prefs *prefs, const char *const *pairs);
/* A relative image path names a file beside the program, such as the
 * shipped pack: the full path in `out`, else `path` itself. */
const char *prefs_beside_program(const char *path, char *out, size_t size);

#endif
