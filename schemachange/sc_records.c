/*
   Copyright 2015 Bloomberg Finance L.P.

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

#include <unistd.h>
#include <poll.h>

#include "schemachange.h"
#include "sc_records.h"
#include "sc_global.h"
#include "sc_schema.h"
#include "sc_callbacks.h"

#include <bdb_fetch.h>
#include "dbinc/db_swap.h"
#include "llog_auto.h"
#include "llog_ext.h"
#include "bdb_osqllog.h"
#include "bdb_osql_log_rec.h"

#include "comdb2_atomic.h"
#include "epochlib.h"
#include "reqlog.h"
#include "logmsg.h"
#include "debug_switches.h"

int gbl_logical_live_sc = 0;

extern __thread snap_uid_t *osql_snap_info; /* contains cnonce */
extern int gbl_partial_indexes;
extern int gbl_debug_omit_zap_on_rebuild;
// Increase max threads to do SC -- called when no contention is detected
// A simple atomic add sufices here since this function is called from one
// place at any given time, currently from lkcounter_check() once per sec
static inline void increase_max_threads(uint32_t *maxthreads, int sc_threads)
{
    if (*maxthreads >= sc_threads) return;
    ATOMIC_ADD32_PTR(maxthreads, 1);
}

// Decrease max threads to do SC -- called when add_record gets an abort
// Used to backoff SC by having fewer threads running, decreasing contention
// We use atomic add here, since it may be called from multiple threads at once
// We also make certain that maxthreads does not go less than 1
static inline void decrease_max_threads(uint32_t *maxthreads)
{
    if (*maxthreads <= 1) return;
    /* ADDING -1 */
    if (ATOMIC_ADD32_PTR(maxthreads, -1) < 1) XCHANGE32((*maxthreads), 1);
}

// increment number of rebuild threads in use
// if we are at capacity, then return 1 for failure
// if we were successful we return 0
static inline int use_rebuild_thr(uint32_t *thrcount, uint32_t *maxthreads)
{
    if (*thrcount >= *maxthreads) return 1;
    ATOMIC_ADD32_PTR(thrcount, 1);
    return 0;
}

// decrement number of rebuild threads in use
static inline void release_rebuild_thr(uint32_t *thrcount)
{
    assert(*thrcount >= 1);
    /* ADDING -1 */
    ATOMIC_ADD32_PTR(thrcount, -1);
}

/* Return true if there were writes to table undergoing SC (data->from)
 * Note that data is local to this thread so this accounting happens
 * for each thread performing SC.
 */
static inline int tbl_had_writes(struct convert_record_data *data)
{
    int64_t oldcount = data->write_count;
    data->write_count = data->from->write_count[RECORD_WRITE_INS] +
                        data->from->write_count[RECORD_WRITE_UPD] +
                        data->from->write_count[RECORD_WRITE_DEL];
    return (data->write_count - oldcount) != 0;
}

static inline void print_final_sc_stat(struct convert_record_data *data)
{
    sc_printf(
        data->s,
        "[%s] TOTAL converted %lld sc_adds %d sc_updates %d sc_deletess %d\n",
        data->from->tablename,
        data->from->sc_nrecs - (data->from->sc_adds + data->from->sc_updates +
                                data->from->sc_deletes),
        data->from->sc_adds, data->from->sc_updates, data->from->sc_deletes);
}

/* prints global stats if not printed in the last sc_report_freq,
 * returns 1 if successful
 */
static inline int print_aggregate_sc_stat(struct convert_record_data *data,
                                          int now, int sc_report_freq)
{
    uint32_t copy_total_lasttime = data->cmembers->total_lasttime;

    /* Do work without locking */
    if (now < copy_total_lasttime + sc_report_freq) return 0;

    /* If time is up to print, atomically set total_lastime
     * if this thread successful in setting, it can continue
     * to print. If it failed, another thread is doing that work.
     */

    int res = CAS32(data->cmembers->total_lasttime, copy_total_lasttime, now);
    if (!res) return 0;

    /* number of adds after schema cursor (by definition, all adds)
     * number of updates before cursor
     * number of deletes before cursor
     * number of genids added since sc began (adds + updates)
     */
    if (data->live)
        sc_printf(data->s,
                  "[%s] >> adds %u upds %d dels %u extra genids "
                  "%u\n",
                  data->from->tablename, data->from->sc_adds,
                  data->from->sc_updates, data->from->sc_deletes,
                  data->from->sc_adds + data->from->sc_updates);

    /* totals across all threads */
    if (data->scanmode != SCAN_PARALLEL) return 1;

    long long total_nrecs_diff =
        data->from->sc_nrecs - data->from->sc_prev_nrecs;
    data->from->sc_prev_nrecs = data->from->sc_nrecs;
    sc_printf(data->s,
              "[%s] progress TOTAL %lld +%lld actual "
              "progress total %lld rate %lld r/s\n",
              data->from->tablename, data->from->sc_nrecs, total_nrecs_diff,
              data->from->sc_nrecs -
                  (data->from->sc_adds + data->from->sc_updates),
              total_nrecs_diff / sc_report_freq);
    return 1;
}

static inline void lkcounter_check(struct convert_record_data *data, int now)
{
    uint32_t copy_lasttime = data->cmembers->lkcountercheck_lasttime;
    int lkcounter_freq = bdb_attr_get(data->from->dbenv->bdb_attr,
                                      BDB_ATTR_SC_CHECK_LOCKWAITS_SEC);
    /* Do work without locking */
    if (now < copy_lasttime + lkcounter_freq) return;

    /* If time is up to do work, atomically set total_lastime
     * if this thread successful in setting, it can continue
     * to adjust num threads. If it failed, another thread is doing that work.
     */
    int res = CAS32(data->cmembers->lkcountercheck_lasttime, copy_lasttime, now);
    if (!res) return;

    /* check lock waits -- there is no way to differentiate lock waits because
     * of writes, with the exception that if there were writes in the last n
     * seconds we may have been slowing them down.  */

    int64_t ndeadlocks = 0, nlockwaits = 0;
    bdb_get_lock_counters(thedb->bdb_env, &ndeadlocks, NULL, &nlockwaits, NULL);

    int64_t diff_deadlocks = ndeadlocks - data->cmembers->ndeadlocks;
    int64_t diff_lockwaits = nlockwaits - data->cmembers->nlockwaits;

    data->cmembers->ndeadlocks = ndeadlocks;
    data->cmembers->nlockwaits = nlockwaits;
    logmsg(LOGMSG_DEBUG,
           "%s: diff_deadlocks=%" PRId64 ", diff_lockwaits=%" PRId64
           ", maxthr=%d, currthr=%d\n",
           __func__, diff_deadlocks, diff_lockwaits, data->cmembers->maxthreads,
           data->cmembers->thrcount);
    increase_max_threads(
        &data->cmembers->maxthreads,
        bdb_attr_get(data->from->dbenv->bdb_attr, BDB_ATTR_SC_USE_NUM_THREADS));
}

/* If the schema is resuming it sets sc_genids to be the last genid for each
 * stripe.
 * If the schema change is not resuming it sets them all to zero
 * If success it returns 0, if failure it returns <0 */
int init_sc_genids(struct dbtable *db, struct schema_change_type *s)
{
    void *rec;
    int orglen, bdberr, stripe;
    unsigned long long *sc_genids;

    if (db->sc_genids == NULL) {
        db->sc_genids = malloc(sizeof(unsigned long long) * MAXDTASTRIPE);
        if (db->sc_genids == NULL) {
            logmsg(LOGMSG_ERROR,
                   "init_sc_genids: failed to allocate sc_genids\n");
            return -1;
        }
    }

    sc_genids = db->sc_genids;

    /* if we aren't resuming simply zero the genids */
    if (!s->resume) {
        /* if we may have to resume this schema change, clear the progress in
         * llmeta */
        if (bdb_clear_high_genid(NULL /*input_trans*/, db->tablename,
                                 db->dtastripe, &bdberr) ||
            bdberr != BDBERR_NOERROR) {
            logmsg(LOGMSG_ERROR, "init_sc_genids: failed to clear high "
                                 "genids\n");
            return -1;
        }

        bzero(sc_genids, sizeof(unsigned long long) * MAXDTASTRIPE);
        return 0;
    }

    /* prepare for the largest possible data */
    orglen = MAXLRL;
    rec = malloc(orglen);

    /* get max genid for each stripe */
    for (stripe = 0; stripe < db->dtastripe; ++stripe) {
        int rc;
        uint8_t ver;
        int dtalen = orglen;

        /* get this stripe's newest genid and store it in sc_genids,
         * if we have been rebuilding the data files we can grab the genids
         * straight from there, otherwise we look in the llmeta table */
        if (is_dta_being_rebuilt(db->plan)) {
            rc = bdb_find_newest_genid(db->handle, NULL, stripe, rec, &dtalen,
                                       dtalen, &sc_genids[stripe], &ver,
                                       &bdberr);
            if (rc == 1)
                sc_genids[stripe] = 0ULL;
        } else
            rc = bdb_get_high_genid(db->tablename, stripe, &sc_genids[stripe],
                                    &bdberr);
        if (rc < 0 || bdberr != BDBERR_NOERROR) {
            sc_errf(s, "init_sc_genids: failed to find newest genid for "
                       "stripe: %d\n",
                    stripe);
            free(rec);
            return -1;
        }
        sc_printf(s, "[%s] resuming stripe %2d from 0x%016llx\n", db->tablename,
                  stripe, sc_genids[stripe]);
    }

    free(rec);
    return 0;
}

// this is only good for converting old schema to new schema full record
// because we only have one map from old schema to new schema
//(ie no index mapping--that can speedup insertion into indices too)
static inline int convert_server_record_cachedmap(
    struct dbtable *db, const char *table, int tagmap[], const void *inbufp, char *outbuf,
    struct schema_change_type *s, struct schema *from, struct schema *to,
    blob_buffer_t *blobs, int maxblobs)
{
    char err[1024];
    struct convert_failure reason;
    int rc =
        stag_to_stag_buf_cachedmap(db, tagmap, from, to, (char *)inbufp, outbuf,
                                   0 /*flags*/, &reason, blobs, maxblobs);

    if (rc) {
        convert_failure_reason_str(&reason, table, from->tag, to->tag, err,
                                   sizeof(err));
        sc_client_error(s, "cannot convert data %s", err);
        return rc;
    }
    return 0;
}

static int convert_server_record_blobs(const void *inbufp, const char *from_tag,
                                       struct dbrecord *db,
                                       struct schema_change_type *s,
                                       blob_buffer_t *blobs, int maxblobs)
{
    char *inbuf = (char *)inbufp;
    struct convert_failure reason;
    char err[1024];

    if (from_tag == NULL) from_tag = ".ONDISK";

    int rc = stag_to_stag_buf_blobs(get_dbtable_by_name(db->table), from_tag, inbuf,
                                    db->tag, db->recbuf, &reason, blobs, maxblobs, 1);
    if (rc) {
        convert_failure_reason_str(&reason, db->table, from_tag, db->tag, err,
                                   sizeof(err));
        sc_client_error(s, "cannot convert data %s", err);
        return 1;
    }
    return 0;
}

/* free/cleanup all resources associated with convert_record_data */
void convert_record_data_cleanup(struct convert_record_data *data)
{

    if (data->trans) {
        trans_abort(&data->iq, data->trans);
        data->trans = NULL;
    }
    if (data->dmp) {
        bdb_dtadump_done(data->from->handle, data->dmp);
        data->dmp = NULL;
    }
    free_blob_status_data(&data->blb);
    free_blob_status_data(&data->blbcopy);
    free_blob_buffers(data->freeblb,
                      sizeof(data->freeblb) / sizeof(data->freeblb[0]));
    if (data->dta_buf) {
        free(data->dta_buf);
        data->dta_buf = NULL;
    }
    if (data->old_dta_buf) {
        free(data->old_dta_buf);
        data->old_dta_buf = NULL;
    }
    if (data->unpack_dta_buf) {
        free(data->unpack_dta_buf);
        data->unpack_dta_buf = NULL;
    }
    if (data->unpack_old_dta_buf) {
        free(data->unpack_old_dta_buf);
        data->unpack_old_dta_buf = NULL;
    }
    if (data->blb_buf) {
        free(data->blb_buf);
        data->blb_buf = NULL;
    }
    if (data->old_blb_buf) {
        free(data->old_blb_buf);
        data->old_blb_buf = NULL;
    }
    if (data->rec) {
        free_db_record(data->rec);
        data->rec = NULL;
    }
}

static inline int convert_server_record(const void *inbufp,
                                        const char *from_tag,
                                        struct dbrecord *db,
                                        struct schema_change_type *s)
{
    return convert_server_record_blobs(inbufp, from_tag, db, s, NULL /*blobs*/,
                                       0 /*maxblobs*/);
}

static void delay_sc_if_needed(struct convert_record_data *data,
                               db_seqnum_type *ss)
{
    const int mult = 100;
    static int inco_delay = 0; /* all stripes will see this */
    int rc;

    /* wait for replication on what we just committed */
    if ((data->nrecs % data->num_records_per_trans) == 0) {
        if ((rc = trans_wait_for_seqnum(&data->iq, gbl_myhostname, ss)) != 0) {
            sc_errf(data->s, "delay_sc_if_needed: error waiting for replication rcode %d\n", rc);
        } else if (gbl_sc_inco_chk) { /* committed successfully */
            int num;
            if ((num = bdb_get_num_notcoherent(thedb->bdb_env)) != 0) {
                if (num > inco_delay) { /* only goes up, or resets to 0 */
                    inco_delay = num;
                    sc_printf(data->s, "%d incoherent nodes - throttle sc %dms\n",
                              num, inco_delay * mult);
                }
            } else if (inco_delay != 0) {
                inco_delay = 0;
                sc_printf(data->s, "0 incoherent nodes - pedal to the metal\n");
            }
        } else { /* no incoherent chk */
            inco_delay = 0;
        }
    }

    if (inco_delay)
        poll(NULL, 0, inco_delay * mult);

    /* if we're in commitdelay mode, magnify the delay by 5 here */
    int delay = bdb_attr_get(data->from->dbenv->bdb_attr, BDB_ATTR_COMMITDELAY);
    if (delay != 0)
        poll(NULL, 0, delay * 5);
    else if (BDB_ATTR_GET(thedb->bdb_attr, SC_FORCE_DELAY))
        usleep(gbl_sc_usleep);

    /* if sanc list is not ok, snooze for 100 ms */
    if (!net_sanctioned_list_ok(data->from->dbenv->handle_sibling))
        poll(NULL, 0, 100);
}

static int report_sc_progress(struct convert_record_data *data, int now)
{
    int copy_sc_report_freq = gbl_sc_report_freq;

    if (copy_sc_report_freq > 0 &&
        now >= data->lasttime + copy_sc_report_freq) {
        /* report progress to interested parties */
        long long diff_nrecs = data->nrecs - data->prev_nrecs;
        data->lasttime = now;
        data->prev_nrecs = data->nrecs;

        /* print thread specific stats */
        sc_printf(data->s,
                  "[%s] progress stripe %d changed genids %u progress %lld"
                  " recs +%lld (%lld r/s)\n",
                  data->from->tablename, data->stripe, data->n_genids_changed,
                  data->nrecs, diff_nrecs, diff_nrecs / copy_sc_report_freq);

        /* now do global sc data */
        int res = print_aggregate_sc_stat(data, now, copy_sc_report_freq);
        /* check headroom only if this thread printed the global stats */
        if (res && check_sc_headroom(data->s, data->from, data->to)) {
            if (data->s->force) {
                sc_printf(data->s, "Proceeding despite low disk headroom\n");
            } else {
                return -1;
            }
        }
    }
    return 0;
}

static int prepare_and_verify_newdb_record(struct convert_record_data *data,
                                           void *dta, int dtalen,
                                           unsigned long long *dirty_keys,
                                           int leakcheck)
{
    int rc = 0;
    int dta_needs_conversion = 1;
    if ((!data->to->plan || !data->to->plan->dta_plan) &&
        data->s->rebuild_index)
        dta_needs_conversion = 0;

    if (dta_needs_conversion) {
        if (!data->s->force_rebuild &&
            !data->s->use_old_blobs_on_rebuild) /* We have correct blob data in
                                                   this. */
            bzero(data->wrblb, sizeof(data->wrblb));

        /* convert current.  this converts blob fields, but we need to make sure
         * we add the right blobs separately. */
        rc = convert_server_record_cachedmap(data->to, data->to->tablename, data->tagmap,
                                             dta, data->rec->recbuf, data->s, data->from->schema,
                                             data->to->schema, data->wrblb,
                                             sizeof(data->wrblb) / sizeof(data->wrblb[0]));
        if (rc) {
            logmsg(LOGMSG_ERROR, "%s:%d failed to convert record rc=%d\n",
                   __func__, __LINE__, rc);
            return -2;
        }

        /* TODO do the blobs returned by convert_server_record_blobs() need to
         * be converted to client blobs? */

        /* we are responsible for freeing any blob data that
         * convert_server_record_blobs() returns to us with free_blob_buffers().
         * if the plan calls for a full blob rebuild, data retrieved by
         * bdb_fetch_blobs_by_rrn_and_genid() may be added into wrblb in the
         * loop below, this blob data MUST be freed with free_blob_status_data()
         * so we need to make a copy of what we have right now so we can free it
         * seperately */
        free_blob_buffers(data->freeblb,
                          sizeof(data->freeblb) / sizeof(data->freeblb[0]));
        memcpy(data->freeblb, data->wrblb, sizeof(data->freeblb));
    }

    /* map old blobs to new blobs */
    if (!data->s->force_rebuild && !data->s->use_old_blobs_on_rebuild &&
        ((gbl_partial_indexes && data->to->ix_partial) || data->to->ix_expr ||
         !gbl_use_plan || !data->to->plan || !data->to->plan->plan_blobs || data->to->n_check_constraints)) {
        if (!leakcheck)
            bzero(data->wrblb, sizeof(data->wrblb));
        for (int ii = 0; ii < data->to->numblobs; ii++) {
            int fromblobix = data->toblobs2fromblobs[ii];
            if (fromblobix >= 0 && data->blb.blobptrs[fromblobix] != NULL) {
                if (data->wrblb[ii].data) {
                    /* this shouldn't happen because only bcstr to vutf8
                     * conversions should return any blob data from
                     * convert_server_record_blobs() and if we're createing a
                     * new vutf8 blob it should not have a fromblobix */
                    sc_errf(data->s,
                            "convert_record: attempted to "
                            "overwrite blob data retrieved from "
                            "convert_server_record_blobs() with data from "
                            "bdb_fetch_blobs_by_rrn_and_genid().  This would "
                            "leak memory and shouldn't ever happen. to blob %d "
                            "from blob %d\n",
                            ii, fromblobix);
                    return -2;
                }

                data->wrblb[ii].exists = 1;
                data->wrblb[ii].data =
                    ((char *)data->blb.blobptrs[fromblobix]) +
                    data->blb.bloboffs[fromblobix];
                data->wrblb[ii].length = data->blb.bloblens[fromblobix];
                data->wrblb[ii].collected = data->wrblb[ii].length;
            }
        }
    }

    /* Write record to destination table */
    data->iq.usedb = data->to;

    int rebuild = (data->to->plan && data->to->plan->dta_plan) ||
                  data->s->schema_change == SC_CONSTRAINT_CHANGE;

    uint8_t *p_buf_data = data->rec->recbuf;

    if (!dta_needs_conversion) {
        p_buf_data = dta;
    }

    *dirty_keys = -1ULL;
    if (gbl_partial_indexes && data->to->ix_partial) {
        *dirty_keys =
            verify_indexes(data->to, p_buf_data, data->wrblb, MAXBLOBS, 1);
        if (*dirty_keys == -1ULL) {
            rc = ERR_VERIFY_PI;
            return rc;
        }
    }

    assert(data->trans != NULL);

    rc = verify_check_constraints(data->iq.usedb, p_buf_data, data->wrblb,
                                  MAXBLOBS, 1);
    if (rc < 0) {
        logmsg(LOGMSG_DEBUG, "%s:%d internal error during CHECK constraint\n",
               __func__, __LINE__);
        return ERR_CONSTR;
    } else if (rc > 0) {
        logmsg(LOGMSG_DEBUG, "%s:%d CHECK constraint failed for '%s'\n",
               __func__, __LINE__,
               data->iq.usedb->check_constraints[rc - 1].consname);
        return ERR_CONSTR;
    }

    rc = verify_record_constraint(&data->iq, data->to, data->trans, p_buf_data,
                                  *dirty_keys, data->wrblb, MAXBLOBS,
                                  ".NEW..ONDISK", rebuild, 0);
    if (rc)
        return rc;

    if (gbl_partial_indexes && data->to->ix_partial) {
        rc = verify_partial_rev_constraint(data->from, data->to, data->trans,
                                           p_buf_data, *dirty_keys,
                                           ".NEW..ONDISK");
        if (rc)
            return rc;
    }

    return 0;
}

