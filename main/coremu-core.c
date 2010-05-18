/*
 * COREMU Parallel Emulator Framework
 *
 * core emulation support for COREMU parallel
 * emulation library.
 *
 * Copyright (C) 2010 PPI, Fudan Univ. <http://ppi.fudan.edu.cn/system_research_group>
 *
 * Authors:
 *  Zhaoguo Wang    <zgwang@fudan.edu.cn>
 *  Yufei Chen      <chenyufei@fudan.edu.cn>
 *  Ran Liu         <naruilone@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

/*#define DEBUG_COREMU*/
#define VERBOSE_COREMU

#define _GNU_SOURCE             /* for some GNU specific interfaces */

#include "coremu-utils.h"
#include "coremu-hw.h"
#include "coremu-timer.h"
#include "coremu-sched.h"
#include "coremu-hw-lockfree.h"
#include "coremu-intr.h"

/* pause condition */
pthread_cond_t pause_cond=COREMU_COND_INITIALIZER;
pthread_mutex_t pause_mutex=COREMU_LOCK_INITIALIZER;

/* coremu cores init related */
static volatile int init_done = false;

/* coremu core itself for each thread */
__thread pthread_mutexattr_t attr;
static pthread_attr_t thr_attr;

/* coremu retry profiling */
__thread unsigned long int cm_retry_num = 0;

/* the per core signal blocking set */
//__thread sigset_t cm_blk_sigset;

extern int cm_adaptive_intr_delay;
extern int cm_intr_delay_step;

/* Number of cores. */
int cm_smp_cpus;
/* Array holding all the core object. */
CMCore *cm_cores;
/* Pointer to the core object of the thread. */
COREMU_THREAD CMCore *cm_core_self;

extern void coremu_cpu_signal_handler(int signo, siginfo_t *info, void *context);
void coremu_init(int smp_cpus)
{
    cm_print("\nTIMERRTSIG:\t\t%d"
             "\nCOREMU_TIMER_SIGNAL:\t%d"
             "\nCOREMU_TIMER_ALARM:\t%d"
             "\nCOREMU_SIGNAL:\t\t%d"
             "\nCOREMU_AIO_SIG:\t\t%d\n",
             TIMERRTSIG, COREMU_TIMER_SIGNAL,
             COREMU_TIMER_ALARM, COREMU_SIGNAL, COREMU_AIO_SIG);
    cm_smp_cpus = smp_cpus;

    /* step 0: init scheduling support */
    coremu_init_sched_all();

    /* the adaptive intr delay mechanism works well 
        when core's number is more than 64 (test enviroment R900)*/
    if(smp_cpus > 64)
        cm_adaptive_intr_delay = 1;
    else
        cm_adaptive_intr_delay = 0;

     /**
      * we define the step is 1, if the number of cores
      * is not more than 128
      */
     if(cm_adaptive_intr_delay)
        cm_intr_delay_step = (smp_cpus + 127)/128;

    /* step 1: init the global core array */
    cm_cores = (CMCore *) qemu_mallocz(smp_cpus * sizeof(*cm_cores));

    /* step 2: init the coremu timer thread */

    /* clear the block signal set */
    //sigemptyset(&cm_blk_sigset);

    /* block TIMERRTSIG */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, TIMERRTSIG);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    /* add the block signal to block signal set */
    //sigaddset(&cm_blk_sigset, TIMERRTSIG);

    coremu_start_timer_thread();
    coremu_init_hw(smp_cpus);

    /* step 3: register CORMEU signal handling */
    struct sigaction act;
    sigfillset(&act.sa_mask);
    act.sa_flags = 0;
    act.sa_flags |= SA_SIGINFO;
    act.sa_sigaction = coremu_cpu_signal_handler;

    sigaction(COREMU_SIGNAL, &act, NULL);
}

CMCore *coremu_core_init(int id, void* opaque)
{
    int err = 0;

    /* step 1: get the core */
    CMCore *core = &cm_cores[id];
    cm_core_self = core;

    /* step 2: init the hardware event queue and its lock */
    err = pthread_mutexattr_init(&attr);
    cm_assert((err == 0), "cannot init the attribute");

    err = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
    cm_assert((err == 0), "cannot set mutex type");

    core->intr_queue = new_queue();

    /* step 3: init opaque state */
    core->opaque = opaque;

    /* step 4: set core state to run */
    core->state = STATE_RUN;

    /* reset the profile */
    core->core_profile.retry_num_addr = &cm_retry_num;

    return core;
}

void coremu_run_all_cores(thr_start_routine thr_fn, void *arg)
{
    CMCore *cur;
    int err;

    pthread_attr_init(&thr_attr);
    pthread_attr_setdetachstate(&thr_attr, PTHREAD_CREATE_JOINABLE);
    pthread_attr_setschedpolicy(&thr_attr, SCHED_RR);

    for (cur = cm_cores; cur != cm_cores + cm_smp_cpus; cur++){
        err = pthread_create(&cur->coreid, &thr_attr, thr_fn, arg);
        cm_assert((! err), "pthread creation fails...");
    }

    pthread_attr_destroy(&thr_attr);
    init_done = true;
}

int coremu_init_done_p()
{
    int done_flag;
    done_flag = init_done;

    return done_flag;
}

void coremu_assert_core_thr()
{
    if (pthread_self() == coremu_get_hw_id()) {
        coremu_backtrace();
        assert(0);
    }
    return;
}

inline CMCore *coremu_get_core(int coreid)
{
    return &cm_cores[coreid];
}

inline CMCore *coremu_get_core_self()
{
    return cm_core_self;
}

void coremu_wait_init(void)
{
    struct timespec tsp;
    while(! init_done) {
        maketimeout(&tsp, 1);   /* 1 second per check */
        nanosleep(&tsp, NULL);
    }
}

int coremu_wait_tid(CMCore *core, void **val_ptr)
{
    return pthread_join(core->coreid, val_ptr);
}

void coremu_core_exit(void *value_ptr)
{
    pthread_exit(value_ptr);
}

void coremu_pause_core()
{
    CMCore *self = coremu_get_core_self();
    coremu_mutex_lock(&pause_mutex,"coremu_pause_core");
    if(self->state==STATE_RUN){
        self->state=STATE_PAUSE;
        coremu_cond_wait(&pause_cond, &pause_mutex);
        self->state=STATE_RUN;
    }
    coremu_mutex_unlock(&pause_mutex,"coremu_pause_core");
}

void coremu_wait_pause(CMCore *core)
{
    coremu_mutex_lock(&pause_mutex,"coremu_wait_pause");
    while(core->state!=STATE_PAUSE){
        coremu_mutex_unlock(&pause_mutex,"coremu_wait_pause");
        coremu_mutex_lock(&pause_mutex,"coremu_wait_pause");
    }
    coremu_mutex_unlock(&pause_mutex,"coremu_wait_pause");
}

void coremu_restart_all_cores()
{
    coremu_cond_broadcast(&pause_cond);
}
