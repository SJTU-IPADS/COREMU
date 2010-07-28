/*
 * COREMU Parallel Emulator Framework
 *
 * Copyright (C) 2010 Parallel Processing Institute, Fudan Univ.
 *  <http://ppi.fudan.edu.cn/system_research_group>
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

#include <unistd.h>
#include <time.h>
#include "coremu-malloc.h"
#include "coremu-logbuffer.h"
/*#define DEBUG_COREMU*/
#include "coremu-debug.h"

CMLogbuf *coremu_logbuf_new(int n, int ele_size, coremu_log_func func)
{
    CMLogbuf *logbuf = coremu_mallocz(sizeof(*logbuf));

    logbuf->ele_size = ele_size;
    logbuf->size = n * ele_size;

    logbuf->buf = logbuf->cur = coremu_mallocz(logbuf->size);
    logbuf->end = logbuf->size + logbuf->buf;
    logbuf->func = func;

    logbuf->queue = new_queue();

    return logbuf;
}

void coremu_logbuf_free(CMLogbuf *buf)
{
    char *pos;
    struct timespec tv = {0, 300000000}; /* 0.3 second */

    /* Wait for the thread to stop. Can't use pthread_join since thread is
     * detached. */
    while (buf->thread_running)
        nanosleep(&tv, NULL);

    /* First output the remaining record. */
    for (pos = buf->buf; pos != buf->cur; pos += buf->ele_size)
        buf->func(pos);

    /* Free the memory. */
    coremu_free(buf->buf);
    destroy_queue(buf->queue);
    coremu_free(buf);
}

/* Call log function for each element in the buffer, then free the buffer. */
static void *log_thr(void *logbuf)
{
    char *pos;
    char *curbuf;
    struct timespec tv = {0, 100000000}; /* 0.1 second */
    CMLogbuf *buf = (CMLogbuf *)logbuf;
    int try = 0;

    coremu_debug("spawn new thread writing log buffer");

    /* Wait for 1 second for new log record. */
    while (try < 10) {
        while (dequeue(buf->queue, (data_type *)&curbuf)) {
            for (pos = curbuf; pos != curbuf + buf->size; pos += buf->ele_size)
                buf->func(pos);
            coremu_free(curbuf);
            try = 0;
        }
        try++;
        coremu_debug("sleep to wait log");
        nanosleep(&tv, NULL);
    }
    /* No record now, exit. */
    coremu_debug("log write thread exit");
    buf->thread_running = false;
    return NULL;
}

void coremu_logbuf_flush(CMLogbuf *buf)
{
    coremu_debug("called");

    /* Add old buffer to buffer list. */
    enqueue(buf->queue, (data_type) buf->buf);

    buf->buf = buf->cur = coremu_mallocz(buf->size);
    buf->end = buf->buf + buf->size;

    if (!buf->thread_running) {
        buf->thread_running = true;
        pthread_create(&(buf->thread), NULL, log_thr, buf);
        pthread_detach(buf->thread);
    }
}
