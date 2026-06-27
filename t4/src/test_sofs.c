/*
 * test_sofs.c - Integration tests for the sofs filesystem.
 *
 * Exercises every group-implemented function: create/write/read roundtrip,
 * single and double indirection (large files), opendir/readdir, delete,
 * softlinks, hardlinks (RefCounter semantics) and truncate-on-recreate.
 *
 * Deterministic data: byte i of a file equals (i * 31 + seed) & 0xFF.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "sofs.h"

static int g_fail = 0;

static void check(int cond, const char *msg)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond)
        g_fail++;
}

static void fill_pattern(char *buf, int n, int seed)
{
    int i;
    for (i = 0; i < n; i++)
        buf[i] = (char)((i * 31 + seed) & 0xFF);
}

static int verify_pattern(const char *buf, int n, int seed)
{
    int i;
    for (i = 0; i < n; i++)
        if ((unsigned char)buf[i] != (unsigned char)((i * 31 + seed) & 0xFF))
            return 0;
    return 1;
}

/* Count valid entries in the root directory via opendir/readdir. */
static int count_dir(void)
{
    SOFS_DIRENT d;
    int n = 0;
    if (sofs_opendir() != 0)
        return -1;
    while (sofs_readdir(&d) == 0)
        n++;
    sofs_closedir();
    return n;
}

static int find_in_dir(const char *name, SOFS_DIRENT *out)
{
    SOFS_DIRENT d;
    if (sofs_opendir() != 0)
        return 0;
    while (sofs_readdir(&d) == 0) {
        if (strcmp(d.name, name) == 0) {
            if (out) *out = d;
            sofs_closedir();
            return 1;
        }
    }
    sofs_closedir();
    return 0;
}

int main(void)
{
    int particao = 0, spb = 2;
    SOFS_FILE f;
    int n;

    printf("=== format + mount ===\n");
    check(sofs_format(particao, spb) == 0, "format");
    check(sofs_mount(particao) == 0, "mount");
    check(count_dir() == 0, "empty dir after format");

    /* --- small file roundtrip --- */
    printf("=== small file roundtrip ===\n");
    {
        char msg[] = "Hello, sofs filesystem!";
        char rbuf[128];
        f = sofs_create("hello.txt");
        check(f >= 0, "create hello.txt");
        n = sofs_write(f, msg, (int)strlen(msg));
        check(n == (int)strlen(msg), "write returns full length");
        check(sofs_close(f) == 0, "close after write");

        f = sofs_open("hello.txt");
        check(f >= 0, "open hello.txt");
        memset(rbuf, 0, sizeof(rbuf));
        n = sofs_read(f, rbuf, sizeof(rbuf));
        check(n == (int)strlen(msg), "read returns written length");
        check(strcmp(rbuf, msg) == 0, "content matches");
        sofs_close(f);
    }

    /* --- large file: single + double indirection --- */
    printf("=== large file (indirection) ===\n");
    {
        int big = 70000; /* > (2+128)*512 = 66560 -> forces double indirection */
        char *wbuf = malloc(big);
        char *rbuf = malloc(big);
        fill_pattern(wbuf, big, 7);

        f = sofs_create("big.bin");
        check(f >= 0, "create big.bin");
        n = sofs_write(f, wbuf, big);
        check(n == big, "write 70000 bytes (single+double indirection)");
        sofs_close(f);

        f = sofs_open("big.bin");
        memset(rbuf, 0, big);
        n = sofs_read(f, rbuf, big);
        check(n == big, "read back 70000 bytes");
        check(verify_pattern(rbuf, big, 7), "large content matches byte-for-byte");
        sofs_close(f);

        /* size reported by readdir */
        {
            SOFS_DIRENT d;
            check(find_in_dir("big.bin", &d) && d.fileSize == (DWORD)big,
                  "readdir reports correct size for big.bin");
        }
        free(wbuf);
        free(rbuf);
    }

    /* --- directory listing --- */
    printf("=== directory listing ===\n");
    check(count_dir() == 2, "two entries (hello.txt, big.bin)");

    /* --- delete --- */
    printf("=== delete ===\n");
    check(sofs_delete("hello.txt") == 0, "delete hello.txt");
    check(count_dir() == 1, "one entry after delete");
    check(!find_in_dir("hello.txt", NULL), "hello.txt gone");
    check(sofs_open("hello.txt") < 0, "open deleted file fails");

    /* --- truncate on recreate --- */
    printf("=== truncate on recreate ===\n");
    {
        char rbuf[64];
        f = sofs_create("big.bin"); /* recreate existing -> truncate to 0 */
        check(f >= 0, "recreate big.bin truncates");
        sofs_close(f);
        f = sofs_open("big.bin");
        n = sofs_read(f, rbuf, sizeof(rbuf));
        check(n == 0, "truncated file reads 0 bytes");
        sofs_close(f);
    }

    /* --- softlink --- */
    printf("=== softlink ===\n");
    {
        char msg[] = "target file contents";
        char rbuf[128];
        f = sofs_create("target.txt");
        sofs_write(f, msg, (int)strlen(msg));
        sofs_close(f);

        check(sofs_sln("soft.lnk", "target.txt") == 0, "create softlink");
        {
            SOFS_DIRENT d;
            check(find_in_dir("soft.lnk", &d) && d.fileType == TYPEVAL_LINK,
                  "softlink entry has type LINK");
        }
        f = sofs_open("soft.lnk"); /* must resolve to target.txt */
        check(f >= 0, "open softlink");
        memset(rbuf, 0, sizeof(rbuf));
        n = sofs_read(f, rbuf, sizeof(rbuf));
        check(n == (int)strlen(msg) && strcmp(rbuf, msg) == 0,
              "softlink resolves to target content");
        sofs_close(f);
    }

    /* --- hardlink + RefCounter --- */
    printf("=== hardlink ===\n");
    {
        char msg[] = "shared inode data";
        char rbuf[128];
        f = sofs_create("orig.dat");
        sofs_write(f, msg, (int)strlen(msg));
        sofs_close(f);

        check(sofs_hln("hard.lnk", "orig.dat") == 0, "create hardlink");
        f = sofs_open("hard.lnk");
        check(f >= 0, "open hardlink");
        memset(rbuf, 0, sizeof(rbuf));
        n = sofs_read(f, rbuf, sizeof(rbuf));
        check(n == (int)strlen(msg) && strcmp(rbuf, msg) == 0,
              "hardlink reads same data");
        sofs_close(f);

        /* delete original: data must survive via the hardlink (RefCounter) */
        check(sofs_delete("orig.dat") == 0, "delete original name");
        f = sofs_open("hard.lnk");
        check(f >= 0, "hardlink still open-able after deleting original");
        memset(rbuf, 0, sizeof(rbuf));
        n = sofs_read(f, rbuf, sizeof(rbuf));
        check(n == (int)strlen(msg) && strcmp(rbuf, msg) == 0,
              "data intact through hardlink after original deleted");
        sofs_close(f);
        check(sofs_delete("hard.lnk") == 0, "delete last hardlink");
    }

    printf("=== umount ===\n");
    check(sofs_umount() == 0, "umount");

    printf("\n%s (failures: %d)\n", g_fail == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
