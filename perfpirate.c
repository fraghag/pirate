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


#define _GNU_SOURCE

#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/ptrace.h>
#include <sys/stat.h>

#ifndef PFM_INC
#include <perfmon/pfmlib_perf_event.h>
#define PFM_INC
#endif


#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <poll.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <assert.h>
#include <pthread.h>

#include <argp.h>
#include <stdint.h>
#include <sys/types.h>


#include "expect.h"
#include "perfpirate.h"
#include "perf_common.h"
#include "perf_data.h"


/* Configuration options */
static char **exec_argv = NULL;
static int exec_argc;
static char *pb_output_name = "perfpirate.pb";

static int target_cpu = 0;
static pid_t target_pid = NO_PID;
static volatile target_state_t target_state = TARGET_WAIT_EXEC;
static int target_ctrs_len = 0;
static long t_heat_usek = 10000; /* Default value for target heating */


static int n_pirates = 0;
static int pirate_cpus[MAX_PIRATES];
static char *extra_p_ctrs[MAX_EXTRA_P_CTRS];
static int no_extra_p_ctrs=0;
static volatile pirate_state_t *pirate_state;
static ctr_list_t *pirate_ctrs;
static int pirate_ctrs_len = 0;

static pthread_t *pirate_thread;
static pirate_pthread_conf_t *pirate_pthread_conf;
static pirate_conf_t pirate_conf = {
    .data = NULL,
    .current_size = 0,
    .no_sweep = 0,
    .no_reference = 0,
};
static pthread_barrier_t pirate_barrier;


static void
finalize(void) {

    ctrs_close(&perf_ctrs);
    for(int i = 0; i<n_pirates; i++)
        ctrs_close(&pirate_ctrs[i]);
    pfm_terminate();
}

static read_format_t *
read_counter_list(int fd_in, int n_counters)
{
    int data_size, ret;
    read_format_t *data;

    data_size = sizeof(read_format_t) + sizeof(struct ctr_data) * n_counters;

    data = (read_format_t *)malloc(data_size);
    memset(data, '\0', data_size);

    EXPECT_ERRNO((ret = read(fd_in, data, data_size)) != -1);
    if (ret == 0) {
        perror("Got EOF while reading counter\n");
        exit(EXIT_FAILURE);
    } else if (ret != data_size)
        fprintf(stderr,
                "Warning: Got short read. Expected %i bytes, "
                "but got %i bytes.\n",
                data_size, ret);

    return data;
}

// static void
// write_textfile_headers(FILE *file_out, ctr_list_t *list) 
// {
//     fprintf(file_out, "    SIZE  ");
//     for (ctr_t *cur = list->head; cur; cur = cur->next) {
//         fprintf(file_out, "%s ", cur->event_name);
//     }
//     fprintf(file_out, "\n");
// }

// static void
// write2textfile(FILE *file_out, read_format_t *data, int n_counters, int size)
// {
//     fprintf(file_out, "%8d ",size);
//     for(int i = 0; i<n_counters; i++)
//         fprintf(file_out, " %" PRIu64, data->ctr[i].val);
//     fprintf(file_out, "\n");
// }

static void
dump_all_events()
{   
    if(target_state != TARGET_HEATING) {
        read_format_t *data[n_pirates+1];

        for(int i = 0; i < n_pirates; i++)
            data[i+1] = read_counter_list(pirate_ctrs[i].head->fd, pirate_ctrs_len);
        data[0] = read_counter_list(perf_ctrs.head->fd, target_ctrs_len);

        int p_size = pirate_conf.current_size;
        int t_size = pirate_conf.size - p_size;

        pb_dump_sample(data, t_size, p_size);

        for(int i = 0; i < (n_pirates+1) ; i++)
            free(data[i]);
    }
}

static void
my_ptrace_cont(int pid, int signal)
{
    if (ptrace(PTRACE_CONT, pid, NULL, (void *)((long)signal)) == -1) {
        perror("Failed to continue child process");
        abort();
    }
}

static void
reset_events(ctr_list_t *list)
{
    for (ctr_t *cur = list->head; cur; cur = cur->next)
        EXPECT_ERRNO(-1 != ioctl(cur->fd, 
                                    PERF_EVENT_IOC_RESET, 0));
}

static void
reset_all_events() 
{
    reset_events(&perf_ctrs);
    for(int i = 0; i < n_pirates; i++)
        reset_events(&pirate_ctrs[i]);
}

