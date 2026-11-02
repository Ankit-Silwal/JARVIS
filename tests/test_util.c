/* Unit tests for src/util.c */
#include "../src/jarvis.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

/* util.c's log_line() reads g_cfg; config.c is not linked here.
 * data_dir stays empty, so log_line() is a no-op. */
Config g_cfg;

static void test_number_word(void)
{
    TEST_SECTION("number_word");
    CHECK(number_word(L"two") == 2, "two -> 2");
    CHECK(number_word(L"second") == 2, "second -> 2");
    CHECK(number_word(L"3rd") == 3, "3rd -> 3");
    CHECK(number_word(L"for") == 4, "for -> 4");
    CHECK(number_word(L"x") == 0, "x -> 0");
    CHECK(number_word(L"one") == 1, "one -> 1");
    CHECK(number_word(L"tenth") == 10, "tenth -> 10");
    CHECK(number_word(L"7") == 7, "7 -> 7");
    CHECK(number_word(L"12") == 12, "12 -> 12");
    CHECK(number_word(L"1st") == 1, "1st -> 1");
    CHECK(number_word(L"21st") == 21, "21st -> 21");
    CHECK(number_word(L"2x") == 0, "2x -> 0 (digits + junk)");
    CHECK(number_word(L"") == 0, "empty -> 0");
    CHECK(number_word(L"twos") == 0, "twos -> 0 (no prefix match)");
}

static void test_tokenize(void)
{
    TEST_SECTION("tokenize");
    Token t[16];
    int n = tokenize(L"  Open Chapter-Two, for 3rd! to second  ", t, 16);
    CHECKF(n == 7, "token count: got %d", n);
    if (n == 7) {
        CHECK(str_eq(t[0].w, L"open") && !t[0].alt[0], "open, no alt");
        CHECK(str_eq(t[1].w, L"chapter") && !t[1].alt[0], "chapter lowercased");
        CHECK(str_eq(t[2].w, L"two") && str_eq(t[2].alt, L"2"), "two -> alt 2");
        CHECK(str_eq(t[3].w, L"for") && !t[3].alt[0], "for has no alt");
        CHECK(str_eq(t[4].w, L"3rd") && !t[4].alt[0], "3rd (digit word) has no alt");
        CHECK(str_eq(t[5].w, L"to") && !t[5].alt[0], "to has no alt");
        CHECK(str_eq(t[6].w, L"second") && str_eq(t[6].alt, L"2"), "second -> alt 2");
    }

    n = tokenize(L"too won ate tree three ten", t, 16);
    CHECKF(n == 6, "homophone count: got %d", n);
    if (n == 6) {
        CHECK(!t[0].alt[0] && !t[1].alt[0] && !t[2].alt[0] && !t[3].alt[0],
              "too/won/ate/tree have no alt");
        CHECK(str_eq(t[4].alt, L"3"), "three -> alt 3");
        CHECK(str_eq(t[5].alt, L"10"), "ten -> alt 10");
    }

    CHECK(tokenize(L"", t, 16) == 0, "empty string -> 0 tokens");
    CHECK(tokenize(L" ,.-_ ", t, 16) == 0, "only separators -> 0 tokens");
    CHECK(tokenize(L"a b c d e", t, 3) == 3, "max respected");
    n = tokenize(L"IMG_2041.jpg", t, 16);
    CHECK(n == 3 && str_eq(t[0].w, L"img") && str_eq(t[1].w, L"2041") && str_eq(t[2].w, L"jpg"),
          "underscore/dot split");

    wchar_t longw[100];
    for (int i = 0; i < 99; i++) longw[i] = L'a';
    longw[99] = 0;
    n = tokenize(longw, t, 16);
    CHECK(n == 1 && wcslen(t[0].w) == 63, "long word truncated to 63 chars, one token");
}

