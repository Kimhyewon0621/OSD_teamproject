#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int passed = 0;
int failed = 0;

void check(const char *test, int actual, int expected)
{
    if (actual == expected)
    {
        printf("[PASS] %s : got %d\n", test, actual);
        passed++;
    }
    else
    {
        printf("[FAIL] %s : expected %d, got %d\n", test, expected, actual);
        failed++;
    }
}

int main()
{
    int ret;

    printf("[INFO] Three processes are created at startup: init(pid 1), sh(pid 2), and mytest(pid 3).\n");
    printf("[INFO] This program verifies the correctness of 5 system calls.\n");

    // ============================================================
    printf("========== Current Process State ==========\n");
    // ============================================================
    printf("[INFO] Listing all currently active processes:\n");
    ps(0);
    printf("\n");

    // ============================================================
    printf("========== Testing getnice() ==========\n");
    // ============================================================

    // pid 1 (init) exists, default nice = 20
    ret = getnice(1);
    check("getnice(1) - checking default nice value of init", ret, 20);

    // pid 2 (sh) exists, default nice = 20
    ret = getnice(2);
    check("getnice(2) - checking default nice value of sh", ret, 20);

    // pid 3 (mytest) exists, default nice = 20
    ret = getnice(3);
    check("getnice(3) - checking default nice value of mytest", ret, 20);

    // pid 15 does not exist → should return -1
    ret = getnice(15);
    check("getnice(15) - process does not exist", ret, -1);

    printf("\n");

    // ============================================================
    printf("========== Testing setnice() ==========\n");
    // ============================================================

    // Set nice of pid 1 (init) to 10 → should succeed (return 0)
    ret = setnice(1, 10);
    check("setnice(1, 10) - setting valid nice value", ret, 0);

    // Verify the change
    ret = getnice(1);
    check("getnice(1) - verifying nice value after setnice", ret, 10);

    // Restore to default
    ret = setnice(1, 20);
    check("setnice(1, 20) - restoring to default nice value", ret, 0);

    // boundary value test: nice = 0 (minimum valid)
    ret = setnice(1, 0);
    check("setnice(1, 0) - minimum boundary value", ret, 0);
    ret = getnice(1);
    check("getnice(1) - verifying minimum boundary value", ret, 0);

    // boundary value test: nice = 39 (maximum valid)
    ret = setnice(1, 39);
    check("setnice(1, 39) - maximum boundary value", ret, 0);
    ret = getnice(1);
    check("getnice(1) - verifying maximum boundary value", ret, 39);

    // Restore to default
    ret = setnice(1, 20);
    check("setnice(1, 20) - restoring to default nice value", ret, 0);

    // pid 15 does not exist → should return -1
    ret = setnice(15, 10);
    check("setnice(15, 10) - process does not exist", ret, -1);

    // nice value 40 is out of range (valid: 0–39) → should return -1
    ret = setnice(1, 40);
    check("setnice(1, 40) - nice value exceeds valid range", ret, -1);

    // nice value -1 is out of range → should return -1
    ret = setnice(1, -1);
    check("setnice(1, -1) - nice value below valid range", ret, -1);

    printf("\n");

    // ============================================================
    printf("========== Testing ps() ==========\n");
    // ============================================================

    // ps(0) prints all processes — verify visually
    printf("[INFO] ps(0) - listing all active processes (init, sh, mytest):\n");
    ps(0);
    printf("\n");

    // ps(1) prints only init
    printf("[INFO] ps(1) - listing only init process:\n");
    ps(1);
    printf("\n");

    // ps(2) prints only sh
    printf("[INFO] ps(2) - listing only sh process:\n");
    ps(2);
    printf("\n");

    // ps(3) prints only mytest
    printf("[INFO] ps(3) - listing only mytest process:\n");
    ps(3);
    printf("\n");

    // ps(15) — no such process, should print nothing
    printf("[INFO] ps(15) - process does not exist, expecting no output:\n");
    ps(15);
    printf("[INFO] (no output expected above)\n");

    printf("\n");

    // ============================================================
    printf("========== Testing meminfo() ==========\n");
    // ============================================================

    uint64 mem1 = meminfo();
    if (mem1 > 0)
    {
        printf("[PASS] meminfo() - available free memory: %lu bytes\n", mem1);
        passed++;
    }
    else
    {
        printf("[FAIL] meminfo() - expected > 0, got %lu\n", mem1);
        failed++;
    }

    // verify meminfo changes after fork
    int mpid = fork();
    if (mpid == 0)
    {
        exit(0);
    }
    else
    {
        uint64 mem2 = meminfo();
        if (mem2 <= mem1)
        {
            printf("[PASS] meminfo() - memory decreased after fork: %lu -> %lu bytes\n", mem1, mem2);
            passed++;
        }
        else
        {
            printf("[FAIL] meminfo() - expected memory to decrease after fork\n");
            failed++;
        }
        waitpid(mpid);
    }

    printf("\n");

    // ============================================================
    printf("========== Testing waitpid() ==========\n");
    // ============================================================
    // Note: waitpid suspends execution until a specific process terminates.
    // To test this, we use fork() to create child processes that exit immediately,
    // then verify that waitpid correctly waits for them.

    // Test 1: fork a child, then waitpid for it
    int pid1 = fork();
    if (pid1 < 0)
    {
        printf("[FAIL] fork() failed\n");
        failed++;
    }
    else if (pid1 == 0)
    {
        // child process — exit immediately
        exit(0);
    }
    else
    {
        // parent — wait for pid1 to terminate
        ret = waitpid(pid1);
        check("waitpid(pid1) - waiting for first child to exit", ret, 0);
    }

    // Test 2: fork another child, waitpid for it
    int pid2 = fork();
    if (pid2 < 0)
    {
        printf("[FAIL] fork() failed\n");
        failed++;
    }
    else if (pid2 == 0)
    {
        // child process — exit immediately
        exit(0);
    }
    else
    {
        // parent — wait for pid2 to terminate
        ret = waitpid(pid2);
        check("waitpid(pid2) - waiting for second child to exit", ret, 0);
    }

    // Test 3: fork a third child, waitpid for it
    int pid3 = fork();
    if (pid3 < 0)
    {
        printf("[FAIL] fork() failed\n");
        failed++;
    }
    else if (pid3 == 0)
    {
        // child process — exit immediately
        exit(0);
    }
    else
    {
        // parent — wait for pid3 to terminate
        ret = waitpid(pid3);
        check("waitpid(pid3) - waiting for third child to exit", ret, 0);
    }

    // Test 4: waitpid for init (pid 1) — should fail
    // (init never exits / caller is not init's parent)
    ret = waitpid(1);
    check("waitpid(1) - init is not our child", ret, -1);

    // Test 5: waitpid for non-existent pid — should fail
    ret = waitpid(99);
    check("waitpid(99) - process does not exist", ret, -1);

    // Test 6: waitpid for already terminated pid — should fail
    ret = waitpid(pid1);
    check("waitpid(pid1) - already terminated process", ret, -1);

    printf("\n");

    // ============================================================
    printf("========== Summary ==========\n");
    // ============================================================
    printf("Total: %d passed, %d failed\n", passed, failed);

    exit(0);
}
