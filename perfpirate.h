/*
 * Copyright (C) 2013, Ragnar Hagg
 * Copyright (C) 2012, Andreas Sandberg
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
 
#ifndef PERFPIRATE_H
#define PERFPIRATE_H
 

#define MIN(a,b) ((a) < (b) ? (a) : (b))

#ifndef MEM_HUGE_SIZE
/* The size of a huge page */
#define MEM_HUGE_SIZE (2*(1<<20))
#endif

#define DEFAULT_MODE (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH)

#define NO_PID -1

#define MAX_PIRATES 10
#define MAX_EXTRA_P_CTRS 10

#define DEFAULT_SAMPLE_PERIOD 10000000

typedef enum {
    PIRATE_RUNNING,
    PIRATE_NEXT_SIZE,
    PIRATE_FINISHED,
} pirate_state_t;

typedef struct {
    void *data;
    int ways;
    int size;
    int alloc_size;
    int current_size;
    int stride;
    int way_size;
    int loop_fix;
    int l2_size;
    int no_sweep;
    int no_reference;
} pirate_conf_t;

typedef struct {
    int cpu;
    int pirate_number;
} pirate_pthread_conf_t;

typedef enum {
    TARGET_WAIT_EXEC,
    TARGET_RUNNING,
    TARGET_HEATING,
} target_state_t;

enum {
    KEY_SAMPLE_PERIOD = -1,
    KEY_SAMPLE_FREQ = -2,
    KEY_NO_REFERENCE = -3,
};

typedef struct {
    uint64_t nr;
    uint64_t time_enabled;
    uint64_t time_running;
    struct ctr_data {
        uint64_t val;
    } ctr[];
} read_format_t;


#endif

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * c-file-style: "k&r"
 * End:
 */
