// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Socket protocol I/O tests. Exercise the production framing helpers over
 * socketpairs, including fragmented requests and complete response writes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>

#include "../../userspace/daemon/bdfs_socket_io.h"

static int tests_run;
static int tests_failed;

#define CHECK(desc, expr) do { \
    tests_run++; \
    if (!(expr)) { \
        fprintf(stderr, "FAIL: %s\n", desc); \
        tests_failed++; \
    } else { \
        printf("PASS: %s\n", desc); \
    } \
} while (0)

struct writer_args {
    int fd;
    const char *data;
    size_t length;
};

static void *fragmented_writer(void *opaque)
{
    struct writer_args *args = opaque;
    for (size_t i = 0; i < args->length; i++) {
        ssize_t n = write(args->fd, args->data + i, 1);
        if (n != 1)
            return (void *)1;
    }
    return NULL;
}

static void test_fragmented_request(void)
{
    int fds[2];
    const char request[] = "{\"cmd\":\"ping\"}\n";
    char buffer[64] = {0};
    pthread_t thread;
    struct writer_args args;
    void *thread_result = NULL;

    CHECK("socketpair for fragmented request", socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    args.fd = fds[0];
    args.data = request;
    args.length = sizeof(request) - 1;
    CHECK("start fragmented writer", pthread_create(&thread, NULL, fragmented_writer, &args) == 0);
    int rc = bdfs_socket_read_request(fds[1], buffer, sizeof(buffer));
    pthread_join(thread, &thread_result);
    CHECK("fragmented newline request accepted", rc == (int)(sizeof(request) - 2));
    CHECK("fragmented request content preserved", strcmp(buffer, "{\"cmd\":\"ping\"}") == 0);
    CHECK("fragment writer completed", thread_result == NULL);
    close(fds[0]);
    close(fds[1]);
}

static void test_oversized_request(void)
{
    int fds[2];
    char buffer[8] = {0};
    const char request[] = "12345678";
    CHECK("socketpair for oversized request", socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    CHECK("write oversized request bytes", write(fds[0], request, sizeof(request) - 1) == (ssize_t)(sizeof(request) - 1));
    CHECK("oversized request rejected", bdfs_socket_read_request(fds[1], buffer, sizeof(buffer)) == -EMSGSIZE);
    close(fds[0]);
    close(fds[1]);
}

static void test_embedded_nul_request(void)
{
    int fds[2];
    char buffer[32] = {0};
    const char request[] = {'{', '}', '\0', '\n'};
    CHECK("socketpair for NUL request", socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    CHECK("write NUL request bytes", write(fds[0], request, sizeof(request)) == (ssize_t)sizeof(request));
    CHECK("embedded NUL request rejected", bdfs_socket_read_request(fds[1], buffer, sizeof(buffer)) == -EINVAL);
    close(fds[0]);
    close(fds[1]);
}

static void test_complete_response_write(void)
{
    int fds[2];
    char buffer[64] = {0};
    const char response[] = "{\"status\":\"ok\"}\n";
    CHECK("socketpair for response", socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    CHECK("complete response write", bdfs_socket_write_response(fds[0], response, sizeof(response) - 1) == 0);
    ssize_t n = read(fds[1], buffer, sizeof(buffer) - 1);
    CHECK("response bytes received", n == (ssize_t)(sizeof(response) - 1));
    CHECK("response contents preserved", strcmp(buffer, response) == 0);
    close(fds[0]);
    close(fds[1]);
}

int main(void)
{
    test_fragmented_request();
    test_oversized_request();
    test_embedded_nul_request();
    test_complete_response_write();
    printf("\n%d/%d tests passed\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}
