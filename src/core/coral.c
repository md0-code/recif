#include "coral.h"

#include <string.h>

/* The historical descriptions and the execution cores have different
 * sources. See the model evidence and DEC references in docs/NOTES.md. */
const CoralProfile coral_profiles[CORAL_MODEL_COUNT] = {
    {
        "4001", "CORAL 4001", PDP_04, 64, 64,
        "64 KB reported in 1979. Four AMD 2901 slices reported.",
        "The RT-11 catalogue and early product report give no controller assignments.",
        "64 KB raportați în 1979. Patru circuite AMD 2901 raportate.",
        "Catalogul RT-11 și raportul de lansare nu indică adresele cuploarelor.",
        false, false, DISKS_NONE, 0, false
    },
    {
        "4011", "CORAL 4011", PDP_34, 256, 256,
        "256 KB reported in 1979. Eight AMD 2900-family slices and PDP-11/34 compatibility reported.",
        "The available 4011 sources give no controller assignments.",
        "256 KB raportați în 1979. Opt circuite din familia AMD 2900 și compatibilitate PDP-11/34 raportate.",
        "Sursele disponibile pentru 4011 nu indică adresele cuploarelor.",
        false, false, DISKS_NONE, 0, false
    },
    {
        "4021", "CORAL 4021", PDP_44, 2048, 4096,
        "2 MB installed in the dispatch system. 4096 KB listed. Floating-point processor reported.",
        "Dispatch system: HDD 40 MB, FDD 0.5 MB, magnetic tape, MPX 40 A/B, ALC 41 and territorial I/O DEN 40.",
        "2 MB instalați în sistemul de dispecer. 4096 KB în tabele. Procesor de virgulă mobilă raportat.",
        "Sistemul de dispecer: disc fix 40 MB, dischete 0,5 MB, bandă magnetică, MPX 40 A/B, ALC 41 și I/O teritorial DEN 40.",
        true, true, DISKS_RP03, 0, false
    },
    {
        "4030", "CORAL 4030", PDP_44, 4096, 4096,
        "Up to 4 MB reported. Microprogrammed CPU and modular asynchronous CORAL bus.",
        "QB-40 CORAL bus to QBUS bridge. Reported options include disk, magnetic tape, printer and card reader.",
        "Până la 4 MB raportați. Unitate centrală microprogramată și magistrală CORAL modulară asincronă.",
        "Interfața QB-40 între magistrala CORAL și QBUS. Opțiuni raportate: disc, bandă magnetică, imprimantă și cititor de cartele.",
        /* RSX-11M V4.1 HWDDF.MAC on the RSX41FP tape: "SR3=165410 ; CORECTIE PT.
         * CORAL 4030". Its boot code also clears 172516. */
        true, false, DISKS_RM03, 0165410, true
    }
};

static const char *const disk_names[DISKS_COUNT] = {"none", "rm03", "rp06", "rp03"};

const char *disk_system_name(DiskSystem disks)
{
    return disks < DISKS_COUNT ? disk_names[disks] : "none";
}

bool disk_system_parse(const char *text, DiskSystem *disks)
{
    for (unsigned i = 0; i < DISKS_COUNT; ++i) {
        const char *a = text, *b = disk_names[i];
        while (*a && *b && (*a | 040) == *b)
            ++a, ++b;
        if (!*a && !*b) {
            *disks = (DiskSystem)i;
            return true;
        }
    }
    return false;
}

const CoralProfile *coral_find(const char *id)
{
    for (unsigned i = 0; i < CORAL_MODEL_COUNT; ++i) {
        if (!strcmp(coral_profiles[i].id, id))
            return &coral_profiles[i];
    }
    return NULL;
}