int gbl_sc_logbytes_per_second = 4 * (40 * 1024 * 1024); // 4 logs/sec

static pthread_mutex_t sc_bps_lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sc_bps_cd = PTHREAD_COND_INITIALIZER;
static int64_t sc_bytes_this_second;
static int sc_current_millisecond;

static void throttle_sc_logbytes(int estimate)
{
    if (gbl_sc_logbytes_per_second == 0)
        return;

    Pthread_mutex_lock(&sc_bps_lk);
    do
    {
        int now = comdb2_time_epochms();
        if (sc_current_millisecond < now - 1000) {
            sc_current_millisecond = now;
            sc_bytes_this_second = 0;
        }
        if (gbl_sc_logbytes_per_second > 0 && sc_bytes_this_second > gbl_sc_logbytes_per_second) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&sc_bps_cd, &sc_bps_lk, &ts);
        }
    } 
    while ((gbl_sc_logbytes_per_second > 0) && (sc_bytes_this_second > gbl_sc_logbytes_per_second));
    sc_bytes_this_second += estimate;
    Pthread_mutex_unlock(&sc_bps_lk);
}

static void increment_sc_logbytes(int64_t bytes)
{
    Pthread_mutex_lock(&sc_bps_lk);
    int now = comdb2_time_epochms();
    if (sc_current_millisecond < now - 1000) {
        sc_current_millisecond = now;
        sc_bytes_this_second = 0;
    }
    sc_bytes_this_second += bytes;
    Pthread_mutex_unlock(&sc_bps_lk);
}

/* converts a single record and prepares for the next one
 * should be called from a while loop
 * param data: pointer to all the state information
 * ret code:   1 means there are more records to convert
 *             0 means all work successfully done
 *             <0 means there was a failure (-2 skips some cleanup steps)
 */
static int convert_record(struct convert_record_data *data)
{
    int dtalen = 0, rc, rrn, opfailcode = 0, ixfailnum = 0;
    unsigned long long genid, ngenid, check_genid;
    int64_t logbytes = 0;
    void *dta = NULL;
    int no_wait_rowlock = 0;
    int64_t estimate = 0;

    if (debug_switch_convert_record_sleep())
        sleep(5);

    if (data->s->sc_thd_failed) {
        if (!data->s->retry_bad_genids)
            sc_errf(data->s,
                    "Stoping work on stripe %d because the thread for stripe %d failed\n",
                    data->stripe, data->s->sc_thd_failed - 1);
        return -1;
    }

    if (gbl_sc_abort || data->from->sc_abort ||
        (data->s->iq && data->s->iq->sc_should_abort)) {
        sc_client_error(data->s, "Schema change aborted");
        return -1;
    }
    if (tbl_had_writes(data)) {
        /* NB: if we return here, writes could block SC forever, so lets not */
        usleep(gbl_sc_usleep);
    }

    /* if master queue latency increased, slow down*/
    if (gbl_altersc_latency && gbl_altersc_delay_usec > 0)
        usleep(gbl_altersc_delay_usec);

    if (data->trans == NULL) {
        /* Schema-change writes are always page-lock, not rowlock */
        throttle_sc_logbytes(0);
        rc = trans_start_sc_lowpri(&data->iq, &data->trans);
        if (rc) {
            sc_errf(data->s, "Error %d starting transaction\n", rc);
            return -2;
        }
    }

    data->iq.debug = debug_this_request(gbl_debug_until);
    Pthread_mutex_lock(&gbl_sc_lock);
    if (gbl_who > 0) {
        gbl_who--;
        data->iq.debug = 1;
    }
    Pthread_mutex_unlock(&gbl_sc_lock);

    /* Get record to convert.  We support four scan modes:-
     * - SCAN_STRIPES - DEPRECATED AND REMOVED:
     *   read one record at a time from one of the
     *   stripe files, in order.  This is primarily to support
     *   live schema change.
     * - SCAN_PARALLEL - start one thread for each stripe, the thread
     *   reads all the records in its stripe in order
     * - SCAN_DUMP - bulk dump the data file(s).  Fastest possible
     *   scan mode.
     * - SCAN_INDEX - use regular ix_ routines to scan the primary
     *   key.  Dog slow because it causes the data file scan to be
     *   in essentially random order so you get lots and lots of
     *   cache misses.  However this is a good way to uncorrupt
     *   databases that were hit by the "oops, dtastripe didn't delete
     *   records" bug in the early days.
     */
    data->iq.usedb = data->from;
    data->iq.timeoutms = gbl_sc_timeoutms;

    /* Make sure that we do not insert random bytes (data->dta_buf is malloc'd)
       for inline field stored offline (e.g., vutf[100] but payload is longer than 100).
       It is technically fine to insert random bytes, since the inline portion is ignored
       for payload longer than the inline size. However inserting uniform bytes here
       is going to help us achieve a better compression ratio and lower disk use. */
    if (!gbl_debug_omit_zap_on_rebuild) { // test that field is zeroed out regardless (by default = 0)
        memset(data->dta_buf, 0, data->from->lrl);
    }

    /* Make sure that we do not inherit inline data from previous row
       (e.g., previous row has inline data, current row does not). See the comment above. */
    if (!gbl_debug_omit_zap_on_rebuild) { // test that field is zeroed out regardless (by default = 0)
        memset(data->rec->recbuf, 0, data->rec->bufsize);
    }

    if (data->scanmode == SCAN_PARALLEL || data->scanmode == SCAN_PAGEORDER) {
        if (data->scanmode == SCAN_PARALLEL) {
            rc = dtas_next(&data->iq, data->sc_genids, &genid, &data->stripe, 1,
                           data->dta_buf, data->trans, data->from->lrl, &dtalen,
                           NULL);
        } else {
            rc = dtas_next_pageorder(
                &data->iq, data->sc_genids, &genid, &data->stripe, 1,
                data->dta_buf, data->trans, data->from->lrl, &dtalen, NULL);
        }

#ifdef LOGICAL_LIVESC_DEBUG
        logmsg(LOGMSG_DEBUG, "(%u) %s rc=%d genid %llx (%llu)\n", (unsigned int)pthread_self(), __func__, rc, genid,
               genid);
#endif
        if (rc == 0) {
            dta = data->dta_buf;
            check_genid = bdb_normalise_genid(data->to->handle, genid);

            /* Whatever be the case, leave the lock*/
            if (check_genid != genid && !data->s->retry_bad_genids) {
                logmsg(LOGMSG_ERROR,
                       "Have old-style genids in table, disabling plan\n");
                data->s->retry_bad_genids = 1;
                return -1;
            }
            if (gbl_rowlocks) {
                rc = bdb_trylock_row_write(data->from->handle, data->trans,
                                           genid);
                if (rc) {
                    rc = RC_INTERNAL_RETRY;
                    no_wait_rowlock = 1;
                    goto err;
                }
            }
        } else if (rc == 1) {
            /* we have finished all the records in our stripe
             * set pointer to -1 so all insert/update/deletes will be
             * the the left of SC pointer. This works because we now hold
             * a lock to the last page of the stripe.
             */

            if (data->s->logical_livesc) {
                data->s->sc_convert_done[data->stripe] = 1;
                sc_printf(
                    data->s,
                    "[%s] finished converting stripe %d, last genid %llx\n",
                    data->from->tablename, data->stripe,
                    data->sc_genids[data->stripe]);
                return 0;
            }

            // AZ: determine what locks we hold at this time
            // bdb_dump_active_locks(data->to->handle, stdout);
            data->sc_genids[data->stripe] = -1ULL;

            if (debug_switch_scconvert_finish_delay()) {
                logmsg(LOGMSG_WARN, "scgenid reset. sleeping 10 sec.\n");
                sleep(10);
            }

            int usellmeta = 0;
            if (!data->to->plan) {
                usellmeta = 1; /* new dta does not have old genids */
            } else if (data->to->plan->dta_plan) {
                usellmeta = 0; /* the genid is in new dta */
            } else {
                usellmeta = 1; /* dta is not being built */
            }
            rc = 0;
            if (usellmeta && !is_dta_being_rebuilt(data->to->plan)) {
                int bdberr;
                rc = bdb_set_high_genid_stripe(NULL, data->to->tablename,
                                               data->stripe, -1ULL, &bdberr);
                if (rc != 0) rc = -1; // convert_record expects -1
            }
            sc_printf(data->s,
                      "[%s] finished stripe %d, setting genid %llx, rc %d\n",
                      data->from->tablename, data->stripe,
                      data->sc_genids[data->stripe], rc);
            return rc;
        } else if (rc == RC_INTERNAL_RETRY) {
            trans_abort(&data->iq, data->trans);
            data->trans = NULL;

            data->totnretries++;
            if (data->cmembers->is_decrease_thrds)
                decrease_max_threads(&data->cmembers->maxthreads);
            else
                poll(0, 0, (rand() % 500 + 10));
            return 1;
        } else if (rc != 0) {
            sc_errf(data->s, "error %d reading database records\n", rc);
            return -2;
        }
        rrn = 2;
    } else if (data->scanmode == SCAN_DUMP) {
        int bdberr;
        uint8_t ver;
        if (data->dmp == NULL) {
            data->dmp = bdb_dtadump_start(data->from->handle, &bdberr, 0, 0);
            if (data->dmp == NULL) {
                sc_errf(data->s, "bdb_dtadump_start rc %d\n", bdberr);
                return -1;
            }
        }
        rc = bdb_dtadump_next(data->from->handle, data->dmp, &dta, &dtalen,
                              &rrn, &genid, &ver, &bdberr);
        vtag_to_ondisk(data->iq.usedb, dta, &dtalen, ver, genid);
        if (rc == 1) {
            /* no more records - success! */
            return 0;
        } else if (rc != 0) {
            sc_errf(data->s, "bdb error %d reading database records\n", bdberr);
            return -2;
        }

        check_genid = bdb_normalise_genid(data->to->handle, genid);
        if (check_genid != genid && !data->s->retry_bad_genids) {
            logmsg(LOGMSG_ERROR,
                   "Have old-style genids in table, disabling plan\n");
            data->s->retry_bad_genids = 1;
            return -1;
        }
    } else if (data->scanmode == SCAN_INDEX) {
        if (data->nrecs == 0) {
            bzero(data->lastkey, MAXKEYLEN);
            rc = ix_find(&data->iq, 0 /*ixnum*/, data->lastkey, 0 /*keylen*/,
                         data->curkey, &rrn, &genid, data->dta_buf, &dtalen,
                         data->from->lrl);
        } else {
            char *tmp = data->curkey;
            data->curkey = data->lastkey;
            data->lastkey = tmp;
            rc = ix_next(&data->iq, 0 /*ixnum*/, data->lastkey, 0 /*keylen*/,
                         data->lastkey, data->lastrrn, data->lastgenid,
                         data->curkey, &rrn, &genid, data->dta_buf, &dtalen,
                         data->from->lrl, 0 /*context - 0 means don't care*/);
        }
        if (rc == IX_FND || rc == IX_FNDMORE) {
            /* record found */
            data->lastrrn = rrn;
            data->lastgenid = genid;
            dta = data->dta_buf;

            check_genid = bdb_normalise_genid(data->to->handle, genid);
            if (check_genid != genid && !data->s->retry_bad_genids) {
                logmsg(LOGMSG_ERROR,
                       "Have old-style genids in table, disabling plan\n");
                data->s->retry_bad_genids = 1;
                return -1;
            }
        } else if (rc == IX_NOTFND || rc == IX_PASTEOF || rc == IX_EMPTY) {
            /* no more records - success! */
            return 0;
        } else {
            sc_errf(data->s, "ix_find/ix_next error rcode %d\n", rc);
            return -2;
        }
    } else {
        sc_errf(data->s, "internal error - bad scan mode!\n");
        return -2;
    }

    /* Report wrongly sized records */
    if (dtalen != data->from->lrl) {
        sc_errf(data->s, "invalid record size for rrn %d genid 0x%llx (%d bytes"
                         " but expected %d)\n",
                rrn, genid, dtalen, data->from->lrl);
        return -2;
    }

    /* Read associated blobs.  We usually don't need to do this in
     * planned schema change (since blobs aren't convertible the btrees
     * don't change.. unless we're changing compression options. */
    if (data->from->numblobs != 0 &&
        ((gbl_partial_indexes && data->to->ix_partial) || data->to->ix_expr ||
         !gbl_use_plan || !data->to->plan || !data->to->plan->plan_blobs ||
         data->s->force_rebuild || data->s->use_old_blobs_on_rebuild || data->to->n_check_constraints)) {
        int bdberr;
        free_blob_status_data(&data->blb);
        bdb_fetch_args_t args = {0};
        bzero(data->wrblb, sizeof(data->wrblb));

        int blobrc;
        memcpy(&data->blb, &data->blbcopy, sizeof(data->blb));
        blobrc = bdb_fetch_blobs_by_rrn_and_genid_tran(
            data->from->handle, data->trans, rrn, genid, data->from->numblobs,
            data->blobix, data->blb.bloblens, data->blb.bloboffs,
            (void **)data->blb.blobptrs, &args, &bdberr);
        if (blobrc != 0 && bdberr == BDBERR_DEADLOCK) {
            trans_abort(&data->iq, data->trans);
            data->trans = NULL;
            data->totnretries++;
            if (data->cmembers->is_decrease_thrds)
                decrease_max_threads(&data->cmembers->maxthreads);
            else
                poll(0, 0, (rand() % 500 + 10));

            return 1;
        }
        if (blobrc != 0) {
            sc_errf(data->s, "convert_record: "
                             "bdb_fetch_blobs_by_rrn_and_genid bdberr %d\n",
                    bdberr);
            return -2;
        }
        rc = check_and_repair_blob_consistency(
            &data->iq, data->iq.usedb, ".ONDISK", &data->blb, dta);

        if (data->s->force_rebuild || data->s->use_old_blobs_on_rebuild) {
            for (int ii = 0; ii < data->from->numblobs; ii++) {
                if (data->blb.blobptrs[ii] != NULL) {
                    data->wrblb[ii].exists = 1;
                    data->wrblb[ii].data = malloc(data->blb.bloblens[ii]);
                    memcpy(data->wrblb[ii].data,
                           ((char *)data->blb.blobptrs[ii]) +
                               data->blb.bloboffs[ii],
                           data->blb.bloblens[ii]);
                    data->wrblb[ii].length = data->blb.bloblens[ii];
                    data->wrblb[ii].collected = data->wrblb[ii].length;
                }
            }
        }

        if (rc != 0) {
            sc_errf(data->s,
                    "unexpected blob inconsistency rc %d, rrn %d, genid "
                    "0x%llx\n",
                    rc, rrn, genid);
            free_blob_status_data(&data->blb);
            return -2;
        }
    }

    int usellmeta = 0;
    if (!data->to->plan) {
        usellmeta = 1; /* new dta does not have old genids */
    } else if (data->to->plan->dta_plan) {
        usellmeta = 0; /* the genid is in new dta */
    } else {
        usellmeta = 1; /* dta is not being built */
    }

    int dta_needs_conversion = 1;
    if (usellmeta && data->s->rebuild_index)
        dta_needs_conversion = 0;

    /* Write record to destination table */
    data->iq.usedb = data->to;

    unsigned long long dirty_keys = -1ULL;

    if (data->s->use_new_genids) {
        assert(!gbl_use_plan);
        ngenid = get_genid(thedb->bdb_env, get_dtafile_from_genid(check_genid));
    } else {
        ngenid = check_genid;
    }
    if (ngenid != genid)
        data->n_genids_changed++;

    rc = prepare_and_verify_newdb_record(data, dta, dtalen, &dirty_keys, 1);
    if (rc) {
        sc_errf(data->s,
                "failed to prepare and verify newdb record rc %d, rrn %d, genid 0x%llx\n",
                rc, rrn, genid);
        if (rc == -2)
            return -2; /* convertion failure */
        goto err;
    }

    int addflags = RECFLAGS_NO_TRIGGERS | RECFLAGS_NO_CONSTRAINTS |
                   RECFLAGS_NEW_SCHEMA | RECFLAGS_KEEP_GENID;

    if (data->to->plan && gbl_use_plan) addflags |= RECFLAGS_NO_BLOBS;

    char *tagname = ".NEW..ONDISK";
    uint8_t *p_tagname_buf = (uint8_t *)tagname;
    uint8_t *p_tagname_buf_end = p_tagname_buf + 12;
    uint8_t *p_buf_data = data->rec->recbuf;
    uint8_t *p_buf_data_end = p_buf_data + data->rec->bufsize;
    estimate = data->rec->bufsize;

    if (!dta_needs_conversion) {
        p_buf_data = dta;
        p_buf_data_end = p_buf_data + dtalen;
    }

    assert(data->trans != NULL);

    if (data->s->schema_change != SC_CONSTRAINT_CHANGE) {
        int nrrn = rrn;

        for (int i = 0; i != MAXBLOBS; ++i)
            estimate += data->wrblb[i].length;

        /* Estimate how many log bytes this convert thread will write, and throttle if needed.
           We'll adjust it after add_record() when we know the actual number of log bytes. */
        throttle_sc_logbytes(estimate);

        rc = add_record(
            &data->iq, data->trans, p_tagname_buf, p_tagname_buf_end,
            p_buf_data, p_buf_data_end, NULL, data->wrblb, MAXBLOBS,
            &opfailcode, &ixfailnum, &nrrn, &ngenid,
            (gbl_partial_indexes && data->to->ix_partial) ? dirty_keys : -1ULL,
            BLOCK2_ADDKL, /* opcode */
            0,            /* blkpos */
            addflags, 0);

        if (rc)
            goto err;
    }

    /* if we have been rebuilding the data files we're gonna
       call bdb_get_high_genid to resume, not look at llmeta */
    if (usellmeta && !is_dta_being_rebuilt(data->to->plan) &&
        (data->nrecs %
         BDB_ATTR_GET(thedb->bdb_attr, INDEXREBUILD_SAVE_EVERY_N)) == 0) {
        int bdberr;
        rc = bdb_set_high_genid(data->trans, data->to->tablename, genid,
                                &bdberr);
        if (rc != 0) {
            if (bdberr == BDBERR_DEADLOCK)
                rc = RC_INTERNAL_RETRY;
            else
                rc = ERR_INTERNAL;
        }
    }

err:
    if (data->s->logical_livesc && (rc == IX_DUP || rc == ERR_CONSTR)) {
        /* handle constraints violations */
        if (data->cv_genid != ngenid) {
            /* get current lsn if this is a new constraint violation */
            bdb_get_commit_genid(thedb->bdb_env, &data->cv_wait_lsn);
            if (data->cv_wait_lsn.file <= 1 && data->cv_wait_lsn.offset == 0) {
                logmsg(LOGMSG_ERROR, "%s:%d failed to get current lsn\n",
                       __func__, __LINE__);
                rc = ERR_INTERNAL;
            }
            /* remember genid of the record that last violated constraints */
            data->cv_genid = ngenid;
        }

        Pthread_mutex_lock(&data->s->livesc_mtx);
        /* wait for logical redo thread to catch up to the lsn at which the
         * constraint violation was first detected */
        if (data->s->curLsn &&
            log_compare(data->s->curLsn, &data->cv_wait_lsn) < 0)
            rc = RC_INTERNAL_RETRY;
        Pthread_mutex_unlock(&data->s->livesc_mtx);
        if (rc == RC_INTERNAL_RETRY) {
            logmsg(LOGMSG_DEBUG,
                   "%s: got constraints violations on genid %llx, stripe %d, "
                   "waiting for logical redo to catch up at [%u:%u]\n",
                   __func__, ngenid, data->stripe, data->cv_wait_lsn.file,
                   data->cv_wait_lsn.offset);
            logbytes = bdb_tran_logbytes(data->trans);
            increment_sc_logbytes(logbytes - estimate);
            trans_abort(&data->iq, data->trans);
            data->trans = NULL;
            poll(0, 0, 200);
            return 1;
        }
    }

    if (gbl_sc_abort || data->from->sc_abort ||
        (data->s->iq && data->s->iq->sc_should_abort)) {
        logbytes = bdb_tran_logbytes(data->trans);
        increment_sc_logbytes(logbytes - estimate);
        trans_abort(&data->iq, data->trans);
        data->trans = NULL;
        return -1;
    }

    /* if we should retry the operation */
    if (rc == RC_INTERNAL_RETRY) {
        logbytes = bdb_tran_logbytes(data->trans);
        increment_sc_logbytes(logbytes - estimate);
        trans_abort(&data->iq, data->trans);
        data->trans = NULL;
        data->num_retry_errors++;
        data->totnretries++;
        if (!no_wait_rowlock && data->cmembers->is_decrease_thrds)
            decrease_max_threads(&data->cmembers->maxthreads);
        else
            poll(0, 0, (rand() % 500 + 10));
        return 1;
    } else if (rc == IX_DUP) {
        if ((data->scanmode == SCAN_PARALLEL ||
             data->scanmode == SCAN_PAGEORDER) &&
            data->s->rebuild_index) {
            /* if we are resuming an index rebuild schemachange,
             * and the stored llmeta genid is stale, some of the records
             * will fail insertion, and that is ok */

            sc_errf(data->s, "Skipping duplicate entry in index %d rrn %d genid 0x%llx\n",
                    ixfailnum, rrn, genid);
            data->sc_genids[data->stripe] = genid;
            logbytes = bdb_tran_logbytes(data->trans);
            increment_sc_logbytes(logbytes - estimate);
            trans_abort(&data->iq, data->trans);
            data->trans = NULL;
            return 1;
        }

        sc_client_error(data->s, "Could not add duplicate entry in index %d rrn %d genid 0x%llx", ixfailnum, rrn,
                        genid);
        return -2;
    } else if (rc == ERR_CONSTR) {
        sc_client_error(data->s, "Record violates foreign constraints rrn %d genid 0x%llx", rrn, genid);
        return -2;
    } else if (rc == ERR_VERIFY_PI) {
        sc_client_error(data->s, "Error verifying partial indexes! rrn %d genid 0x%llx", rrn, genid);
        return -2;
    } else if (rc != 0) {
        sc_client_error(data->s,
                        "Error adding record rcode %d opfailcode %d ixfailnum %d rrn %d genid 0x%llx, stripe %d", rc,
                        opfailcode, ixfailnum, rrn, genid, data->stripe);
        return -2;
    }

    /* Advance our progress markers */
    data->nrecs++;
    if (data->scanmode == SCAN_PARALLEL || data->scanmode == SCAN_PAGEORDER) {
        data->sc_genids[data->stripe] = genid;
    }

    // now do the commit
    db_seqnum_type ss;
    if (data->live) {
        rc = trans_commit_seqnum(&data->iq, data->trans, &ss);
    } else {
        rc = trans_commit(&data->iq, data->trans, gbl_myhostname);
    }
    increment_sc_logbytes(data->iq.txnsize - estimate);

    data->trans = NULL;

    if (rc) {
        sc_errf(data->s, "convert_record: trans_commit failed with rcode %d", rc);
        /* If commit fail we are failing the whole operation */
        return -2;
    }

    if (data->live)
        delay_sc_if_needed(data, &ss);

    ATOMIC_ADD64(data->from->sc_nrecs, 1);

    int now = comdb2_time_epoch();
    if ((rc = report_sc_progress(data, now))) return rc;

    // do the following check every second or so
    if (data->cmembers->is_decrease_thrds) lkcounter_check(data, now);

    return 1;
}

