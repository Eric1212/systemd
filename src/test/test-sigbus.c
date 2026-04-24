/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#if HAVE_VALGRIND_VALGRIND_H
#  include <valgrind/valgrind.h>
#endif

#include "fd-util.h"
#include "fs-util.h"
#include "memory-util.h"
#include "sigbus.h"
#include "tests.h"

int main(int argc, char *argv[]) {
        _cleanup_close_ int fd = -EBADF;
        char template[] = "/tmp/sigbus-test-XXXXXX";
        void *addr = NULL;
        uint8_t *p;

        test_setup_logging(LOG_INFO);

#if HAS_FEATURE_ADDRESS_SANITIZER
        return log_tests_skipped("address-sanitizer is enabled");
#endif
#if HAVE_VALGRIND_VALGRIND_H
        if (RUNNING_ON_VALGRIND)
                return log_tests_skipped("This test cannot run on valgrind");
#endif

        /* Pre-flight: sigbus_handler() (src/basic/sigbus.c) relies on being able
         * to replace a faulting page with MAP_ANONYMOUS|MAP_FIXED. Some
         * restricted environments (mock/nspawn, TCG-emulated kernels) reject
         * MAP_FIXED at arbitrary addresses, which would cause the handler
         * itself to abort. Detect that upfront and skip rather than crash. */
        {
                void *probe = mmap(NULL, page_size(), PROT_NONE,
                                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
                if (probe == MAP_FAILED)
                        return log_tests_skipped_errno(errno, "probe mmap() failed");

                void *replaced = mmap(probe, page_size(), PROT_READ|PROT_WRITE,
                                      MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, -1, 0);
                if (replaced != probe) {
                        if (replaced != MAP_FAILED)
                                (void) munmap(replaced, page_size());
                        else
                                (void) munmap(probe, page_size());
                        return log_tests_skipped("MAP_ANONYMOUS|MAP_FIXED not honored in this environment");
                }
                (void) munmap(replaced, page_size());
        }

        sigbus_install();

        assert_se(sigbus_pop(&addr) == 0);

        assert_se((fd = mkostemp(template, O_RDWR|O_CREAT|O_EXCL)) >= 0);
        assert_se(unlink(template) >= 0);
        assert_se(posix_fallocate_loop(fd, 0, page_size() * 8) >= 0); /* NOLINT (posix-return) */

        p = mmap(NULL, page_size() * 16, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
        assert_se(p != MAP_FAILED);

        assert_se(sigbus_pop(&addr) == 0);

        p[0] = 0xFF;
        assert_se(sigbus_pop(&addr) == 0);

        p[page_size()] = 0xFF;
        assert_se(sigbus_pop(&addr) == 0);

        p[page_size()*8] = 0xFF;
        p[page_size()*8+1] = 0xFF;
        p[page_size()*10] = 0xFF;
        assert_se(sigbus_pop(&addr) > 0);
        assert_se(addr == p + page_size() * 8);
        assert_se(sigbus_pop(&addr) > 0);
        assert_se(addr == p + page_size() * 10);
        assert_se(sigbus_pop(&addr) == 0);

        sigbus_reset();
}