static void test_str_after_word(void)
{
    TEST_SECTION("str_after_word");
    const wchar_t *r;
    r = str_after_word(L"open chrome", L"open");
    CHECK(r && str_eq(r, L"chrome"), "rest after word");
    r = str_after_word(L"open", L"open");
    CHECK(r && r[0] == 0, "exact match -> empty rest");
    CHECK(str_after_word(L"opened file", L"open") == NULL, "prefix of longer word -> NULL");
    CHECK(str_after_word(L"close it", L"open") == NULL, "different word -> NULL");
    r = str_after_word(L"show me file", L"show me");
    CHECK(r && str_eq(r, L"file"), "multi-word prefix");
}

static void test_spaces(void)
{
    TEST_SECTION("str_trim / str_collapse_spaces");
    wchar_t s[64];
    str_copy(s, 64, L"  a b  "); str_trim(s);
    CHECK(str_eq(s, L"a b"), "trim both ends");
    str_copy(s, 64, L"   "); str_trim(s);
    CHECK(s[0] == 0, "trim all-space -> empty");
    str_copy(s, 64, L""); str_trim(s);
    CHECK(s[0] == 0, "trim empty");
    str_copy(s, 64, L"\t\r\nx\n"); str_trim(s);
    CHECK(str_eq(s, L"x"), "trim tabs/newlines");

    str_copy(s, 64, L"  a \t  b   c  "); str_collapse_spaces(s);
    CHECK(str_eq(s, L"a b c"), "collapse inner + trim");
    str_copy(s, 64, L"abc"); str_collapse_spaces(s);
    CHECK(str_eq(s, L"abc"), "collapse no-op");
    str_copy(s, 64, L" \t "); str_collapse_spaces(s);
    CHECK(s[0] == 0, "collapse all-space -> empty");

    str_copy(s, 4, L"abcdef");
    CHECK(str_eq(s, L"abc"), "str_copy truncates to cap-1");
    str_copy(s, 8, L"ab"); str_cat(s, 8, L"cdefghij");
    CHECK(str_eq(s, L"abcdefg"), "str_cat truncates to cap-1");
    CHECK(str_starts(L"hello", L"he") && !str_starts(L"he", L"hello"), "str_starts");
    CHECK(str_ends(L"file.pdf", L".pdf") && !str_ends(L"df", L".pdf"), "str_ends");
}

static void test_edit_distance(void)
{
    TEST_SECTION("edit_distance");
    CHECK(edit_distance(L"kitten", 6, L"sitting", 7, 3) == 3, "kitten/sitting = 3");
    CHECK(edit_distance(L"kitten", 6, L"sitting", 7, 2) == 3, "over limit 2 -> limit+1");
    CHECK(edit_distance(L"abc", 3, L"abc", 3, 0) == 0, "identical = 0");
    CHECK(edit_distance(L"", 0, L"abc", 3, 3) == 3, "empty vs abc = 3");
    CHECK(edit_distance(L"", 0, L"", 0, 0) == 0, "empty vs empty = 0");
    CHECK(edit_distance(L"notes", 5, L"notez", 5, 1) == 1, "one substitution");
    CHECK(edit_distance(L"physics", 7, L"physic", 6, 1) == 1, "one deletion");
    /* length difference alone exceeds the limit -> immediate limit+1 */
    CHECK(edit_distance(L"a", 1, L"abcdef", 6, 2) == 3, "length gap > limit -> limit+1");
    /* early exit: every cell in a row exceeds the limit */
    CHECK(edit_distance(L"aaaa", 4, L"bbbb", 4, 1) == 2, "early exit -> limit+1");
    CHECK(edit_distance(L"abcdefgh", 8, L"zzzzzzzz", 8, 2) == 3, "early exit -> limit+1 (long)");
    /* words longer than 63 are refused */
    wchar_t big[70];
    for (int i = 0; i < 69; i++) big[i] = L'x';
    big[69] = 0;
    CHECK(edit_distance(big, 64, big, 64, 5) == 6, "len > 63 -> limit+1");
    CHECK(edit_distance(big, 63, big, 63, 5) == 0, "len 63 still computed");

    /* The header comment says "returns limit+1 when over the limit", but when
     * the last row's minimum is within the limit while the final cell is not,
     * the true distance (which can be > limit+1) is returned.
     * KNOWN BUG (minor, contract only): edit_distance("xab",3,"abyy",4,1)
     * returns 3, not 2. Callers only test `<= limit`, so behaviour is OK. */
    int d = edit_distance(L"xab", 3, L"abyy", 4, 1);
    CHECKF(d > 1, "xab/abyy over limit 1 must be > limit (got %d)", d);
    CHECK(d == 2, "edit_distance(\"xab\",\"abyy\",limit=1) should return limit+1=2");
}

