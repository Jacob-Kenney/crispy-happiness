/* Included by cf_rc_dev.c. Launch the preferred game once at boot, after the
 * RAM cart task opens the library. */
static bool canyon_autostart;

static void canyon_play_saved(void)
{
    if (!canyon_autostart || _LibOp.busy || !_L) return;
    canyon_autostart = false;
    if (_P.req || CfRc_Playing()) return;

    uint32_t road_uid = 0;
    uint32_t road_edition_uid = 0;
    uint32_t canyon_uid = 0;
    uint32_t rollback_uid = 0;
    for (unsigned i = 0; i < _L->ix.n; ++i) {
        const cf_lib_ent_t *entry = &_L->ix.e[i];
        if (!strcmp(entry->title, "RL RACER")) {
            CfRc_LibPlay(entry->uid);
            return;
        }
        if (!strcmp(entry->title, "Road Racer 3 Lives")) road_uid = entry->uid;
        if (!strcmp(entry->title, "Road Racer Road Edition")) road_edition_uid = entry->uid;
        if (!strcmp(entry->title, "Canyon-Select")) canyon_uid = entry->uid;
        if (!strcmp(entry->title, "Canyon-Mailbox")) rollback_uid = entry->uid;
    }
    if (road_uid) CfRc_LibPlay(road_uid);
    else if (road_edition_uid) CfRc_LibPlay(road_edition_uid);
    else if (canyon_uid) CfRc_LibPlay(canyon_uid);
    else if (rollback_uid) CfRc_LibPlay(rollback_uid);
}
