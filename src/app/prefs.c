#include "prefs.h"

#include <stdio.h>
#include <string.h>

static PrefsEntry *find(Prefs *p, const char *key)
{
    for (unsigned i = 0; i < p->count; ++i)
        if (!strcmp(p->entries[i].key, key))
            return &p->entries[i];
    return NULL;
}

static void unquote(char *value)
{
    size_t n = strlen(value);
    while (n && (value[n - 1] == '\n' || value[n - 1] == '\r' || value[n - 1] == ' '))
        value[--n] = '\0';
    if (n < 2 || value[0] != '\'' || value[n - 1] != '\'')
        return;
    char *out = value;
    for (const char *in = value + 1; in < value + n - 1; ++in) {
        if (*in == '\'' && in[1] == '\'')
            ++in;
        *out++ = *in;
    }
    *out = '\0';
}

void prefs_open(Prefs *p, const char *file_name)
{
    memset(p, 0, sizeof(*p));
    rc_host_program_dir(p->path, sizeof(p->path));
    if (strlen(p->path) + strlen(file_name) >= sizeof(p->path)) {
        p->path[0] = '\0';
        return;
    }
    strcat(p->path, file_name);
    FILE *file = fopen(p->path, "r");
    if (!file)
        return;
    p->found = true;
    char line[PREFS_VALUE + 64];
    while (fgets(line, sizeof(line), file)) {
        char *colon = strchr(line, ':');
        if (line[0] == '#' || !colon || colon == line || (size_t)(colon - line) >= sizeof(p->entries[0].key))
            continue;
        *colon = '\0';
        char *value = colon + 1;
        while (*value == ' ')
            ++value;
        unquote(value);
        prefs_set(p, line, value);
    }
    fclose(file);
}

const char *prefs_get(const Prefs *p, const char *key, const char *fallback)
{
    PrefsEntry *e = find((Prefs *)p, key);
    return e ? e->value : fallback;
}

void prefs_set(Prefs *p, const char *key, const char *value)
{
    PrefsEntry *e = find(p, key);
    if (!e) {
        if (p->count == PREFS_ENTRIES || strlen(key) >= sizeof(e->key))
            return;
        e = &p->entries[p->count++];
        strcpy(e->key, key);
    }
    snprintf(e->value, sizeof(e->value), "%s", value);
}

void prefs_first_start(Prefs *p, const char *const *pairs)
{
    char pack[RC_HOST_PATH];
    rc_host_program_dir(pack, sizeof(pack));
    if (p->found || !*pack || strlen(pack) + sizeof(PREFS_BSD_PACK) > sizeof(pack))
        return;
    strcat(pack, PREFS_BSD_PACK);
    FILE *file = fopen(pack, "rb");
    if (!file)
        return;
    fclose(file);
    for (; pairs[0] && pairs[1]; pairs += 2)
        prefs_set(p, pairs[0], pairs[1]);
}

const char *prefs_beside_program(const char *path, char *out, size_t size)
{
    if (!*path || path[0] == '\\' || path[0] == '/' || path[1] == ':')
        return path;
    rc_host_program_dir(out, size);
    if (!*out || strlen(out) + strlen(path) >= size)
        return path;
    strcat(out, path);
    return out;
}

bool prefs_save(const Prefs *p)
{
    if (!p->path[0])
        return false;
    FILE *file = fopen(p->path, "w");
    if (!file)
        return false;
    for (unsigned i = 0; i < p->count; ++i) {
        fprintf(file, "%s: '", p->entries[i].key);
        for (const char *c = p->entries[i].value; *c; ++c) {
            if (*c == '\'')
                fputc('\'', file);
            fputc(*c, file);
        }
        fputs("'\n", file);
    }
    return fclose(file) == 0;
}