static void test_path_name(void)
{
    TEST_SECTION("path_name");
    CHECK(str_eq(path_name(L"C:\\a\\b\\file.txt"), L"file.txt"), "backslash path");
    CHECK(str_eq(path_name(L"C:/a/b/file.txt"), L"file.txt"), "forward slash path");
    CHECK(str_eq(path_name(L"file.txt"), L"file.txt"), "bare name");
    CHECK(str_eq(path_name(L"C:\\Users\\Me\\Docs"), L"Docs"), "folder");
    /* trailing separator: the last non-empty component is kept with its slash */
    CHECK(str_eq(path_name(L"C:\\dir\\"), L"dir\\"), "trailing slash keeps last component");
    CHECK(str_eq(path_name(L""), L""), "empty");
}

static void test_utf8(void)
{
    TEST_SECTION("utf8 <-> wide");
    const wchar_t *src = L"R\u00e9sum\u00e9 \u2713 \u65e5\u672c \U0001F600 end";
    char *u = utf8_from_wide(src);
    CHECK(u != NULL, "utf8_from_wide allocates");
    if (u) {
        CHECK(strncmp(u, "R\xc3\xa9sum\xc3\xa9", 8) == 0, "utf8 bytes for e-acute");
        wchar_t *w = wide_from_utf8(u, -1);
        CHECK(w && str_eq(w, src), "round trip with len=-1");
        free(w);
        int len = (int)strlen(u);
        w = wide_from_utf8(u, len);
        CHECK(w && str_eq(w, src), "round trip with explicit length");
        free(w);
        w = wide_from_utf8(u, 1);
        CHECK(w && str_eq(w, L"R"), "partial length");
        free(w);
        free(u);
    }
    u = utf8_from_wide(L"");
    CHECK(u && u[0] == 0, "empty -> empty");
    free(u);
    wchar_t *w = wide_from_utf8("", 0);
    CHECK(w && w[0] == 0, "empty utf8 (len 0) -> empty wide");
    free(w);

    wchar_t *d = wcs_dup(L"dup me");
    CHECK(d && str_eq(d, L"dup me"), "wcs_dup");
    free(d);

    wchar_t s[32];
    str_copy(s, 32, L"MiXeD"); str_lower(s);
    CHECK(str_eq(s, L"mixed"), "str_lower ASCII");
    /* KNOWN BUG: str_lower()/tokenize() use towlower() in the default "C"
     * locale, which only folds ASCII. "\u00c9" (E-acute) stays upper case,
     * so a file named "R\u00c9SUM\u00c9.pdf" is not found by a spoken "r\u00e9sum\u00e9". */
    str_copy(s, 32, L"\u00c9T\u00c9"); str_lower(s);
    CHECK(str_eq(s, L"\u00e9t\u00e9"), "str_lower does not lowercase non-ASCII (E-acute)");
}

int wmain(void)
{
    test_number_word();
    test_tokenize();
    test_str_after_word();
    test_spaces();
    test_edit_distance();
    test_path_name();
    test_utf8();
    return TEST_SUMMARY();
}
