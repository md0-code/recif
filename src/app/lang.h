#ifndef RECIF_LANG_H
#define RECIF_LANG_H

/* The interface language of both programs: English or Romanian, from the
 * saved setting or else the user's locale. Romanian follows the DAF-2020 and
 * CORAL manuals' vocabulary. */

#include "stream.h"

#include <stdbool.h>
#include <string.h>

typedef enum { LANG_EN, LANG_RO } Language;

#define TR(lang, en, ro) ((lang) == LANG_RO ? (ro) : (en))

static inline const char *lang_name(Language lang)
{
    return lang == LANG_RO ? "ro" : "en";
}

static inline Language lang_choose(const char *saved)
{
    if (saved && !strcmp(saved, "ro"))
        return LANG_RO;
    if (saved && !strcmp(saved, "en"))
        return LANG_EN;
    char locale[64];
    rc_host_locale(locale, sizeof(locale));
    return !strncmp(locale, "ro", 2) ? LANG_RO : LANG_EN;
}

#endif
