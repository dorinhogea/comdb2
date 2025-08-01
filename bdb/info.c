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

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <segstr.h>

#include "net.h"
#include "bdb_int.h"
#include "locks.h"
#include "sys_wrap.h"
#include <build/db.h>
#include <str0.h>
#include <ctrace.h>
#include <endian_core.h>
#include <averager.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "sqlresponse.pb-c.h"
#include "logmsg.h"
#include "thread_stats.h"
#include <compat.h>

extern char *lsn_to_str(char lsn_str[], DB_LSN *lsn);
extern void bdb_dump_table_dbregs(bdb_state_type *bdb_state);
extern void __test_last_checkpoint(DB_ENV *dbenv);
extern void __pgdump(DB_ENV *dbenv, int32_t fileid, uint8_t *ufid, db_pgno_t pgno);
extern void __pgtrash(DB_ENV *dbenv, int32_t fileid, db_pgno_t pgno);
extern void __txn_commit_map_print_info(DB_ENV *dbenv, loglvl lvl, int should_lock);

static void txn_stats(FILE *out, bdb_state_type *bdb_state);
static void log_stats(FILE *out, bdb_state_type *bdb_state);
static void lock_stats(FILE *out, bdb_state_type *bdb_state);
static void rep_stats(FILE *out, bdb_state_type *bdb_state);
static void bdb_state_dump(FILE *out, const char *prefix,
                           bdb_state_type *bdb_state);
static void cache_stats(FILE *out, bdb_state_type *bdb_state, int extra);
static void sanc_dump(FILE *out, bdb_state_type *bdb_state);
static void netinfo_dump(FILE *out, bdb_state_type *bdb_state);
static void test_send(bdb_state_type *bdb_state);
static void process_add(bdb_state_type *bdb_state, char *host);
static void process_del(bdb_state_type *bdb_state, char *host);
static void process_reptrca(bdb_state_type *bdb_state, int on_off);
void lock_info(FILE *out, bdb_state_type *bdb_state, char *line, int st,
               int lline);
void add_dummy(bdb_state_type *bdb_state);
int dump_llmeta(bdb_state_type *, int *bdberr);
void bdb_show_reptimes(bdb_state_type *bdb_state);
void bdb_lc_cache_trap(bdb_state_type *bdb_state, char *line, int lline);

int __lock_dump_region __P((DB_ENV *, const char *, FILE *));
int __latch_dump_region __P((DB_ENV *, FILE *));
void __dbenv_heap_dump __P((DB_ENV * dbenv));
int __lock_dump_region_int(DB_ENV *, const char *area, FILE *,
                           int just_active_locks);
int __lock_dump_active_locks(DB_ENV *, FILE *);
int __db_cprint(DB *dbp);
char *bdb_coherent_state_string(struct interned_string *host);

static void bdb_queue_extent_info(FILE *out, bdb_state_type *bdb_state,
                                  char *name);

#define prn_stat(x) logmsgf(LOGMSG_USER, out, #x ": %u\n", (unsigned)stats->x)
#define prn_lstat(x) logmsgf(LOGMSG_USER, out, #x ": %" PRId64 "\n", (u_int64_t)stats->x)
#define prn_statstr(x) logmsgf(LOGMSG_USER, out, #x ": %s\n", stats->x)

extern int gbl_namemangle_loglevel;
extern int __db_dump_freepages(DB *dbp, FILE *out);
extern int __memp_dump_region(DB_ENV *dbenv, const char *area, FILE *fp);
extern int bdb_temp_table_insert_test(bdb_state_type *bdb_state, int recsz,
                                      int maxins);
extern int __qam_extent_names(DB_ENV *dbenv, char *name, char ***namelistp);

static pthread_mutex_t hostname_lookup_lk = PTHREAD_MUTEX_INITIALIZER;
int gbl_verbose_hostname_lookup = 0;
static void printf_wrapper(void *userptr, const char *fmt, ...)
{
    va_list args;
    FILE *out = userptr;
    if (!out)
        out = stderr;
    va_start(args, fmt);
    logmsgvf(LOGMSG_USER, out, fmt, args);
    va_end(args);
}

static void ltran_stats(FILE *out, bdb_state_type *bdb_state)
{
    bdb_state->dbenv->txn_dump_ltrans(bdb_state->dbenv, out, 0);
}

static void txn_stats(FILE *out, bdb_state_type *bdb_state)
{
    DB_TXN_STAT *stats;
    DB_TXN_ACTIVE *active;
    int i;
    char str[100];

    bdb_state->dbenv->txn_stat(bdb_state->dbenv, &stats, 0);

    logmsgf(LOGMSG_USER, out, "st_last_ckp: %s\n", lsn_to_str(str, &(stats->st_last_ckp)));
    logmsgf(LOGMSG_USER, out, "st_ckp_lsn: %s\n", lsn_to_str(str, &(stats->st_ckp_lsn)));
    prn_stat(st_time_ckp);
    prn_stat(st_last_txnid);
    prn_stat(st_maxtxns);
    prn_stat(st_nactive);
    prn_stat(st_maxnactive);
    prn_stat(st_nbegins);
    prn_stat(st_naborts);
    prn_stat(st_ncommits);
    prn_stat(st_nrestores);
    prn_stat(st_regsize);
    prn_stat(st_region_wait);
    prn_stat(st_region_nowait);

    active = stats->st_txnarray;
    for (i = 0; i < stats->st_nactive; i++) {
        logmsgf(LOGMSG_USER, out, "active transactions:\n");
        logmsgf(LOGMSG_USER, out, " %d %d %s\n", active->txnid, active->parentid,
                lsn_to_str(str, &(active->lsn)));
        active++;
    }

    free(stats);
}

static void log_stats(FILE *out, bdb_state_type *bdb_state)
{
    DB_LOG_STAT *stats;

    bdb_state->dbenv->log_stat(bdb_state->dbenv, &stats, 0);

    prn_stat(st_magic);
    prn_stat(st_version);
    prn_stat(st_mode);
    prn_stat(st_lg_bsize);
    prn_stat(st_lg_size);
    if (bdb_state->attr->logsegments > 1) {
        prn_stat(st_lg_nsegs);
        prn_stat(st_lg_segsz);
    }
    prn_stat(st_w_mbytes);
    prn_stat(st_w_bytes);
    prn_stat(st_wc_mbytes);
    prn_stat(st_wc_bytes);
    prn_stat(st_wcount);
    prn_stat(st_wcount_fill);
    prn_stat(st_scount);
    prn_stat(st_swrites);
    prn_stat(st_cur_file);
    prn_stat(st_cur_offset);
    prn_stat(st_disk_file);
    prn_stat(st_disk_offset);
    prn_stat(st_maxcommitperflush);
    prn_stat(st_mincommitperflush);
    prn_stat(st_regsize);
    prn_stat(st_region_wait);
    prn_stat(st_region_nowait);
    prn_stat(st_in_cursor_get);
    prn_stat(st_in_region_get);
    prn_stat(st_part_region_get);
    prn_stat(st_ondisk_get);

    if (bdb_state->attr->logsegments > 1) {
        prn_stat(st_wrap_copy);
        prn_stat(st_inmem_trav);
        prn_stat(st_max_td_written);
        prn_stat(st_total_wakeups);
        prn_stat(st_false_wakeups);
        prn_stat(st_inline_writes);
    }

    free(stats);
}

int bdb_get_lock_counters(bdb_state_type *bdb_state, int64_t *deadlocks,
                          int64_t *locks_aborted, int64_t *waits,
                          int64_t *requests)
{
    int rc;
    DB_LOCK_STAT *lock_stats = NULL;

    rc = bdb_state->dbenv->lock_stat(bdb_state->dbenv, &lock_stats, 0);
    if (rc)
        return rc;
    if (deadlocks)
        *deadlocks = lock_stats->st_ndeadlocks;
    if (locks_aborted)
        *locks_aborted = lock_stats->st_locks_aborted;
    if (waits)
        *waits = lock_stats->st_nconflicts;
    if (requests)
        *requests = lock_stats->st_nrequests;

    free(lock_stats);
    return 0;
}

int bdb_get_bpool_counters(bdb_state_type *bdb_state, int64_t *bpool_hits,
                           int64_t *bpool_misses, int64_t *rw_evicts)
{
    int rc;
    DB_MPOOL_STAT *mpool_stats;

    rc = bdb_state->dbenv->memp_stat(bdb_state->dbenv, &mpool_stats, NULL,
                                     0);
    if (rc)
        return rc;

    *bpool_hits = mpool_stats->st_cache_hit;
    *bpool_misses = mpool_stats->st_cache_miss;
    *rw_evicts = mpool_stats->st_rw_evict;

    free(mpool_stats);
    return 0;
}

const char *deadlock_policy_str(u_int32_t policy)
{
    switch (policy) {
    case DB_LOCK_NORUN: return "DB_LOCK_NORUN";
    case DB_LOCK_DEFAULT: return "DB_LOCK_DEFAULT";
    case DB_LOCK_EXPIRE:
        return "DB_LOCK_EXPIRE";
    case DB_LOCK_MAXLOCKS:
        return "DB_LOCK_MAXLOCKS";
    case DB_LOCK_MINLOCKS:
        return "DB_LOCK_MINLOCKS";
    case DB_LOCK_MINWRITE:
        return "DB_LOCK_MINWRITE";
    case DB_LOCK_OLDEST:
        return "DB_LOCK_OLDEST";
    case DB_LOCK_RANDOM:
        return "DB_LOCK_RANDOM";
    case DB_LOCK_YOUNGEST:
        return "DB_LOCK_YOUNGEST";
    case DB_LOCK_MAXWRITE:
        return "DB_LOCK_MAXWRITE";
    case DB_LOCK_MINWRITE_NOREAD:
        return "DB_LOCK_MINWRITE_NOREAD";
    case DB_LOCK_YOUNGEST_EVER:
        return "DB_LOCK_YOUNGEST_EVER";
    case DB_LOCK_MINWRITE_EVER:
        return "DB_LOCK_MINWRITE_EVER";
    default:
        return "UNKNOWN_DEADLOCK_POLICY";
    }
}

int deadlock_policy_max()
{
    return DB_LOCK_MAX;
}

int bdb_lock_stats(bdb_state_type *bdb_state, int64_t *nlocks)
{
    DB_LOCK_STAT *stats = NULL;
    int rc;
    rc = bdb_state->dbenv->lock_stat(bdb_state->dbenv, &stats, 0);
    if (rc)
        return -1;
    *nlocks = stats->st_nlocks;
    free(stats);
    return 0;
}