static void
handle_child_signal(const int pid, int signal)  
{
    assert(target_pid == pid);

    switch (target_state) {
    case TARGET_WAIT_EXEC:
        switch (signal) {
        case SIGTRAP:
            target_state = TARGET_RUNNING;
            reset_all_events();
            my_ptrace_cont(pid, 0);
            break;

        default:
            fprintf(stderr,
                    "Unexpected signal (%i) in target while in "
                    "the TARGET_WAIT_EXEC state.\n", signal);
            my_ptrace_cont(pid, signal);
            break;
        };
        break;

    case TARGET_RUNNING:
        switch (signal) {
        case SIGIO:

            if (pirate_conf.no_sweep){
                dump_all_events();
                reset_all_events();
                my_ptrace_cont(pid, 0);
            } else {

                if (pirate_conf.current_size >= \
                    pirate_conf.size - pirate_conf.way_size) {

                    dump_all_events();                

                    EXPECT_ERRNO(-1 != ioctl(perf_ctrs.head->fd, 
                                        PERF_EVENT_IOC_DISABLE, 0));

                    pirate_conf.current_size = 0;
                    
                    target_state=TARGET_HEATING;
                    
                    for(int i = 0; i < n_pirates; i++)
                        pirate_state[i] = PIRATE_NEXT_SIZE;

                    for(int i = 0; i < n_pirates; i++)
                        while (pirate_state[i] == PIRATE_NEXT_SIZE);
                    
                    my_ptrace_cont(pid, 0);

                    EXPECT(usleep(t_heat_usek) == 0);

                    target_state=TARGET_RUNNING;

                    EXPECT_ERRNO(-1 != ioctl(perf_ctrs.head->fd, 
                                        PERF_EVENT_IOC_ENABLE, 0));

                    reset_all_events();

                } else {
                    
                    dump_all_events();
                    
                    pirate_conf.current_size+=pirate_conf.way_size;
                    
                    for(int i = 0; i < n_pirates; i++)
                        pirate_state[i] = PIRATE_NEXT_SIZE;
                    for(int i = 0; i < n_pirates; i++)
                        while (pirate_state[i] == PIRATE_NEXT_SIZE);
                    assert(pirate_conf.current_size > 0);
                    
                    reset_all_events();
                    my_ptrace_cont(pid, 0);
                }
            }
            break;

        case SIGTRAP:
            fprintf(stderr, "Unexpected SIGTRAP in target.\n");
            /* FALL THROUGH */

        default:
            my_ptrace_cont(pid, signal);
            break;
        };
        break;

    case TARGET_HEATING:
        switch (signal) {
        case SIGIO:
            
            fprintf(stderr, "Error: Got SIGIO while TARGET_HEATING\n");
            my_ptrace_cont(pid, signal);
            break;

        case SIGTRAP:
            fprintf(stderr, "Unexpected SIGTRAP in target.\n");
            /* FALL THROUGH */

        default:
            my_ptrace_cont(pid, signal);
            break;
        };
        break;
    }
}

static void
handle_child_event(const int pid, const int status)
{
    assert(target_pid != NO_PID);
    assert(target_pid == pid);

    if (WIFEXITED(status)) {
        fprintf(stderr, "Child exited with status '%i'.\n",
                WEXITSTATUS(status));
        dump_all_events();
        finalize();
        exit(WEXITSTATUS(status) == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "Child terminated by signal '%i'.\n",
                WTERMSIG(status));
        dump_all_events();
        finalize();
        if (WCOREDUMP(status))
            fprintf(stderr, "Core dumped.\n");
        exit(EXIT_FAILURE);
    } else if (WIFSTOPPED(status)) {
        handle_child_signal(pid, WSTOPSIG(status));
    } else
        EXPECT(0);
}

static void
handle_signal(int sfd)
{
    struct signalfd_siginfo fdsi;
    EXPECT(read(sfd, &fdsi, sizeof(fdsi)) == sizeof(fdsi));

    switch (fdsi.ssi_signo) {
    case SIGINT:
        dump_all_events();
        /* Try to terminate the child, if this succeeds, we'll
         * get a SIGCHLD and terminate ourselves. */
        fprintf(stderr, "Killing target process...\n");
        kill(target_pid, SIGKILL);
        break;

    case SIGCHLD: {
        int status;

        EXPECT_ERRNO(waitpid(fdsi.ssi_pid, &status, WNOHANG) > 0);
        handle_child_event(fdsi.ssi_pid, status);
    } break;


    default:
        fprintf(stderr, "Unhandled signal: %i\n", fdsi.ssi_signo);
        break;
    }
}