/* Thread local flag to disable page compaction when rebuild.
   Initialized in mp_fget.c */
extern pthread_key_t no_pgcompact;

/* prepares for and then calls convert_record until success or failure
 * param data: state data
 * return code: not used
 *              success or failure returned in data->outrc: 0 for success,
 *                  -1 for failure
 */
static void *convert_records_thd(struct convert_record_data *data)
{
    comdb2_name_thread(__func__);
    ENABLE_PER_THREAD_MALLOC(__func__);
    struct thr_handle *thr_self = thrman_self();
    enum thrtype oldtype = THRTYPE_UNKNOWN;
    int rc = 1;

    if (data->isThread) thread_started("convert records");

    if (thr_self) {
        oldtype = thrman_get_type(thr_self);
        thrman_change_type(thr_self, THRTYPE_SCHEMACHANGE);
    } else {
        thr_self = thrman_register(THRTYPE_SCHEMACHANGE);
    }

    if (data->isThread) {
        backend_thread_event(thedb, COMDB2_THR_EVENT_START_RDWR);
    }

    data->iq.reqlogger = thrman_get_reqlogger(thr_self);
    data->outrc = -1;
    data->curkey = data->key1;
    data->lastkey = data->key2;
    data->rec = allocate_db_record(data->to, ".NEW..ONDISK");
    data->dta_buf = malloc(data->from->lrl);
    if (!data->dta_buf) {
        sc_errf(data->s, "convert_records_thd: ran out of memory trying to "
                         "malloc dta_buf: %d\n",
                data->from->lrl);
        data->outrc = -1;
        goto cleanup;
    }

    /* from this point onwards we must get to the cleanup code before
     * returning.  assume failure unless we explicitly succeed.  */
    data->lasttime = comdb2_time_epoch();

    data->num_records_per_trans = gbl_num_record_converts;
    data->num_retry_errors = 0;

    if (gbl_pg_compact_thresh > 0) {
        /* Disable page compaction only if page compaction is enabled. */
        Pthread_setspecific(no_pgcompact, (void *)1);
    }
    snap_uid_t loc_snap_info;
    osql_snap_info = &loc_snap_info; /* valid for the duration of the rebuild */
    loc_snap_info.keylen = snprintf(loc_snap_info.key, sizeof(loc_snap_info.key),
                                    "internal-schemachange-%p", (void *)pthread_self());

    if (data->iq.debug) {
        reqlog_new_request(&data->iq);
        reqpushprefixf(&data->iq, "0x%llx: CONVERT_REC ", pthread_self());
    }

    int prev_preempted = data->s->preempted;
    /* convert each record */
    while (rc > 0) {
        if (data->cmembers->is_decrease_thrds &&
            use_rebuild_thr(&data->cmembers->thrcount,
                            &data->cmembers->maxthreads)) {
            /* num thread at max, sleep few microsec then try again */
            usleep(bdb_attr_get(data->from->dbenv->bdb_attr,
                                BDB_ATTR_SC_NO_REBUILD_THR_SLEEP));
            continue;
        }

        /* convert_record returns 1 to continue, 0 on completion, < 0 if failed
         */
        rc = convert_record(data);
        if (data->cmembers->is_decrease_thrds)
            release_rebuild_thr(&data->cmembers->thrcount);

        if (get_stopsc(__func__, __LINE__)) { // set from downgrade
            data->outrc = SC_MASTER_DOWNGRADE;
            goto cleanup_no_msg;
        }
        if (prev_preempted != data->s->preempted) {
            logmsg(LOGMSG_INFO, "%s schema change preempted %d\n", __func__,
                   data->s->preempted);
            data->outrc = SC_PREEMPTED;
            goto cleanup_no_msg;
        }
    }

    if (rc == -2) {
        data->outrc = -1;
        goto cleanup;
    } else {
        data->outrc = rc;
    }

    if (data->trans) {
        /* can only get here for non-live schema change, shouldn't ever get here
         * now since bulk transactions have been disabled in all schema changes
         */
        rc = trans_commit(&data->iq, data->trans, gbl_myhostname);
        data->trans = NULL;
        if (rc) {
            sc_errf(data->s, "convert_records_thd: trans_commit failed due "
                             "to RC_TRAN_TOO_COMPLEX\n");
            data->outrc = -1;
            goto cleanup;
        }
    }

    if (data->outrc == 0 && data->n_genids_changed > 0) {
        sc_errf(data->s,
                "WARNING %u genids were changed by this schema change\n",
                data->n_genids_changed);
    }

cleanup:
    if (data->outrc == 0) {
        sc_printf(data->s,
                  "[%s] successfully converted %lld records with %d retries "
                  "stripe %d\n",
                  data->from->tablename, data->nrecs, data->totnretries,
                  data->stripe);
    } else {
        if (gbl_sc_abort || data->from->sc_abort ||
            (data->s->iq && data->s->iq->sc_should_abort)) {
            sc_errf(data->s,
                    "conversion aborted after %lld records, while working on"
                    " stripe %d with %d retries\n",
                    data->nrecs, data->stripe, data->totnretries);
        } else if (!data->s->retry_bad_genids) {
            sc_errf(data->s,
                    "conversion failed after %lld records, while working on"
                    " stripe %d with %d retries\n",
                    data->nrecs, data->stripe, data->totnretries);
        }

        data->s->sc_thd_failed = data->stripe + 1;
    }

cleanup_no_msg:
    convert_record_data_cleanup(data);
    if (data->isThread) backend_thread_event(thedb, COMDB2_THR_EVENT_DONE_RDWR);

    if (data->iq.debug)
        reqpopprefixes(&data->iq, 1);

    /* restore our  thread type to what it was before */
    if (oldtype != THRTYPE_UNKNOWN) thrman_change_type(thr_self, oldtype);

    return NULL;
}

static void stop_sc_redo_wait(bdb_state_type *bdb_state,
                              struct schema_change_type *s)
{
    Pthread_mutex_lock(&bdb_state->sc_redo_lk);
    s->sc_convert_done[MAXDTASTRIPE] = 1;
    Pthread_cond_signal(&bdb_state->sc_redo_wait);
    Pthread_mutex_unlock(&bdb_state->sc_redo_lk);
}

int gbl_sc_pause_at_end = 0;
int gbl_sc_is_at_end = 0;

int convert_all_records(struct dbtable *from, struct dbtable *to,
                        unsigned long long *sc_genids,
                        struct schema_change_type *s)
{
    struct convert_record_data data = {0};
    int ii;
    s->sc_thd_failed = 0;

    data.curkey = data.key1;
    data.lastkey = data.key2;
    data.from = from;
    data.to = to;
    data.live = s->live;
    data.scanmode = s->scanmode;
    data.sc_genids = sc_genids;
    data.s = s;

    if (data.live && data.scanmode != SCAN_PARALLEL) {
        sc_errf(data.s,
                "live schema change can only be done in parallel scan mode\n");
        logmsg(LOGMSG_ERROR,
               "live schema change can only be done in parallel scan mode\n");
        return -1;
    }

    /* Calculate blob data file numbers to feed direct into bdb.  This used
     * to be a hard coded array.  And it was wrong.  By employing for loop
     * technology, we can't possibly get this wrong again! */
    for (int ii = 0; ii < MAXBLOBS; ii++)
        data.blobix[ii] = ii + 1;

    /* set up internal rebuild request */
    init_fake_ireq(thedb, &data.iq);
    data.iq.usedb = data.from;
    data.iq.opcode = OP_REBUILD;
    data.iq.debug = 0; /*gbl_who;*/

    /* For first cut, read all blobs.  Later we can optimise by only reading
     * the blobs that the new schema needs.
     * Now, it's later.  Don't read any blobs at all if we are using a plan. */
    if ((gbl_partial_indexes && data.to->ix_partial) || data.to->ix_expr ||
        !gbl_use_plan || !data.to->plan || !data.to->plan->plan_blobs) {
        if (gather_blob_data(&data.iq, ".ONDISK", &data.blb, ".ONDISK")) {
            sc_errf(data.s, "convert_all_records:gather_blob_data failed\n");
            return -1;
        }
        data.blb.numcblobs = data.from->numblobs;
        for (ii = 0; ii < data.blb.numcblobs; ii++) {
            data.blb.cblob_disk_ixs[ii] = ii;
            data.blb.cblob_tag_ixs[ii] =
                get_schema_blob_field_idx(data.from, ".ONDISK", ii);
        }
        for (ii = 0; ii < data.to->numblobs; ii++) {
            int map;
            map =
                tbl_blob_no_to_tbl_blob_no(data.to, ".NEW..ONDISK",
                                           ii, data.from, ".ONDISK");
            if (map < 0 && map != -3) {
                sc_errf(data.s,
                        "convert_all_records: error mapping blob %d "
                        "from %s:%s to %s:%s blob_no_to_blob_no rcode %d\n",
                        ii, data.from->tablename, ".ONDISK", data.to->tablename,
                        ".NEW..ONDISK", map);
                return -1;
            }
            data.toblobs2fromblobs[ii] = map;
        }
        memcpy(&data.blbcopy, &data.blb, sizeof(data.blb));
    } else {
        bzero(&data.blb, sizeof(data.blb));
        bzero(&data.blbcopy, sizeof(data.blbcopy));
    }

    data.cmembers = calloc(1, sizeof(struct common_members));
    int sc_threads =
        bdb_attr_get(data.from->dbenv->bdb_attr, BDB_ATTR_SC_USE_NUM_THREADS);
    if (sc_threads <= 0 || sc_threads > gbl_dtastripe) {
        bdb_attr_set(data.from->dbenv->bdb_attr, BDB_ATTR_SC_USE_NUM_THREADS,
                     gbl_dtastripe);
        sc_threads = gbl_dtastripe;
    }
    data.cmembers->maxthreads = sc_threads;
    data.cmembers->is_decrease_thrds = bdb_attr_get(
        data.from->dbenv->bdb_attr, BDB_ATTR_SC_DECREASE_THRDS_ON_DEADLOCK);

    // tagmap only needed if we are doing work on the data file
    data.tagmap = get_tag_mapping(
        data.from->schema /*tbl .ONDISK tag schema*/,
        data.to->schema /*tbl .NEW..ONDISK schema */); // free tagmap only once
    int outrc = 0;