static void lock_stats(FILE *out, bdb_state_type *bdb_state)
{
    int rc;
    u_int32_t policy;
    extern int gbl_locks_check_waiters;
    extern unsigned long long check_waiters_skip_count;
    extern unsigned long long check_waiters_commit_count;
    extern unsigned long long gbl_rowlocks_deadlock_retries;
    DB_LOCK_STAT *stats = NULL;

    rc = bdb_state->dbenv->lock_stat(bdb_state->dbenv, &stats, 0);

    if (rc != 0) {
        logmsg(LOGMSG_ERROR, "lock_stats returned %d %s\n", rc, db_strerror(rc));
        return;
    }

    rc = bdb_state->dbenv->get_lk_detect(bdb_state->dbenv, &policy);
    if (rc != 0) {
        logmsg(LOGMSG_ERROR, "get_lk_detect returned %d %s\n", rc, db_strerror(rc));
        return;
    }

    logmsgf(LOGMSG_USER, out, "deadlock-detect-policy: %s\n", deadlock_policy_str(policy));
    prn_stat(st_id);
    prn_stat(st_cur_maxid);
    prn_lstat(st_maxlocks);
    prn_lstat(st_maxlockers);
    prn_lstat(st_maxobjects);
    prn_stat(st_nmodes);
    prn_lstat(st_nlocks);
    prn_lstat(st_maxnlocks);
    prn_lstat(st_nlockers);
    prn_lstat(st_maxnlockers);
    prn_lstat(st_nobjects);
    prn_lstat(st_maxnobjects);
    prn_lstat(st_nconflicts);
    prn_lstat(st_nrequests);
    prn_lstat(st_nreleases);
    prn_lstat(st_nnowaits);
    prn_lstat(st_ndeadlocks);
    prn_stat(st_locktimeout);
    prn_lstat(st_nlocktimeouts);
    prn_stat(st_txntimeout);
    prn_lstat(st_ntxntimeouts);
    prn_lstat(st_region_wait);
    prn_lstat(st_region_nowait);
    logmsgf(LOGMSG_USER, out, "locks_check_waiters: %s\n",
            gbl_locks_check_waiters ? "enabled" : "disabled");
    logmsgf(LOGMSG_USER, out, "no_waiter_commit_skips: %llu\n", check_waiters_skip_count);
    logmsgf(LOGMSG_USER, out, "waiter_commits: %llu\n", check_waiters_commit_count);
    logmsgf(LOGMSG_USER, out, "rowlocks_deadlock_retries: %llu\n",
            gbl_rowlocks_deadlock_retries);
    prn_lstat(st_regsize);

    free(stats);
}

static void rep_stats(FILE *out, bdb_state_type *bdb_state)
{
    DB_REP_STAT *stats;
    char str[80];
    extern int64_t gbl_rep_trans_parallel, gbl_rep_trans_serial,
        gbl_rep_trans_deadlocked, gbl_rep_trans_inline,
        gbl_rep_rowlocks_multifile;

    bdb_state->dbenv->rep_stat(bdb_state->dbenv, &stats, 0);

    prn_stat(st_status);
    logmsgf(LOGMSG_USER, out, "st_next_lsn: %s\n", lsn_to_str(str, &(stats->st_next_lsn)));
    logmsgf(LOGMSG_USER, out, "st_waiting_lsn: %s\n",
            lsn_to_str(str, &(stats->st_waiting_lsn)));
    prn_stat(st_dupmasters);
    prn_statstr(st_env_id);
    prn_stat(st_env_priority);
    prn_stat(st_gen);
    prn_stat(st_in_recovery);
    prn_stat(st_log_duplicated);
    prn_stat(st_log_queued);
    prn_stat(st_log_queued_max);
    prn_stat(st_log_queued_total);
    prn_stat(st_log_records);
    prn_stat(st_log_requested);
    prn_statstr(st_master);
    prn_stat(st_master_changes);
    prn_stat(st_msgs_badgen);
    prn_stat(st_msgs_processed);
    prn_stat(st_msgs_recover);
    prn_stat(st_msgs_send_failures);
    prn_stat(st_msgs_sent);
    prn_stat(st_newsites);
    prn_stat(st_nsites);
    prn_stat(st_outdated);
    prn_stat(st_txns_applied);
    prn_stat(st_elections);
    prn_stat(st_elections_won);
    prn_stat(st_election_status);
    prn_statstr(st_election_cur_winner);
    prn_stat(st_election_gen);
    logmsgf(LOGMSG_USER, out, "st_election_lsn: %s\n",
            lsn_to_str(str, &(stats->st_election_lsn)));

    prn_stat(st_election_nsites);
    prn_stat(st_nthrottles);
    prn_stat(st_election_priority);
    prn_stat(st_election_tiebreaker);
    prn_stat(st_election_votes);

    logmsgf(LOGMSG_USER, out, "txn parallel: %" PRId64 "\n",
            gbl_rep_trans_parallel);
    logmsgf(LOGMSG_USER, out, "txn serial: %" PRId64 "\n",
            gbl_rep_trans_serial);
    logmsgf(LOGMSG_USER, out, "txn inline: %" PRId64 "\n",
            gbl_rep_trans_inline);
    logmsgf(LOGMSG_USER, out, "txn multifile rowlocks: %" PRId64 "\n",
            gbl_rep_rowlocks_multifile);
    logmsgf(LOGMSG_USER, out, "txn deadlocked: %" PRId64 "\n",
            gbl_rep_trans_deadlocked);
    prn_lstat(lc_cache_hits);
    prn_lstat(lc_cache_misses);
    prn_stat(lc_cache_size);
    logmsgf(LOGMSG_USER, out, "durable lsn: [%d][%d] generation %u\n", 
            stats->durable_lsn.file, stats->durable_lsn.offset, 
            stats->durable_gen);
    free(stats);
}

void bdb_get_rep_stats(bdb_state_type *bdb_state,
                       unsigned long long *msgs_processed,
                       unsigned long long *msgs_sent,
                       unsigned long long *txns_applied,
                       unsigned long long *retry, int *max_retry)
{
    DB_REP_STAT *rep_stats;

    if (bdb_state->parent)
        bdb_state = bdb_state->parent;

    bdb_state->dbenv->rep_stat(bdb_state->dbenv, &rep_stats, 0);

    *msgs_processed = rep_stats->st_msgs_processed;
    *msgs_sent = rep_stats->st_msgs_sent;
    *txns_applied = rep_stats->st_txns_applied;
    *retry = rep_stats->retry;
    *max_retry = rep_stats->max_replication_trans_retries;

    free(rep_stats);
}

static char *genid_format_str(int format)
{
    if (format == LLMETA_GENID_48BIT)
        return "48-BIT";
    if (format == LLMETA_GENID_ORIGINAL)
        return "ORIGINAL";
    return "UNKNOWN/BROKEN";
}

void bdb_dump_freelist(FILE *out, int datafile, int stripe, int ixnum,
                       bdb_state_type *bdb_state)
{
    if (ixnum == -1 && datafile == -1 && stripe == -1) {
        int ix, df, st;
        for (ix = 0; ix < bdb_state->numix; ix++) {
            logmsgf(LOGMSG_USER, out, "%s ix %u\n", bdb_state->name, ix);
            __db_dump_freepages(bdb_state->dbp_ix[ix], out);
            logmsgf(LOGMSG_USER, out, "\n");
        }

        for (df = 0; df < bdb_state->numdtafiles; df++) {
            for (st = 0; st < bdb_state->attr->dtastripe; st++) {
                logmsgf(LOGMSG_USER, out, "%s datafile %u stripe %u\n", bdb_state->name, df,
                        st);
                __db_dump_freepages(bdb_state->dbp_data[df][st], out);
                logmsgf(LOGMSG_USER, out, "\n");
            }
        }
        return;
    }
    if (ixnum >= 0) {
        if (ixnum >= bdb_state->numix) {
            logmsgf(LOGMSG_USER, out, "Index is out of range\n");
            return;
        }

        __db_dump_freepages(bdb_state->dbp_ix[ixnum], out);
        return;
    } else {
        if (stripe < 0 || stripe >= bdb_state->attr->dtastripe) {
            logmsgf(LOGMSG_USER, out, "stripe is out of range\n");
            return;
        }
        if (datafile < 0 || datafile >= bdb_state->numdtafiles) {
            logmsgf(LOGMSG_USER, out, "datafile is out of range\n");
            return;
        }
        if (stripe > 0 && datafile > 0 && !bdb_state->attr->blobstripe) {
            logmsgf(LOGMSG_USER, out, "stripe is set for a blob but db isn't blobstripe\n");
            return;
        }

        __db_dump_freepages(bdb_state->dbp_data[datafile][stripe], out);
        return;
    }
}

static void bdb_state_dump(FILE *out, const char *prefix,
                           bdb_state_type *bdb_state)
{
    int ii;
    char namebuf[64];

    logmsgf(LOGMSG_USER, out, "%s->name = %s\n", prefix, bdb_state->name);
    logmsgf(LOGMSG_USER, out, "%s->isopen = %d\n", prefix, bdb_state->isopen);
    logmsgf(LOGMSG_USER, out, "%s->read_write = %d\n", prefix, bdb_state->read_write);
    logmsgf(LOGMSG_USER, out, "%s->master_cmpcontext = 0x%08llx\n", prefix,
            bdb_state->master_cmpcontext);
    logmsgf(LOGMSG_USER, out, "%s->got_gblcontext = %d (gblcontext=0x%llx)\n",
            prefix, bdb_state->got_gblcontext, get_gblcontext(bdb_state));
    logmsgf(LOGMSG_USER, out, "%s->genid_format = %s\n", prefix,
            genid_format_str(bdb_state->genid_format));

    for (ii = 0; ii < bdb_state->numchildren; ii++) {
        if (bdb_state->children[ii]) {
            snprintf(namebuf, sizeof(namebuf), "  %s->child[%d]", prefix, ii);
            bdb_state_dump(out, namebuf, bdb_state->children[ii]);
        }
    }
}

static void cache_info(FILE *out, bdb_state_type *bdb_state)
{
    bdb_state->dbenv->memp_dump_bufferpool_info(bdb_state->dbenv, out);
}

static void cache_stats(FILE *out, bdb_state_type *bdb_state, int extra)
{
    DB_MPOOL_STAT *stats;
    DB_MPOOL_FSTAT **fsp, **i;

    bdb_state->dbenv->memp_stat(bdb_state->dbenv, &stats, extra ? &fsp : NULL,
                                0);

    prn_lstat(st_gbytes);
    prn_lstat(st_bytes);
    prn_lstat(st_total_bytes);
    prn_lstat(st_used_bytes);
    logmsgf(LOGMSG_USER, out, "st_ff_pct: %d\n", (int)(stats->st_used_bytes * 100 / stats->st_total_bytes));
    prn_lstat(st_ncache);
    prn_lstat(st_regsize);
    prn_lstat(st_map);
    prn_lstat(st_cache_hit);
    prn_lstat(st_cache_miss);
    prn_lstat(st_cache_ihit);
    prn_lstat(st_cache_imiss);
    prn_lstat(st_cache_lhit);
    prn_lstat(st_cache_lmiss);
    prn_lstat(st_page_pf_in);
    prn_lstat(st_page_pf_in_late);
    prn_lstat(st_page_in);
    prn_lstat(st_page_out);
    prn_lstat(st_ro_merges);
    prn_lstat(st_rw_merges);
    prn_lstat(st_ro_evict);
    prn_lstat(st_rw_evict);
    prn_lstat(st_ro_levict);
    prn_lstat(st_rw_levict);
    prn_lstat(st_pf_evict);
    prn_lstat(st_rw_evict_skip);
    prn_lstat(st_page_trickle);
    prn_lstat(st_pages);
    prn_lstat(st_page_clean);
    logmsgf(LOGMSG_USER, out, "st_page_dirty: %d\n", stats->st_page_dirty);
    prn_lstat(st_hash_buckets);
    prn_lstat(st_hash_searches);
    prn_lstat(st_hash_longest);
    prn_lstat(st_hash_examined);
    prn_lstat(st_hash_nowait);
    prn_lstat(st_hash_wait);
    prn_lstat(st_hash_max_wait);
    prn_lstat(st_region_wait);
    prn_lstat(st_region_nowait);
    prn_lstat(st_alloc);
    prn_lstat(st_alloc_buckets);
    prn_lstat(st_alloc_max_buckets);
    prn_lstat(st_alloc_pages);
    prn_lstat(st_alloc_max_pages);
    prn_lstat(st_ckp_pages_sync);
    prn_lstat(st_ckp_pages_skip);

    if (extra) {
        bdb_state->dbenv->memp_dump_region(bdb_state->dbenv, "A", out);

        for (i = fsp; i != NULL && *i != NULL; ++i) {
            logmsgf(LOGMSG_USER, out, "Pool file [%s]:-\n", (*i)->file_name);
            logmsgf(LOGMSG_USER, out, "  st_pagesize   : %zu\n",
                    (*i)->st_pagesize);
            logmsgf(LOGMSG_USER, out, "  st_map        : %"PRId64"\n", (*i)->st_map);
            logmsgf(LOGMSG_USER, out, "  st_cache_hit  : %"PRId64"\n", (*i)->st_cache_hit);
            logmsgf(LOGMSG_USER, out, "  st_cache_miss : %"PRId64"\n", (*i)->st_cache_miss);
            logmsgf(LOGMSG_USER, out, "  st_cache_ihit : %"PRId64"\n", (*i)->st_cache_ihit);
            logmsgf(LOGMSG_USER, out, "  st_cache_imiss: %"PRId64"\n", (*i)->st_cache_imiss);
            logmsgf(LOGMSG_USER, out, "  st_cache_lhit : %"PRId64"\n", (*i)->st_cache_lhit);
            logmsgf(LOGMSG_USER, out, "  st_cache_lmiss: %"PRId64"\n", (*i)->st_cache_lmiss);
            logmsgf(LOGMSG_USER, out, "  st_page_create: %"PRId64"\n", (*i)->st_page_create);
            logmsgf(LOGMSG_USER, out, "  st_page_in    : %"PRId64"\n", (*i)->st_page_in);
            logmsgf(LOGMSG_USER, out, "  st_page_out   : %"PRId64"\n", (*i)->st_page_out);
        }

        free(fsp);
    }

    free(stats);
}