static int
create_sig_fd()
{
    sigset_t mask;
    int sfd;

    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGCHLD);
    EXPECT_ERRNO(sigprocmask(SIG_BLOCK, &mask, NULL) != -1);
    EXPECT_ERRNO((sfd = signalfd(-1, &mask, 0)) != -1);

    return sfd;
}

static void
pin_process(pid_t pid, int cpu)
{
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    CPU_SET(cpu, &cpu_set);
    EXPECT_ERRNO(sched_setaffinity(pid, sizeof(cpu_set_t), &cpu_set) != -1);
}

static void
setup_target(void *data)
{
    pin_process(0, target_cpu);

    EXPECT_ERRNO(ptrace(PTRACE_TRACEME, 0, NULL, NULL) != -1);
}

__attribute__((noinline))
static void
pirate_loop(char *_data, const int size, const int stride, const int pirate_number)
{
    volatile char *data = (volatile char *)_data;
    const int chunk = size/n_pirates;
    const int start = pirate_number*chunk;
    const int stop = start + chunk;

    do {
        for (int i = start; i < stop; i += stride) {
            char discard __attribute__((unused));
            discard = data[i];
        }
    } while (pirate_state[pirate_number] == PIRATE_RUNNING);
}

__attribute__((noinline))
static void
pirate_loop_fix(char *_data, const int size, const int stride, const int pirate_number)
{
    volatile char *data = (volatile char *)_data;
    const int chunk = pirate_conf.way_size/n_pirates;
    const int start = pirate_number*chunk;
    const int last_element = (size / pirate_conf.way_size) * MEM_HUGE_SIZE \
        + (size % pirate_conf.way_size);

    do {
        for (int i = start; i < last_element; i += MEM_HUGE_SIZE) {
            const int limit = MIN(i + chunk, last_element);
            for (int j = i; j < limit; j += stride) {
                char discard __attribute__((unused));
                discard = data[j];
            }
        } 
    } while (pirate_state[pirate_number] == PIRATE_RUNNING);
}

static void
run_pirate_loop(const pirate_conf_t *conf, const pirate_pthread_conf_t *pth_conf) 
{
    if (conf->loop_fix){
        pirate_loop_fix(conf->data, conf->current_size, \
                        conf->stride, pth_conf->pirate_number);
    } else {
        pirate_loop(conf->data, conf->current_size, \
                    conf->stride, pth_conf->pirate_number);
    }
}

// static int 
// roundUp(int numToRound, int multiple)  
// {  
//     if(multiple == 0)  
//         return numToRound;  

//     int remainder = numToRound % multiple; 
//     if (remainder == 0)
//         return numToRound; 
//     return numToRound + multiple - remainder; 
// }  

static void
pirate_reference(ctr_list_t *ctrs, pirate_conf_t *conf, pirate_pthread_conf_t *pth_conf)
{
    read_format_t *data;
    pirate_conf_t temp_conf = *conf;
    temp_conf.current_size = temp_conf.size/2; //roundUp(2*temp_conf.l2_size, temp_conf.way_size);
    run_pirate_loop(&temp_conf, pth_conf); //Warm up pirate
    run_pirate_loop(&temp_conf, pth_conf); //Warm up pirate

    reset_events(ctrs);
    run_pirate_loop(&temp_conf, pth_conf); //Reference run
    data = read_counter_list(ctrs->head->fd, pirate_ctrs_len);

    pb_write_reference(data, temp_conf.current_size);

    free(data);
}

static void *
pirate_main(void *_conf)
{
    pirate_pthread_conf_t *pth_conf = (pirate_pthread_conf_t *)_conf;
    pirate_conf_t *conf = &pirate_conf;

    pthread_t thread;   
    cpu_set_t cpu_set;

    CPU_ZERO(&cpu_set);
    CPU_SET(pth_conf->cpu, &cpu_set);
    thread = pthread_self();
    EXPECT(pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpu_set) == 0);

    /** Write some data to the data array, this makes sure that we get
     * backing storage for the entire allocation */
    for (int i = 0; i < conf->alloc_size; i += conf->stride)
        ((char *)conf->data)[i] = i & 0xFF;

    /* TODO: Check if this is a PID or TID */
    EXPECT(ctrs_attach(&pirate_ctrs[pth_conf->pirate_number],
                       0 /* pid */,
                       -1, //conf->cpu
                       0 /* flags */) != -1);

    if(pth_conf->pirate_number == 0) {
        if(!conf->no_reference)
            pirate_reference(&pirate_ctrs[0], conf, pth_conf);
        pb_header2file();
    }

    pthread_barrier_wait(&pirate_barrier);

    while (1) {

            run_pirate_loop(conf, pth_conf); /* Warming pirate */
            
            pirate_state[pth_conf->pirate_number]=PIRATE_RUNNING;
            while(target_state == TARGET_HEATING);

            run_pirate_loop(conf, pth_conf);
    }

    pirate_state[pth_conf->pirate_number] = PIRATE_FINISHED;
    fprintf(stderr, "Pirate finished...\n");
    return NULL;
}


