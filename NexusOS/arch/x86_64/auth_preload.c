/* LD_PRELOAD probe for tests/test_x64_auth_ext4.py: leaves /tmp/authpre-ran when ld.so loads it (it must not for
 * set-user-ID programs: AT_SECURE). */
#include <fcntl.h>
#include <unistd.h>
__attribute__((constructor)) static void loaded(void) {
    int fd = open("/tmp/authpre-ran", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) close(fd);
}