static void temp_cache_stats(FILE *out, bdb_state_type *bdb_state)
{
    DB_MPOOL_STAT *stats;

    bdb_temp_table_stat(bdb_state, &stats);

    prn_lstat(st_cache_hit);
    prn_lstat(st_cache_miss);
    prn_lstat(st_cache_ihit);
    prn_lstat(st_cache_imiss);
    prn_lstat(st_cache_lhit);
    prn_lstat(st_cache_lmiss);
    prn_lstat(st_page_pf_in);
    prn_lstat(st_page_in);
    prn_lstat(st_page_out);
    prn_lstat(st_ro_merges);
    prn_lstat(st_rw_merges);
    prn_lstat(st_ro_evict);
    prn_lstat(st_rw_evict);
    prn_lstat(st_pf_evict);
    prn_lstat(st_rw_evict_skip);
    prn_lstat(st_page_trickle);
    prn_lstat(st_hash_searches);
    prn_lstat(st_hash_longest);
    prn_lstat(st_hash_examined);
    prn_lstat(st_hash_nowait);
    prn_lstat(st_hash_wait);
    prn_lstat(st_hash_max_wait);
    prn_lstat(st_region_wait);
    prn_lstat(st_region_nowait);
    prn_lstat(st_alloc);
    prn_lstat(st_alloc_buckets);
    prn_lstat(st_alloc_max_buckets);
    prn_lstat(st_alloc_pages);
    prn_lstat(st_alloc_max_pages);

    free(stats);
}

static void sanc_dump(FILE *out, bdb_state_type *bdb_state)
{
    const char *nodes[REPMAX];
    int numnodes, ii;

    numnodes = net_get_sanctioned_node_list(bdb_state->repinfo->netinfo, REPMAX,
                                            nodes);

    logmsgf(LOGMSG_USER, out, "sanc dump:\n");
    for (ii = 0; ii < numnodes && ii < REPMAX; ii++)
        logmsgf(LOGMSG_USER, out, "node %s\n", nodenum_str((char *)nodes[ii]));

    if (net_sanctioned_list_ok(bdb_state->repinfo->netinfo))
        logmsgf(LOGMSG_USER, out, "sanc is intact\n");
    else
        logmsgf(LOGMSG_USER, out, "sanc nodes are missin\n");
}

void fill_ssl_info(CDB2DBINFORESPONSE *dbinfo_response)
{
    extern ssl_mode gbl_client_ssl_mode;
    if (gbl_client_ssl_mode <= SSL_UNKNOWN)
        return;
    dbinfo_response->has_require_ssl = 1;
    dbinfo_response->require_ssl = SSL_IS_REQUIRED(gbl_client_ssl_mode);
}

void fill_dbinfo(CDB2DBINFORESPONSE *dbinfo_response, bdb_state_type *bdb_state)
{
    struct host_node_info nodes[REPMAX];
    int num_nodes = 0, i = 0;

    num_nodes = net_get_nodes_info(bdb_state->repinfo->netinfo, REPMAX, nodes);

    dbinfo_response->n_nodes = num_nodes;
    CDB2DBINFORESPONSE__Nodeinfo **nodeinfos =
        malloc(sizeof(CDB2DBINFORESPONSE__Nodeinfo *) * num_nodes);
    CDB2DBINFORESPONSE__Nodeinfo *master =
        malloc(sizeof(CDB2DBINFORESPONSE__Nodeinfo));
    cdb2__dbinforesponse__nodeinfo__init(master);
    int our_room = 0;

    dbinfo_response->has_sync_mode = 1;
    dbinfo_response->sync_mode = bdb_state->callback->syncmode_rtn(bdb_state);

    if (bdb_state->callback->getroom_rtn)
        our_room = (bdb_state->callback->getroom_rtn(
            bdb_state, bdb_state->repinfo->myhost));

    for (int j = 0; j < num_nodes && j < REPMAX; j++) {
        if (our_room ==
            bdb_state->callback->getroom_rtn(bdb_state, nodes[j].host)) {
            nodeinfos[i] = malloc(sizeof(CDB2DBINFORESPONSE__Nodeinfo));
            cdb2__dbinforesponse__nodeinfo__init(nodeinfos[i]);
            nodeinfos[i]->number = j;
            nodeinfos[i]->port = nodes[j].port;
            nodeinfos[i]->has_port = 1;
            nodeinfos[i]->has_room = 1;
            nodeinfos[i]->room =
                bdb_state->callback->getroom_rtn(bdb_state, nodes[j].host);
            nodeinfos[i]->name = strdup(nodes[j].host);
            if (strcmp(bdb_state->repinfo->master_host, nodes[j].host) == 0) {
                master->number = j;
                master->incoherent = 0;
                master->has_port = 1;
                master->has_room = 1;
                master->room =
                    bdb_state->callback->getroom_rtn(bdb_state, nodes[j].host);
                master->port = nodes[j].port;
                master->name = strdup(nodes[j].host);
            }
            /* We can only query the master for cluster-coherent state */
            nodeinfos[i]->incoherent = 0;
            if (bdb_state->repinfo->myhost == bdb_state->repinfo->master_host) {
                struct hostinfo *h = retrieve_hostinfo(nodes[j].host_interned);
                if (h->coherent_state != STATE_COHERENT) {
                    nodeinfos[i]->incoherent = 1;
                } else {
                    nodeinfos[i]->incoherent = 0;
                }
            }
            else {
                nodeinfos[i]->incoherent = 0;
            }
            i++;
        }
    }

    for (int j = 0; j < num_nodes && j < REPMAX; j++) {
        if (our_room !=
            bdb_state->callback->getroom_rtn(bdb_state, nodes[j].host)) {
            nodeinfos[i] = malloc(sizeof(CDB2DBINFORESPONSE__Nodeinfo));
            cdb2__dbinforesponse__nodeinfo__init(nodeinfos[i]);
            nodeinfos[i]->number = j;
            nodeinfos[i]->port = nodes[j].port;
            nodeinfos[i]->has_port = 1;
            nodeinfos[i]->has_room = 1;
            nodeinfos[i]->room =
                bdb_state->callback->getroom_rtn(bdb_state, nodes[j].host);
            nodeinfos[i]->name = strdup(nodes[j].host);
            if (strcmp(bdb_state->repinfo->master_host, nodes[j].host) == 0) {
                master->number = j;
                master->incoherent = 0;
                master->has_port = 1;
                master->port = nodes[j].port;
                master->has_room = 1;
                master->room =
                    bdb_state->callback->getroom_rtn(bdb_state, nodes[j].host);
                master->name = strdup(nodes[j].host);
            }
            struct hostinfo *h = retrieve_hostinfo(nodes[j].host_interned);
            if (h->coherent_state != STATE_COHERENT) {
                nodeinfos[i]->incoherent = 1;
            } else {
                nodeinfos[i]->incoherent = 0;
            }
            i++;
        }
    }

    dbinfo_response->nodes = nodeinfos;
    dbinfo_response->master = master;

    fill_ssl_info(dbinfo_response);
}

static void netinfo_dump_hostname(FILE *out, bdb_state_type *bdb_state)
{
    struct host_node_info nodes[REPMAX];
    int num_nodes, ii;

    num_nodes = net_get_nodes_info(bdb_state->repinfo->netinfo, REPMAX, nodes);

    logmsgf(LOGMSG_USER, out, "db engine cluster status\n");
    if (bdb_state->repinfo->master_host != bdb_state->repinfo->myhost) {
        logmsgf(LOGMSG_USER, out, "WARNING: this information is only valid on the master\n");
        logmsgf(LOGMSG_USER, out, "Use 'stat' to find out the coherent state of this node\n");
    }

    for (ii = 0; ii < num_nodes && ii < REPMAX; ii++) {
        char *status;
        char *status_mstr;
        DB_LSN *lsnp;
        char str[100];
        char *coherent_state;

        if (nodes[ii].host == net_get_mynode(bdb_state->repinfo->netinfo))
            status = "l";
        else if (nodes[ii].fd <= 0) {
            if ((bdb_state->callback->nodeup_rtn) &&
                ((bdb_state->callback->nodeup_rtn(bdb_state, nodes[ii].host))))
                status = "n";
            else
                status = "o";
        } else {
            status = "c";
        }
        if (bdb_state->repinfo->master_host == nodes[ii].host) {
            status_mstr = "MASTER";
        } else {
            status_mstr = " ";
        }

        struct hostinfo *h = retrieve_hostinfo(nodes[ii].host_interned);
        lsnp = &h->seqnum.lsn;

        coherent_state = bdb_coherent_state_string(nodes[ii].host_interned);

        logmsgf(LOGMSG_USER, out, "%16s:%d %-6s %-1s fd %-3d lsn %s f %d %s\n",
                nodes[ii].host, nodes[ii].port, status_mstr, status,
                nodes[ii].fd, lsn_to_str(str, lsnp),
                h->filenum, coherent_state);
    }
}
netinfo_dumper *netinfo_dump_impl = netinfo_dump_hostname;
void set_netinfo_dumper(netinfo_dumper *d)
{
    netinfo_dump_impl = d;
}
static void netinfo_dump(FILE *out, bdb_state_type *bdb_state)
{
    netinfo_dump_impl(out, bdb_state);
}

/* This is public (called by db layer) and used for the incoherent
 * alerts, so don't fiddle with the format withouyt taking that into
 * account. */