static void
do_start()
{
    int sfd;

    if (perf_ctrs.head) {
        perf_ctrs.head->attr.disabled = 1;
        perf_ctrs.head->attr.enable_on_exec = 1;
    }
    sfd = create_sig_fd();

    /* Start pirate */
    EXPECT(pthread_barrier_init(&pirate_barrier, NULL, n_pirates + 1) == 0);
    for(int i = 0; i < n_pirates; i++){
        fprintf(stderr, "Starting pirate on CPU %d...\n",
                            pirate_pthread_conf[i].cpu);
        EXPECT(pthread_create(&pirate_thread[i], NULL,
                              &pirate_main, &pirate_pthread_conf[i]) == 0);
    }
    pthread_barrier_wait(&pirate_barrier);

    /* Wait for pirate to heat when not sampling */
    if(pirate_conf.no_sweep)
        for(int i = 0; i < n_pirates; i++)
            while (pirate_state[i] == PIRATE_NEXT_SIZE);

    
    /* Start target */
    target_pid = ctrs_execvp_cb(&perf_ctrs, -1 /* cpu */, 0 /* flags */,
                                &setup_target, NULL,
                                exec_argv[0], exec_argv);
    EXPECT(target_pid != -1);
    

     /* Route SIGIO from the perf FD to the child process */
    EXPECT_ERRNO(fcntl(perf_ctrs.head->fd, F_SETOWN, target_pid) != -1);
    EXPECT_ERRNO(fcntl(perf_ctrs.head->fd, F_SETFL, O_ASYNC) != -1);

    reset_all_events();

    while (1) {//pirate_state != PIRATE_FINISHED) {
        struct pollfd pfd[] = {
            { sfd, POLLIN, 0 }
        };
        if (poll(pfd, sizeof(pfd) / sizeof(*pfd), -1) != -1) {
            if (pfd[0].revents & POLLIN){
                handle_signal(sfd);
                // fprintf(stderr, "Got signal\n");
            }

        } else if (errno != EINTR)
            EXPECT_ERRNO(0);
    }

}

// static char *
// file_to_string(char *syspath, char file[])
// {
//     FILE *fp;
//     char buf[100];
//     char *val;


//     sprintf(buf, "%s%s", syspath, file);
//     fprintf(stderr, "%s\n", buf);
//     EXPECT_ERRNO(fp = fopen(buf,"r"));
//     EXPECT(fscanf(fp,"%s",val)==1);
//     EXPECT_ERRNO(fclose(fp)==0);

//     return val;
// }

static int
file_to_int(char *syspath, char file[])
{
    FILE *fp;
    char buf[100];
    int val;
    char factor;


    sprintf(buf, "%s%s", syspath, file);
    EXPECT_ERRNO(fp = fopen(buf,"r"));
    EXPECT(fscanf(fp,"%d",&val)==1);
    EXPECT(fscanf(fp,"%c",&factor)==1);


    EXPECT_ERRNO(fclose(fp)==0);

    switch(factor){
        case 'K':
            val *= 1024;
        break;

        case 'M':
            val *= 1024 * 1024;
        break;
    }

    return val;
}

static void
read_cache_conf() 
{

    char syspath[100];
    int LLCindex = -1;

    do {
        LLCindex++;
        sprintf(syspath, "/sys/devices/system/cpu/cpu%d/cache/index%d/size", target_cpu, LLCindex);
    } while( access(syspath, F_OK) == 0 );
    LLCindex--;
    sprintf(syspath, "/sys/devices/system/cpu/cpu%d/cache/index%d/", target_cpu, LLCindex);
    

    pirate_conf.ways = file_to_int(syspath, "ways_of_associativity");
    pirate_conf.size = file_to_int(syspath, "size");
    pirate_conf.stride = file_to_int(syspath, "coherency_line_size");

}

