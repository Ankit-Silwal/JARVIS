/* Everyday commands in dry-run (nothing is executed): JARVIS's own voice
 * (mute / unmute / talk less), clearing, and which replies are only
 * confirmations (not spoken with speak_confirm=0). */
#include "../src/jarvis.h"
#include "test.h"
#include <objbase.h>

static CmdResult r;

static const wchar_t *run(const wchar_t *text)
{
    memset(&r, 0, sizeof r);
    cmd_run(text, &r);
    printf("   %-40ls -> %-28ls | %ls\n", text, cmd_last_action(), r.reply);
    return cmd_last_action();
}

int wmain(void)
{
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    config_load();
    cmd_init(NULL);
    cmd_set_dry_run(true);
    g_cfg.speak = true;
    g_cfg.speak_confirm = true;

    TEST_SECTION("mute JARVIS (not the PC)");
    CHECK(str_eq(run(L"mute jarvis"), L"SET speak=0"), "mute jarvis");
    CHECK(!g_cfg.speak, "speak is off");
    CHECK(!r.speak, "the mute reply itself is not spoken");
    CHECK(wcsstr(r.reply, L"unmute jarvis") != NULL, "tells how to turn the voice back on");
    CHECK(str_eq(run(L"Mute Jarvis."), L"SET speak=0"), "spoken punctuation / case");
    CHECK(wcsstr(r.reply, L"already") != NULL, "already muted");
    CHECK(str_eq(run(L"unmute jarvis"), L"SET speak=1"), "unmute jarvis");
    CHECK(g_cfg.speak, "speak is on");
    CHECK(r.speak, "unmute reply is spoken");
    CHECK(str_eq(run(L"stop talking"), L"SET speak=0"), "stop talking");
    CHECK(str_eq(run(L"jarvis be quiet please"), L"SET speak=0"), "be quiet (fillers)");
    CHECK(str_eq(run(L"shut up"), L"SET speak=0"), "shut up");
    CHECK(str_eq(run(L"voice on"), L"SET speak=1"), "voice on");
    CHECK(str_eq(run(L"talk to me"), L"SET speak=1"), "talk to me");
    CHECK(str_eq(run(L"mute"), L"SYS mute"), "plain mute is still the PC volume");
    CHECK(str_eq(run(L"unmute"), L"SYS mute"), "plain unmute is still the PC volume");
    CHECK(str_eq(run(L"stop"), L"CLOSE"), "plain stop still cancels");
    CHECK(str_eq(run(L"close jarvis"), L"CLOSE") && r.quit, "close jarvis still closes");

    TEST_SECTION("talk less / talk more");
    CHECK(str_eq(run(L"talk less"), L"SET speak_confirm=0"), "talk less");
    CHECK(!g_cfg.speak_confirm, "confirmations off");
    CHECK(str_eq(run(L"only answers"), L"SET speak_confirm=0"), "only answers");
    CHECK(str_eq(run(L"talk more"), L"SET speak_confirm=1"), "talk more");
    CHECK(g_cfg.speak_confirm, "confirmations on");

    TEST_SECTION("confirmation vs answer");
    run(L"open downloads");
    CHECK(r.ack, "Opening Downloads is a confirmation");
    run(L"volume up");
    CHECK(r.ack, "Volume up is a confirmation");
    run(L"google black holes");
    CHECK(r.ack, "a web search is a confirmation");
    run(L"what time is it");
    CHECK(!r.ack && r.speak, "the time is an answer");
    run(L"what's the date");
    CHECK(!r.ack, "the date is an answer");
    run(L"battery");
    CHECK(!r.ack, "battery is an answer");
    run(L"help");
    CHECK(!r.ack, "help is not a confirmation");
    run(L"open zzqqxxnotafile");
    CHECK(!r.ack, "not found is not a confirmation");

    TEST_SECTION("misheard open");
    const wchar_t *dl = kv_get(&g_cfg.folders, L"downloads");
    wchar_t want[MAX_PATH];
    swprintf(want, MAX_PATH, L"OPEN %ls", dl);
    CHECK(str_eq(run(L"one downloads"), want), "one downloads = open downloads");
    CHECK(str_eq(run(L"Won downloads,"), want), "won downloads");
    CHECK(str_eq(run(L"opened downloads"), want), "opened downloads");
    run(L"one");
    CHECK(wcsstr(r.reply, L"no list") != NULL, "a bare one with no list is still a number");

    TEST_SECTION("clear");
    CHECK(str_eq(run(L"clear"), L"CLEAR") && r.clear && r.rows_replace && r.nrows == 0, "clear");
    CHECK(!r.reply[0], "clear says nothing");
    CHECK(str_eq(run(L"clear the screen"), L"CLEAR"), "clear the screen");

    return TEST_SUMMARY();
}