void bdb_short_netinfo_dump(FILE *out, bdb_state_type *bdb_state)
{
    struct host_node_info nodes[REPMAX];
    int num_nodes, ii;

    num_nodes = net_get_nodes_info(bdb_state->repinfo->netinfo, REPMAX, nodes);

    for (ii = 0; ii < num_nodes && ii < REPMAX; ii++) {
        char *status;
        char *status_mstr;
        DB_LSN *lsnp;
        char str[100];

        if (strcmp(nodes[ii].host,
                   net_get_mynode(bdb_state->repinfo->netinfo)) == 0)
            status = "l";
        else if (nodes[ii].fd <= 0) {
            if ((bdb_state->callback->nodeup_rtn) &&
                ((bdb_state->callback->nodeup_rtn(bdb_state, nodes[ii].host))))
                status = "n";
            else
                status = "o";
        } else {
            status = "c";
        }
        struct hostinfo *h = retrieve_hostinfo(nodes[ii].host_interned);
        if (bdb_state->repinfo->master_host == nodes[ii].host)
            status_mstr = "MASTER";
        else {
            switch (h->coherent_state) {
            case STATE_INCOHERENT:
            case STATE_INCOHERENT_WAIT:
            case STATE_INCOHERENT_SLOW:
            default:
                status_mstr = coherent_state_to_str(h->coherent_state);
                break;
            case STATE_COHERENT:
                status_mstr = " ";
                break;
            }
        }

        lsnp = &h->seqnum.lsn;

        logmsgf(LOGMSG_USER, out, "%10s %-1s lsn %s %s\n", nodes[ii].host, status,
                lsn_to_str(str, lsnp), status_mstr);
    }
}

/* some kind of node validation would be required */
void bdb_get_cur_lsn_str_node(bdb_state_type *bdb_state, uint64_t *lsnbytes,
                              char *lsnstr, size_t len, char *host)
{
    char buf[64];
    struct interned_string *host_interned = intern_ptr(host);
    struct hostinfo *h = retrieve_hostinfo(host_interned);
    DB_LSN *lsn = &h->seqnum.lsn;
    lsn_to_str(buf, lsn);
    strncpy0(lsnstr, buf, len);
    *lsnbytes = ((uint64_t)lsn->file * (uint64_t)bdb_state->attr->logfilesize) +
                (uint64_t)lsn->offset;
}

void bdb_get_cur_lsn_str(bdb_state_type *bdb_state, uint64_t *lsnbytes,
                         char *lsnstr, size_t len)
{
    bdb_get_cur_lsn_str_node(bdb_state, lsnbytes, lsnstr, len,
                             net_get_mynode(bdb_state->repinfo->netinfo));
}

static void test_send(bdb_state_type *bdb_state)
{
    int count;
    const char *hostlist[REPMAX];
    int i;
    int rc;

    count = net_get_all_nodes_connected(bdb_state->repinfo->netinfo, hostlist);
    for (i = 0; i < count; i++) {
        int tmp;
        uint8_t *p_buf, *p_buf_end;

        p_buf = (uint8_t *)&tmp;
        p_buf_end = (uint8_t *)&tmp + sizeof(int);
        buf_put(&i, sizeof(int), p_buf, p_buf_end);

        rc = net_send_message(bdb_state->repinfo->netinfo, hostlist[i],
                              USER_TYPE_TEST, &tmp, sizeof(int), 1, 60 * 1000);
        if (rc != 0)
            logmsg(LOGMSG_ERROR, "got bad rc %d in test_send\n", rc);
    }
}

static int write_hostname(netinfo_type *net, const char *to, char *host, int type)
{
    int len = strlen(host) + 1;
    return net_send_message(net, to, type, host, len, 1, 5 * 1000);
}

static int write_add_hostname(netinfo_type *netinfo, const char *to, char *host)
{
    return write_hostname(netinfo, to, host, USER_TYPE_ADD_NAME);
}
static add_writer *write_add_impl = write_add_hostname;
void set_add_writer(add_writer *impl)
{
    write_add_impl = impl;
}
int write_add(netinfo_type *n, const char *t, char *a)
{
    return write_add_impl(n, t, a);
}

static int write_del_hostname(netinfo_type *netinfo, const char *to, char *host)
{
    return write_hostname(netinfo, to, host, USER_TYPE_DEL_NAME);
}
static del_writer *write_del_impl = write_del_hostname;
void set_del_writer(del_writer *impl)
{
    write_del_impl = impl;
}
int write_del(netinfo_type *n, const char *t, char *a)
{
    return write_del_impl(n, t, a);
}

static void process_add(bdb_state_type *bdb_state, char *host)
{
    const char *hostlist[REPMAX];
    netinfo_type *netinfo = bdb_state->repinfo->netinfo;
    net_add_to_sanctioned(netinfo, host, 0);
    int count = net_get_all_nodes_connected(netinfo, hostlist);
    for (int i = 0; i < count; i++) {
        int rc = write_add(netinfo, hostlist[i], host);
        if (rc != 0)
            logmsg(LOGMSG_ERROR, "got bad rc %d in process_add\n", rc);
    }
}
static void process_del(bdb_state_type *bdb_state, char *host)
{
    const char *hostlist[REPMAX];
    netinfo_type *netinfo = bdb_state->repinfo->netinfo;
    net_del_from_sanctioned(netinfo, host);
    int count = net_get_all_nodes_connected(netinfo, hostlist);
    for (int i = 0; i < count; i++) {
        int rc = write_del(netinfo, hostlist[i], host);
        if (rc != 0)
            logmsg(LOGMSG_ERROR, "got bad rc %d in process_del\n", rc);
    }
}

static void process_reptrca(bdb_state_type *bdb_state, int on_off)
{
    int count;
    const char *hostlist[REPMAX];
    int i;
    int rc;

    logmsgf(LOGMSG_USER, stderr, "setting rep tracing to %d on all nodes\n", on_off);

    bdb_state->rep_trace = on_off;

    count = net_get_all_nodes(bdb_state->repinfo->netinfo, hostlist);
    for (i = 0; i < count; i++) {
        int tmp;
        uint8_t *p_buf, *p_buf_end;

        p_buf = (uint8_t *)&tmp;
        p_buf_end = (uint8_t *)&tmp + sizeof(int);
        buf_put(&on_off, sizeof(int), p_buf, p_buf_end);

        rc = net_send_message(bdb_state->repinfo->netinfo, hostlist[i],
                              USER_TYPE_REPTRC, &tmp, sizeof(int), 1, 5 * 1000);
        if (rc != 0) {
            logmsg(LOGMSG_ERROR, "trouble sending to node %s\n", hostlist[i]);
        }
    }
}

void bdb_dump_active_locks(bdb_state_type *bdb_state, FILE *out)
{
    if (bdb_state == NULL)
        bdb_state = gbl_bdb_state;
    __lock_dump_active_locks(bdb_state->dbenv, out);
}

void bdb_lock_stats_me(bdb_state_type *bdb_state, FILE *out)
{
    if (bdb_state == NULL)
        bdb_state = gbl_bdb_state;
    __lock_dump_region_int(bdb_state->dbenv, "t", out, 0);
}

void bdb_dump_cache(bdb_state_type *bdb_state, FILE *out)
{
    __memp_dump_region(bdb_state->dbenv, "A", out);
}

void bdb_truncate_repdb(bdb_state_type *bdb_state, FILE *out)
{
    int ret;

    BDB_WRITELOCK("truncate_repdb");
    ret = bdb_state->dbenv->rep_truncate_repdb(bdb_state->dbenv);
    BDB_RELLOCK();

    if (ret == 0)
        logmsgf(LOGMSG_USER, out, "Truncated repdb.\n");
    else
        logmsgf(LOGMSG_USER, out, "Error truncating repdb, ret=%d\n", ret);
}

void bdb_dump_cursors(bdb_state_type *bdb_state, FILE *out)
{
    int dbn, blob, stripe, ix;
    bdb_state_type *db;

    BDB_READLOCK("dumpcurlist");
    for (dbn = 0; dbn < bdb_state->numchildren; dbn++) {
        db = bdb_state->children[dbn];

        if (db) {
            for (blob = 0; blob < db->numdtafiles; blob++) {
                for (stripe = 0; stripe < bdb_state->attr->dtastripe + 1 &&
                                 db->dbp_data[blob][stripe];
                     stripe++) {
                    logmsg(LOGMSG_USER, "%s dta %d stripe %d:\n", db->name, blob, stripe);
                    __db_cprint(db->dbp_data[blob][stripe]);
                }
            }

            for (ix = 0; ix < db->numix && db->dbp_ix[ix]; ix++) {
                logmsg(LOGMSG_USER, "%s ix %d\n", db->name, ix);
                __db_cprint(db->dbp_ix[ix]);
            }
        }
    }
    BDB_RELLOCK();
}

void bdb_dump_cursor_count(bdb_state_type *bdb_state, FILE *out)
{
    logmsgf(LOGMSG_USER, out, "Number of cursors: %d\n",
            __dbenv_count_cursors_dbenv(bdb_state->dbenv));
}

struct bdb_thread_args {
    void (*func)(bdb_state_type *);
    bdb_state_type *bdb_state;
};

static void *bdb_thread_wrapper(void *p)
{
    struct bdb_thread_args *args = (struct bdb_thread_args *)p;

    thread_started("bdb_thread");
    bdb_thread_event(args->bdb_state, BDBTHR_EVENT_START_RDWR);
    args->func(args->bdb_state);
    bdb_thread_event(args->bdb_state, BDBTHR_EVENT_DONE_RDWR);
    free(args);
    return NULL;
}

int bdb_run_in_a_thread(bdb_state_type *bdb_state,
                        void (*func)(bdb_state_type *))
{
    pthread_t tid;
    struct bdb_thread_args *args;

    pthread_attr_t attr;
    Pthread_attr_init(&attr);
    Pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    Pthread_attr_setstacksize(&attr, 1024 * 128);
    args = malloc(sizeof(struct bdb_thread_args));
    if (args == NULL) {
        Pthread_attr_destroy(&attr);
        errno = ENOMEM;
        return -1;
    }
    args->bdb_state = bdb_state;
    args->func = func;

    Pthread_create(&tid, &attr, bdb_thread_wrapper, args);
    Pthread_attr_destroy(&attr);
    return 0;
}

static volatile int count_freepages_abort = 0;

void bdb_count_freepages_abort(void)
{
    __berkdb_count_freeepages_abort();
    count_freepages_abort = 1;
}

/* TODO: use stdio instead */
uint64_t bdb_dump_freepage_info_table(bdb_state_type *bdb_state, FILE *out)
{
    int stripe, blobno, ix;
    int fd = -1;
    char fname[PATH_MAX];
    char tmpname[PATH_MAX];
    int numstripes;
    int bdberr;
    unsigned int npages;
    uint64_t total_npages = 0;

    numstripes = bdb_state->attr->dtastripe ? bdb_state->attr->dtastripe : 1;

    for (stripe = 0; stripe < numstripes; stripe++) {
        for (blobno = 0; blobno < MAXDTAFILES; blobno++) {
            if (bdb_state->dbp_data[blobno][stripe]) {
                if (bdb_get_data_filename(bdb_state, stripe, blobno, tmpname,
                                          sizeof(tmpname), &bdberr) == 0) {
                    bdb_trans(tmpname, fname);
                    fd = open(fname, O_RDONLY);
                    if (fd == -1) {
                        logmsg(LOGMSG_ERROR, "open(\"%s\") => %d %s\n", fname, errno,
                               strerror(errno));
                        continue;
                    }

                    npages = __berkdb_count_freepages(fd);
                    total_npages += npages;
                    Close(fd);
                    if (count_freepages_abort)
                        return 0;
                    if (blobno == 0)
                        logmsg(LOGMSG_USER, "  %s data stripe %d  => %u\n", bdb_state->name,
                               stripe, npages);
                    else
                        logmsg(LOGMSG_INFO, "  %s blob %d stripe %d   => %u\n",
                               bdb_state->name, blobno, stripe, npages);
                }
            }
        }
    }
    for (ix = 0; ix < bdb_state->numix; ix++) {
        if (bdb_get_index_filename(bdb_state, ix, tmpname, sizeof(tmpname),
                                   &bdberr) == 0) {
            bdb_trans(tmpname, fname);
            fd = open(fname, O_RDONLY);
            if (fd == -1) {
                logmsg(LOGMSG_ERROR, "open(\"%s\") => %d %s\n", fname, errno,
                       strerror(errno));
                continue;
            }

            npages = __berkdb_count_freepages(fd);
            total_npages += npages;
            Close(fd);
            if (count_freepages_abort)
                return 0;
            logmsg(LOGMSG_USER, "  %s ix %d   => %u\n", bdb_state->name, ix, npages);
        }
    }
    logmsg(LOGMSG_USER, "total freelist pages for %s: %" PRIu64 "\n",
           bdb_state->name, total_npages);
    return total_npages;
}