    if (gbl_logical_live_sc) {
        int rc = 0, bdberr = 0;
        struct convert_record_data *thdData =
            calloc(1, sizeof(struct convert_record_data));
        if (!thdData) {
            logmsg(LOGMSG_ERROR, "%s:%d out of memory\n", __func__, __LINE__);
            return -1;
        }
        /* set s->sc_convert_done[MAXDTASTRIPE] = 1 if all stripes are done */
        s->sc_convert_done = calloc(MAXDTASTRIPE + 1, sizeof(int));
        if (s->sc_convert_done == NULL) {
            logmsg(LOGMSG_ERROR, "%s:%d out of memory\n", __func__, __LINE__);
            return -1;
        }
        *thdData = data;
        if (s->resume) {
            /* get lsn where we left off */
            rc = bdb_get_sc_start_lsn(NULL, s->tablename, &(thdData->start_lsn),
                                      &bdberr);
            if (rc) {
                logmsg(
                    LOGMSG_ERROR,
                    "%s:%d failed to restore start lsn, rc = %d, bdberr = %d\n",
                    __func__, __LINE__, rc, bdberr);
                return -1;
            }
        } else {
            /* start lsn of this schema change */
            bdb_get_commit_genid(thedb->bdb_env, &(thdData->start_lsn));
            if (thdData->start_lsn.file <= 1 &&
                thdData->start_lsn.offset == 0) {
                logmsg(LOGMSG_ERROR, "%s:%d failed to get start lsn\n",
                       __func__, __LINE__);
                return -1;
            }
            rc = bdb_set_sc_start_lsn(NULL, s->tablename, &(thdData->start_lsn),
                                      &bdberr);
            if (rc) {
                logmsg(LOGMSG_ERROR,
                       "%s:%d failed to write start lsn to llmeta, rc = %d, "
                       "bdberr = %d\n",
                       __func__, __LINE__, rc, bdberr);
                return -1;
            }

            /* If we aren't resuming, delete everything from this table */
            bdb_newsc_del_all_redo_genids(NULL, s->tablename, &bdberr);
        }
        sc_set_logical_redo_lwm(s->tablename, thdData->start_lsn.file);
        thdData->stripe = -1;
        sc_printf(s, "[%s] starting thread for logical live schema change\n",
                  s->tablename);
        thdData->isThread = 1;

        /* enable logical logging for this table for the duration of the schema change */
        rc = bdb_set_logical_live_sc(s->db->handle, 1 /* lock table */);
        if (rc) {
            logmsg(LOGMSG_ERROR,
                    "%s:%d failed to set logical live sc, rc = %d\n",
                    __func__, __LINE__, rc);
            free(s->sc_convert_done);
            s->sc_convert_done = NULL;
            return -1;
        }

        s->logical_livesc = 1;
        Pthread_rwlock_wrlock(&s->db->sc_live_lk);
        s->db->sc_live_logical = 1;
        Pthread_rwlock_unlock(&s->db->sc_live_lk);
        Pthread_create(&thdData->tid, &gbl_pthread_attr_detached,
                            (void *(*)(void *))live_sc_logical_redo_thd,
                            thdData);
    } else {
        if (s->resume) {
            /* if schema change was run in logical redo mode and logical redo sc
             * is disabled in resume, abort the schema change */
            int rc = 0, bdberr = 0;
            rc = bdb_get_sc_start_lsn(NULL, s->tablename, &(data.start_lsn),
                                      &bdberr);
            if (rc == 0 || bdberr != BDBERR_FETCH_DTA) {
                sc_errf(data.s,
                        "rc = %d bdberr = %d trying to resume from "
                        "[%u][%u] while logical live sc is turned off\n",
                        rc, bdberr, data.start_lsn.file, data.start_lsn.offset);
                return -1;
            }
        }
        s->logical_livesc = 0;
    }

    /* if were not in parallel, dont start any threads */
    if (data.scanmode != SCAN_PARALLEL && data.scanmode != SCAN_PAGEORDER) {
        convert_records_thd(&data);
        outrc = data.outrc;
    } else {
        struct convert_record_data threadData[gbl_dtastripe];
        int threadSkipped[gbl_dtastripe];
        pthread_attr_t attr;
        int rc = 0;

        data.isThread = 1;

        Pthread_attr_init(&attr);
        Pthread_attr_setstacksize(&attr, DEFAULT_THD_STACKSZ);
        Pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

        /* start one thread for each stripe */
        for (ii = 0; ii < gbl_dtastripe; ++ii) {
            /* create a copy of the data, modifying the necessary
             * thread specific values
             */
            threadData[ii] = data;
            threadData[ii].stripe = ii;

            if (sc_genids[ii] == -1ULL) {
                sc_printf(threadData[ii].s, "[%s] stripe %d was done\n",
                          from->tablename, threadData[ii].stripe);
                threadSkipped[ii] = 1;
                continue;
            } else
                threadSkipped[ii] = 0;

            sc_printf(threadData[ii].s, "[%s] starting thread for stripe: %d\n",
                      from->tablename, threadData[ii].stripe);

            /* start thread */
            /* convert_records_thd( &threadData[ ii ]); |+ serialized calls +|*/
            Pthread_create(&threadData[ii].tid, &attr,
                                (void *(*)(void *))convert_records_thd,
                                &threadData[ii]);

        }

        /* wait for all convert threads to complete */
        for (ii = 0; ii < gbl_dtastripe; ++ii) {
            void *ret;

            if (threadSkipped[ii]) continue;

            /* if the threadid is NULL, skip this one */
            if (!threadData[ii].tid) {
                sc_errf(threadData[ii].s, "skip joining thread failed for "
                                          "stripe: %d because tid is null\n",
                        threadData[ii].stripe);
                outrc = -1;
                continue;
            }

            rc = pthread_join(threadData[ii].tid, &ret);

            /* if join failed */
            if (rc) {
                sc_errf(threadData[ii].s, "joining thread failed for"
                                          " stripe: %d with return code: %d\n",
                        threadData[ii].stripe, rc);
                outrc = -1;
                continue;
            }

            /* if thread's conversions failed return error code */
            if (threadData[ii].outrc != 0) outrc = threadData[ii].outrc;
        }

        /* destroy attr */
        Pthread_attr_destroy(&attr);
    }

    print_final_sc_stat(&data);

    convert_record_data_cleanup(&data);

    gbl_sc_is_at_end = 1;

    while (gbl_sc_pause_at_end) {
        logmsg(LOGMSG_USER, "%s pausing after converted all threads\n",
               __func__);
        sleep(1);
    }

    gbl_sc_is_at_end = 0;

    if (data.cmembers) {
        free(data.cmembers);
        data.cmembers = NULL;
    }

    if (data.tagmap) {
        free(data.tagmap);
        data.tagmap = NULL;
    }

    if (s->logical_livesc) {
        if (outrc == 0) {
            sc_printf(s, "[%s] All convert threads finished\n",
                      from->tablename);
        }
        stop_sc_redo_wait(from->handle, s);
    }
    /* wait for logical redo thread */
    while (s->logical_livesc && !s->hitLastCnt) {
        poll(NULL, 0, 200);
    }

    return outrc;
}

/*
** Similar to convert_record(), with the following exceptions.
** 1. continue working on the rest stripes if some stripes failed
** 2. no retry effort if upd_record() deadlock'd
** 3. no blob/index r/w
*/
static int upgrade_records(struct convert_record_data *data)
{
    static const int mult = 100;
    static int inco_delay = 0;

    int rc;
    int nretries;
    int commitdelay;
    int now;
    int copy_sc_report_freq = gbl_sc_report_freq;
    int opfailcode = 0;
    int ixfailnum = 0;
    int dtalen = 0;
    unsigned long long genid = 0;
    int recver;
    uint8_t *p_buf_data, *p_buf_data_end;
    u_int64_t logbytes = 0;
    db_seqnum_type ss;

    // if sc has beed aborted, return
    if (gbl_sc_abort || data->from->sc_abort ||
        (data->s->iq && data->s->iq->sc_should_abort)) {
        sc_errf(data->s, "Schema change aborted\n");
        return -1;
    }

    if (data->trans == NULL) {
        /* Schema-change writes are always page-lock, not rowlock */
        throttle_sc_logbytes(0);
        rc = trans_start_sc_lowpri(&data->iq, &data->trans);
        if (rc) {
            sc_errf(data->s, "error %d starting transaction\n", rc);
            return -2;
        }
    }

    // set debug info
    if (gbl_who > 0 || data->iq.debug > 0) {
        Pthread_mutex_lock(&gbl_sc_lock);
        data->iq.debug = gbl_who;
        if (data->iq.debug > 0) {
            gbl_who = data->iq.debug - 1;
            data->iq.debug = 1;
        }
        Pthread_mutex_unlock(&gbl_sc_lock);
    }

    data->iq.timeoutms = gbl_sc_timeoutms;

    // get next converted record. retry up to gbl_maxretries times
    for (nretries = 0, rc = RC_INTERNAL_RETRY;
         rc == RC_INTERNAL_RETRY && nretries++ != gbl_maxretries;) {

        if (data->nrecs > 0 || data->sc_genids[data->stripe] == 0) {
            rc = dtas_next(&data->iq, data->sc_genids, &genid, &data->stripe,
                           data->scanmode == SCAN_PARALLEL, data->dta_buf,
                           data->trans, data->from->lrl, &dtalen, &recver);
        } else {
            genid = data->sc_genids[data->stripe];
            rc = ix_find_ver_by_rrn_and_genid_tran(
                &data->iq, 2, genid, data->dta_buf, &dtalen, data->from->lrl,
                data->trans, &recver);
        }

        switch (rc) {
        case 0: // okay
            break;

        case 1: // finish reading all records
            sc_printf(data->s, "finished stripe %d\n", data->stripe);
            return 0;

        case RC_INTERNAL_RETRY: // retry
            logbytes = bdb_tran_logbytes(data->trans);
            increment_sc_logbytes(logbytes);
            trans_abort(&data->iq, data->trans);
            data->trans = NULL;
            break;

        default: // fatal error
            sc_errf(data->s, "error %d reading database records\n", rc);
            return -2;
        }
    }

    // exit after too many retries
    if (rc == RC_INTERNAL_RETRY) {
        sc_errf(data->s, "%s: *ERROR* dtas_next "
                         "too much contention count %d genid 0x%llx\n",
                __func__, nretries, genid);
        return -2;
    }

    /* Report wrongly sized records */
    if (dtalen != data->from->lrl) {
        sc_errf(data->s, "invalid record size for genid 0x%llx (%d bytes"
                         " but expected %d)\n",
                genid, dtalen, data->from->lrl);
        return -2;
    }

    if (recver != data->to->schema_version) {
        // rewrite the record if not ondisk version
        p_buf_data = (uint8_t *)data->dta_buf;
        p_buf_data_end = p_buf_data + data->from->lrl;
        rc = upgrade_record(&data->iq, data->trans, genid, p_buf_data,
                            p_buf_data_end, &opfailcode, &ixfailnum,
                            BLOCK2_UPTBL, 0);
    }

    // handle rc
    switch (rc) {
    default: /* bang! */
        sc_errf(data->s, "Error upgrading record "
                         "rc %d opfailcode %d genid 0x%llx\n",
                rc, opfailcode, genid);
        return -2;

    case RC_INTERNAL_RETRY: /* deadlock */
                            /*
                             ** if deadlk occurs, abort txn and skip this record.
                             ** leaving a single record unconverted does little harm.
                             ** also the record has higher chances to be updated
                             ** by the other txns which are holding resources this txn requires.
                             */
        ++data->nrecskip;
        logbytes = bdb_tran_logbytes(data->trans);
        increment_sc_logbytes(logbytes);
        trans_abort(&data->iq, data->trans);
        data->trans = NULL;
        break;

    case 0: /* all good */
        ++data->nrecs;
        rc = trans_commit_seqnum(&data->iq, data->trans, &ss);
        increment_sc_logbytes(data->iq.txnsize);
        data->trans = NULL;

        if (rc) {
            sc_errf(data->s, "%s: trans_commit failed with "
                             "rcode %d",
                    __func__, rc);
            /* If commit fail we are failing the whole operation */
            return -2;
        }

        // txn contains enough records, wait for replicants
        if ((data->nrecs % data->num_records_per_trans) == 0) {
            if ((rc = trans_wait_for_seqnum(&data->iq, gbl_myhostname, &ss)) !=
                0) {
                sc_errf(data->s, "%s: error waiting for "
                                 "replication rcode %d\n",
                        __func__, rc);
            } else if (gbl_sc_inco_chk) {
                int num;
                if ((num = bdb_get_num_notcoherent(thedb->bdb_env)) != 0) {
                    if (num > inco_delay) { /* only goes up, or resets to 0 */
                        inco_delay = num;
                        sc_printf(data->s, "%d incoherent nodes - "
                                           "throttle sc %dms\n",
                                  num, inco_delay * mult);
                    }
                } else if (inco_delay != 0) {
                    inco_delay = 0;
                    sc_printf(data->s, "0 incoherent nodes - "
                                       "pedal to the metal\n");
                }
            } else {
                inco_delay = 0;
            }
        }

        if (inco_delay) poll(0, 0, inco_delay * mult);

        /* if we're in commitdelay mode, magnify the delay by 5 here */
        if ((commitdelay = bdb_attr_get(data->from->dbenv->bdb_attr,
                                        BDB_ATTR_COMMITDELAY)) != 0)
            poll(NULL, 0, commitdelay * 5);

        /* if sanc list is not ok, snooze for 100 ms */
        if (!net_sanctioned_list_ok(data->from->dbenv->handle_sibling))
            poll(NULL, 0, 100);

        /* snooze for a bit if writes have been coming in */
        if (gbl_sc_last_writer_time >= comdb2_time_epoch() - 5)
            usleep(gbl_sc_usleep);
        break;
    } // end of rc check

    ATOMIC_ADD64(data->from->sc_nrecs, 1);

    data->sc_genids[data->stripe] = genid;
    now = comdb2_time_epoch();

    if (copy_sc_report_freq > 0 &&
        now >= data->lasttime + copy_sc_report_freq) {
        /* report progress to interested parties */
        long long diff_nrecs = data->nrecs - data->prev_nrecs;
        data->lasttime = now;
        data->prev_nrecs = data->nrecs;

        /* print thread specific stats */
        sc_printf(
            data->s,
            "TABLE UPGRADE progress stripe %d changed genids %u progress %lld"
            " recs +%lld conversions/sec %lld\n",
            data->stripe, data->n_genids_changed, data->nrecs, diff_nrecs,
            diff_nrecs / copy_sc_report_freq);

        /* now do global sc data */
        int res = print_aggregate_sc_stat(data, now, copy_sc_report_freq);
        /* check headroom only if this thread printed the global stats */
        if (res && check_sc_headroom(data->s, data->from, data->to)) {
            if (data->s->force) {
                sc_printf(data->s, "Proceeding despite low disk headroom\n");
            } else {
                return -1;
            }
        }
    }

    if (data->s->kind == SC_FULLUPRECS)
        return 1;
    else if (recver == data->to->schema_version)
        return 0;
    else if (data->nrecs >= data->s->partialuprecs)
        return 0;
    else
        return 1;
}

static void *upgrade_records_thd(void *vdata)
{
    comdb2_name_thread(__func__);
    int rc;

    struct convert_record_data *data = (struct convert_record_data *)vdata;
    struct thr_handle *thr_self = thrman_self();
    enum thrtype oldtype = THRTYPE_UNKNOWN;

    // transfer thread type
    if (data->isThread) thread_started("upgrade records");

    if (thr_self) {
        oldtype = thrman_get_type(thr_self);
        thrman_change_type(thr_self, THRTYPE_SCHEMACHANGE);
    } else {
        thr_self = thrman_register(THRTYPE_SCHEMACHANGE);
    }

    if (data->isThread)
        backend_thread_event(thedb, COMDB2_THR_EVENT_START_RDWR);

    // initialize convert_record_data
    data->outrc = -1;
    data->lasttime = comdb2_time_epoch();
    data->num_records_per_trans = gbl_num_record_converts;
    data->num_records_per_trans = gbl_num_record_converts;

    data->dta_buf = malloc(data->from->lrl);
    if (!data->dta_buf) {
        sc_errf(data->s, "%s: ran out of memory trying to "
                         "malloc dta_buf: %d\n",
                __func__, data->from->lrl);
        data->outrc = -1;
        goto cleanup;
    }

    while ((rc = upgrade_records(data)) > 0) {
        if (get_stopsc(__func__, __LINE__)) {
            if (data->isThread)
                backend_thread_event(thedb, COMDB2_THR_EVENT_DONE_RDWR);
            return NULL;
        }
    }

    if (rc == -2) {
        data->outrc = -1;
    } else {
        data->outrc = rc;
    }

cleanup:
    if (data->outrc == 0) {
        sc_printf(data->s,
                  "successfully upgraded %lld records. skipped %lld records.\n",
                  data->nrecs, data->nrecskip);
    } else {
        if (gbl_sc_abort || data->from->sc_abort ||
            (data->s->iq && data->s->iq->sc_should_abort)) {
            sc_errf(data->s, "conversion aborted after %lld records upgraded "
                             "and %lld records skipped, "
                             "while working on stripe %d\n",
                    data->nrecs, data->nrecskip, data->stripe);
        }
        data->s->sc_thd_failed = data->stripe + 1;
    }

    convert_record_data_cleanup(data);

    if (data->isThread) backend_thread_event(thedb, COMDB2_THR_EVENT_DONE_RDWR);

    if (oldtype != THRTYPE_UNKNOWN) thrman_change_type(thr_self, oldtype);
    return NULL;
}

int upgrade_all_records(struct dbtable *db, unsigned long long *sc_genids,
                        struct schema_change_type *s)
{
    int idx;
    int rc = 0;
    int outrc = 0;

    struct convert_record_data data = {0};

    s->sc_thd_failed = 0;

    data.from = db;
    data.to = db;
    data.sc_genids = sc_genids;
    data.s = s;
    data.scanmode = s->scanmode;

    // set up internal block request
    init_fake_ireq(thedb, &data.iq);
    data.iq.usedb = db;
    data.iq.opcode = OP_UPGRADE;
    data.iq.debug = 0;

