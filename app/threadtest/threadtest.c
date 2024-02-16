/*
 * Copyright (c) 2022, Google Inc. All rights reserved
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <err.h>
#include <list.h>
#include <shared/lk/macros.h>
#include <kernel/thread.h>
#include <lib/unittest/unittest.h>
#include <lib/rand/rand.h>

/* run test in thread that will exit on panic rather than halting execution */
static int threadtest_run_in_thread(const char* thread_name,
                                    int (*func)(void* arg),
                                    void* arg) {
    int ret;
    int thread_ret;
    struct thread* thread;
    thread = thread_create(thread_name, func, arg, DEFAULT_PRIORITY,
                           DEFAULT_STACK_SIZE);
    if (!thread) {
        return ERR_NO_MEMORY;
    }

    thread_set_flag_exit_on_panic(thread, true);

    ret = thread_resume(thread);
    if (ret) {
        return ret;
    }

    ret = thread_join(thread, &thread_ret, INFINITE_TIME);
    if (ret) {
        return ret;
    }

    return thread_ret;
}

static void thread_test_corrupt_current_thread_cookie() {
    thread_t *curr = get_current_thread();
    curr->cookie = curr->cookie + 1;
}

static int thread_test_corrupt_cookie_before_yield_fn(void *unused) {
    thread_test_corrupt_current_thread_cookie();
    /* put thread at the end run queue before calling thread_resched */
    thread_yield();

    /* should not get here */
    return ERR_GENERIC;
}

TEST(threadtest, cookie_corruption_before_yield_must_panic) {
    int ret;
    ret = threadtest_run_in_thread("yielding_cookie_corrupter_thread",
                                   thread_test_corrupt_cookie_before_yield_fn,
                                   NULL);
    /*
     * The thread will corrupt its own cookie which will cause a panic.
     * Because the test thread is set to exit on panic, the thread_exit
     * function will set its return value to ERR_FAULT.
     */
    EXPECT_EQ(ret, ERR_FAULT);
}

static int thread_test_corrupt_cookie_before_preempt_fn(void *unused) {
    thread_test_corrupt_current_thread_cookie();
    /*
     * put thread at the head of run queue before calling thread_resched
     *
     * note: this relies on the thread having a positive remaining quantum
     * which *should* be satisfied given that the thread was just started.
     */
    thread_preempt();

    /* should not get here */
    return ERR_GENERIC;
}

TEST(threadtest, cookie_corruption_before_preempt_must_panic) {
    int ret;
    ret = threadtest_run_in_thread("preempted_cookie_corrupter_thread",
                                   thread_test_corrupt_cookie_before_preempt_fn,
                                   NULL);
    EXPECT_EQ(ret, ERR_FAULT);
}

static int thread_test_corrupt_cookie_before_exit_fn(void *unused) {
    thread_test_corrupt_current_thread_cookie();

    /* exit thread with corrupt cookie */
    return ERR_GENERIC;
}

TEST(threadtest, cookie_corruption_before_exit_must_panic) {
    int ret;
    ret = threadtest_run_in_thread("exiting_cookie_corrupter_thread",
                                   thread_test_corrupt_cookie_before_exit_fn,
                                   NULL);
    EXPECT_EQ(ret, ERR_FAULT);
}

static int cookie_tester(void *_unused) {
    return 0;
}

TEST(threadtest, threads_have_valid_cookies) {
    thread_t *curr = get_current_thread();
    thread_t *new;

    new = thread_create("cookie tester", &cookie_tester, NULL, DEFAULT_PRIORITY,
                        DEFAULT_STACK_SIZE);

    /*
     * Threads must have the same cookie value modulo the effects of
     * xor'ing the cookie with the address of the associated thread.
     */
    EXPECT_EQ(new->cookie ^ (uint64_t)new, curr->cookie ^ (uint64_t)curr);

    /*
     * xor'ing the cookie with the address of the associated thread should
     * make thread cookies unique to each thread because addresses differ.
     */
    EXPECT_NE(new->cookie, curr->cookie);

    /* start and join the thread so it gets reclaimed */
    thread_resume(new);
    thread_join(new, NULL, INFINITE_TIME);
}

PORT_TEST(threadtest, "com.android.kernel.threadtest");
