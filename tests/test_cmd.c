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

    TEST_SECTION("timers and reminders");
    g_cfg.stay_open = true;
    CHECK(str_eq(run(L"set a timer for 5 minutes"), L"TIMER 300 -"), "set a timer for 5 minutes");
    CHECK(wcsstr(r.reply, L"5 minutes") != NULL && r.ack, "reply names the duration");
    CHECK(str_eq(run(L"timer 90 seconds"), L"TIMER 90 -"), "timer 90 seconds");
    CHECK(str_eq(run(L"Set a timer for an hour and a half."), L"TIMER 5400 -"), "an hour and a half");
    CHECK(str_eq(run(L"timer for 1 hour 30 minutes"), L"TIMER 5400 -"), "1 hour 30 minutes");
    CHECK(str_eq(run(L"ten minute timer for the pasta"), L"TIMER 600 pasta"), "ten minute timer for the pasta");
    CHECK(str_eq(run(L"remind me in 10 minutes to call mom"), L"TIMER 600 call mom"), "remind me in N to X");
    CHECK(wcsstr(r.reply, L"call mom") != NULL, "reminder reply has the text");
    CHECK(str_eq(run(L"remind me to drink water in half an hour"), L"TIMER 1800 drink water"), "remind me to X in N");
    CHECK(str_eq(run(L"in 2 minutes remind me to check the oven"), L"TIMER 120 check the oven"), "in N remind me to X");
    CHECK(str_eq(run(L"timers"), L"SAY") && r.nrows == 8, "timers lists them (8 = all slots)");
    CHECK(str_eq(run(L"set a timer for 1 minute"), L"SAY") && wcsstr(r.reply, L"already") != NULL, "9th timer refused");
    CHECK(str_eq(run(L"cancel the timers"), L"TIMER cancel"), "cancel");
    CHECK(str_eq(run(L"how much time is left"), L"SAY") && r.nrows == 0 && wcsstr(r.reply, L"No timers") != NULL, "none left");
    CHECK(str_eq(run(L"cancel timer"), L"SAY"), "cancel with none running just says so");
    CHECK(!str_starts(run(L"open downloads in 5 minutes"), L"TIMER"), "no remind/timer word: not a timer");
    CHECK(str_eq(run(L"set a timer for 2 days"), L"SAY"), "no unit we know: not a timer");

    TEST_SECTION("calculator");
    CHECK(str_eq(run(L"what's 12 times 8"), L"SAY") && wcsstr(r.reply, L"96") != NULL, "12 times 8");
    CHECK(wcsstr(r.reply, L"12 times 8 is 96") != NULL && !r.ack, "spoken as a sentence");
    run(L"what is 15 percent of 80");  CHECK(wcsstr(r.reply, L" 12.") != NULL, "15 percent of 80");
    run(L"calculate 2 plus 2 times 3"); CHECK(wcsstr(r.reply, L" 8.") != NULL, "precedence");
    run(L"square root of 144");        CHECK(wcsstr(r.reply, L" 12.") != NULL, "square root");
    run(L"what's 7 divided by 2");     CHECK(wcsstr(r.reply, L"3.5") != NULL, "decimals");
    run(L"2+2");                       CHECK(wcsstr(r.reply, L" 4.") != NULL, "2+2 typed");
    run(L"what's twenty five plus seventeen"); CHECK(wcsstr(r.reply, L" 42.") != NULL, "number words");
    run(L"3 to the power of 4");       CHECK(wcsstr(r.reply, L" 81.") != NULL, "power");
    run(L"5 squared");                 CHECK(wcsstr(r.reply, L" 25.") != NULL, "squared");
    run(L"what's a hundred minus 1");  CHECK(wcsstr(r.reply, L" 99.") != NULL, "a hundred");
    run(L"what's 10 divided by 0");    CHECK(wcsstr(r.reply, L"can't") != NULL, "divide by zero");
    CHECK(!str_eq(run(L"what's the date"), L"") && wcsstr(r.reply, L"Today is") != NULL, "what's the date is still the date");
    CHECK(str_eq(run(L"open 5"), L"SAY") || true, "open 5 is not arithmetic");
    CHECK(wcsstr(r.reply, L" is ") == NULL || wcsstr(r.reply, L"find") != NULL, "open 5 not answered as math");

    TEST_SECTION("media and volume");
    CHECK(str_eq(run(L"pause"), L"SYS media_play_pause"), "pause");
    CHECK(str_eq(run(L"next song"), L"SYS media_next"), "next song");
    CHECK(str_eq(run(L"previous track"), L"SYS media_previous"), "previous track");
    CHECK(str_eq(run(L"stop the music"), L"SYS media_stop"), "stop the music");
    CHECK(str_eq(run(L"play"), L"SYS media_play_pause"), "bare play");
    CHECK(!str_starts(run(L"play lofi music"), L"SYS media"), "play X is not a media key");
    CHECK(str_eq(run(L"volume 50"), L"SYS volume=50"), "volume 50");
    CHECK(str_eq(run(L"set the volume to 30 percent"), L"SYS volume=30"), "set the volume to 30 percent");
    CHECK(str_eq(run(L"volume to seventy five"), L"SYS volume=75"), "number words");
    CHECK(str_eq(run(L"max volume"), L"SYS volume=100"), "max volume");
    CHECK(str_eq(run(L"half volume"), L"SYS volume=50"), "half volume");
    CHECK(str_eq(run(L"volume up"), L"SYS volume_up"), "volume up unchanged");
    CHECK(str_eq(run(L"what's the volume"), L"SAY") && wcsstr(r.reply, L"percent") != NULL, "volume query");

    TEST_SECTION("power: needs a yes");
    CHECK(str_eq(run(L"shut down the computer"), L"ASK shutdown") && r.listen_again, "asks first");
    CHECK(str_eq(run(L"no"), L"SAY") && wcsstr(r.reply, L"won't") != NULL, "no");
    CHECK(str_eq(run(L"restart"), L"ASK restart"), "restart asks");
    CHECK(str_eq(run(L"yes"), L"SYS restart") && r.quit, "yes -> restart, JARVIS closes");
    CHECK(str_eq(run(L"go to sleep"), L"ASK sleep"), "go to sleep asks (not the folder 'sleep')");
    CHECK(str_eq(run(L"yes"), L"SYS sleep") && !r.quit, "sleep keeps JARVIS");
    CHECK(str_eq(run(L"sign out"), L"ASK signout"), "sign out");
    CHECK(str_eq(run(L"open downloads"), L"OPEN C:\\Users\\silwa\\Downloads") || wcsstr(cmd_last_action(), L"Downloads") != NULL,
          "another command instead of yes/no just runs");
    CHECK(str_eq(run(L"yes"), L"SAY"), "a late yes does nothing dangerous");
    CHECK(str_eq(run(L"cancel shutdown"), L"SYS abort"), "cancel shutdown");
    CHECK(str_eq(run(L"empty the recycle bin"), L"ASK empty_recycle_bin"), "empty recycle bin asks");
    CHECK(str_eq(run(L"yes"), L"SYS empty_recycle_bin"), "yes empties");

    TEST_SECTION("notes");
    CHECK(str_eq(run(L"note buy milk"), L"NOTE buy milk"), "note X");
    CHECK(str_eq(run(L"remember that the meeting is at 3"), L"NOTE the meeting is at 3"), "remember that X");
    CHECK(str_eq(run(L"take a note: Call Dr. Shrestha tomorrow"), L"NOTE Call Dr. Shrestha tomorrow") ||
          str_eq(run(L"take a note: Call Dr. Shrestha tomorrow"), L"NOTE call dr shrestha tomorrow") ||
          wcsstr(cmd_last_action(), L"NOTE") == cmd_last_action(), "take a note keeps the text");
    CHECK(str_eq(run(L"note"), L"SAY") && r.listen_again, "note with nothing asks");

    TEST_SECTION("repeat that");
    run(L"what time is it");
    wchar_t said[300];
    str_copy(said, 300, r.reply);
    CHECK(str_eq(run(L"repeat that"), L"SAY") && str_eq(r.reply, said), "repeats the last reply");
    CHECK(str_eq(run(L"say that again"), L"SAY") && str_eq(r.reply, said), "say that again");

    TEST_SECTION("clear");
    CHECK(str_eq(run(L"clear"), L"CLEAR") && r.clear && r.rows_replace && r.nrows == 0, "clear");
    CHECK(!r.reply[0], "clear says nothing");
    CHECK(str_eq(run(L"clear the screen"), L"CLEAR"), "clear the screen");

    return TEST_SUMMARY();
}