    if (data.scanmode != SCAN_PARALLEL) {
        if (s->start_genid != 0) {
            data.stripe = get_dtafile_from_genid(s->start_genid);
            data.sc_genids[data.stripe] = s->start_genid;
        }
        upgrade_records_thd(&data);
        outrc = data.outrc;
    } else {
        struct convert_record_data thread_data[gbl_dtastripe];
        pthread_attr_t attr;
        void *ret;

        data.isThread = 1;

        // init pthread attributes
        Pthread_attr_init(&attr);
        Pthread_attr_setstacksize(&attr, DEFAULT_THD_STACKSZ);
        Pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

        for (idx = 0; idx != gbl_dtastripe; ++idx) {
            thread_data[idx] = data;
            thread_data[idx].stripe = idx;
            sc_printf(thread_data[idx].s,
                      "[%s] starting thread for stripe: %d\n", db->tablename,
                      thread_data[idx].stripe);

            Pthread_create(&thread_data[idx].tid, &attr,
                                upgrade_records_thd, &thread_data[idx]);
        }

        if (outrc == -1) {
            for (; idx >= 0; --idx)
                pthread_cancel(thread_data[idx].tid);
        } else {
            for (idx = 0; idx != gbl_dtastripe; ++idx) {
                rc = pthread_join(thread_data[idx].tid, &ret);
                /* if join failed */
                if (rc) {
                    sc_errf(thread_data[idx].s,
                            "joining thread failed for stripe: %d with return code: %d\n",
                            thread_data[idx].stripe, rc);
                    outrc = -1;
                    continue;
                }

                /* if thread's conversions failed return error code */
                if (thread_data[idx].outrc != 0) outrc = thread_data[idx].outrc;
            }
        }

        Pthread_attr_destroy(&attr);
    }

    convert_record_data_cleanup(&data);

    return outrc;
}

struct blob_recs {
    unsigned long long genid;
    LISTC_T(bdb_osql_log_rec_t) recs; /* list of undo records */
};

static void clear_recs_list(void *recs)
{
    listc_t *l = (listc_t *)recs;
    bdb_osql_log_rec_t *rec = NULL, *tmp = NULL;
    LISTC_FOR_EACH_SAFE(l, rec, tmp, lnk)
    {
        listc_rfl(l, rec);
        if (rec->comprec) {
            if (rec->comprec->table)
                free(rec->comprec->table);
            free(rec->comprec);
        }
        if (rec->table)
            free(rec->table);
        free(rec);
    }
}

static int free_blob_recs(void *obj, void *arg)
{
    struct blob_recs *pbrecs = obj;
    clear_recs_list(&pbrecs->recs);
    free(pbrecs);
    return 0;
}

static void clear_blob_hash(hash_t *h)
{
    if (!h)
        return;
    hash_for(h, free_blob_recs, NULL);
    hash_clear(h);
}

static int free_redo_genid_recs(void *obj, void *arg)
{
    free(obj);
    return 0;
}

static void clear_redo_genid_hash(hash_t *h)
{
    if (!h)
        return;
    hash_for(h, free_redo_genid_recs, NULL);
    hash_clear(h);
}

static int unpack_blob_record(struct convert_record_data *data, void *blb_buf,
                              int dtalen, blob_status_t *blb, int blbix)
{
    int rc = 0;
    size_t sz;
    void *unpackbuf = NULL;
    if ((rc = bdb_unpack(data->from->handle, blb_buf, dtalen, NULL, 0,
                         &data->odh, &unpackbuf)) != 0) {
        logmsg(LOGMSG_ERROR, "%s:%d error unpacking buf rc=%d\n", __func__,
               __LINE__, rc);
        return rc;
    }

    if (blb->blobptrs[blbix]) {
        logmsg(LOGMSG_ERROR,
               "%s:%d attempted to overwrite blob data that is currently in "
               "use, which would leak memory and shouldn't ever happen.\n",
               __func__, __LINE__);
        if (unpackbuf)
            free(unpackbuf);
        return -1;
    }

    blb->bloblens[blbix] = data->odh.length;
    blb->bloboffs[blbix] = 0;
    blb->blobptrs[blbix] = NULL;

    if (unpackbuf) {
        blb->blobptrs[blbix] = data->odh.recptr;
    } else {
        sz = data->odh.length;
        if (sz == 0)
            sz = 1;
        blb->blobptrs[blbix] = malloc(sz);
        if (!blb->blobptrs[blbix]) {
            logmsg(LOGMSG_ERROR, "%s:%d failed to malloc blob buffer\n",
                   __func__, __LINE__);
            return -1;
        }
        memcpy(blb->blobptrs[blbix], data->odh.recptr, data->odh.length);
    }

#ifdef LOGICAL_LIVESC_DEBUG
    logmsg(LOGMSG_DEBUG, "blob[%d], length %zu\n", blbix, blb->bloblens[blbix]);
    if (blb->blobptrs[blbix])
        fsnapf(stdout, blb->blobptrs[blbix], blb->bloblens[blbix]);
    else
        logmsg(LOGMSG_DEBUG, "NULL BLOB\n");
#endif

    return 0;
}

static int reconstruct_blob_records(struct convert_record_data *data,
                                    DB_LOGC *logc, struct blob_recs *pbrecs,
                                    DBT *logdta)
{
    bdb_state_type *bdb_state = thedb->bdb_env;

    u_int32_t rectype;
    int rc, dtalen, page, index, ixlen;
    unsigned long long genid, oldgenid;
    int prevlen = 0, updlen = 0;
    llog_undo_del_dta_args *del_dta = NULL;
    llog_undo_del_dta_lk_args *del_dta_lk = NULL;
    llog_undo_add_dta_args *add_dta = NULL;
    llog_undo_add_dta_lk_args *add_dta_lk = NULL;
    llog_undo_upd_dta_args *upd_dta = NULL;
    llog_undo_upd_dta_lk_args *upd_dta_lk = NULL;
    void *free_ptr = NULL;

    bdb_osql_log_rec_t *rec = NULL;
    int blbix = 0;

    if (!data->blb_buf) {
        data->blb_buf = malloc(MAXBLOBLENGTH + ODH_SIZE);
        if (!data->blb_buf) {
            logmsg(LOGMSG_ERROR, "%s:%d failed to malloc blob buffer\n",
                   __func__, __LINE__);
            return -1;
        }
    }

    LISTC_FOR_EACH(&pbrecs->recs, rec, lnk)
    {
        if ((rc = logc->get(logc, &rec->lsn, logdta, DB_SET)) != 0) {
            logmsg(LOGMSG_ERROR, "%s:%d rc %d retrieving lsn %d:%d\n", __func__,
                   __LINE__, rc, rec->lsn.file, rec->lsn.offset);
            goto error;
        }
        LOGCOPY_32(&rectype, logdta->data);
        normalize_rectype(&rectype);
        assert(rectype == rec->type);

        assert(rec->dtafile >= 1);
        blbix = rec->dtafile - 1;

#ifdef LOGICAL_LIVESC_DEBUG
        logmsg(LOGMSG_DEBUG,
               "%s: [%s] redo lsn[%u:%u] type[%d] dtafile %d, "
               "datastripe %d, genid %llx\n",
               __func__, data->s->tablename, rec->lsn.file, rec->lsn.offset,
               rec->type, rec->dtafile, rec->dtastripe, rec->genid);
#endif

        switch (rec->type) {
        case DB_llog_undo_add_dta:
        case DB_llog_undo_add_dta_lk:
            if (rec->type == DB_llog_undo_add_dta_lk) {
                if ((rc = llog_undo_add_dta_lk_read(
                         bdb_state->dbenv, logdta->data, &add_dta_lk)) != 0) {
                    logmsg(LOGMSG_ERROR, "%s:%d error unpacking rc=%d\n",
                           __func__, __LINE__, rc);
                    goto error;
                }
                genid = add_dta_lk->genid;
                free_ptr = add_dta_lk;
            } else {
                if ((rc = llog_undo_add_dta_read(bdb_state->dbenv, logdta->data,
                                                 &add_dta)) != 0) {
                    logmsg(LOGMSG_ERROR, "%s:%d error unpacking rc=%d\n",
                           __func__, __LINE__, rc);
                    goto error;
                }
                genid = add_dta->genid;
                free_ptr = add_dta;
            }

            /* Reconstruct the add. */
            if ((rc = bdb_reconstruct_add(
                     bdb_state, &rec->lsn, NULL, sizeof(genid_t), data->blb_buf,
                     MAXBLOBLENGTH + ODH_SIZE, &dtalen, &ixlen)) != 0) {
                logmsg(LOGMSG_ERROR, "%s:%d failed to reconstruct add rc=%d\n",
                       __func__, __LINE__, rc);
                goto error;
            }

            if ((rc = unpack_blob_record(data, data->blb_buf, dtalen,
                                         &data->blb, blbix)) != 0) {
                logmsg(LOGMSG_ERROR, "%s:%d error unpacking buf rc=%d\n",
                       __func__, __LINE__, rc);
                goto error;
            }
            break;
        case DB_llog_undo_del_dta:
        case DB_llog_undo_del_dta_lk:
            if (rec->type == DB_llog_undo_del_dta_lk) {
                if ((rc = llog_undo_del_dta_lk_read(
                         bdb_state->dbenv, logdta->data, &del_dta_lk)) != 0) {
                    logmsg(LOGMSG_ERROR, "%s:%d error unpacking rc=%d\n",
                           __func__, __LINE__, rc);
                    goto error;
                }
                genid = del_dta_lk->genid;
                dtalen = del_dta_lk->dtalen;
                free_ptr = del_dta_lk;
            } else {
                if ((rc = llog_undo_del_dta_read(bdb_state->dbenv, logdta->data,
                                                 &del_dta)) != 0) {
                    logmsg(LOGMSG_ERROR, "%s:%d error unpacking rc=%d\n",
                           __func__, __LINE__, rc);
                    goto error;
                }
                genid = del_dta->genid;
                dtalen = del_dta->dtalen;
                free_ptr = del_dta;
            }

            /* Reconstruct the delete. */
            if ((rc = bdb_reconstruct_delete(
                     bdb_state, &rec->lsn, &page, &index, NULL, sizeof(genid_t),
                     data->blb_buf, dtalen, &dtalen)) != 0) {
                logmsg(LOGMSG_ERROR,
                       "%s:%d failed to reconstruct delete rc=%d\n", __func__,
                       __LINE__, rc);
                goto error;
            }
            if ((rc = unpack_blob_record(data, data->blb_buf, dtalen,
                                         &data->blbcopy, blbix)) != 0) {
                logmsg(LOGMSG_ERROR, "%s:%d error unpacking buf rc=%d\n",
                       __func__, __LINE__, rc);
                goto error;
            }
            break;
        case DB_llog_undo_upd_dta:
        case DB_llog_undo_upd_dta_lk:
            if (!data->old_blb_buf) {
                data->old_blb_buf = malloc(MAXBLOBLENGTH + ODH_SIZE);
                if (!data->old_blb_buf) {
                    logmsg(LOGMSG_ERROR, "%s:%d failed to malloc blob buffer\n",
                           __func__, __LINE__);
                    rc = -1;
                    goto error;
                }
            }

            if (rec->type == DB_llog_undo_upd_dta_lk) {
                if ((rc = llog_undo_upd_dta_lk_read(
                         bdb_state->dbenv, logdta->data, &upd_dta_lk)) != 0) {
                    logmsg(LOGMSG_ERROR, "%s:%d error unpacking rc=%d\n",
                           __func__, __LINE__, rc);
                    goto error;
                }
                genid = upd_dta_lk->newgenid;
                oldgenid = upd_dta_lk->oldgenid;
                dtalen = upd_dta_lk->old_dta_len;
            } else {
                if ((rc = llog_undo_upd_dta_read(bdb_state->dbenv, logdta->data,
                                                 &upd_dta)) != 0) {
                    logmsg(LOGMSG_ERROR, "%s:%d error unpacking rc=%d\n",
                           __func__, __LINE__, rc);
                    goto error;
                }
                genid = upd_dta->newgenid;
                oldgenid = upd_dta->oldgenid;
                dtalen = upd_dta->old_dta_len;
            }

            if (0 ==
                bdb_inplace_cmp_genids(data->from->handle, oldgenid, genid)) {
                rc = bdb_reconstruct_inplace_update(
                    bdb_state, &rec->lsn, data->old_blb_buf, &prevlen,
                    data->blb_buf, &updlen, NULL, NULL, NULL);
            } else {
                prevlen = updlen = MAXBLOBLENGTH + ODH_SIZE;
                rc = bdb_reconstruct_update(bdb_state, &rec->lsn, &page, &index,
                                            NULL, NULL, data->old_blb_buf,
                                            &prevlen, NULL, NULL, data->blb_buf,
                                            &updlen);
            }
            if (rc != 0) {
                logmsg(LOGMSG_ERROR,
                       "%s:%d failed to reconstruct update rc=%d\n", __func__,
                       __LINE__, rc);
                goto error;
            }
            if (dtalen > 0) {
                if ((rc = unpack_blob_record(data, data->old_blb_buf, prevlen,
                                             &data->blbcopy, blbix)) != 0) {
                    logmsg(LOGMSG_ERROR, "%s:%d error unpacking buf rc=%d\n",
                           __func__, __LINE__, rc);
                    goto error;
                }
                if ((rc = unpack_blob_record(data, data->blb_buf, updlen,
                                             &data->blb, blbix)) != 0) {
                    logmsg(LOGMSG_ERROR, "%s:%d error unpacking buf rc=%d\n",
                           __func__, __LINE__, rc);
                    goto error;
                }
            } else if (0 == bdb_inplace_cmp_genids(data->from->handle, oldgenid,
                                                   genid)) {
                if ((rc = unpack_blob_record(data, data->blb_buf, updlen,
                                             &data->blb, blbix)) != 0) {
                    logmsg(LOGMSG_ERROR, "%s:%d error unpacking buf rc=%d\n",
                           __func__, __LINE__, rc);
                    goto error;
                }
                /* no old blob since old_dta_len == 0 */
            } else {
                /* old_blb_buf has the new blob */
                if ((rc = unpack_blob_record(data, data->old_blb_buf, prevlen,
                                             &data->blb, blbix)) != 0) {
                    logmsg(LOGMSG_ERROR, "%s:%d error unpacking buf rc=%d\n",
                           __func__, __LINE__, rc);
                    goto error;
                }
                /* no old blob since old_dta_len == 0 */
            }
            break;
        default:
            logmsg(LOGMSG_FATAL, "%s: unhandle logical record type %d\n",
                   __func__, rec->type);
            abort();
        }

        if (free_ptr) {
            free(free_ptr);
            free_ptr = NULL;
        }
    }
    data->blb.numcblobs = data->from->numblobs;
    data->blbcopy.numcblobs = data->from->numblobs;
    return 0;

error:
    if (free_ptr)
        free(free_ptr);
    return rc;
}

static int unpack_and_upgrade_ondisk_record(struct convert_record_data *data,
                                            void *dta, int *dtalen,
                                            void *unpack, struct odh *odh)
{
    int rc = 0;
    if ((rc = bdb_unpack(data->from->handle, dta, *dtalen, unpack,
                         data->from->lrl + ODH_SIZE, odh, NULL)) != 0) {
        logmsg(LOGMSG_ERROR, "%s:%d error unpacking buf rc=%d\n", __func__,
               __LINE__, rc);
        return rc;
    }

    if ((rc = vtag_to_ondisk_vermap(data->from, odh->recptr, dtalen,
                                    odh->csc2vers)) <= 0) {
        logmsg(LOGMSG_ERROR, "%s:%d vtag-to-ondisk error rc=%d\n", __func__,
               __LINE__, rc);
        return rc;
    }

    return 0;
}

static void populate_redo_genids(struct convert_record_data *data)
{
    llmeta_sc_redo_data *redo_genids;
    int num = 0, bdberr = 0, rc = 0;

    if ((rc = bdb_llmeta_get_all_sc_redo_genids(NULL, data->s->tablename, &redo_genids, &num, &bdberr)) == 0) {
        for (int i = 0; i < num; i++) {
            struct redo_genid_lsns *r = calloc(sizeof(struct redo_genid_lsns), 1), *before_lsn;
            r->genid = redo_genids[i].genid;
            r->lsn.file = redo_genids[i].file;
            r->lsn.offset = redo_genids[i].offset;
#ifdef LOGICAL_LIVESC_DEBUG
            logmsg(LOGMSG_DEBUG, "%s adding genid %llx [%u][%u] to redo_lsns list\n", __func__, r->genid, r->lsn.file,
                   r->lsn.offset);
#endif
            hash_add(data->redo_genids, r);
            LISTC_FOR_EACH(&data->redo_lsns, before_lsn, linkv)
            {
                if (log_compare(&r->lsn, &before_lsn->lsn) <= 0) {
                    listc_add_before(&data->redo_lsns, r, before_lsn);
                    break;
                }
            }
            if (before_lsn == NULL) {
                listc_abl(&data->redo_lsns, r);
            }
        }
        free(redo_genids);
    }
}

static void set_redo_genid(struct convert_record_data *data, unsigned long long genid, const DB_LSN *lsn)
{

    int bdberr = 0;
    int rc = bdb_newsc_set_redo_genid(data->trans, data->s->tablename, genid, lsn->file, lsn->offset, &bdberr);
    if (rc != 0) {
        logmsg(LOGMSG_ERROR, "%"PRIxPTR": Error setting redo genid, %d bdberr=%d\n", (uintptr_t) pthread_self(), rc, bdberr);
    }

    struct redo_genid_lsns *r, *fnd;

    if ((fnd = hash_find(data->redo_genids, &genid)) != NULL) {
        logmsg(LOGMSG_ERROR, "%s: adding genid %llx to redo-hash twice?\n", __func__, genid);
        abort();
    }

    r = calloc(sizeof(struct redo_genid_lsns), 1);
    r->genid = genid;
    r->lsn.file = lsn->file;
    r->lsn.offset = lsn->offset;
    listc_abl(&data->redo_lsns, r);
    hash_add(data->redo_genids, r);
}

static int get_redo_genid(struct convert_record_data *data, unsigned long long genid, DB_LSN *lsn)
{
    int rc = -1;
    struct redo_genid_lsns *r;

    if ((r = hash_find(data->redo_genids, &genid)) != NULL) {
        int rc2, bdberr;
        if ((rc2 = bdb_newsc_del_redo_genid(data->trans, data->s->tablename, genid, &bdberr)) != 0) {
            logmsg(LOGMSG_ERROR, "%"PRIxPTR": %s del_redo_genid returns %d bdberr=%d\n", (uintptr_t)pthread_self(), __func__,
                   rc2, bdberr);
        }
        hash_del(data->redo_genids, r);
        listc_rfl(&data->redo_lsns, r);
        (*lsn) = r->lsn;
        free(r);
        rc = 0;
    }

    return rc;
}

static int get_min_redo_lsn(struct convert_record_data *data, unsigned long long *genid, DB_LSN *lsn)
{
    if (listc_size(&data->redo_lsns) == 0) {
        return -1;
    }
    struct redo_genid_lsns *l = LISTC_TOP(&data->redo_lsns);
    (*lsn) = l->lsn;
    (*genid) = l->genid;
    return 0;
}