void bdb_dump_freepage_info_all(bdb_state_type *bdb_state)
{
    int i;
    uint64_t npages = 0;

    count_freepages_abort = 0;

    for (i = 0; i < bdb_state->numchildren; i++) {
        if (bdb_state->children[i] &&
            bdb_state->children[i]->bdbtype == BDBTYPE_TABLE)
            npages +=
                bdb_dump_freepage_info_table(bdb_state->children[i], stdout);
        if (count_freepages_abort) {
            logmsg(LOGMSG_USER, "freepage count aborted\n");
            return;
        }
    }
    logmsg(LOGMSG_USER, "total free pages: %" PRIu64 "\n", npages);
}

static const char *find_host_in_list(bdb_state_type *bdb_state, const char *host,
                                     const char **hostlist, int nhosts) 
{
    size_t hlen = strlen(host);
    const char *fnd = NULL;
    int multiple = 0;
    char *me;

    me = bdb_state->repinfo->myhost;

    /* Is it me? Do this check since net_get_all_nodes doesn't return the
     * current machine. */
    if (strncmp(me, host, hlen) == 0 && (me[hlen] == '.' || me[hlen] == 0))
        fnd = me;

    for (int i = 0; i < nhosts; i++) {
        if (strncmp(hostlist[i], host, hlen) == 0 &&
            (hostlist[i][hlen] == '.' || hostlist[i][hlen] == 0)) {
            if (fnd) {
                if (!multiple)
                    logmsg(LOGMSG_ERROR, "host matches multiple machines:\n");
                multiple = 1;
                logmsg(LOGMSG_ERROR, "   %s\n", hostlist[i]);
            } else
                fnd = hostlist[i];
        }
    }
    if (multiple)
        fnd = NULL;
    return fnd;
}

extern netinfo_type *osql_get_netinfo(void);
const char *bdb_find_net_host(bdb_state_type *bdb_state, const char *host)
{
    int nhosts;
    const char *hosts[REPMAX];
    const char *fnd = NULL;

    nhosts = net_get_all_nodes(bdb_state->repinfo->netinfo, hosts);
    fnd = find_host_in_list(bdb_state, host, hosts, nhosts);
    if (fnd == NULL) {
        nhosts = net_get_all_nodes(osql_get_netinfo(), hosts);
        fnd = find_host_in_list(bdb_state, host, hosts, nhosts);
    }

    return fnd;
}

/* return interned hostname */
static char *tokdup_hostname(char *tok, int ltok)
{
    tok = tokdup(tok, ltok);
    char *host = intern(hostname_str(tok));
    free(tok);
    return host;
}

