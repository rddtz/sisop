// test_sofs.c - our own tests for the sofs filesystem.
// Covers the happy path and also edge cases that actually (sometimes) broke things while we were
// implementing: indirection, truncate, hardlink refcount, open limits, etc.
// File data is deterministic: byte i is (i * 31 + seed) & 0xFF, so we can write
// a pattern and check it came back untouched, this is easier to verify the results.
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

// walk the root dir and count valid entries
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

// look for a name in the dir, hand back its entry if found
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

int main()
{
    int particao = 0, spb = 2;
    SOFS_FILE f;
    int n;

    printf("=== format + mount ===\n");
    check(sofs_format(particao, spb) == 0, "format");
    check(sofs_mount(particao) == 0, "mount");
    check(count_dir() == 0, "dir is empty right after format");

    // write a short string, close, reopen, read it back
    printf("=== small file roundtrip ===\n");
    {
        char msg[] = "Hello, sofs filesystem!";
        char rbuf[128];
        f = sofs_create("hello.txt");
        check(f >= 0, "create hello.txt");
        n = sofs_write(f, msg, (int)strlen(msg));
        check(n == (int)strlen(msg), "write returns the full length");
        check(sofs_close(f) == 0, "close after write");

        f = sofs_open("hello.txt");
        check(f >= 0, "open hello.txt");
        memset(rbuf, 0, sizeof(rbuf));
        n = sofs_read(f, rbuf, sizeof(rbuf));
        check(n == (int)strlen(msg), "read returns what we wrote");
        check(strcmp(rbuf, msg) == 0, "content matches");
        sofs_close(f);
    }

    // 100 KB is past (2 + 128) * 512, so this hits both single and double
    // indirection. if the block mapping is wrong this is where it shows up.
    printf("=== large file (indirection) ===\n");
    {
        int big = 102400;
        char *wbuf = malloc(big);
        char *rbuf = malloc(big);
        fill_pattern(wbuf, big, 7);

        f = sofs_create("big.bin");
        check(f >= 0, "create big.bin");
        n = sofs_write(f, wbuf, big);
        check(n == big, "write 100 KB (single + double indirection)");
        sofs_close(f);

        f = sofs_open("big.bin");
        memset(rbuf, 0, big);
        n = sofs_read(f, rbuf, big);
        check(n == big, "read all 100 KB back");
        check(verify_pattern(rbuf, big, 7), "every byte survived the round trip");
        sofs_close(f);

        // the size readdir reports should match
        {
            SOFS_DIRENT d;
            check(find_in_dir("big.bin", &d) && d.fileSize == (DWORD)big,
                  "readdir reports the right size for big.bin");
        }
        free(wbuf);
        free(rbuf);
    }

    // one write call that crosses block 1 -> 2, i.e. the direct -> single
    // indirect handoff.
    printf("=== write across the direct/indirect boundary ===\n");
    {
        int sz = 1100; // > 1024 (two 512 blocks), spills into single indirect
        char wbuf[1100], rbuf[1100];
        fill_pattern(wbuf, sz, 99);
        f = sofs_create("cross.bin");
        check(sofs_write(f, wbuf, sz) == sz, "single write of 1100 bytes");
        sofs_close(f);
        f = sofs_open("cross.bin");
        memset(rbuf, 0, sz);
        check(sofs_read(f, rbuf, sz) == sz, "read 1100 bytes back");
        check(verify_pattern(rbuf, sz, 99), "bytes intact across the boundary");
        sofs_close(f);
    }

    printf("=== directory listing ===\n");
    check(count_dir() == 3, "three entries so far");

    // two writes without closing in between: position has to keep going
    printf("=== append / current position ===\n");
    {
        char rbuf[16];
        f = sofs_create("seq.txt");
        check(sofs_write(f, "AAAA", 4) == 4, "first write");
        check(sofs_write(f, "BBBB", 4) == 4, "second write continues, no seek");
        sofs_close(f);
        f = sofs_open("seq.txt"); // fresh open -> position back to 0
        memset(rbuf, 0, sizeof(rbuf));
        n = sofs_read(f, rbuf, sizeof(rbuf));
        check(n == 8 && memcmp(rbuf, "AAAABBBB", 8) == 0, "file is AAAABBBB, size 8");
        sofs_close(f);
    }

    // asking for more than the file has should give back only what exists
    printf("=== short read at end of file ===\n");
    {
        char rbuf[100];
        f = sofs_open("seq.txt");
        n = sofs_read(f, rbuf, 100);
        check(n == 8, "read past EOF returns the 8 real bytes");
        sofs_close(f);
    }

    // names are capped at 50 chars
    printf("=== name too long is rejected ===\n");
    {
        char longname[64];
        memset(longname, 'a', 51);
        longname[51] = '\0';
        check(sofs_create(longname) < 0, "create with 51-char name fails");
    }

    // open the same file up to the limit, the one past it must fail
    printf("=== open file limit ===\n");
    {
        SOFS_FILE h[12];
        int i, opened = 0, extra;
        for (i = 0; i < 12; i++) {
            h[i] = sofs_open("seq.txt");
            if (h[i] >= 0)
                opened++;
        }
        extra = h[11]; // the 12th request
        check(opened == 10, "exactly 10 handles open at once");
        check(extra < 0, "the 11th open is refused");
        for (i = 0; i < 12; i++)
            if (h[i] >= 0)
                sofs_close(h[i]);
    }

    // delete a name and make sure it is gone for good
    printf("=== delete ===\n");
    check(sofs_delete("hello.txt") == 0, "delete hello.txt");
    check(!find_in_dir("hello.txt", NULL), "hello.txt no longer listed");
    check(sofs_open("hello.txt") < 0, "opening a deleted file fails");

    // creating over an existing file wipes it back to zero
    printf("=== recreate truncates ===\n");
    {
        char rbuf[64];
        f = sofs_create("big.bin");
        check(f >= 0, "recreate big.bin");
        sofs_close(f);
        f = sofs_open("big.bin");
        check(sofs_read(f, rbuf, sizeof(rbuf)) == 0, "recreated file is empty");
        sofs_close(f);
    }

    // softlink: open should follow it to the real file
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
                  "softlink shows up as a LINK");
        }
        f = sofs_open("soft.lnk"); // should land on target.txt
        check(f >= 0, "open the softlink");
        memset(rbuf, 0, sizeof(rbuf));
        n = sofs_read(f, rbuf, sizeof(rbuf));
        check(n == (int)strlen(msg) && strcmp(rbuf, msg) == 0,
              "reading the link gives the target's data");
        sofs_close(f);
    }

    // a link pointing at nothing: creating it is fine, opening it is not
    printf("=== dangling softlink ===\n");
    {
        check(sofs_sln("dangling.lnk", "ghost.txt") == 0, "create link to missing file");
        check(sofs_open("dangling.lnk") < 0, "opening a dangling link fails");
    }

    // removing the link must not touch the file it pointed at
    printf("=== delete softlink keeps target ===\n");
    {
        check(sofs_delete("soft.lnk") == 0, "delete the softlink");
        check(!find_in_dir("soft.lnk", NULL), "link is gone");
        f = sofs_open("target.txt");
        check(f >= 0, "target file is still there");
        sofs_close(f);
    }

    // hardlink shares the inode; deleting one name must keep the data alive
    printf("=== hardlink + refcount ===\n");
    {
        char msg[] = "shared inode data";
        char rbuf[128];
        f = sofs_create("orig.dat");
        sofs_write(f, msg, (int)strlen(msg));
        sofs_close(f);

        check(sofs_hln("hard.lnk", "orig.dat") == 0, "create hardlink");
        f = sofs_open("hard.lnk");
        check(f >= 0, "open the hardlink");
        memset(rbuf, 0, sizeof(rbuf));
        n = sofs_read(f, rbuf, sizeof(rbuf));
        check(n == (int)strlen(msg) && strcmp(rbuf, msg) == 0,
              "hardlink reads the same bytes");
        sofs_close(f);

        // drop the original name, data has to live on through the hardlink
        check(sofs_delete("orig.dat") == 0, "delete the original name");
        f = sofs_open("hard.lnk");
        check(f >= 0, "hardlink still opens after the original is gone");
        memset(rbuf, 0, sizeof(rbuf));
        n = sofs_read(f, rbuf, sizeof(rbuf));
        check(n == (int)strlen(msg) && strcmp(rbuf, msg) == 0,
              "data intact through the hardlink");
        sofs_close(f);
        check(sofs_delete("hard.lnk") == 0, "delete the last name");
    }

    printf("=== umount ===\n");
    check(sofs_umount() == 0, "umount");

    printf("\n%s (failures: %d)\n",
           g_fail == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
