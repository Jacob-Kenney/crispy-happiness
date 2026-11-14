/* Included by cf_rc_dev.c. Launch the saved Canyon Racer mailbox build once
 * at cold boot, after the library has been opened on the RAM cart task. */
static bool canyon_autostart;

static void canyon_play_saved(void)
{
    if (!canyon_autostart || _LibOp.busy || !_L) return;
    canyon_autostart = false;
    if (_P.req || CfRc_Playing()) return;

    for (unsigned i = 0; i < _L->ix.n; ++i) {
        const cf_lib_ent_t *entry = &_L->ix.e[i];
        if (!strcmp(entry->title, "Canyon-Mailbox")) {
            CfRc_LibPlay(entry->uid);
            return;
        }
    }
}