void bdb_process_user_command(bdb_state_type *bdb_state, char *line, int lline,
                              int st)
{
    int ltok;
    char *tok;
    int rc;
    FILE *out = stderr;
    FILE *logfile = NULL;
    static const char *help[] = {
        "backend engine commands",
        "*bbstat         - dump hacked up bloomberg stats",
        "*bdbstat        - general backend status",
        "*cluster        - cluster status", 
        "*cachestat      - cache stats",
        " cachestatall   - cache stats and dump of memory pool",
        " cacheinfo      - list files, pages, & priorities of mpool buffers",
        " clminfo        - print commit lsn map internal structures",
        " tempcachestat  - cache stats for temp region",
        " tempcachestatall - cache stats and dump of temp region memory pool",
        " tempcacheinfo  - list files, pages, & priorities of temp mpool "
        "buffers",
        "*repstat        - replication stats",
        "*bdbstate       - dump bdb state information",
        "*logstat        - log stats", 
        "*txnstat        - transaction stats",
        "*ltranstat      - logical transaction stats",
        "*lockstat       - lock subsystem stats",
        " fulldiag       - dump loads of stuff - please use with f prefix",
        "*sanc           - list 'sanctioned' cluster members",
        " repdbg[yn]     - verbose replication yes/no",
        " checkpoint     - force a checkpoint",
        "*log_archive    - list log files that may be safely deleted",
        " add #          - add node # to sanc list",
        " del #          - remove node # from sanc list",
        " rem #          - force removal node # from cluster",
        " hostname #     - print hostname associated with node #", 
        " dummy          - add a dummy record",
        " reptrcy        - turn on replication trace",
        " reptrcn        - turn off replication trace",
        " reptrcay       - turn on replication trace on all nodes",
        " reptrcan       - turn off replication trace on all nodes",
        " oslog #        - set os_namemangle log level (0=none, 1=normal, "
        "2=spew)",
        " dblist         - dump berkeley's list of open files",
        " alldblist      - dump all of the entries in berkeley's dblist structure",
        " curlist        - dump berkeley's cursor list for all dbs",
        " curcount       - dump count of berkeley cursors allocated",
        " lockinfo       - old style lock information",
        " activelocks    - dump all active locks",
        " truncrepdb     - truncate repdb",
        " dumpcache      - dump berkeley cache",
        "*attr           - dump attributes",
        " setattr name # - set value of attribute to #",
        " setskip # 1/0  - mark node # as coherent (1) or incoherent (0)",
        " f <name> ..    - redirect answer to command .. to file <name>",
        " memdump        - dump region memory usage",
        "*bdblockdump    - dump bdb lock thread info structures",
        " llmeta         - dump llmeta information",
        " freepages      - dump free page counts",
        " lccache        - lsn collection cache commands",
        " temptable      - temptable status", 
        "*help           - this",
        "NB '*' means you can run the command via stat e.g.",
        "'send mydb stat bdb cluster'"};
    static char *safecmds[] = {
        "bdbstat",  "cluster",   "cachestat", "repstat",     "logstat",
        "txnstat",  "ltranstat", "sanc",      "log_archive", "help",
        "bdbstate", "lockstat",  "attr",      "bbstat",      "bdblockdump"};

    /* if we were passed a child, find his parent */
    if (bdb_state->parent)
        bdb_state = bdb_state->parent;

    tok = segtok(line, lline, &st, &ltok);

    if (tokcmp(tok, ltok, "stat") == 0) {
        /* We have received "stat bdb ..." ie a backend status request from an
         * op1 window.  Make sure it is one of the safe commands before
         * continuing. */
        int ii, safe = 0;
        tok = segtok(line, lline, &st, &ltok);
        if (tokcmp(tok, ltok, "bdb") == 0) {
            tok = segtok(line, lline, &st, &ltok);
            if (ltok == 0) {
                logmsg(LOGMSG_ERROR, "missing command\n");
                return;
            }
            for (ii = 0; ii < sizeof(safecmds) / sizeof(safecmds[0]); ii++)
                if (tokcmp(tok, ltok, safecmds[ii]) == 0) {
                    safe = 1;
                    break;
                }
        }
        if (!safe) {
            logmsg(LOGMSG_ERROR, 
                    "command <%.*s> not allowed when preceeded by stat\n", ltok,
                    tok);
            return;
        }
    }

    if (tokcmp(tok, ltok, "f") == 0) {
        char fname[80];
        char stime[30]; /* must be at least 26 chars for asctime_r on AIX */
        struct tm tm;
        time_t now;
        int st2, ltok2;
        char *tok2;
        int ii;
        /* redirect to a file */
        tok = segtok(line, lline, &st, &ltok);
        if (ltok == 0) {
            logmsg(LOGMSG_ERROR, "expected output file name\n");
            return;
        }
        tokcpy0(tok, ltok, fname, sizeof(fname));
        out = fopen(fname, "a+");
        if (!out) {
            logmsg(LOGMSG_ERROR, "could not open %s: %d %s\n", fname, errno,
                    strerror(errno));
            return;
        }
        logmsg(LOGMSG_USER, "Opened logfile %s for answer\n", fname);
        logfile = out;
        time(&now);
        localtime_r(&now, &tm);
        asctime_r(&tm, stime);
        for (ii = strlen(stime) - 1; ii >= 0; ii--) {
            if (isspace(stime[ii]))
                stime[ii] = 0;
            else
                break;
        }
        tok = segtok(line, lline, &st, &ltok);
        logmsgf(LOGMSG_USER, out, "---------- %s: <", stime);
        ltok2 = ltok;
        tok2 = tok;
        st2 = st;
        ii = 0;
        while (ltok2 > 0) {
            if (ii++)
                logmsgf(LOGMSG_USER, out, " ");
            logmsgf(LOGMSG_USER, out, "%*.*s", ltok2, ltok2, tok2);
            tok2 = segtok(line, lline, &st2, &ltok2);
        }
        logmsgf(LOGMSG_USER, out, "> ----------\n");
    }

    if (tokcmp(tok, ltok, "help") == 0) {
        int ii;
        for (ii = 0; ii < sizeof(help) / sizeof(help[0]); ii++)
            logmsg(LOGMSG_USER, "%s\n", help[ii]);
    }

    else if (tokcmp(tok, ltok, "gblcontext") == 0)
        logmsg(LOGMSG_USER, "gblcontext = 0x%08llx\n",
               get_gblcontext(bdb_state));

    else if (tokcmp(tok, ltok, "cluster") == 0)
        netinfo_dump(out, bdb_state);
    else if (tokcmp(tok, ltok, "tempcachestat") == 0)
        temp_cache_stats(out, bdb_state);
    else if (tokcmp(tok, ltok, "cachestat") == 0)
        cache_stats(out, bdb_state, 0);
    else if (tokcmp(tok, ltok, "cacheinfo") == 0)
        cache_info(out, bdb_state);
    else if (tokcmp(tok, ltok, "cachestatall") == 0)
        cache_stats(out, bdb_state, 1);
    else if (tokcmp(tok, ltok, "clminfo") == 0)
        __txn_commit_map_print_info(bdb_state->dbenv, LOGMSG_USER, 1);
    else if (tokcmp(tok, ltok, "repstat") == 0)
        rep_stats(out, bdb_state);
    else if (tokcmp(tok, ltok, "bdbstate") == 0)
        bdb_state_dump(out, "bdb_state", bdb_state);
    else if (tokcmp(tok, ltok, "logstat") == 0)
        log_stats(out, bdb_state);
    else if (tokcmp(tok, ltok, "lockstat") == 0)
        lock_stats(out, bdb_state);
    else if (tokcmp(tok, ltok, "lockinfo") == 0)
        lock_info(out, bdb_state, line, st, lline);
    else if (tokcmp(tok, ltok, "activelocks") == 0)
        bdb_dump_active_locks(bdb_state, out);
    else if (tokcmp(tok, ltok, "dumpcache") == 0)
        bdb_dump_cache(bdb_state, out);
    else if (tokcmp(tok, ltok, "truncrepdb") == 0)
        bdb_truncate_repdb(bdb_state, out);
    else if (tokcmp(tok, ltok, "txnstat") == 0)
        txn_stats(out, bdb_state);
    else if (tokcmp(tok, ltok, "ltranstat") == 0)
        ltran_stats(out, bdb_state);
    else if (tokcmp(tok, ltok, "sanc") == 0)
        sanc_dump(out, bdb_state);
    else if (tokcmp(tok, ltok, "test") == 0)
        test_send(bdb_state);
    else if (tokcmp(tok, ltok, "temptable") == 0) {
        if (gbl_temptable_pool_capacity == 0) {
            logmsg(LOGMSG_USER, "Temptable pool not enabled.\n");
            return;
        }

        tok = segtok(line, lline, &st, &ltok);
        if (ltok <= 0)
            comdb2_objpool_stats(bdb_state->temp_table_pool);
        else if (tokcmp(tok, ltok, "capacity") == 0) {
            tok = segtok(line, lline, &st, &ltok);
            if (ltok <= 0)
                logmsg(LOGMSG_ERROR, "Expected # for temptable pool capacity.\n");
            else
                comdb2_objpool_setopt(bdb_state->temp_table_pool, OP_CAPACITY,
                                      (int)toknum(tok, ltok));
        } else if (tokcmp(tok, ltok, "max_idles") == 0) {
            tok = segtok(line, lline, &st, &ltok);
            if (ltok <= 0)
                logmsg(LOGMSG_ERROR, "Expected # for temptable pool max idles.\n");
            else
                comdb2_objpool_setopt(bdb_state->temp_table_pool, OP_MAX_IDLES,
                                      (int)toknum(tok, ltok));
        } else if (tokcmp(tok, ltok, "max_idle_rate") == 0) {
            tok = segtok(line, lline, &st, &ltok);
            if (ltok <= 0)
                logmsg(LOGMSG_ERROR, 
                       "Expected # for temptable pool max idle rate.\n");
            else
                comdb2_objpool_setopt(bdb_state->temp_table_pool,
                                      OP_MAX_IDLE_RATIO,
                                      (int)toknum(tok, ltok));
        } else if (tokcmp(tok, ltok, "evict_intv_ms") == 0) {
            tok = segtok(line, lline, &st, &ltok);
            if (ltok <= 0)
                logmsg(LOGMSG_ERROR, 
                       "Expected # for temptable pool eviction interval.\n");
            else
                comdb2_objpool_setopt(bdb_state->temp_table_pool,
                                      OP_EVICT_INTERVAL,
                                      (int)toknum(tok, ltok));
        } else if (tokcmp(tok, ltok, "evict_rate") == 0) {
            tok = segtok(line, lline, &st, &ltok);
            if (ltok <= 0)
                logmsg(LOGMSG_ERROR, 
                       "Expected # for temptable pool eviction rate.\n");
            else
                comdb2_objpool_setopt(bdb_state->temp_table_pool,
                                      OP_EVICT_RATIO, (int)toknum(tok, ltok));
        } else if (tokcmp(tok, ltok, "min_idles") == 0) {
            tok = segtok(line, lline, &st, &ltok);
            if (ltok <= 0)
                logmsg(LOGMSG_ERROR, 
                       "Expected # for temptable pool min idles.\n");
            else
                comdb2_objpool_setopt(bdb_state->temp_table_pool, OP_MIN_IDLES,
                                      (int)toknum(tok, ltok));
        } else if (tokcmp(tok, ltok, "min_idle_rate") == 0) {
            tok = segtok(line, lline, &st, &ltok);
            if (ltok <= 0)
                logmsg(LOGMSG_ERROR, 
                       "Expected # for temptable pool min idle rate.\n");
            else
                comdb2_objpool_setopt(bdb_state->temp_table_pool,
                                      OP_MIN_IDLE_RATIO,
                                      (int)toknum(tok, ltok));
        } else if (tokcmp(tok, ltok, "idle_time") == 0) {
            tok = segtok(line, lline, &st, &ltok);
            if (ltok <= 0)
                logmsg(LOGMSG_ERROR, 
                       "Expected # for temptable pool idle time.\n");
            else
                comdb2_objpool_setopt(bdb_state->temp_table_pool, OP_IDLE_TIME,
                                      (int)toknum(tok, ltok));
        }
    } else if (tokcmp(tok, ltok, "fulldiag") == 0) {
        logmsgf(LOGMSG_USER, out, "==========[netinfo_dump]==========\n");
        netinfo_dump(out, bdb_state);
        logmsgf(LOGMSG_USER, out, "==========[cache_stats]==========\n");
        cache_stats(out, bdb_state, 1);
        logmsgf(LOGMSG_USER, out, "==========[temp_cache_stats]==========\n");
        temp_cache_stats(out, bdb_state);
        logmsgf(LOGMSG_USER, out, "==========[rep_stats]==========\n");
        rep_stats(out, bdb_state);
        logmsgf(LOGMSG_USER, out, "==========[bdb_state_dump]==========\n");
        bdb_state_dump(out, "bdb_state", bdb_state);
        logmsgf(LOGMSG_USER, out, "==========[log_stats]==========\n");
        log_stats(out, bdb_state);
        logmsgf(LOGMSG_USER, out, "==========[lock_stats]==========\n");
        lock_stats(out, bdb_state);
        logmsgf(LOGMSG_USER, out, "==========[txn_stats]==========\n");
        txn_stats(out, bdb_state);
        logmsgf(LOGMSG_USER, out, "==========[sanc_dump]==========\n");
        sanc_dump(out, bdb_state);
        logmsgf(LOGMSG_USER, out, "==========[lock conflicts]==========\n");
        __lock_dump_region(bdb_state->dbenv, "c", out);
        logmsgf(LOGMSG_USER, out, "==========[lock lockers]==========\n");
        __lock_dump_region(bdb_state->dbenv, "l", out);
        logmsgf(LOGMSG_USER, out, "==========[lock locks]==========\n");
        __lock_dump_region(bdb_state->dbenv, "o", out);
        logmsgf(LOGMSG_USER, out, "==========[lock region]==========\n");
        __lock_dump_region(bdb_state->dbenv, "m", out);
        logmsgf(LOGMSG_USER, out, "==========[lock params]==========\n");
        __lock_dump_region(bdb_state->dbenv, "p", out);
        logmsgf(LOGMSG_USER, out, "==========[latches]==============\n");
        __latch_dump_region(bdb_state->dbenv, out);
        logmsgf(LOGMSG_USER, out, "==========[thread lock info]==========\n");
        bdb_locks_dump(bdb_state, out);
    }

    else if (tokcmp(tok, ltok, "elect") == 0) {
        logmsg(LOGMSG_USER, "forcing an election\n");
        call_for_election(bdb_state, __func__, __LINE__);
    }

    else if (tokcmp(tok, ltok, "repdbgy") == 0)
        rc = bdb_state->dbenv->set_verbose(bdb_state->dbenv,
                                           DB_VERB_REPLICATION, 1);
    else if (tokcmp(tok, ltok, "repdbgn") == 0)
        rc = bdb_state->dbenv->set_verbose(bdb_state->dbenv,
                                           DB_VERB_REPLICATION, 0);
    else if (tokcmp(tok, ltok, "verbdeadlock") == 0) {
        int num;
        tok = segtok(line, lline, &st, &ltok);

        num = toknum(tok, ltok);
        if ((num < 0) || (num > 1)) {
            logmsg(LOGMSG_ERROR, "bad num %d\n", num);
            if (logfile)
                fclose(logfile);
            return;
        }

        rc = bdb_state->dbenv->set_verbose(bdb_state->dbenv, DB_VERB_DEADLOCK,
                                           num);
    } else if (tokcmp(tok, ltok, "verbwaitsfor") == 0) {
        int num;
        tok = segtok(line, lline, &st, &ltok);

        num = toknum(tok, ltok);
        if ((num < 0) || (num > 1)) {
            logmsg(LOGMSG_ERROR, "bad num %d\n", num);
            if (logfile)
                fclose(logfile);
            return;
        }

        rc = bdb_state->dbenv->set_verbose(bdb_state->dbenv, DB_VERB_WAITSFOR,
                                           num);
    }
    else if (tokcmp(tok, ltok, "temptbltest") == 0) {
        //@send bdb temptbltest 200 1000
        logmsg(LOGMSG_USER, "Testing temp tables\n");
        int recsz = 20;
        int maxins = 10000;

        tok = segtok(line, lline, &st, &ltok);
        int y = toknum(tok, ltok);
        if (y > 0) { 
            recsz = y;
            tok = segtok(line, lline, &st, &ltok);
            int z = toknum(tok, ltok);
            if (z > 0) { 
                maxins = z;
            }
        }

        bdb_temp_table_insert_test(bdb_state, recsz, maxins);
    } 
    else if (tokcmp(tok, ltok, "reptrcy") == 0) {
        logmsg(LOGMSG_USER, "turning on replication trace\n");
        bdb_state->rep_trace = 1;
    }

    else if (tokcmp(tok, ltok, "reptrcn") == 0) {
        logmsg(LOGMSG_USER, "turning off replication trace\n");
        bdb_state->rep_trace = 0;
    }

    else if (tokcmp(tok, ltok, "reptrcay") == 0)
        process_reptrca(bdb_state, 1);

    else if (tokcmp(tok, ltok, "reptrcan") == 0)
        process_reptrca(bdb_state, 0);

    else if (tokcmp(tok, ltok, "checkpoint") == 0) {
        int rc;
        rc = bdb_state->dbenv->txn_checkpoint(bdb_state->dbenv, 0, 0, 0);
        if (rc != 0) {
            logmsg(LOGMSG_ERROR, "checkpoint failed with %d\n", rc);
        }
    }

    else if (tokcmp(tok, ltok, "log_archive") == 0) {
        bdb_print_log_files(bdb_state);
    }

    else if (tokcmp(tok, ltok, "oslog") == 0) {
        tok = segtok(line, lline, &st, &ltok);
        if (ltok > 0)
            gbl_namemangle_loglevel = toknum(tok, ltok);
       logmsg(LOGMSG_USER, "os_namemangle log level now %d\n", gbl_namemangle_loglevel);
    }

    else if (tokcmp(tok, ltok, "add") == 0) {
        char *host;
        host = segtok(line, lline, &st, &ltok);
        if (ltok == 0) {
            logmsg(LOGMSG_USER, "expected hostname\n");
            return;
        }
        host = tokdup_hostname(host, ltok);
        process_add(bdb_state, host);
    } else if (tokcmp(tok, ltok, "hostname") == 0) {
        char *iHost, *oHost;
        Pthread_mutex_lock(&hostname_lookup_lk);
        gbl_verbose_hostname_lookup = 1;
        iHost = segtok(line, lline, &st, &ltok);
        if (ltok == 0) {
            logmsg(LOGMSG_USER, "expected nodenum\n");
            gbl_verbose_hostname_lookup = 0;
            Pthread_mutex_unlock(&hostname_lookup_lk);
            return;
        }
        oHost = tokdup_hostname(iHost, ltok);
        logmsg(LOGMSG_USER, "%s:%d got hostname: %s for node: %s\n",__func__, __LINE__, oHost, iHost); 
        gbl_verbose_hostname_lookup = 0;
        Pthread_mutex_unlock(&hostname_lookup_lk);
    } else if (tokcmp(tok, ltok, "del") == 0) {
        char *host;
        host = segtok(line, lline, &st, &ltok);
        if (ltok == 0) {
            logmsg(LOGMSG_USER, "expected hostname\n");
            return;
        }
        host = tokdup_hostname(host, ltok);
        process_del(bdb_state, host);
    }
    else if (tokcmp(tok, ltok, "rem") == 0 || tokcmp(tok, ltok, "bdbrem") == 0) {
        char cmd[ltok+1];
        tokcpy(tok, ltok, cmd);
        char *host;
        host = segtok(line, lline, &st, &ltok);
        if (ltok == 0) {
            logmsg(LOGMSG_USER, "expected hostname\n");
            return;
        }
        host = tokdup_hostname(host, ltok);
        char *realhost = (char *)bdb_find_net_host(bdb_state, host);
        if (realhost == NULL) {
            logmsg(LOGMSG_USER, "WARNING: don't know about %s.\n", host);
            return;
        }
        realhost = intern(realhost);
        net_send_decom_all(bdb_state->repinfo->netinfo, realhost);
        osql_process_message_decom(realhost);
    }
    else if (tokcmp(tok, ltok, "bdbstat") == 0) {
        logmsgf(LOGMSG_USER, out, "rep_process_message %d\n",
                bdb_state->repinfo->repstats.rep_process_message);
        logmsgf(LOGMSG_USER, out, "rep_zerorc %d\n",
                bdb_state->repinfo->repstats.rep_zerorc);
        logmsgf(LOGMSG_USER, out, "rep_newsite %d\n",
                bdb_state->repinfo->repstats.rep_newsite);
        logmsgf(LOGMSG_USER, out, "rep_holdelection %d\n",
                bdb_state->repinfo->repstats.rep_holdelection);
        logmsgf(LOGMSG_USER, out, "rep_newmaster %d\n",
                bdb_state->repinfo->repstats.rep_newmaster);
        logmsgf(LOGMSG_USER, out, "rep_isperm %d\n",
                bdb_state->repinfo->repstats.rep_isperm);
        logmsgf(LOGMSG_USER, out, "rep_notperm %d\n",
                bdb_state->repinfo->repstats.rep_notperm);
        logmsgf(LOGMSG_USER, out, "rep_outdated %d\n",
                bdb_state->repinfo->repstats.rep_outdated);
        logmsgf(LOGMSG_USER, out, "rep_other %d\n", bdb_state->repinfo->repstats.rep_other);
        logmsgf(LOGMSG_USER, out, "dummy_adds %d\n",
                bdb_state->repinfo->repstats.dummy_adds);
        logmsgf(LOGMSG_USER, out, "commits %d\n", bdb_state->repinfo->repstats.commits);
    }

    else if (tokcmp(tok, ltok, "dummy") == 0) {
        logmsg(LOGMSG_USER, "adding dummy record\n");
        add_dummy(bdb_state);
    }

    else if (tokcmp(tok, ltok, "dblist") == 0) {
        __bb_dbreg_print_dblist(bdb_state->dbenv, printf_wrapper, out);
    } else if (tokcmp(tok, ltok, "alldblist") == 0) {
        __bb_dbreg_print_all_dblist(bdb_state->dbenv, printf_wrapper, out);
    } else if (tokcmp(tok, ltok, "dbs") == 0) {
        bdb_dump_table_dbregs(bdb_state);
    } else if (tokcmp(tok, ltok, "curlist") == 0) {
        bdb_dump_cursors(bdb_state, out);
    } else if (tokcmp(tok, ltok, "attr") == 0) {
        bdb_attr_dump(out, bdb_state->attr);
    } else if (tokcmp(tok, ltok, "setattr") == 0) {
        char name[48];
        int value;
        tok = segtok(line, lline, &st, &ltok);
        if (ltok == 0) {
            logmsg(LOGMSG_ERROR, "expected attribute name\n");
            if (logfile)
                fclose(logfile);
            return;
        }
        tokcpy0(tok, ltok, name, sizeof(name));
        tok = segtok(line, lline, &st, &ltok);
        if (ltok == 0) {
            logmsg(LOGMSG_ERROR, "expected attribute value\n");
            if (logfile)
                fclose(logfile);
            return;
        }
        value = toknum(tok, ltok);
        if (bdb_attr_set_by_name(bdb_state, bdb_state->attr, name, value) ==
            0) {
           logmsg(LOGMSG_USER, "Attribute set\n");
        }
    } else if (tokcmp(tok, ltok, "bbstat") == 0) {
        const struct berkdb_thread_stats *p = bdb_get_process_stats();
        unsigned n_lock_waits = p->n_lock_waits ? p->n_lock_waits : 1;
        unsigned n_preads = p->n_preads ? p->n_preads : 1;
        unsigned n_pwrites = p->n_pwrites ? p->n_pwrites : 1;
        logmsgf(LOGMSG_USER, out, "  %u lock waits took %u ms (%u ms/wait)\n",
                p->n_lock_waits, U2M(p->lock_wait_time_us),
                U2M(p->lock_wait_time_us / n_lock_waits));
        logmsgf(LOGMSG_USER, out, "  %u preads took %u ms total of %u bytes\n",
                p->n_preads, U2M(p->pread_time_us), p->pread_bytes);
        if (p->n_preads > 0)
            logmsgf(LOGMSG_USER, out, "  average pread time %u ms\n",
                    U2M(p->pread_time_us) / n_preads);
        logmsgf(LOGMSG_USER, out, "  %u pwrites took %u ms total of %u bytes\n",
                p->n_pwrites, U2M(p->pwrite_time_us), p->pwrite_bytes);
        if (p->n_pwrites > 0)
            logmsgf(LOGMSG_USER, out, "  average pwrite time %u ms\n",
                    U2M(p->pwrite_time_us / n_pwrites));
    }

    else if (tokcmp(tok, ltok, "memdump") == 0) {
        __dbenv_heap_dump(bdb_state->dbenv);
    } else if (tokcmp(tok, ltok, "bdblockdump") == 0) {
        bdb_locks_dump(bdb_state, out);
    } else if (tokcmp(tok, ltok, "bdblocktest") == 0) {
       logmsg(LOGMSG_USER, "doing lock tests\n");
        BDB_READLOCK("lock test 0");
        bdb_dump_my_lock_state(out);
        BDB_READLOCK("lock test 1");
        bdb_dump_my_lock_state(out);
        BDB_WRITELOCK("lock test 2");
        bdb_dump_my_lock_state(out);
        BDB_READLOCK("lock test 3");
        bdb_dump_my_lock_state(out);
        BDB_WRITELOCK("lock test 4");
        bdb_dump_my_lock_state(out);
        BDB_READLOCK("lock test 5");
        bdb_dump_my_lock_state(out);
        BDB_WRITELOCK("lock test 6");
        bdb_dump_my_lock_state(out);
        BDB_RELLOCK();
        bdb_dump_my_lock_state(out);
        BDB_RELLOCK();
        bdb_dump_my_lock_state(out);
        BDB_RELLOCK();
        bdb_dump_my_lock_state(out);
        BDB_RELLOCK();
        bdb_dump_my_lock_state(out);
        BDB_RELLOCK();
        bdb_dump_my_lock_state(out);
        BDB_RELLOCK();
        bdb_dump_my_lock_state(out);
        BDB_RELLOCK();
        bdb_dump_my_lock_state(out);
    } else if (tokcmp(tok, ltok, "dumptrans") == 0) {
        DB_LSN *lsn = NULL;
        int rc;
        int numtrans = 0;
        int bdberr;
        int i;

        rc = bdb_get_active_logical_transaction_lsns(bdb_state, &lsn, &numtrans,
                                                     &bdberr, NULL);
        if (rc)
            logmsg(LOGMSG_USER, "get rc %d bdberr %d\n", rc, bdberr);
        logmsg(LOGMSG_USER, "got %d transactions\n", numtrans);
        for (i = 0; i < numtrans; i++)
            logmsg(LOGMSG_USER, "%u:%u\n", lsn[i].file, lsn[i].offset);
        if (lsn)
            free(lsn);
    } else if (tokcmp(tok, ltok, "dumpqext") == 0) {
        int i;
        bdb_state_type *chld;
        for (i = 0; i < bdb_state->numchildren; i++) {
            chld = bdb_state->children[i];
            if (chld) {
                if (chld->bdbtype == BDBTYPE_QUEUE) {
                    logmsgf(LOGMSG_USER, out, "queue %s\n", chld->name);
                    bdb_queue_extent_info(out, chld, chld->name);
                }
            }
        }
    } else if (tokcmp(tok, ltok, "llmeta") == 0) {
        int bdberr;
        rc = dump_llmeta(bdb_state, &bdberr);
        if (rc)
           logmsg(LOGMSG_ERROR, "dump_llmeta rc %d bdberr %d\n", rc, bdberr);
    } else if (tokcmp(tok, ltok, "curcount") == 0) {
        bdb_dump_cursor_count(bdb_state, out);
    } else if (tokcmp(tok, ltok, "testckp") == 0) {
        __test_last_checkpoint(bdb_state->dbenv);
    } else if (tokcmp(tok, ltok, "repworkers") == 0 ||
               tokcmp(tok, ltok, "repprocs") == 0) {
        struct thdpool *pool;
        if (tokcmp(tok, ltok, "repworkers") == 0)
            pool = bdb_state->dbenv->recovery_workers;
        else
            pool = bdb_state->dbenv->recovery_processors;
        thdpool_process_message(pool, line, lline, st);
    } else if (tokcmp(tok, ltok, "freepages") == 0) {
        bdb_run_in_a_thread(bdb_state, bdb_dump_freepage_info_all);
    } else if (tokcmp(tok, ltok, "lccache") == 0) {
        bdb_lc_cache_trap(bdb_state, line + st, lline - st);
    } else if (tokcmp(tok, ltok, "freepages_abort") == 0) {
        bdb_count_freepages_abort();
    } else if (tokcmp(tok, ltok, "reptimes") == 0) {
        bdb_show_reptimes(bdb_state);
    } else if (tokcmp(tok, ltok, "pgdump") == 0) {
        int fileid, pgno;
        tok = segtok(line, lline, &st, &ltok);
        if (ltok == 0)
            return;
        fileid = toknum(tok, ltok);
        tok = segtok(line, lline, &st, &ltok);
        if (ltok == 0)
            return;
        pgno = toknum(tok, ltok);

        /* Can probably extend the trap to also accept a ufid. But for now pass a NULL. */
        __pgdump(bdb_state->dbenv, fileid, NULL, pgno);
    } else if (tokcmp(tok, ltok, "pgtrash") == 0) {
        int fileid, pgno;
        tok = segtok(line, lline, &st, &ltok);
        if (ltok == 0)
            return;
        fileid = toknum(tok, ltok);
        tok = segtok(line, lline, &st, &ltok);
        if (ltok == 0)
            return;
        pgno = toknum(tok, ltok);

        __pgtrash(bdb_state->dbenv, fileid, pgno);
    } else {
        logmsg(LOGMSG_ERROR, "backend engine unknown cmd <%.*s>. try help\n", ltok,
                tok);
    }

    if (logfile)
        fclose(logfile);
}

