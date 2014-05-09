/*
 * Copyright (C) 2010-2012, Andreas Sandberg
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <errno.h>

#include <sys/mman.h>
#include <sys/socket.h>

#include "perf_common.h"
#include "expect.h"

typedef enum {
    SYNC_WAITING = 0,
    SYNC_GO,
    SYNC_ABORT,
} sync_type_t;

typedef struct {
    sync_type_t type;
} sync_msg_t;

#if !defined(MAP_HUGETLB)
/* MAP_HUGETLB is supported for kernels newer than 2.6.32 (might have
 * been supported earlier). Define MAP_HUGETLB in case it wasn't
 * defined by the system headers. */
#define MAP_HUGETLB     0x40000
#endif 

/* Round size (upwards) to the nearest multiple of 2 MiB */
#define ROUND_U(x) (((x) + (1 << 21)) & ~((1 << 21) - 1))

struct perf_event_attr perf_base_attr = {
    .disabled = 0,
    .inherit = 0,

    /* The following should only be set for the group leader */
    .pinned = 0,
    .exclusive = 0,

    .exclude_user = 0,
    .exclude_kernel = 1,
    .exclude_hv = 1,
    .exclude_idle = 1,

    .mmap = 0,
    .comm = 0,
    .freq = 0,
    .inherit_stat = 0,
    .enable_on_exec = 0,
    .task = 0,
    .watermark = 0,
    // .sample_period = 10000000,
};

ctr_list_t perf_ctrs = { NULL, NULL };

long
perf_argp_parse_long(const char *name, const char *arg, struct argp_state *state)
{
    char *endptr;
    long value;

    errno = 0;
    value = strtol(arg, &endptr, 0);
    if (errno)
        argp_failure(state, EXIT_FAILURE, errno,
                     "Invalid %s", name);
    else if (*arg == '\0' || *endptr != '\0')
        argp_error(state, "Invalid %s: '%s' is not a number.\n", name, arg);

    return value;
}

size_t
write_all(int fd, const void *buf, size_t size)
{
    char *_buf = (char *)buf;
    size_t _size = size;
    do {
        ssize_t ret;
        ret = write(fd, _buf, _size);
        if (ret == -1)
            EXPECT_ERRNO(errno == EAGAIN);
        else {
            _size -= ret;
            _buf += ret;
        }
    } while(_size);

    return size;
}

void *
mem_huge_alloc(size_t size)
{
    void *ptr;
    ptr = mmap(NULL, ROUND_U(size),
           PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
           -1, 0);

    return ptr == MAP_FAILED ? NULL : ptr;
}

void
mem_huge_free(void *addr, size_t size)
{
    munmap(addr, ROUND_U(size));
}


ctr_t *
ctr_create(const struct perf_event_attr *base_attr)
{
    ctr_t *ctr;
    ctr = malloc(sizeof(ctr_t));
    if (!ctr)
        return NULL;

    if (!base_attr)
        memset(ctr, 0, sizeof(ctr_t));
    else
        ctr->attr = *base_attr;

    ctr->fd = -1;
    ctr->next = NULL;

    return ctr;
}


int
ctr_attach(ctr_t *ctr, pid_t pid, int cpu, int group_fd, int flags)
{
    assert(ctr->fd == -1);

    ctr->attr.size = PERF_ATTR_SIZE_VER0;
    ctr->fd = perf_event_open(&ctr->attr, pid, cpu, group_fd, flags);

    fprintf(stderr, "Name: %s Type: %d Config 0x%" PRIx64 " Config1 0x%" PRIx64 
            " Config2 0x%" PRIx64 "\n",ctr->event_name, ctr->attr.type, 
            ctr->attr.config, ctr->attr.config1, ctr->attr.config2 );

    if (ctr->fd == -1) {
        perror("Failed to attach performance counter");
        return -1;
    }

    return ctr->fd;
}

int
ctrs_attach(ctr_list_t *list, pid_t pid, int cpu, int flags)
{
    for (ctr_t *cur = list->head; cur; cur = cur->next) {
        /* Use the first counter as the group_fd */
        ctr_attach(cur, pid, cpu,
                   cur != list->head ? list->head->fd : -1,
                   flags);

        if (cur->fd == -1) {
            ctrs_close(list);
            return -1;
        }
    }
    return 0;
}

void
ctrs_close(ctr_list_t *list)
{
    for (ctr_t *cur = list->head; cur; cur = cur->next) {
        if (cur->fd != -1) {
            close(cur->fd);
            cur->fd = -1;
        }
    }
}

ctr_t *
ctrs_add(ctr_list_t *list, ctr_t *ctr)
{
    ctr->next = NULL;

    if (list->tail) {
        assert(list->head);
        list->tail->next = ctr;
        list->tail = ctr;
    } else {
        list->head = ctr;
        list->tail = ctr;
    }

    return ctr;
}