static void
setup_pirate() 
{

    read_cache_conf();

    pirate_pthread_conf = malloc(n_pirates*sizeof(pirate_pthread_conf_t));
    pirate_ctrs = malloc(n_pirates*sizeof(ctr_list_t));
    pirate_thread = malloc(n_pirates*sizeof(pthread_t));
    pirate_state = malloc(n_pirates*sizeof(pirate_state_t));

    pirate_conf_t *p = &pirate_conf;

    p->way_size = p->size/p->ways;

    /* Check if way_size is power of 2 */
    if ((p->way_size != 0) && !(p->way_size & (p->way_size - 1)))
        p->loop_fix=0;
    else
        p->loop_fix=1;

    assert(p->way_size>=0);


    if (p->loop_fix == 0) {
        p->alloc_size = p->size;
    } else {
        p->alloc_size = (p->ways - 1 +
            ((p->size % p->way_size) ? 1 : 0)) * MEM_HUGE_SIZE;
    }

    EXPECT_ERRNO(p->data = mem_huge_alloc(p->alloc_size));

    for(int i = 0; i < n_pirates; i++){
        pirate_pthread_conf[i].cpu = pirate_cpus[i];
        pirate_pthread_conf[i].pirate_number = i;

        pirate_state[i]=PIRATE_NEXT_SIZE;

        pirate_ctrs[i].head=NULL;
        pirate_ctrs[i].tail=NULL;


        setup_ctr("PERF_COUNT_HW_INSTRUCTIONS", &pirate_ctrs[i]);
        setup_ctr("PERF_COUNT_HW_CPU_CYCLES", &pirate_ctrs[i]);
        
        for(int j = 0; j < no_extra_p_ctrs; j++)
            setup_ctr(extra_p_ctrs[j], &pirate_ctrs[i]);
        
    }

    EXPECT((pirate_ctrs_len = ctrs_len(&pirate_ctrs[0])) != 0 );

}