void bdb_set_os_log_level(int level)
{
    gbl_namemangle_loglevel = level;
    logmsg(LOGMSG_USER, "os_namemangle log level is now %d\n", level);
}

void lock_info_help(void)
{
    logmsg(LOGMSG_USER, "lockinfo types:\n"
           "  conflict    - dump conflict matrix\n"
           "  lockers     - group by lockers\n"
           "  locks       - group by lock objects\n"
           "  region      - dump lock region\n"
           "  params      - dump region params\n"
           "  latches     - dump latches\n");
}

void lock_info_lockers(FILE *out, bdb_state_type *bdb_state)
{
    char parm[2] = {0};
    parm[0] = 'l';
    __lock_dump_region(bdb_state->dbenv, parm, out);
}

void lock_info(FILE *out, bdb_state_type *bdb_state, char *line, int st,
               int lline)
{
    int ltok;
    const char *tok;
    char parm[2] = {0};

    tok = segtok(line, lline, &st, &ltok);
    if (ltok == 0) {
        lock_info_help();
        return;
    }
    if (tokcmp(tok, ltok, "conflict") == 0) {
        parm[0] = 'c';
    } else if (tokcmp(tok, ltok, "lockers") == 0) {
        parm[0] = 'l';
    } else if (tokcmp(tok, ltok, "locks") == 0) {
        parm[0] = 'o';
    } else if (tokcmp(tok, ltok, "region") == 0) {
        parm[0] = 'm';
    } else if (tokcmp(tok, ltok, "params") == 0) {
        parm[0] = 'p';
    } else if (tokcmp(tok, ltok, "latches") == 0) {
        __latch_dump_region(bdb_state->dbenv, out);
        return;
    } else {
        lock_info_help();
        return;
    }
    __lock_dump_region(bdb_state->dbenv, parm, out);
}