static int live_sc_redo_add(struct convert_record_data *data, DB_LOGC *logc,
                            bdb_osql_log_rec_t *rec, DBT *logdta)
{
    bdb_state_type *bdb_state = thedb->bdb_env;
    struct blob_recs brecs = {0};
    struct blob_recs *pbrecs = NULL;

    u_int32_t rectype;
    int rc = 0, dtalen, ixlen, opfailcode = 0, ixfailnum = 0;
    unsigned long long genid, ngenid, check_genid;
    llog_undo_add_dta_args *add_dta = NULL;
    llog_undo_add_dta_lk_args *add_dta_lk = NULL;

    dtalen = data->from->lrl + ODH_SIZE;
    brecs.genid = rec->genid;
    pbrecs = hash_find(data->blob_hash, &brecs);
    if (pbrecs) {
        rc = reconstruct_blob_records(data, logc, pbrecs, logdta);
        bzero(data->wrblb, sizeof(data->wrblb));
        blob_status_to_blob_buffer(&data->blb, data->wrblb);
    }

    if ((rc = logc->get(logc, &rec->lsn, logdta, DB_SET)) != 0) {
        logmsg(LOGMSG_ERROR, "%s:%d rc %d retrieving lsn %d:%d\n", __func__,
               __LINE__, rc, rec->lsn.file, rec->lsn.offset);
        goto done;
    }
    LOGCOPY_32(&rectype, logdta->data);
    normalize_rectype(&rectype);
    assert(rectype == rec->type);

    if (rec->type == DB_llog_undo_add_dta_lk) {
        if ((rc = llog_undo_add_dta_lk_read(bdb_state->dbenv, logdta->data,
                                            &add_dta_lk)) != 0) {
            logmsg(LOGMSG_ERROR, "%s:%d error unpacking rc=%d\n", __func__,
                   __LINE__, rc);
            goto done;
        }
        genid = add_dta_lk->genid;
    } else {
        if ((rc = llog_undo_add_dta_read(bdb_state->dbenv, logdta->data,
                                         &add_dta)) != 0) {
            logmsg(LOGMSG_ERROR, "%s:%d error unpacking rc=%d\n", __func__,
                   __LINE__, rc);
            goto done;
        }
        genid = add_dta->genid;
    }

    /* Reconstruct the add. */
    if ((rc = bdb_reconstruct_add(bdb_state, &rec->lsn, NULL, sizeof(genid_t),
                                  data->dta_buf, dtalen, &dtalen, &ixlen)) !=
        0) {
        logmsg(LOGMSG_ERROR, "%s:%d failed to reconstruct add rc=%d\n",
               __func__, __LINE__, rc);
        goto done;
    }

    if ((rc = unpack_and_upgrade_ondisk_record(data, data->dta_buf, &dtalen,
                                               data->unpack_dta_buf,
                                               &data->odh)) != 0) {
        logmsg(LOGMSG_ERROR,
               "%s:%d failed to unpack and upgrade ondisk record rc=%d\n",
               __func__, __LINE__, rc);
        goto done;
    }

#ifdef LOGICAL_LIVESC_DEBUG
    logmsg(LOGMSG_DEBUG, "dtalen %d\n", dtalen);
    fsnapf(stdout, data->odh.recptr, dtalen);
#endif

    data->iq.usedb = data->to;

    int usellmeta = 0;
    if (!data->to->plan) {
        usellmeta = 1; /* new dta does not have old genids */
    } else if (data->to->plan->dta_plan) {
        usellmeta = 0; /* the genid is in new dta */
    } else {
        usellmeta = 1; /* dta is not being built */
    }

    int dta_needs_conversion = 1;
    if (usellmeta && data->s->rebuild_index)
        dta_needs_conversion = 0;

    unsigned long long dirty_keys = -1ULL;

    rc = prepare_and_verify_newdb_record(data, data->odh.recptr, dtalen,
                                         &dirty_keys, 0);
    if (rc) {
        logmsg(LOGMSG_ERROR,
               "%s:%d failed to prepare and verify new record rc=%d\n",
               __func__, __LINE__, rc);
        goto done;
    }

    check_genid = bdb_normalise_genid(data->to->handle, genid);
    if (data->s->use_new_genids) {
        assert(!gbl_use_plan);
        ngenid = get_genid(thedb->bdb_env, get_dtafile_from_genid(check_genid));
    } else {
        ngenid = check_genid;
    }
    if (ngenid != genid)
        data->n_genids_changed++;

    int addflags = RECFLAGS_NO_TRIGGERS | RECFLAGS_NO_CONSTRAINTS |
                   RECFLAGS_NEW_SCHEMA | RECFLAGS_ADD_FROM_SC_LOGICAL |
                   RECFLAGS_KEEP_GENID;

    if (data->to->plan && gbl_use_plan)
        addflags |= RECFLAGS_NO_BLOBS;

    char *tagname = ".NEW..ONDISK";
    uint8_t *p_tagname_buf = (uint8_t *)tagname;
    uint8_t *p_tagname_buf_end = p_tagname_buf + 12;
    uint8_t *p_buf_data = data->rec->recbuf;
    uint8_t *p_buf_data_end = p_buf_data + data->rec->bufsize;

    if (!dta_needs_conversion) {
        p_buf_data = data->odh.recptr;
        p_buf_data_end = p_buf_data + dtalen;
    }

    assert(data->trans != NULL);
    if (data->s->schema_change != SC_CONSTRAINT_CHANGE) {
        int nrrn = 2;
        rc = add_record(
            &data->iq, data->trans, p_tagname_buf, p_tagname_buf_end,
            p_buf_data, p_buf_data_end, NULL, data->wrblb, MAXBLOBS,
            &opfailcode, &ixfailnum, &nrrn, &ngenid,
            (gbl_partial_indexes && data->to->ix_partial) ? dirty_keys : -1ULL,
            BLOCK2_ADDKL, /* opcode */
            0,            /* blkpos */
            addflags, 0);
        if (rc == ERR_VERIFY) {
            /* not an error if the row is already in the new btree */
            rc = 0;
            goto done;
        }
        if (rc) {
            if (data->s->iq) {
                if (rc == IX_DUP) {
                    DB_LSN current;
                    rc = del_new_record(&data->iq, data->trans, ngenid, -1ULL, data->odh.recptr, data->wrblb, 0);
                    if (rc && rc != ERR_VERIFY) {
                        logmsg(LOGMSG_FATAL, "Debug assert - unexpected condition\n");
                        abort();
                    }
                    bdb_get_commit_genid(thedb->bdb_env, &current);
#ifdef LOGICAL_LIVESC_DEBUG
                    logmsg(LOGMSG_DEBUG, "%u: setting newsc genid %llx lsn=[%u][%u] on addindex conflict\n",
                           (unsigned int)pthread_self(), genid, current.file, current.offset);
#endif
                    set_redo_genid(data, ngenid, &current);
                    rc = 0;
                    goto done;
                } else
                    sc_client_error(data->s, "unable to add record rc = %d", rc);
            }
            logmsg(LOGMSG_ERROR, "%s:%d failed to add new record rc=%d %s\n",
                   __func__, __LINE__, rc,
                   errstat_get_str(&(data->iq.errstat)));
            goto done;
        }
    }

    if (!is_dta_being_rebuilt(data->to->plan)) {
        int bdberr;
        rc = bdb_set_high_genid(data->trans, data->to->tablename, genid,
                                &bdberr);
        if (rc != 0) {
            if (bdberr == BDBERR_DEADLOCK)
                rc = RC_INTERNAL_RETRY;
            else {
                logmsg(LOGMSG_ERROR, "%s:%d failed to set high genid rc=%d\n",
                       __func__, __LINE__, rc);
                rc = ERR_INTERNAL;
            }
        }
    }

done:
#ifdef LOGICAL_LIVESC_DEBUG
    logmsg(LOGMSG_DEBUG,
           "%s: [%s] redo lsn[%u:%u] type[ADD_DTA] rec->dtafile %d, "
           "rec->datastripe %d, rec->genid %llx\n",
           __func__, data->s->tablename, rec->lsn.file, rec->lsn.offset,
           rec->dtafile, rec->dtastripe, rec->genid);
#endif

    free_blob_status_data(&data->blbcopy);
    bzero(data->freeblb, sizeof(data->freeblb));
    free_blob_status_data(&data->blb);
    bzero(data->wrblb, sizeof(data->wrblb));

    if (add_dta)
        free(add_dta);
    if (add_dta_lk)
        free(add_dta_lk);

    return rc;
}

static int live_sc_redo_delete(struct convert_record_data *data, DB_LOGC *logc,
                               bdb_osql_log_rec_t *rec, DBT *logdta)
{
    bdb_state_type *bdb_state = thedb->bdb_env;
    struct blob_recs brecs = {0};
    struct blob_recs *pbrecs = NULL;

    u_int32_t rectype;
    int rc, dtalen, page, index;
    unsigned long long genid;
    llog_undo_del_dta_args *del_dta = NULL;
    llog_undo_del_dta_lk_args *del_dta_lk = NULL;

    brecs.genid = rec->genid;
    pbrecs = hash_find(data->blob_hash, &brecs);
    if (pbrecs && data->to->ix_blob) {
        rc = reconstruct_blob_records(data, logc, pbrecs, logdta);
        if (rc) {
            logmsg(LOGMSG_ERROR,
                   "%s:%d failed to reconstruct blob records rc %d\n", __func__,
                   __LINE__, rc);
            goto done;
        }
        bzero(data->freeblb, sizeof(data->freeblb));
        blob_status_to_blob_buffer(&data->blbcopy, data->freeblb);
    }

    if ((rc = logc->get(logc, &rec->lsn, logdta, DB_SET)) != 0) {
        logmsg(LOGMSG_ERROR, "%s:%d rc %d retrieving lsn %d:%d\n", __func__,
               __LINE__, rc, rec->lsn.file, rec->lsn.offset);
        goto done;
    }
    LOGCOPY_32(&rectype, logdta->data);
    normalize_rectype(&rectype);
    assert(rectype == rec->type);
    if (rec->type == DB_llog_undo_del_dta_lk) {
        if ((rc = llog_undo_del_dta_lk_read(bdb_state->dbenv, logdta->data,
                                            &del_dta_lk)) != 0) {
            logmsg(LOGMSG_ERROR, "%s:%d error unpacking rc=%d\n", __func__,
                   __LINE__, rc);
            goto done;
        }
        genid = del_dta_lk->genid;
        dtalen = del_dta_lk->dtalen;
    } else {
        if ((rc = llog_undo_del_dta_read(bdb_state->dbenv, logdta->data,
                                         &del_dta)) != 0) {
            logmsg(LOGMSG_ERROR, "%s:%d error unpacking rc=%d\n", __func__,
                   __LINE__, rc);
            goto done;
        }
        genid = del_dta->genid;
        dtalen = del_dta->dtalen;
    }

    /* Reconstruct the delete. */
    if ((rc = bdb_reconstruct_delete(bdb_state, &rec->lsn, &page, &index, NULL,
                                     sizeof(genid_t), data->dta_buf, dtalen,
                                     &dtalen)) != 0) {
        logmsg(LOGMSG_ERROR, "%s:%d failed to reconstruct delete rc=%d\n",
               __func__, __LINE__, rc);
        goto done;
    }

    if ((rc = unpack_and_upgrade_ondisk_record(data, data->dta_buf, &dtalen,
                                               data->unpack_dta_buf,
                                               &data->odh)) != 0) {
        logmsg(LOGMSG_ERROR,
               "%s:%d failed to unpack and upgrade ondisk record rc=%d\n",
               __func__, __LINE__, rc);
        goto done;
    }

#ifdef LOGICAL_LIVESC_DEBUG
    logmsg(LOGMSG_DEBUG, "dtalen %d\n", dtalen);
    fsnapf(stdout, data->odh.recptr, dtalen);
#endif

    data->iq.usedb = data->to;
    DB_LSN redolsn;
    if (get_redo_genid(data, genid, &redolsn) == 0) {
#ifdef LOGICAL_LIVESC_DEBUG
        logmsg(LOGMSG_DEBUG, "%s found redo-genid %llu with redolsn[%d][%d], mylsn is [%d][%d]\n", __func__, genid,
               redolsn.file, redolsn.offset, rec->lsn.file, rec->lsn.offset);
#endif

        // FAIL SC HERE
        if (log_compare(&redolsn, &rec->lsn) < 0) {
            logmsg(LOGMSG_FATAL, "%s:%d genid %llx lsn %u:%u less than redo-lsn %u:%u\n", __func__, __LINE__, genid,
                   redolsn.file, redolsn.offset, rec->lsn.file, rec->lsn.offset);
            abort();
        }
        //#endif
    } else {
        rc = del_new_record(&data->iq, data->trans, genid, -1ULL, data->odh.recptr, data->freeblb, 0);
    }
    if (rc == ERR_VERIFY) {
        /* not an error if we dont find it */
        rc = 0;
    }
done:
#ifdef LOGICAL_LIVESC_DEBUG
    logmsg(LOGMSG_DEBUG,
           "%s: [%s] redo lsn[%u:%u] type[DEL_DTA] rec->dtafile %d, "
           "rec->datastripe %d, rec->genid %llx\n",
           __func__, data->s->tablename, rec->lsn.file, rec->lsn.offset,
           rec->dtafile, rec->dtastripe, rec->genid);
#endif

    free_blob_status_data(&data->blbcopy);
    bzero(data->freeblb, sizeof(data->freeblb));
    free_blob_status_data(&data->blb);
    bzero(data->wrblb, sizeof(data->wrblb));

    if (del_dta)
        free(del_dta);
    if (del_dta_lk)
        free(del_dta_lk);

    return rc;
}

