/*
   Copyright 2015, 2017, Bloomberg Finance L.P.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
 */

/*
 * Manages a thread whose job it is to push the log files forward past a
 * given lsn.
 */

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <time.h>
#include <sys/types.h>
#include <unistd.h>

#include <epochlib.h>

#include <bdb_api.h>

#include "comdb2.h"
#include "logmsg.h"

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static char target[SIZEOF_SEQNUM];
static int have_thread = 0;

static int delayms = 0;

static char junk[2048];

static void *pushlogs_thread(void *voidarg)
{
    comdb2_name_thread(__func__);
    int rc;
    int lastreport = 0;
    thrman_register(THRTYPE_PUSHLOG);
    int nwrites = 0;

    thread_started("pushlog");

    memset(junk, '@', sizeof(junk));

    backend_thread_event(thedb, COMDB2_THR_EVENT_START_RDWR);

    while (1) {
        tran_type *trans;
        struct ireq iq;
        int now;
        char cur_seqnum[SIZEOF_SEQNUM];
        struct dbtable *db;
        int done;

        /* get current lsn */
        rc = bdb_get_seqnum(thedb->bdb_env, (seqnum_type *)cur_seqnum);
        if (rc != 0) {
            logmsg(LOGMSG_ERROR, "pushlogs_thread: error getting seqnum\n");
            Pthread_mutex_lock(&mutex);
            have_thread = 0;
            Pthread_mutex_unlock(&mutex);
            break;
        }

        /* report progress */
        now = comdb2_time_epoch();
        if (now - lastreport >= 5) {
            lastreport = now;
            char b1[32];
            char b2[32];
           logmsg(LOGMSG_USER, "pushlogs: target %s, current %s, nwrites=%d\n",
                   bdb_format_seqnum((seqnum_type *)target, b1, sizeof(b1)),
                   bdb_format_seqnum((seqnum_type *)cur_seqnum, b2, sizeof(b2)),
                   nwrites);
        }

        /* see if we're still needed */
        done = 0;
        Pthread_mutex_lock(&mutex);
        if (bdb_seqnum_compare(thedb->bdb_env, (seqnum_type *)target,
                               (seqnum_type *)cur_seqnum) <= 0) {
            logmsg(LOGMSG_USER, "Have reached target LSN\n");
            have_thread = 0;
            done = 1;
        }
        Pthread_mutex_unlock(&mutex);
        if (done)
            break;

        /* put some junk into meta table */
        init_fake_ireq(thedb, &iq);
        db = &thedb->static_table;
        /* if we do not have a meta, try creating one on the spot. */
        if (thedb->meta == NULL && db->meta == NULL) {
            wrlock_schema_lk();
            int ret = open_auxdbs(&thedb->static_table, 1);
            unlock_schema_lk();
            if (ret != 0) {
                logmsg(LOGMSG_ERROR, "pushlogs_thread: failed opening meta\n");
                Pthread_mutex_lock(&mutex);
                have_thread = 0;
                Pthread_mutex_unlock(&mutex);
                break;
            }
        }
        iq.usedb = db;
        rc = trans_start(&iq, NULL, &trans);
        if (rc != 0) {
            logmsg(LOGMSG_ERROR, "pushlogs_thread: cannot create transaction\n");
            Pthread_mutex_lock(&mutex);
            have_thread = 0;
            Pthread_mutex_unlock(&mutex);
            break;
        }
        rc = put_csc2_stuff(db, trans, junk, sizeof(junk));
        if (rc != 0) {
            logmsg(LOGMSG_ERROR, "pushlogs_thread: error %d adding to meta table\n",
                    rc);
            trans_abort(&iq, trans);
            Pthread_mutex_lock(&mutex);
            have_thread = 0;
            Pthread_mutex_unlock(&mutex);
            break;
        }
        rc = trans_commit(&iq, trans, gbl_myhostname);
        if (rc != 0) {
            logmsg(LOGMSG_ERROR, "pushlogs_thread: cannot commit txn %d\n", rc);
            Pthread_mutex_lock(&mutex);
            have_thread = 0;
            Pthread_mutex_unlock(&mutex);
            break;
        }
        nwrites++;

        /* throttle this */
        if (delayms)
            usleep(delayms * 1000);
    }

    if (nwrites > 0) {
        flush_db();
        broadcast_flush_all();
    }

    backend_thread_event(thedb, COMDB2_THR_EVENT_DONE_RDWR);
    logmsg(LOGMSG_USER, "pushlogs thread exiting\n");
    return NULL;
}

void set_target_lsn(uint32_t logfile, uint32_t logbyte)
{
    Pthread_mutex_lock(&mutex);
    bdb_make_seqnum((seqnum_type *)target, logfile, logbyte);
    if (!have_thread && logfile > 0) {
        pthread_t tid;
        Pthread_create(&tid, &gbl_pthread_attr_detached, pushlogs_thread,
                            NULL);
        have_thread = 1;
    }
    Pthread_mutex_unlock(&mutex);
}

void push_next_log(void)
{
    int filenum;
    int rc;

    char cur_seqnum[SIZEOF_SEQNUM];

    /* get current lsn */
    rc = bdb_get_seqnum(thedb->bdb_env, (seqnum_type *)cur_seqnum);
    if (rc != 0)
        return;

    memcpy(&filenum, cur_seqnum, sizeof(int));

    logmsg(LOGMSG_USER, "pushing to logfile %d\n", filenum + 1);

    set_target_lsn(filenum + 1, 0);
}