/*** argument handling ************************************************/
static error_t
parse_opt (int key, char *arg, struct argp_state *state)
{   
    switch (key)
    {

    case 'o':
        pb_output_name = arg;
        break;

    case 'c':
        target_cpu = perf_argp_parse_long("CPU", arg, state);
        if (target_cpu < 0)
            argp_error(state, "CPU number must be positive\n");
        break;

    case 'C':
        if(n_pirates>=MAX_PIRATES) {
            argp_error(state, "Too many pirates, limit is 5\n");
            break;
        }
        int cpu = perf_argp_parse_long("CPU", arg, state);
        
        pirate_cpus[n_pirates] = cpu;
        n_pirates++;
        if (pirate_cpus[n_pirates] < 0)
            argp_error(state, "CPU number must be positive\n");
        break;

    case 's':
        pirate_conf.current_size = perf_argp_parse_long("SIZE", arg, state);
        pirate_conf.no_sweep = 1;
        if (pirate_conf.current_size < 0)
            argp_error(state, "Size must be positive\n");
        break;

    case 'e':
        setup_ctr(arg, &perf_ctrs);
    break;

    case 'E':
        if(no_extra_p_ctrs >= MAX_EXTRA_P_CTRS) {
            fprintf(stderr, "Too many extra counters: %s\n", arg);
        } else {
            extra_p_ctrs[no_extra_p_ctrs] = arg;
            no_extra_p_ctrs++;
        }
    break;

    case 'r':
        setup_raw_ctr(arg, &perf_ctrs);
    break;

    case 'h':
        t_heat_usek = perf_argp_parse_long("TIME", arg, state);
        if (t_heat_usek < 0)
            argp_error(state, "Time number must be positive\n");
    break;

    case KEY_SAMPLE_PERIOD:
        perf_ctrs.head->attr.sample_period =
            perf_argp_parse_long("sample period", arg, state);
        perf_ctrs.head->attr.freq = 0;
        if( perf_ctrs.head->attr.sample_period == 0 )
            pirate_conf.no_sweep = 1;
        break;

    case KEY_SAMPLE_FREQ:
        perf_ctrs.head->attr.sample_freq =
            perf_argp_parse_long("sample freq", arg, state);
        perf_ctrs.head->attr.freq = 1;
        break;

    case KEY_NO_REFERENCE:
        pirate_conf.no_reference = 1;
        break;


    case ARGP_KEY_ARG:
        if (!state->quoted)
            argp_error(state, "Illegal argument\n");
        break;
     
    case ARGP_KEY_END:
        if (state->quoted && state->quoted < state->argc){
            exec_argv = &state->argv[state->quoted];
            exec_argc = exec_argc - state->quoted;
        }

        if(n_pirates == 0){
            n_pirates = 1;
            pirate_cpus[0] = (target_cpu == 0 ? 1 : target_cpu -1);
        }

        fprintf(stderr, "Target_cpu: %d Pirate_cpus: ", target_cpu);
        for(int i = 0; i < n_pirates; i++){
            fprintf(stderr, " %d", pirate_cpus[i]);
        } fprintf(stderr, "\n");

        
        for(int i=0; i<n_pirates; i++) {
            for(int j = 0; j<n_pirates; j++)
                if( i != j && pirate_cpus[i] == pirate_cpus[j] )
                    argp_failure(state, EXIT_FAILURE, errno, 
                        "Only one pirate per CPU\n");
            if ( pirate_cpus[i] == target_cpu )
                argp_failure(state, EXIT_FAILURE, errno, 
                     "Pirate on same CPU as target.\n");
        }

        if (!exec_argv)
            argp_error(state,
                       "No target command specified.\n");

        target_ctrs_len = ctrs_len(&perf_ctrs);

        break;

    default:
        return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

const char *argp_program_version =
    "perfpirate\n"
    "\n"
    "  Copyright (C) 2013, Ragnar Hagg\n"
    "  Copyright (C) 2012, Andreas Sandberg\n"
    "\n"
    "  This program is free software; you can redistribute it and/or modify\n"
    "  it under the terms set out in the COPYING file, which is included\n"
    "  in the perfpirate source distribution.\n";

const char *argp_program_bug_address =
    "ragnar.hagg@gmail.com";


static struct argp_option arg_options[] = {
    { "output", 'o', "FILE", 0, "Protobuf output file", 0 },
    { "target-cpu", 'c', "CPU", 0,
      "Pin target process to CPU. Default is 0.", 0 },
    { "pirate-cpu", 'C', "CPU", 0,
      "Pin pirate to CPU. Repeat this option for more pirates.", 0 },
    { "pirate-size", 's', "SIZE", 0, "Pirate data set size.", 0 },
    { "target-event", 'e', "EVENT", 0, "Events to measure on target", 1},
    { "pirate-event", 'E', "EVENT", 0, "Events to measure on Pirate", 1},
    { "target-raw-event", 'r', "EVENT", 0, "Raw events to measure on target", 1},
    { "target-heat-time", 'h', "TIME", 0, 
      "Time in microseconds for target to heat between sample cycles. Default is 10,000.", 2},
    { "sample-period", KEY_SAMPLE_PERIOD, "N", 0, 
      "Use sample period N of first event", 2 },
    { "sample-freq", KEY_SAMPLE_FREQ, "N", 0, 
      "Use sample frequency N of first event", 2 },
    { 0 }
};


static struct argp argp = 
{    .options = arg_options,
    .parser = parse_opt,
    .args_doc = "[-- command [arg ...]]",
    .doc = "Simple cache pirating implementation for perf events"
    "\v"
    "perfpirate runs a target application and a stress microbenchmark, the "
    "Pirate. Both applications are monitored simultaneously, when a user "
    "defined event overflow, the counters from both applications are "
    "dumped to disk.\n",
};


static void
initialize(int argc, char **argv){

    perf_base_attr.sample_type =
        PERF_SAMPLE_READ;
    perf_base_attr.read_format = 
        PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING |
        PERF_FORMAT_GROUP;
    perf_base_attr.pinned = 1;

    int ret = pfm_initialize();
    if (ret != PFM_SUCCESS)
        perror("Internal error in pfm_initialize");


    setup_ctr("PERF_COUNT_HW_INSTRUCTIONS", &perf_ctrs);
    perf_ctrs.head->attr.sample_period=1000000; //Default value
    
    exec_argc = argc;
    argp_parse (&argp, argc, argv,
            ARGP_IN_ORDER,
            0,
            NULL);

    setup_pirate();

    pb_initialize(target_cpu, pirate_conf.no_reference, 
        perf_ctrs.head->attr.sample_period, &perf_ctrs, 
        &pirate_conf, pirate_pthread_conf, n_pirates, 
        pirate_ctrs, pb_output_name, exec_argv, exec_argc);

}

int
main(int argc, char **argv) 
{

    initialize(argc, argv);

    do_start();

    finalize();

    return 0;
}