static int live_sc_redo_update(struct convert_record_data *data, DB_LOGC *logc,
                               bdb_osql_log_rec_t *rec, DBT *logdta)
{
    bdb_state_type *bdb_state = thedb->bdb_env;

    struct blob_recs brecs = {0};
    struct blob_recs *pbrecs = NULL;

    u_int32_t rectype;
    int rc, page, index;
    unsigned long long genid = 0, oldgenid = 0;
    int prevlen = 0, updlen = 0;
    llog_undo_upd_dta_args *upd_dta = NULL;
    llog_undo_upd_dta_lk_args *upd_dta_lk = NULL;
    int do_blob = 0;

    brecs.genid = rec->genid;
    pbrecs = hash_find(data->blob_hash, &brecs);
    if (pbrecs) {
        rc = reconstruct_blob_records(data, logc, pbrecs, logdta);
        if (rc) {
            logmsg(LOGMSG_ERROR,
                   "%s:%d failed to reconstruct blob records rc %d\n", __func__,
                   __LINE__, rc);
            goto done;
        }
        do_blob = 1;
    }

    if ((rc = logc->get(logc, &rec->lsn, logdta, DB_SET)) != 0) {
        logmsg(LOGMSG_ERROR, "%s:%d rc %d retrieving lsn %d:%d\n", __func__,
               __LINE__, rc, rec->lsn.file, rec->lsn.offset);
        goto done;
    }
    LOGCOPY_32(&rectype, logdta->data);
    normalize_rectype(&rectype);
    assert(rectype == rec->type);
    if (rec->type == DB_llog_undo_upd_dta_lk) {
        if ((rc = llog_undo_upd_dta_lk_read(bdb_state->dbenv, logdta->data,
                                            &upd_dta_lk)) != 0) {
            logmsg(LOGMSG_ERROR, "%s:%d error unpacking rc=%d\n", __func__,
                   __LINE__, rc);
            goto done;
        }
        genid = upd_dta_lk->newgenid;
        oldgenid = upd_dta_lk->oldgenid;
    } else {
        if ((rc = llog_undo_upd_dta_read(bdb_state->dbenv, logdta->data,
                                         &upd_dta)) != 0) {
            logmsg(LOGMSG_ERROR, "%s:%d error unpacking rc=%d\n", __func__,
                   __LINE__, rc);
            goto done;
        }
        genid = upd_dta->newgenid;
        oldgenid = upd_dta->oldgenid;
    }

    brecs.genid = genid;
    pbrecs = hash_find(data->blob_hash, &brecs);
    if (pbrecs) {
        rc = reconstruct_blob_records(data, logc, pbrecs, logdta);
        if (rc) {
            logmsg(LOGMSG_ERROR,
                   "%s:%d failed to reconstruct blob records rc %d\n", __func__,
                   __LINE__, rc);
            goto done;
        }
        do_blob = 1;
    }
    if (do_blob) {
        bzero(data->freeblb, sizeof(data->freeblb));
        bzero(data->wrblb, sizeof(data->wrblb));
        blob_status_to_blob_buffer(&data->blbcopy, data->freeblb);
        blob_status_to_blob_buffer(&data->blb, data->wrblb);
    }

    if (0 == bdb_inplace_cmp_genids(data->from->handle, oldgenid, genid)) {
        rc = bdb_reconstruct_inplace_update(
            bdb_state, &rec->lsn, data->old_dta_buf, &prevlen, data->dta_buf,
            &updlen, NULL, NULL, NULL);
    } else {
        unsigned long long prevgenid, newgenid;
        int prevgenidlen, newgenidlen;
        prevlen = updlen = data->from->lrl + ODH_SIZE;
        prevgenidlen = newgenidlen = sizeof(unsigned long long);
        rc = bdb_reconstruct_update(bdb_state, &rec->lsn, &page, &index,
                                    &prevgenid, &prevgenidlen,
                                    data->old_dta_buf, &prevlen, &newgenid,
                                    &newgenidlen, data->dta_buf, &updlen);
    }
    if (rc != 0) {
        logmsg(LOGMSG_ERROR, "%s:%d failed to reconstruct update rc=%d\n",
               __func__, __LINE__, rc);
        goto done;
    }

    /* Decompress and upgrade new record to current version */
    if ((rc = unpack_and_upgrade_ondisk_record(data, data->dta_buf, &updlen,
                                               data->unpack_dta_buf,
                                               &data->odh)) != 0) {
        logmsg(LOGMSG_ERROR,
               "%s:%d failed to unpack and upgrade ondisk record rc=%d\n",
               __func__, __LINE__, rc);
        goto done;
    }

    /* Decompress and upgrade prev record to current version */
    if ((rc = unpack_and_upgrade_ondisk_record(
             data, data->old_dta_buf, &prevlen, data->unpack_old_dta_buf,
             &data->oldodh)) != 0) {
        logmsg(LOGMSG_ERROR,
               "%s:%d failed to unpack and upgrade ondisk record rc=%d\n",
               __func__, __LINE__, rc);
        goto done;
    }

#ifdef LOGICAL_LIVESC_DEBUG
    logmsg(LOGMSG_DEBUG, "%s:%d old dtalen %d\n", __func__, __LINE__, prevlen);
    fsnapf(stdout, data->oldodh.recptr, prevlen);
    logmsg(LOGMSG_DEBUG, "%s:%d new dtalen %d\n", __func__, __LINE__, updlen);
    fsnapf(stdout, data->odh.recptr, updlen);
#endif

    int updCols[MAXCOLUMNS + 1] = {0};
    updCols[0] = data->from->schema->nmembers;
    for (int i = 0; i < data->from->schema->nmembers; i++) {
        int blbix = data->from->schema->member[i].blob_index;
        if (blbix != -1 && !data->freeblb[blbix].exists &&
            !data->wrblb[blbix].exists)
            updCols[i + 1] = -1;
        else
            updCols[i + 1] = i;
    }

    data->iq.usedb = data->to;

    if (!data->s->sc_convert_done[rec->dtastripe] &&
        is_genid_right_of_stripe_pointer(data->to->handle, genid,
                                         data->sc_genids)) {
        /* if the newgenid is to the right of the sc cursor, we only need to
         * delete the old record */
        rc = del_new_record(&data->iq, data->trans, oldgenid, -1ULL,
                            data->oldodh.recptr, data->freeblb, 0);
    } else {
        /* Search for genid in llmeta */
        DB_LSN current, redolsn;
        if ((rc = get_redo_genid(data, oldgenid, &redolsn)) == 0) {

#ifdef LOGICAL_LIVESC_DEBUG
            logmsg(LOGMSG_DEBUG, "%s found redo-genid %llu with redolsn[%d][%d], mylsn is [%d][%d]\n", __func__,
                   oldgenid, redolsn.file, redolsn.offset, rec->lsn.file, rec->lsn.offset);
            logmsg(LOGMSG_DEBUG, "%s:%d Adding rather than updating oldgenid= %llx genid=%llx\n", __func__, __LINE__,
                   oldgenid, genid);
#endif

            unsigned long long dirty_keys = -1ULL;
            int dta_needs_conversion = 1;
            int usellmeta = 0;
            if (!data->to->plan) {
                usellmeta = 1; /* new dta does not have old genids */
            } else if (data->to->plan->dta_plan) {
                usellmeta = 0; /* the genid is in new dta */
            } else {
                usellmeta = 1; /* dta is not being built */
            }

            if (usellmeta && data->s->rebuild_index)
                dta_needs_conversion = 0;

            rc = prepare_and_verify_newdb_record(data, data->odh.recptr, updlen, &dirty_keys, 0);
            if (rc) {
                logmsg(LOGMSG_ERROR, "%s:%d failed to prepare and verify new record rc=%d\n", __func__, __LINE__, rc);
                goto done;
            }

            char *tagname = ".NEW..ONDISK";
            uint8_t *p_tagname_buf = (uint8_t *)tagname;
            uint8_t *p_tagname_buf_end = p_tagname_buf + 12;
            uint8_t *p_buf_data = data->rec->recbuf;
            uint8_t *p_buf_data_end = p_buf_data + data->rec->bufsize;
            if (!dta_needs_conversion) {
                p_buf_data = data->odh.recptr;
                p_buf_data_end = p_buf_data + updlen;
            }
            unsigned long long ngenid = genid;
            int opfailcode = 0, ixfailnum = 0;
            int nrrn = 2;
            int addflags = RECFLAGS_NO_TRIGGERS | RECFLAGS_NO_CONSTRAINTS | RECFLAGS_NEW_SCHEMA |
                           RECFLAGS_ADD_FROM_SC_LOGICAL | RECFLAGS_KEEP_GENID | RECFLAGS_NO_BLOBS;

            rc = add_record(&data->iq, data->trans, p_tagname_buf, p_tagname_buf_end, p_buf_data, p_buf_data_end, NULL,
                            data->wrblb, MAXBLOBS, &opfailcode, &ixfailnum, &nrrn, &ngenid,
                            (gbl_partial_indexes && data->to->ix_partial) ? dirty_keys : -1ULL, BLOCK2_ADDKL, 0,
                            addflags, 0);

#ifdef LOGICAL_LIVESC_DEBUG
            logmsg(LOGMSG_DEBUG, "%u: add_record (%llx to %llx) returns %d opfailcode=%d ixfailnum=%d\n",
                   (unsigned int)pthread_self(), oldgenid, genid, rc, opfailcode, ixfailnum);
#endif

            if (rc == IX_DUP) {
                rc = ERR_INDEX_CONFLICT;
            }
        } else {
#ifdef LOGICAL_LIVESC_DEBUG
            logmsg(LOGMSG_DEBUG, "Updating record [%d][%d], updlen=%d, odhlen=%d oldodhlen=%d\n", rec->lsn.file,
                   rec->lsn.offset, updlen, data->odh.length, data->oldodh.length);
#endif
            /* try to update the record in the new btree */
            rc = upd_new_record(&data->iq, data->trans, oldgenid, data->oldodh.recptr, genid, data->odh.recptr, -1ULL,
                                -1ULL, updlen, updCols, data->wrblb, 0, data->freeblb, data->wrblb, 0);
            logmsg(LOGMSG_USER, "%"PRIxPTR": Upd_new_record %llx to %llx returns %d\n", (uintptr_t)pthread_self(), oldgenid,
                   genid, rc);
        }
#ifdef LOGICAL_LIVESC_DEBUG
        int saverc = rc;
#endif
        if (rc == ERR_VERIFY || rc == ERR_INDEX_CONFLICT) {
            /* either the oldgenid is not found or the newgenid already exists
             * -- try delete the oldgenid again.
             * TODO: differentiate the above two cases and no need to call
             * delete again for the first case */
            if (rc == ERR_INDEX_CONFLICT) {
#ifdef LOGICAL_LIVESC_DEBUG
                logmsg(LOGMSG_DEBUG, "%u: Deleting new record %llx on ix conflict\n", (unsigned int)pthread_self(),
                       genid);
#endif
                rc = del_new_record(&data->iq, data->trans, genid, -1ULL, data->odh.recptr, data->wrblb, 0);
                if (rc && rc != ERR_VERIFY) {
                    logmsg(LOGMSG_FATAL, "Debug assert - unexpected condition\n");
                    abort();
                }
#ifdef LOGICAL_LIVESC_DEBUG
                logmsg(LOGMSG_DEBUG, "%u: rc from del_new_record is %d\n", (unsigned int)pthread_self(), rc);
                // rc from del_new_record is %d\n", (unsigned int)pthread_self(), rc);
#endif
                bdb_get_commit_genid(thedb->bdb_env, &current);
#ifdef LOGICAL_LIVESC_DEBUG
                logmsg(LOGMSG_USER, "%u: setting newsc genid to %llx lsn=[%u][%u]\n", (unsigned int)pthread_self(),
                       genid, current.file, current.offset);
#endif
                set_redo_genid(data, genid, &current);
                rc = 0;
            }

#ifdef LOGICAL_LIVESC_DEBUG
            logmsg(LOGMSG_USER, "%u: Deleting old record %llx on %d newgenid=%llx\n", (unsigned int)pthread_self(),
                   oldgenid, saverc, genid);
#endif
            /* This is a race between the converter threads and the redo threads */
            rc = del_new_record(&data->iq, data->trans, oldgenid, -1ULL,
                                data->oldodh.recptr, data->freeblb, 0);
#ifdef LOGICAL_LIVESC_DEBUG
            logmsg(LOGMSG_DEBUG, "%u: del_new_record oldgenid=%llx rcode=%d\n", (unsigned int)pthread_self(), oldgenid,
                   rc);
#endif
        }
    }
    if (rc == ERR_VERIFY) {
        /* not an error if we dont find it */
        rc = 0;
    }

done:
#ifdef LOGICAL_LIVESC_DEBUG
    logmsg(LOGMSG_DEBUG,
           "%s: [%s] redo lsn[%u:%u] type[UPD_DTA] rec->dtafile %d, "
           "rec->datastripe %d, rec->genid %llx, newgenid %llx\n",
           __func__, data->s->tablename, rec->lsn.file, rec->lsn.offset,
           rec->dtafile, rec->dtastripe, rec->genid, genid);
#endif

    free_blob_status_data(&data->blbcopy);
    bzero(data->freeblb, sizeof(data->freeblb));
    free_blob_status_data(&data->blb);
    bzero(data->wrblb, sizeof(data->wrblb));

    if (upd_dta)
        free(upd_dta);
    if (upd_dta_lk)
        free(upd_dta_lk);

    return rc;
}

static inline int is_logical_data_op(bdb_osql_log_rec_t *rec)
{
    assert (rec->type < 12000);
    switch (rec->type) {
    case DB_llog_undo_add_dta:
    case DB_llog_undo_add_dta_lk:
    case DB_llog_undo_del_dta:
    case DB_llog_undo_del_dta_lk:
    case DB_llog_undo_upd_dta:
    case DB_llog_undo_upd_dta_lk:
        return 1;
    default:
        break;
    }
    return 0;
}

static int live_sc_redo_logical_rec(struct convert_record_data *data,
                                    DB_LOGC *logc, bdb_osql_log_rec_t *rec,
                                    DBT *logdta)
{
    unsigned long long mingenid;
    DB_LSN minlsn;
    int rc = 0;

    if (rec->dtastripe < 0 || rec->dtastripe >= gbl_dtastripe) {
        logmsg(LOGMSG_ERROR,
               "%s:%d rec->dtastripe %d out of range, type %d, lsn[%u:%u]\n",
               __func__, __LINE__, rec->dtastripe, rec->type, rec->lsn.file,
               rec->lsn.offset);
        return -1;
    }
    if (!data->s->sc_convert_done[rec->dtastripe] &&
        is_genid_right_of_stripe_pointer(data->to->handle, rec->genid,
                                         data->sc_genids)) {
        /* skip those still to the right of sc cursor */
#ifdef LOGICAL_LIVESC_DEBUG
        logmsg(LOGMSG_DEBUG, "%s:%d genid %llx is to the right of stripe pointer\n", __func__, __LINE__, rec->genid);
#endif
        return 0;
    }

    if (get_min_redo_lsn(data, &mingenid, &minlsn) == 0 && log_compare(&minlsn, &rec->lsn) < 0) {
        logmsg(LOGMSG_ERROR,
               "%s:%d conflicting genid %llx not deleted after "
               "%u:%u, redo-lsn %u:%u\n",
               __func__, __LINE__, mingenid, minlsn.file, minlsn.offset, rec->lsn.file, rec->lsn.offset);
        return ERR_INDEX_CONFLICT;
    }

    assert(rec->type < 12000);

    switch (rec->type) {
    case DB_llog_undo_add_dta:
    case DB_llog_undo_add_dta_lk:
        if (bdb_inplace_cmp_genids(data->to->handle, rec->genid,
                                   data->sc_genids[rec->dtastripe]) == 0) {
#ifdef LOGICAL_LIVESC_DEBUG
            logmsg(LOGMSG_DEBUG, "%s:%d skipping just-converted genid %llx\n", __func__, __LINE__, rec->genid);
#endif
            /* small optimization to skip last record that was done by the
             * convert thread */
            return 0;
        }
#ifdef LOGICAL_LIVESC_DEBUG
        logmsg(LOGMSG_DEBUG, "%s:%d redo thread adding genid %llx\n", __func__, __LINE__, rec->genid);
#endif
        rc = live_sc_redo_add(data, logc, rec, logdta);
        if (rc) {
            logmsg(LOGMSG_ERROR,
                   "%s: [%s] failed to redo add lsn[%u:%u] rc=%d\n", __func__,
                   data->s->tablename, rec->lsn.file, rec->lsn.offset, rc);
            return rc;
        }
        break;
    case DB_llog_undo_del_dta:
    case DB_llog_undo_del_dta_lk:
        rc = live_sc_redo_delete(data, logc, rec, logdta);
#ifdef LOGICAL_LIVESC_DEBUG
        logmsg(LOGMSG_DEBUG, "%s:%d redo thread deleting genid %llx\n", __func__, __LINE__, rec->genid);
#endif
        if (rc) {
            logmsg(LOGMSG_ERROR,
                   "%s: [%s] failed to redo delete lsn[%u:%u] rc=%d\n",
                   __func__, data->s->tablename, rec->lsn.file, rec->lsn.offset,
                   rc);
            return rc;
        }
        break;
    case DB_llog_undo_upd_dta:
    case DB_llog_undo_upd_dta_lk:
#ifdef LOGICAL_LIVESC_DEBUG
        logmsg(LOGMSG_DEBUG, "%s:%d redo thread updating genid %llx\n", __func__, __LINE__, rec->genid);
#endif
        rc = live_sc_redo_update(data, logc, rec, logdta);
        if (rc) {
            logmsg(LOGMSG_ERROR,
                   "%s: [%s] failed to redo update lsn[%u:%u] rc=%d\n",
                   __func__, data->s->tablename, rec->lsn.file, rec->lsn.offset,
                   rc);
            return rc;
        }
        break;
    default:
        break;
    }

    return 0;
}

static int live_sc_redo_logical_log(struct convert_record_data *data,
                                    bdb_llog_cursor *pCur)
{
    int rc = 0;
    bdb_state_type *bdb_state = thedb->bdb_env;
    bdb_osql_log_rec_t *rec = NULL, *tmp = NULL;
    DB_LOGC *logc = NULL;
    int interested = 0;
    u_int64_t logbytes = 0;
    DBT logdta = {0};
    logdta.flags = DB_DBT_REALLOC;
    LISTC_T(bdb_osql_log_rec_t) recs; /* list of relevant undo records */
    listc_init(&recs, offsetof(bdb_osql_log_rec_t, lnk));

    /* pre process recs */
    LISTC_FOR_EACH_SAFE(&pCur->log->impl->recs, rec, tmp, lnk)
    {
        /* skip if it is not data operations */
        if (!is_logical_data_op(rec))
            continue;
        /* skip logical ops that are not relevant to the table */
        if (strcasecmp(data->s->tablename, rec->table) != 0)
            continue;

        /* get one op that needs to be redone - mark interested */
        interested = 1;
        listc_rfl(&pCur->log->impl->recs, rec);

        if (rec->dtafile >= 1) {
            /* put blob records into a list hash based on genid */
            struct blob_recs brecs = {0};
            struct blob_recs *pbrecs = NULL;
            brecs.genid = rec->genid;
            pbrecs = hash_find(data->blob_hash, &brecs);
            if (pbrecs) {
                listc_abl(&pbrecs->recs, rec);
            } else {
                pbrecs = calloc(1, sizeof(struct blob_recs));
                if (pbrecs == NULL) {
                    logmsg(LOGMSG_ERROR, "%s:%d failed to malloc blob rec\n",
                           __func__, __LINE__);
                    rc = -1;
                    goto done;
                }
                pbrecs->genid = rec->genid;
                listc_init(&pbrecs->recs, offsetof(bdb_osql_log_rec_t, lnk));
                listc_abl(&pbrecs->recs, rec);
                hash_add(data->blob_hash, pbrecs);
            }
        } else {
            /* put relevant log records into a linked list so we don't need to
             * loop on the transaction's logical operations list anymore */
            listc_abl(&recs, rec);
        }
    }

    if (!interested) {
        /* nothing to do if none of the logical ops are relevant to the table */
        rc = 0;
        goto done;
    }

    if ((rc = bdb_state->dbenv->log_cursor(bdb_state->dbenv, &logc, 0)) != 0) {
        logmsg(LOGMSG_ERROR, "%s:%d failed to get log-cursor %d\n", __func__,
               __LINE__, rc);
        rc = -1;
        goto done;
    }

again:
    assert(data->trans == NULL);

    /* Schema-change writes are always page-lock, not rowlock */
    throttle_sc_logbytes(0);
    rc = trans_start_sc_lowpri(&data->iq, &data->trans);
    if (rc) {
        logmsg(LOGMSG_ERROR, "%s:%d error %d starting transaction\n", __func__,
               __LINE__, rc);
        rc = -2;
        goto done;
    }
    set_tran_verify_updateid(data->trans);
    data->iq.timeoutms = gbl_sc_timeoutms;
    data->iq.debug = debug_this_request(gbl_debug_until);
    if (data->iq.debug) {
        reqlog_new_request(&data->iq); // TODO: cleanup (reset) logger
        reqpushprefixf(&data->iq, "0x%llx: LOGICAL REDO ", pthread_self());
    }

    /* redo each of the log records revelant to this table */
    LISTC_FOR_EACH(&recs, rec, lnk)
    {
        if (data->s->sc_thd_failed) {
            if (!data->s->retry_bad_genids)
                sc_errf(data->s,
                        "Stoping work on logical redo because the thread for "
                        "stripe %d failed\n",
                        data->s->sc_thd_failed - 1);
            rc = -1;
            goto done;
        }
        if (gbl_sc_abort || data->from->sc_abort ||
            (data->s->iq && data->s->iq->sc_should_abort)) {
            sc_errf(data->s, "[%s] Logical redo aborted\n", data->s->tablename);
            rc = -1;
            goto done;
        }
        /* redo this logical op */
        rc = live_sc_redo_logical_rec(data, logc, rec, &logdta);
        if (rc) {
            logmsg(LOGMSG_ERROR,
                   "[%s] redo failed at record lsn[%u:%u] table[%s] type[%d] "
                   "rc=%d\n",
                   data->s->tablename, rec->lsn.file, rec->lsn.offset,
                   rec->table, rec->type, rc);
            goto done;
        }
    }

    data->nrecs++;
    if (data->nrecs %
            BDB_ATTR_GET(thedb->bdb_attr, SC_LOGICAL_SAVE_LSN_EVERY_N) ==
        0) {
        int bdberr = 0;
        rc = bdb_set_sc_start_lsn(data->trans, data->s->tablename,
                                  &(pCur->curLsn), &bdberr);
        if (rc != 0) {
            if (bdberr == BDBERR_DEADLOCK)
                rc = RC_INTERNAL_RETRY;
            else {
                logmsg(
                    LOGMSG_ERROR,
                    "%s:%d failed to update start lsn, rc = %d, bdberr = %d\n",
                    __func__, __LINE__, rc, bdberr);
                rc = ERR_INTERNAL;
            }
            data->nrecs--;
        } else {
            sc_set_logical_redo_lwm(data->s->tablename, pCur->curLsn.file);
#ifdef LOGICAL_LIVESC_DEBUG
            logmsg(LOGMSG_DEBUG, "[%s] %s:%d sets sc start lsn to [%u][%u]\n",
                   data->s->tablename, __func__, __LINE__, pCur->curLsn.file,
                   pCur->curLsn.offset);
#endif
        }
    }

done:
    if (data->trans) {
        db_seqnum_type ss;
        if (rc) {
            logbytes = bdb_tran_logbytes(data->trans);
            increment_sc_logbytes(logbytes);
            trans_abort(&data->iq, data->trans);
            data->trans = NULL;
            if (rc == RC_INTERNAL_RETRY) {
                data->num_retry_errors++;
                data->totnretries++;
                poll(0, 0, (rand() % 500 + 10));
                goto again;
            }
        } else if (data->live)
            rc = trans_commit_seqnum(&data->iq, data->trans, &ss);
        else
            rc = trans_commit(&data->iq, data->trans, gbl_myhostname);

        increment_sc_logbytes(data->iq.txnsize);
        data->trans = NULL;
    }

    reqlog_end_request(data->iq.reqlogger, 0, __func__, __LINE__);

    /* free memory used in this function */
    clear_recs_list(&recs);
    clear_blob_hash(data->blob_hash);
    if (rc && !data->s->sc_thd_failed)
        data->s->sc_thd_failed = -1;
    if (logc)
        logc->close(logc, 0);
    if (logdta.data)
        free(logdta.data);
    bdb_osql_log_destroy(pCur->log);
    pCur->log = NULL;
    return rc;
}