void all_locks(bdb_state_type *x)
{
    char parm[2] = {0};
    parm[0] = 'o';
    __lock_dump_region(x->dbenv, parm, stdout);

    parm[0] = 'l';
    __lock_dump_region(x->dbenv, parm, stdout);
}


static void bdb_queue_extent_info(FILE *out, bdb_state_type *bdb_state,
                                  char *name)
{
    char **names;
    int rc;
    char qname[PATH_MAX];
    char tran_name[PATH_MAX];

    snprintf(tran_name, sizeof(tran_name), "XXX.%s.queue", name);

    bdb_trans(tran_name, qname);

    rc = __qam_extent_names(bdb_state->dbenv, qname, &names);
    if (rc) {
        logmsg(LOGMSG_USER, "rc %d\n", rc);
        return;
    }
    if (names == NULL) {
        logmsg(LOGMSG_USER, "No extents.\n");
    } else {
        int i;
        for (i = 0; names[i]; i++)
            logmsg(LOGMSG_USER, "%4d) %s\n", i + 1, names[i]);
        free(names);
    }
}

int dump_llmeta(bdb_state_type *bdb_state, int *bdberr)
{
    int rc;
    DB_LSN lsn;

    /* LLMETA_LOGICAL_LSN_LWM */
    rc = bdb_get_file_lwm(bdb_state, NULL, &lsn, bdberr);
    if (rc)
        return rc;
    logmsg(LOGMSG_USER, "lwm: %u:%u\n", lsn.file, lsn.offset);
    return 0;
}

#include <build/db_int.h>
#include "dbinc/log.h"

static void dump_dbreg(DB *dbp)
{
    FNAME *fnp;

    fnp = dbp->log_filename;
    logmsg(LOGMSG_USER, " fnp %p ", fnp);
    if (fnp)
        logmsg(LOGMSG_USER, " id %d ", fnp->id);
    printf("\n");
}

void bdb_dump_table_dbregs(bdb_state_type *bdb_state)
{
    if (bdb_state == NULL)
        bdb_state = gbl_bdb_state;

    for (int table = 0; table < bdb_state->numchildren; table++) {
        bdb_state_type *child;

        child = bdb_state->children[table];

        if (child == NULL)
            continue;

        if (child->bdbtype != BDBTYPE_TABLE)
            continue;

        logmsg(LOGMSG_USER, "%s:\n", child->name);
        for (int dta = 0; dta < child->numdtafiles; dta++) {
            for (int stripe = 0;
                 stripe < bdb_get_datafile_num_files(child, dta); stripe++) {
                logmsg(LOGMSG_USER, "  file %d stripe %d: ", dta, stripe);
                dump_dbreg(child->dbp_data[dta][stripe]);
            }
        }
        for (int ix = 0; ix < child->numix; ix++) {
            logmsg(LOGMSG_USER, "  ix %d: ", ix);
            dump_dbreg(child->dbp_ix[ix]);
        }
    }
}

void bdb_show_reptimes_compact(bdb_state_type *bdb_state)
{
    struct interned_string *nodes[REPMAX];
    int numnodes;
    int numdisplayed = 0;
    int first = 1;

    numnodes = net_get_all_nodes_interned(bdb_state->repinfo->netinfo, nodes);
    for (int i = 0; i < numnodes; i++) {
        double avg[2];
        struct hostinfo *h = retrieve_hostinfo(nodes[i]);
        avg[0] = averager_avg(h->time_10seconds);
        avg[1] = averager_avg(h->time_minute);
        if (avg[0] != 0 || avg[1] != 0) {
            if (first) {
                first = 0;
                logmsg(LOGMSG_USER, "reptimes  ");
            }
            logmsg(LOGMSG_USER, "%s: %.2f %.2f   ", nodes[i]->str, avg[0],
                    avg[1]);
            numdisplayed++;
        }
    }
    if (numdisplayed > 0)
        logmsg(LOGMSG_USER, "\n");
}

void bdb_show_reptimes(bdb_state_type *bdb_state)
{
    struct interned_string *nodes[REPMAX];
    int numnodes;

    numnodes = net_get_all_nodes_interned(bdb_state->repinfo->netinfo, nodes);
    logmsg(LOGMSG_USER, "%5s %10s %10s    (rolling avg over interval)\n", "node",
           "10 seconds", "1 minute");
    Pthread_mutex_lock(&(bdb_state->seqnum_info->lock));
    for (int i = 0; i < numnodes; i++) {
        struct hostinfo *h = retrieve_hostinfo(nodes[i]);
        logmsg(LOGMSG_USER, "%s %10.2f %10.2f\n", nodes[i]->str,
                averager_avg(h->time_10seconds),
                averager_avg(h->time_minute));
    }
    Pthread_mutex_unlock(&(bdb_state->seqnum_info->lock));
}

/* NOTE: this isn't generally safe to call - the underlying structure is
 * lockless,
 * and only updated/read from a different thread, with the exception of this
 * call.
 * Leaving it for debugging. */
extern void __lc_cache_stat(DB_ENV *dbenv);
void bdb_lc_cache_trap(bdb_state_type *bdb_state, char *line, int lline)
{
    const char *tok;
    int st = 0;
    int ltok = 0;

    tok = segtok(line, lline, &st, &ltok);
    if (tokcmp(tok, ltok, "stat") == 0) {
        __lc_cache_stat(bdb_state->dbenv);
    }
}

void bdb_send_analysed_table_to_master(bdb_state_type *bdb_state, char *table)
{
    if (!bdb_attr_get(bdb_state->attr, BDB_ATTR_AUTOANALYZE))
        return;

    net_send(bdb_state->repinfo->netinfo, bdb_state->repinfo->master_host,
             USER_TYPE_ANALYZED_TBL, table, strlen(table), 0);
}

repl_wait_and_net_use_t *bdb_get_repl_wait_and_net_stats(
        bdb_state_type *bdb_state, int *pnnodes)
{
    int rc, i, nnodes;
    DB_LSN *lsnp, *master_lsnp;
    const char *host;
    struct host_node_info nodes[REPMAX];
    netinfo_type *p_netinfo;

    repl_wait_and_net_use_t *rv;
    repl_wait_and_net_use_t *pos;

    p_netinfo = bdb_state->repinfo->netinfo;

    nnodes = net_get_nodes_info(p_netinfo, REPMAX, nodes);
    *pnnodes = nnodes - 1;

    if (nnodes == 1) /* Standalone */
        return NULL;

    pos = rv = malloc(sizeof(repl_wait_and_net_use_t) * nnodes);
    if (rv == NULL) /* Malloc failed. */
        return NULL;

    struct hostinfo *m = retrieve_hostinfo(bdb_state->repinfo->master_host_interned);
    master_lsnp = &m->seqnum.lsn;

    for (i = 0; i != nnodes; ++i) {
        host = nodes[i].host;

        /* net_get_nodes_info() returns all nodes. Exclude myself. */
        if (host == bdb_state->repinfo->myhost)
            continue;

        strncpy0(pos->host, host, sizeof(pos->host));

        rc = net_get_host_network_usage(p_netinfo,
                                        host,
                                        &pos->bytes_written,
                                        &pos->bytes_read,
                                        &pos->throttle_waits,
                                        &pos->reorders);
                                            
        struct hostinfo *h = retrieve_hostinfo(nodes[i].host_interned);
        Pthread_mutex_lock(&(bdb_state->seqnum_info->lock));

        if (rc != 0) {
            /* Make sure we don't read uninitialized data on error. */
            pos->bytes_written = 0;
            pos->bytes_read = 0;
            pos->throttle_waits = 0;
            pos->reorders = 0;
            pos->avg_wait_over_10secs = 0;
            pos->max_wait_over_10secs = 0;
            pos->avg_wait_over_1min = 0;
            pos->max_wait_over_1min = 0;
            pos->lsn_text[0] = '\0';
            pos->lsn_bytes_behind = 0;
        } else {
            pos->avg_wait_over_10secs = averager_avg(h->time_10seconds);
            pos->max_wait_over_10secs = averager_max(h->time_10seconds);
            pos->avg_wait_over_1min = averager_avg(h->time_minute);
            pos->max_wait_over_1min = averager_max(h->time_minute);

            lsnp = &h->seqnum.lsn;
            lsn_to_str(pos->lsn_text, lsnp);
            pos->lsn_bytes_behind = subtract_lsn(bdb_state, master_lsnp, lsnp);
        }

        Pthread_mutex_unlock(&(bdb_state->seqnum_info->lock));

        ++pos;
    }
    return rv;
}

char *bdb_coherent_state_string(struct interned_string *host) {
    char *coherent_state;
    bdb_state_type *bdb_state = gbl_bdb_state;
    int iammaster = bdb_state->repinfo->myhost == bdb_state->repinfo->master_host;
    struct hostinfo *h = retrieve_hostinfo(host);

    switch (h->coherent_state) {
        case STATE_COHERENT:
            coherent_state = "";
            break;

        case STATE_INCOHERENT:
            coherent_state = "INCOHERENT";
            break;

        case STATE_INCOHERENT_SLOW:
            coherent_state = "INCOHERENT_SLOW";
            break;

            /* Incoherent local is only meaningful on the master */
        case STATE_INCOHERENT_WAIT:
            if(iammaster)
                coherent_state = "INCOHERENT_WAIT";
            else
                coherent_state = "";
            break;

        default:
            coherent_state = "???";
    }
    return coherent_state;
}


int bdb_fill_cluster_info(void **data, int *num_nodes) {
    struct cluster_info *info;
    bdb_state_type *bdb_state = gbl_bdb_state;

    struct host_node_info nodes[REPMAX];
    *num_nodes = net_get_nodes_info(bdb_state->repinfo->netinfo,
            REPMAX, nodes);
    info = malloc(sizeof(struct cluster_info) * *num_nodes);
    if (info == NULL)
        return -1;
    for (int i = 0; i < *num_nodes; i++) {
        info[i].host = strdup(nodes[i].host);
        info[i].port = nodes[i].port;
        info[i].is_master = (nodes[i].host == gbl_bdb_state->repinfo->master_host) ? "Y" : "N";
        info[i].coherent_state = bdb_coherent_state_string(nodes[i].host_interned);
        if (info[i].coherent_state[0] == 0)
            info[i].coherent_state = "coherent";
        struct hostinfo *h = retrieve_hostinfo(nodes[i].host_interned);
        DB_LSN *lsnp = &h->seqnum.lsn;
        info[i].logfile = lsnp->file;
        info[i].logoffset = lsnp->offset;
    }
    *data = info;
    return 0;
}
int bdb_rep_deadlocks(bdb_state_type *bdb_state, int64_t *nrep_deadlocks)
{
    *nrep_deadlocks = 0;
    bdb_state->dbenv->rep_deadlocks(bdb_state->dbenv, (uint64_t *)nrep_deadlocks);
    return 0;
}

int bdb_rep_stats(bdb_state_type *bdb_state, int64_t *nrep_deadlocks) {
    DB_REP_STAT *stats;
    bdb_state->dbenv->rep_stat(bdb_state->dbenv, &stats, 0);
    *nrep_deadlocks = stats->retry;
    free(stats);
    return 0;
}