int
ctrs_len(ctr_list_t *list)
{
    int count = 0;
    for (ctr_t *cur = list->head; cur; cur = cur->next)
        count++;
    return count;
}

// void
// ctrs_cpy_conf(ctr_list_t *dest, ctr_list_t *src)
// {
//     assert(!dest->head);
//     assert(!dest->tail);
//     assert(src->head);
//     assert(src->tail);

//     for (ctr_t *cur = src->head; cur; cur = cur->next) {
//         ctr_t *ctr;

//         ctr = ctr_create(&cur->attr);
//         ctrs_add(dest, ctr);
//     }
// }

static void
sync_send(int fd, const sync_msg_t *msg)
{
    EXPECT_ERRNO(send(fd, msg, sizeof(sync_msg_t), 0) == sizeof(sync_msg_t));
}

static void
sync_wait(int fd, sync_msg_t *msg)
{
    EXPECT_ERRNO(recv(fd, msg, sizeof(sync_msg_t), MSG_WAITALL) == sizeof(sync_msg_t));
}

static void
sync_send_simple(int fd, sync_type_t type)
{
    sync_msg_t msg = {
        .type = type,
    };

    sync_send(fd, &msg);
}

static void
sync_wait_simple(int fd, sync_type_t type)
{
    sync_msg_t msg;

    sync_wait(fd, &msg);
    if (msg.type == SYNC_ABORT) {
        fprintf(stderr, "Abort signalled while doing child synchronization.\n");
        exit(EXIT_FAILURE);
    }
    EXPECT(msg.type == type);
}

pid_t
ctrs_execvp_cb(ctr_list_t *list, int cpu, int flags,
               void (*child_callback)(void *data), void *callback_data,
               const char *file, char *const argv[])
{
    pid_t pid;
    int fds[2];

    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fds)) {
        perror("Failed to setup socket pair");
        return -1;
    }

    pid = fork();
    if (pid == -1) {
        perror("Fork failed");
        return -1;
    }

    if (pid == 0) {
        close(fds[0]);

        if (child_callback)
            child_callback(callback_data);

        sync_send_simple(fds[1], SYNC_WAITING);
        sync_wait_simple(fds[1], SYNC_GO);

        close(fds[1]);

        fprintf(stderr, "Starting target...\n");

        execvp(file, argv);
        fprintf(stderr, "%s: %s", file, strerror(errno));
        exit(EXIT_FAILURE);
    } else {
        close(fds[1]);
        sync_wait_simple(fds[0], SYNC_WAITING);

        if (ctrs_attach(list, pid, cpu, flags) == -1) {
            sync_send_simple(fds[0], SYNC_ABORT);
            exit(EXIT_FAILURE);
        }

        sync_send_simple(fds[0], SYNC_GO);
        close(fds[0]);

        return pid;
    }
}

pid_t
ctrs_execvp(ctr_list_t *list,
            int cpu, int flags,
            const char *file, char *const argv[])
{
    return ctrs_execvp_cb(list, cpu, flags, NULL, NULL, file, argv);
}

void
setup_ctr(const char *event, ctr_list_t *ctrs_list) {
    ctr_t *ctr;
    EXPECT(ctr = ctr_create(&perf_base_attr));
    ctr->event_name=event;

    if (ctrs_list->head) {
        /* These should only be set for the group leader */
        ctr->attr.pinned = 0;
        ctr->attr.exclusive = 0;
    }

    int ret = pfm_get_perf_event_encoding(event, PFM_PLM3, &ctr->attr, NULL, NULL);
    if (ret != PFM_SUCCESS){
        fprintf(stderr, "Not a valid event: %s\n"
                   "pfm_get_perf_event_encoding returned: %s \n", 
                   event, pfm_strerror(ret));
        EXPECT(0);
    }

    ctrs_add(ctrs_list, ctr);
}

void
setup_raw_ctr(const char *raw, ctr_list_t *ctrs_list) {
    
    if (strncmp("raw:", raw, 4)) {
        fprintf(stderr, "Error: %s not a valid raw event\n", raw);
        return;
    }

    ctr_t *ctr;
    EXPECT(ctr = ctr_create(&perf_base_attr));
    ctr->event_name=raw;

    if (ctrs_list->head) {
        /* These should only be set for the group leader */
        ctr->attr.pinned = 0;
        ctr->attr.exclusive = 0;
    }

    const char *raw_name = raw + 4; /* Skip "raw:" at start of name */
    long event = perf_argp_parse_long("EVENT", raw_name, NULL);
    ctr->attr.type = PERF_TYPE_RAW;
    ctr->attr.config = (event & 0xFFFFFF);
    ctr->attr.config1 = (event>>24);

    ctrs_add(ctrs_list, ctr);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * c-file-style: "k&r"
 * End:
 */