static struct sc_redo_lsn *get_next_redo_lsn(bdb_state_type *bdb_state,
                                             struct schema_change_type *s)
{
    struct sc_redo_lsn *redo = NULL;
    pthread_mutex_t *mtx = &bdb_state->sc_redo_lk;
    pthread_cond_t *cond = &bdb_state->sc_redo_wait;
    int wait = 1;

    Pthread_mutex_lock(mtx);

    if (!s->sc_convert_done || s->sc_convert_done[MAXDTASTRIPE]) {
        /* all converter threads have finished */
        wait = 0;
    }

    redo = listc_rtl(&bdb_state->sc_redo_list);
    if (redo == NULL && wait) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 10;
        int rc = pthread_cond_timedwait(cond, mtx, &ts);
        if (rc != 0 && rc != ETIMEDOUT)
            logmsg(LOGMSG_ERROR, "%s:pthread_cond_timedwait: %d %s\n", __func__,
                   rc, strerror(rc));
        if (rc == 0) {
            redo = listc_rtl(&bdb_state->sc_redo_list);
        }
    }

    Pthread_mutex_unlock(mtx);
    return redo;
}

static int sc_redo_size(bdb_state_type *bdb_state)
{
    int sz = 0;

    Pthread_mutex_lock(&bdb_state->sc_redo_lk);
    sz = listc_size(&bdb_state->sc_redo_list);
    Pthread_mutex_unlock(&bdb_state->sc_redo_lk);

    return sz;
}

void *live_sc_logical_redo_thd(struct convert_record_data *data)
{
    comdb2_name_thread(__func__);
    struct schema_change_type *s = data->s;
    struct thr_handle *thr_self = thrman_self();
    enum thrtype oldtype = THRTYPE_UNKNOWN;
    int rc = 0;
    int finalizing = 0;
    bdb_llog_cursor llog_cur;
    bdb_llog_cursor *pCur = &llog_cur;
    DB_LSN curLsn = {0};
    DB_LSN finalizeLsn = {0};
    bdb_state_type *bdb_state = data->from->handle;
    struct sc_redo_lsn *redo = NULL;
    DB_LSN eofLsn = {0};
    int serial = 0;
    DB_LSN serialLsn = {0}; /* scan serial upto this LSN for resume */

    bzero(pCur, sizeof(bdb_llog_cursor));

    if (data->isThread)
        thread_started("logical redo thread");

    if (thr_self) {
        oldtype = thrman_get_type(thr_self);
        thrman_change_type(thr_self, THRTYPE_SCHEMACHANGE);
    } else {
        thr_self = thrman_register(THRTYPE_SCHEMACHANGE);
    }

    if (data->isThread) {
        backend_thread_event(thedb, COMDB2_THR_EVENT_START_RDWR);
    }
    data->iq.reqlogger = thrman_get_reqlogger(thr_self);

    /* starting time and lsn of the schema change */
    sc_printf(s, "[%s] logical redo %s at [%u:%u]\n", s->tablename,
              s->resume ? "resumes" : "starts", data->start_lsn.file,
              data->start_lsn.offset);
    data->lasttime = comdb2_time_epoch();
    pCur->minLsn = pCur->curLsn = data->start_lsn;
    eofLsn = data->start_lsn;
    if (s->resume) {
        serial = 1;
        bdb_get_commit_genid(thedb->bdb_env, &serialLsn);
        sc_printf(s, "[%s] logical redo run serial from [%u][%u] to [%u][%u]\n",
                  s->tablename, data->start_lsn.file, data->start_lsn.offset,
                  serialLsn.file, serialLsn.offset);
    }

    /* init all buffer needed by this thread to do logical redo so we don't need
     * to malloc & free every single time */
    data->rec = allocate_db_record(data->to, ".NEW..ONDISK");
    data->iq.usedb = data->to;
    data->blob_hash = hash_init_o(offsetof(struct blob_recs, genid),
                                  sizeof(unsigned long long));
    if (!data->blob_hash) {
        logmsg(LOGMSG_ERROR, "%s: failed to init blob hash\n", __func__);
        s->iq->sc_should_abort = 1;
        goto cleanup;
    }

    data->redo_genids = hash_init(sizeof(unsigned long long));
    if (!data->redo_genids) {
        logmsg(LOGMSG_ERROR, "%s: failed to init redo_genids hash\n", __func__);
        s->iq->sc_should_abort = 1;
        goto cleanup;
    }

    listc_init(&data->redo_lsns, offsetof(struct redo_genid_lsns, linkv));
    data->dta_buf = malloc(data->from->lrl + ODH_SIZE);
    data->old_dta_buf = malloc(data->from->lrl + ODH_SIZE);
    data->unpack_dta_buf = malloc(data->from->lrl + ODH_SIZE);
    data->unpack_old_dta_buf = malloc(data->from->lrl + ODH_SIZE);
    data->blb_buf = NULL;
    data->old_blb_buf = NULL;
    if (!data->dta_buf || !data->old_dta_buf || !data->unpack_dta_buf ||
        !data->unpack_old_dta_buf) {
        logmsg(LOGMSG_ERROR, "%s: failed to malloc buffer\n", __func__);
        s->iq->sc_should_abort = 1;
        goto cleanup;
    }
    bzero(data->freeblb, sizeof(data->freeblb));
    bzero(data->wrblb, sizeof(data->wrblb));
    bzero(&data->blb, sizeof(data->blb));
    bzero(&data->blbcopy, sizeof(data->blbcopy));
    data->tagmap = get_tag_mapping(
        data->from->schema /*tbl .ONDISK tag schema*/,
        data->to->schema /*tbl .NEW..ONDISK schema */); // free tagmap only once

    s->hitLastCnt = 0;

    data->to->sc_from = data->from;

    /* s->curLsn is used to synchronize between logical redo thread and convert
     * record threads in order to correctly detect unique key violations */
    Pthread_mutex_lock(&s->livesc_mtx);
    s->curLsn = &curLsn;
    Pthread_mutex_unlock(&s->livesc_mtx);

    if (serial) {
        populate_redo_genids(data);
    }

    while (1) {
        /* pause right here if we've been told to */
        while (BDB_ATTR_GET(thedb->bdb_attr, SC_PAUSE_REDO)) {
            sc_printf(s, "[%s] logical redo thread pausing on sc_pause_redo\n", __func__);
#ifdef LOGICAL_LIVESC_DEBUG
            logmsg(LOGMSG_DEBUG, "%u %s pausing on SC_PAUSE_REDO\n", (unsigned int)pthread_self(), __func__);
#endif
            sleep(1);
        }

        /* abort schema change if we need to */
        if (gbl_sc_abort || data->from->sc_abort || s->sc_thd_failed ||
            (s->iq && s->iq->sc_should_abort)) {
            sc_errf(s,
                    "[%s] Stoping work on logical redo because we are told to "
                    "abort\n",
                    s->tablename);
            goto cleanup;
        }
        if (get_stopsc(__func__, __LINE__)) {
            sc_errf(s, "[%s] %s stopping due to master swings\n", s->tablename,
                    __func__);
            goto cleanup;
        }

        /* scan serially */
        if (!serial) {
            /* Get the next lsn to redo, wait upto 10s unless all convert
             * threads have finished */
#ifdef LOGICAL_LIVESC_DEBUG
            if (finalizing) {
                logmsg(LOGMSG_DEBUG, "%s final get_next_redo_lsn\n", __func__);
            }
#endif
            redo = get_next_redo_lsn(bdb_state, s);
        }
        if (!serial && redo == NULL) {
#ifdef LOGICAL_LIVESC_DEBUG
            logmsg(LOGMSG_DEBUG, "[%s] no write since [%u][%u]\n", s->tablename,
                   eofLsn.file, eofLsn.offset);
#endif
            /* Table has not been touch since last eofLsn */
            Pthread_mutex_lock(&s->livesc_mtx);
            curLsn = eofLsn;
            Pthread_mutex_unlock(&s->livesc_mtx);
            /* No write against the table, mark it as hitLast */
            pCur->hitLast = 1;
            if (finalizing) {
                /* done */
#ifdef LOGICAL_LIVESC_DEBUG
                logmsg(LOGMSG_DEBUG, "%s %u empty redo and finalize break\n", __func__, __LINE__);
#endif
                break;
            }
            /* Update eofLsn to current end of log file */
            bdb_get_commit_genid(thedb->bdb_env, &eofLsn);
        } else {
#ifdef LOGICAL_LIVESC_DEBUG
            if (serial)
                logmsg(LOGMSG_DEBUG, "[%s] serial log scan at lsn [%u][%u]\n",
                       s->tablename, pCur->curLsn.file, pCur->curLsn.offset);
            else
                logmsg(LOGMSG_DEBUG, "[%s] redo logical commit lsn [%u][%u]\n",
                       s->tablename, redo->lsn.file, redo->lsn.offset);
#endif
            /* traverse upto 1 log file */
            pCur->maxLsn = serial ? pCur->curLsn : redo->lsn;
            pCur->maxLsn.file += 1;

            /* get the next transaction's logical ops from the log files */
            if (serial)
                rc = bdb_llog_cursor_next(pCur);
            else {
                int nretries = 0;
                while (nretries < 500) {
                    rc = bdb_llog_cursor_find(pCur, &(redo->lsn));
                    if (rc || (pCur->log && !pCur->hitLast)) {
#ifdef LOGICAL_LIVESC_DEBUG
                        logmsg(LOGMSG_DEBUG, "%s %u Cant find committed transaction?\n", __func__, __LINE__);
#endif
                        break;
                    }
                    /* retry again and wait for the transaction to commit */
                    nretries++;
                    poll(NULL, 0, 10);
                }
                if (nretries >= 500) {
                    /* Error out if we cannot find the committed transaction */
                    sc_errf(s, "[%s] logical redo failed to find [%u:%u]\n",
                            s->tablename, redo->lsn.file, redo->lsn.offset);
#ifdef LOGICAL_LIVESC_DEBUG
                    logmsg(LOGMSG_DEBUG, "%s %u logical redo failed to find [%u:%u]\n", __func__, __LINE__,
                           redo->lsn.file, redo->lsn.offset);
#endif
                    s->iq->sc_should_abort = 1;
                    goto cleanup;
                }
                if (redo->txnid != pCur->log->txnid) {
                    sc_errf(s,
                            "[%s] logical redo failed to find [%u:%u], expect "
                            "txnid %x, got %x\n",
                            s->tablename, redo->lsn.file, redo->lsn.offset,
                            redo->txnid, pCur->log->txnid);
#ifdef LOGICAL_LIVESC_DEBUG
                    logmsg(LOGMSG_DEBUG,
                           "%s %u logical redo failed to find [%u:%u], "
                           "expect txnid %x, got %x\n",
                           __func__, __LINE__, redo->lsn.file, redo->lsn.offset, redo->txnid, pCur->log->txnid);
#endif
                    s->iq->sc_should_abort = 1;
                    goto cleanup;
                }
            }
            if (rc) {
                sc_errf(s, "[%s] logical redo failed at [%u:%u]\n",
                        s->tablename, pCur->curLsn.file, pCur->curLsn.offset);
#ifdef LOGICAL_LIVESC_DEBUG
                logmsg(LOGMSG_DEBUG, "%s %u logical redo failed at [%u:%u]\n", __func__, __LINE__, redo->lsn.file,
                       redo->lsn.offset);
#endif
                s->iq->sc_should_abort = 1;
                goto cleanup;
            }
            if (pCur->log && !pCur->hitLast) {
                /* redo this transaction against the new btrees */
                rc = live_sc_redo_logical_log(data, pCur);
                if (rc) {
                    sc_errf(s, "[%s] logical redo failed at [%u:%u]\n",
                            s->tablename, pCur->curLsn.file,
                            pCur->curLsn.offset);
                    s->iq->sc_should_abort = 1;
                    goto cleanup;
                }
                assert(pCur->log == NULL /* i.e. consumed */);
                s->hitLastCnt = 0;
            }
            Pthread_mutex_lock(&s->livesc_mtx);
            curLsn = pCur->curLsn;
            Pthread_mutex_unlock(&s->livesc_mtx);

            eofLsn = curLsn;

            if (!serial) {
                free(redo);
                redo = NULL;
            }
            else if (log_compare(&curLsn, &serialLsn) > 0) {
                sc_printf(s, "[%s] logical redo exits serial mode\n",
                          s->tablename);
                serial = 0;
            }
        }

        if (finalizing && log_compare(&curLsn, &finalizeLsn) >= 0) {
#ifdef LOGICAL_LIVESC_DEBUG
            logmsg(LOGMSG_DEBUG, "Redo breaking out of loop, curlsn=%u:%u, finalizelsn=%u:%u\n", curLsn.file,
                   curLsn.offset, finalizeLsn.file, finalizeLsn.offset);
#endif
            break;
        }
        unsigned int lwm = sc_get_logical_redo_lwm_table(s->tablename);
        if (lwm != curLsn.file) {
            int bdberr = 0;
            rc = bdb_set_sc_start_lsn(NULL, s->tablename, &curLsn, &bdberr);
            if (rc || bdberr) {
                logmsg(LOGMSG_ERROR,
                       "%s: failed to update current LSN [%u][%u], rc = %d, "
                       "bdberr = %d\n",
                       __func__, curLsn.file, curLsn.offset, rc, bdberr);
            } else {
#ifdef LOGICAL_LIVESC_DEBUG
                logmsg(LOGMSG_DEBUG,
                       "[%s] %s:%d sets sc start lsn to [%u][%u]\n",
                       data->s->tablename, __func__, __LINE__, curLsn.file,
                       curLsn.offset);
#endif
                sc_set_logical_redo_lwm(s->tablename, curLsn.file);
            }
        }
        int now = comdb2_time_epoch();
        int copy_sc_report_freq = gbl_sc_report_freq;
        if (copy_sc_report_freq > 0 &&
            now >= data->lasttime + copy_sc_report_freq) {
            long long diff_nrecs = data->nrecs - data->prev_nrecs;
            data->lasttime = now;
            data->prev_nrecs = data->nrecs;
            sc_printf(s,
                      "[%s] logical redo at LSN [%u][%u] transactions done "
                      "+%lld (%lld txn/s). Redo List Size: %d\n",
                      data->from->tablename, curLsn.file, curLsn.offset,
                      diff_nrecs, diff_nrecs / copy_sc_report_freq,
                      sc_redo_size(bdb_state));
        }
        if (pCur->hitLast) {
            if (s->got_tablelock) {
                /* loop one more time after we get table lock */
                if (finalizing) {
#ifdef LOGICAL_LIVESC_DEBUG
                    logmsg(LOGMSG_DEBUG, "%s %u Breaking out of redo loop\n", __func__, __LINE__);
#endif
                    break;
                }
                finalizing = 1;
                // get the lsn of current end of log
                bdb_get_commit_genid(thedb->bdb_env, &finalizeLsn);
            }
            poll(NULL, 0, 100);
            s->hitLastCnt++;
            continue;
        }
    }

    if (listc_size(&data->redo_lsns) > 0) {
        struct redo_genid_lsns *redo_lsns;
        int bdberr;
        sc_printf(s, "[%s] Index conflict detected from redo thread\n", s->tablename);
        s->iq->sc_should_abort = 1;
        if (rc == 0) {
            rc = ERR_INDEX_CONFLICT;
        }
        bdb_newsc_del_all_redo_genids(NULL, data->s->tablename, &bdberr);
        LISTC_FOR_EACH(&data->redo_lsns, redo_lsns, linkv)
        {
            sc_printf(s, "[%s] genid %llx -> [%u][%u]\n", s->tablename, redo_lsns->genid, redo_lsns->lsn.file,
                      redo_lsns->lsn.offset);
        }
    }

cleanup:
    convert_record_data_cleanup(data);

    if (data->isThread)
        backend_thread_event(thedb, COMDB2_THR_EVENT_DONE_RDWR);

    /* restore our  thread type to what it was before */
    if (oldtype != THRTYPE_UNKNOWN)
        thrman_change_type(thr_self, oldtype);

    if (data->blob_hash)
        hash_free(data->blob_hash);

    if (data->redo_genids) {
        clear_redo_genid_hash(data->redo_genids);
        hash_free(data->redo_genids);
        data->redo_genids = NULL;
    }

    sc_printf(s,
              "[%s] logical redo thread exiting, log cursor at [%u:%u], redid "
              "%lld txns\n",
              s->tablename, pCur->curLsn.file, pCur->curLsn.offset,
              data->nrecs);

    Pthread_mutex_lock(&s->livesc_mtx);
    s->curLsn = NULL;
    Pthread_mutex_unlock(&s->livesc_mtx);
    bdb_llog_cursor_close(pCur);

    s->logical_livesc = 0;

    data->to->sc_from = NULL;

    free(data);
    return NULL;
}
