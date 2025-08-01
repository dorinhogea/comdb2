/*
   Copyright 2018-2020 Bloomberg Finance L.P.

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


#include "sqlite3ext.h"
#include "tranlog.h"
#include <assert.h>
#include <string.h>
#include "comdb2.h"
#include "build/db.h"
#include "dbinc/db_swap.h"
#include "dbinc_auto/txn_auto.h"
#include "comdb2systbl.h"
#include "parse_lsn.h"
#include "epochlib.h"

/* Column numbers */
#define TRANLOG_COLUMN_START        0
#define TRANLOG_COLUMN_STOP         1
#define TRANLOG_COLUMN_FLAGS        2
#define TRANLOG_COLUMN_TIMEOUT      3
#define TRANLOG_COLUMN_BLOCKLSN     4
#define TRANLOG_COLUMN_LSN          5
#define TRANLOG_COLUMN_RECTYPE      6
#define TRANLOG_COLUMN_GENERATION   7
#define TRANLOG_COLUMN_TIMESTAMP    8
#define TRANLOG_COLUMN_LOG          9
#define TRANLOG_COLUMN_TXNID        10
#define TRANLOG_COLUMN_UTXNID       11
#define TRANLOG_COLUMN_MAXUTXNID    12
#define TRANLOG_COLUMN_CHILDUTXNID  13
#define TRANLOG_COLUMN_LSN_FILE     14 /* Useful for sorting records by LSN */
#define TRANLOG_COLUMN_LSN_OFFSET   15

extern int gbl_apprec_gen;
int gbl_tranlog_default_timeout = 30;

/* Modeled after generate_series */
typedef struct tranlog_cursor tranlog_cursor;
struct tranlog_cursor {
  sqlite3_vtab_cursor base;  /* Base class - must be first */
  sqlite3_int64 iRowid;      /* The rowid */
  sqlite3_int64 idx;
  DB_LSN curLsn;             /* Current LSN */
  DB_LSN minLsn;             /* Minimum LSN */
  DB_LSN maxLsn;             /* Maximum LSN */
  DB_LSN blockLsn;           /* Block until this LSN */
  char *minLsnStr;
  char *maxLsnStr;
  char *curLsnStr;
  char *blockLsnStr;
  int flags;           /* 1 if we should block */
  int hitLast;
  int notDurable;
  int openCursor;
  int startAppRecGen;
  int starttime;
  int timeout;
  DB_LOGC *logc;             /* Log Cursor */
  DBT data;
};

static int tranlogConnect(
  sqlite3 *db,
  void *pAux,
  int argc, const char *const*argv,
  sqlite3_vtab **ppVtab,
  char **pzErr
){
  sqlite3_vtab *pNew;
  int rc;

  rc = sqlite3_declare_vtab(db,
     "CREATE TABLE x(minlsn hidden,maxlsn hidden,flags hidden,timeout hidden,blocklsn hidden,lsn,rectype integer,generation integer,timestamp integer,payload,txnid integer,utxnid integer,maxutxnid hidden, childutxnid hidden, lsnfile hidden, lsnoffset hidden)");
  if( rc==SQLITE_OK ){
    pNew = *ppVtab = sqlite3_malloc( sizeof(*pNew) );
    if( pNew==0 ) return SQLITE_NOMEM;
    memset(pNew, 0, sizeof(*pNew));
  }
  return rc;
}

static int tranlogDisconnect(sqlite3_vtab *pVtab){
  sqlite3_free(pVtab);
  return SQLITE_OK;
}

static int tranlogOpen(sqlite3_vtab *p, sqlite3_vtab_cursor **ppCursor){
  tranlog_cursor *pCur;

  pCur = sqlite3_malloc( sizeof(*pCur) );
  if( pCur==0 ) return SQLITE_NOMEM;
  memset(pCur, 0, sizeof(*pCur));
  pCur->startAppRecGen = gbl_apprec_gen;
  pCur->timeout = gbl_tranlog_default_timeout;
  pCur->starttime = comdb2_time_epoch();
  *ppCursor = &pCur->base;
  return SQLITE_OK;
}

static int tranlogClose(sqlite3_vtab_cursor *cur){
  tranlog_cursor *pCur = (tranlog_cursor*)cur;
  if (pCur->openCursor) {
      assert(pCur->logc);
      pCur->logc->close(pCur->logc, 0);
      pCur->openCursor = 0;
  }
  if (pCur->data.data)
      free(pCur->data.data);
  if (pCur->minLsnStr)
      sqlite3_free(pCur->minLsnStr);
  if (pCur->maxLsnStr)
      sqlite3_free(pCur->maxLsnStr);
  if (pCur->curLsnStr)
      sqlite3_free(pCur->curLsnStr);
  if (pCur->blockLsnStr)
      sqlite3_free(pCur->blockLsnStr);
  sqlite3_free(pCur);
  return SQLITE_OK;
}
#include <bdb/bdb_int.h>

extern pthread_mutex_t gbl_logput_lk;
extern pthread_cond_t gbl_logput_cond;
extern int gbl_num_logput_listeners;
extern pthread_mutex_t gbl_durable_lsn_lk;
extern pthread_cond_t gbl_durable_lsn_cond;
int gbl_tranlog_incoherent_timeout = 10;
int gbl_tranlog_maxpoll = 60;
extern int comdb2_sql_tick();
extern int bdb_am_i_coherent(bdb_state_type *bdb_state);

/*
** Advance a tranlog cursor to the next log entry
*/
static int tranlogNext(sqlite3_vtab_cursor *cur)
{
  struct sql_thread *thd = NULL;
  tranlog_cursor *pCur = (tranlog_cursor*)cur;
  DB_LSN durable_lsn = {0};
  uint32_t durable_gen=0, rc, getflags;
  bdb_state_type *bdb_state = thedb->bdb_env;

  if (pCur->notDurable || pCur->hitLast)
      return SQLITE_OK;

  if (pCur->timeout > 0 && (comdb2_time_epoch() - pCur->starttime) > pCur->timeout) {
      pCur->hitLast = 1;
      return SQLITE_OK;
  }

  if (!pCur->openCursor) {
      if ((rc = bdb_state->dbenv->log_cursor(bdb_state->dbenv, &pCur->logc, 0))
              != 0) {
          logmsg(LOGMSG_ERROR, "%s line %d error getting a log cursor rc=%d\n",
                  __func__, __LINE__, rc);
          return SQLITE_INTERNAL;
      }
      pCur->logc->setflags(pCur->logc, DB_LOG_SILENT_ERR);
      pCur->openCursor = 1;
      pCur->data.flags = DB_DBT_REALLOC;

      if (pCur->minLsn.file == 0) {
          getflags = DB_FIRST;
      } else {
          pCur->curLsn = pCur->minLsn;
          getflags = DB_SET;
      }

      if (pCur->maxLsn.file == 0) {
          if (pCur->flags & TRANLOG_FLAGS_DESCENDING) {
              getflags = DB_LAST;
          }
      }
  } else {
      getflags = (pCur->flags & TRANLOG_FLAGS_DESCENDING) ? DB_PREV : DB_NEXT;
  }

  /* Special case for durable first requests: we need to know the lsn */
  if ((pCur->flags & TRANLOG_FLAGS_DURABLE) && getflags == DB_FIRST) {
      if (pCur->logc->get(
          pCur->logc, &pCur->curLsn, &pCur->data, getflags) != 0) {
          logmsg(LOGMSG_ERROR, "%s line %d error getting a log record rc=%d\n",
                  __func__, __LINE__, rc);
          return SQLITE_INTERNAL;
      }
  }

  /* Don't advance cursor until this is durable */
  if (pCur->flags & TRANLOG_FLAGS_DURABLE) {
      do {
          struct timespec ts;
          bdb_state->dbenv->get_durable_lsn(bdb_state->dbenv,
                  &durable_lsn, &durable_gen);

          /* We've already returned this lsn: break when the next is durable */
          if (log_compare(&durable_lsn, &pCur->curLsn) >= 0)
              break;

          /* Return immediately if we are non-blocking */
          else if (!bdb_amimaster(bdb_state) ||
                  (pCur->flags & TRANLOG_FLAGS_BLOCK) == 0) {
              pCur->notDurable = 1;
              break;
          }

          /* We want to downgrade */
          if (bdb_the_lock_desired()) {
              pCur->notDurable = 1;
              break;
          }

          if (pCur->timeout > 0 && (comdb2_time_epoch() - pCur->starttime) > pCur->timeout) {
              pCur->hitLast = 1;
              pCur->notDurable = 1;
              return SQLITE_OK;
          }

          /* Wait on a condition variable */
          clock_gettime(CLOCK_REALTIME, &ts);
          ts.tv_nsec += (200 * 1000000);
          if (ts.tv_nsec >= 1000000000) {
              ++ts.tv_sec;
              ts.tv_nsec -= 1000000000;
          }
          Pthread_mutex_lock(&gbl_durable_lsn_lk);
          pthread_cond_timedwait(&gbl_durable_lsn_cond, &gbl_durable_lsn_lk, &ts);
          Pthread_mutex_unlock(&gbl_durable_lsn_lk);

      } while (1);
  }

  if ((rc = pCur->logc->get(pCur->logc, &pCur->curLsn, &pCur->data, getflags)) != 0) {
      if (getflags != DB_NEXT && getflags != DB_PREV) {
          logmsg(LOGMSG_ERROR, "%s line %d did not expect logc->get to fail with flag: %d."
                               "got rc %d\n",
                  __func__, __LINE__, getflags, rc);
          return SQLITE_INTERNAL;
      }

      int incoherent_start_time = 0;

      if (pCur->flags & TRANLOG_FLAGS_BLOCK &&
              !(pCur->flags & TRANLOG_FLAGS_DESCENDING)) {
          int poll_start_time = comdb2_time_epoch();
          do {
              /* Tick up. Return an error if sql_tick() fails
                 (peer dropped connection, max query time reached, etc.) */
              if ((rc = comdb2_sql_tick()) != 0)
                  return rc;

              if (pCur->blockLsn.file > 0 &&
                      log_compare(&pCur->curLsn, &pCur->blockLsn) >= 0) {
                  pCur->hitLast = 1;
                  return SQLITE_OK;
              }

              if (pCur->timeout > 0 && (comdb2_time_epoch() - pCur->starttime) > pCur->timeout) {
                  pCur->hitLast = 1;
                  return SQLITE_OK;
              }

              if (gbl_tranlog_maxpoll > 0) {
                  if (comdb2_time_epoch() - poll_start_time > gbl_tranlog_maxpoll) {
                      logmsg(LOGMSG_DEBUG, "%s: returning after poll for %d seconds\n",
                              __func__, gbl_tranlog_maxpoll);
                      pCur->hitLast = 1;
                      return SQLITE_OK;
                  }
              }
              if (gbl_tranlog_incoherent_timeout > 0) {
                  int coherent = bdb_am_i_coherent(bdb_state);
                  if (coherent) {
                      incoherent_start_time = 0;
                  } else if (incoherent_start_time == 0) {
                      incoherent_start_time = comdb2_time_epoch();
                  } else if (comdb2_time_epoch() - incoherent_start_time >
                          gbl_tranlog_incoherent_timeout) {
                      logmsg(LOGMSG_DEBUG, "%s: incoherent for %d seconds\n",
                              __func__, gbl_tranlog_incoherent_timeout);
                      pCur->hitLast = 1;
                      return SQLITE_OK;
                  }
              }

              if (db_is_exiting() || pCur->startAppRecGen != gbl_apprec_gen) {
                    pCur->hitLast = 1;
                    return SQLITE_OK;
              }

              struct timespec ts;
              clock_gettime(CLOCK_REALTIME, &ts);
              ts.tv_nsec += (200 * 1000000);
              if (ts.tv_nsec >= 1000000000) {
                  ++ts.tv_sec;
                  ts.tv_nsec -= 1000000000;
              }
              Pthread_mutex_lock(&gbl_logput_lk);
              ++gbl_num_logput_listeners;
              pthread_cond_timedwait(&gbl_logput_cond, &gbl_logput_lk, &ts);
              --gbl_num_logput_listeners;
              Pthread_mutex_unlock(&gbl_logput_lk);

              while (bdb_the_lock_desired()) {
                  if (thd == NULL) {
                      thd = pthread_getspecific(query_info_key);
                  }
                  recover_deadlock_simple(thedb->bdb_env);
              }
          } while ((rc = pCur->logc->get(pCur->logc, &pCur->curLsn, &pCur->data, DB_NEXT)));
      } else {
          pCur->hitLast = 1;
      }
  }

  pCur->iRowid++;
  return SQLITE_OK;
}

#define skipws(p) { while (*p != '\0' && *p == ' ') p++; }
#define isnum(p) ( *p >= '0' && *p <= '9' )

static inline void tranlog_lsn_to_str(char *st, DB_LSN *lsn)
{
    sprintf(st, "{%d:%d}", lsn->file, lsn->offset);
}

static inline int parse_lsn(const unsigned char *lsnstr, DB_LSN *lsn)
{
    unsigned int file, offset;

    if (char_to_lsn((char *)lsnstr, &file, &offset)) {
        return -1;
    }

    lsn->file = file;
    lsn->offset = offset;
    return 0;
}

static u_int64_t get_timestamp_from_regop_gen_record(char *data)
{
    u_int64_t timestamp;
    u_int32_t rectype;
    LOGCOPY_32(&rectype, data); 
    if ((rectype < 10000 && rectype > 2000) || rectype > 12000) {
        LOGCOPY_64( &timestamp, &data[ 4 + 4 + 8 + 8 + 4 + 4 + 8] );
    } else {
        LOGCOPY_64( &timestamp, &data[ 4 + 4 + 8 + 4 + 4 + 8] );
    }
    return timestamp;
}

static u_int32_t get_generation_from_regop_gen_record(char *data)
{
    u_int32_t generation;
    u_int32_t rectype;
    LOGCOPY_32(&rectype, data); 
    if ((rectype < 10000 && rectype > 2000) || rectype > 12000) {
        LOGCOPY_32( &generation, &data[ 4 + 4 + 8 + 8 + 4] );
    } else {
        LOGCOPY_32( &generation, &data[ 4 + 4 + 8 + 4] );
    }
    return generation;
}

static u_int32_t get_generation_from_dist_commit_record(char *data)
{
    u_int32_t generation;
    u_int32_t rectype;
    LOGCOPY_32(&rectype, data); 
    if ((rectype < 10000 && rectype > 2000) || rectype > 12000) {
        LOGCOPY_32( &generation, &data[4 + 4 + 8 + 8] );
    } else {
        LOGCOPY_32( &generation, &data[4 + 4 + 8] );
    }
    return generation;
}

static u_int32_t get_timestamp_from_dist_commit_record(char *data)
{
    u_int64_t timestamp;
    u_int32_t rectype;
    LOGCOPY_32(&rectype, data); 
    if ((rectype < 10000 && rectype > 2000) || rectype > 12000) {
        LOGCOPY_64( &timestamp, &data[4 + 4 + 8 + 4 + 8 + 8] );
    } else {
        LOGCOPY_64( &timestamp, &data[4 + 4 + 8 + 4 + 8] );
    }
    return timestamp;
}

static u_int32_t get_generation_from_dist_abort_record(char *data)
{
    u_int32_t generation;
    u_int32_t rectype;
    LOGCOPY_32(&rectype, data); 
    if ((rectype < 10000 && rectype > 2000) || rectype > 12000) {
        LOGCOPY_32( &generation, &data[4 + 4 + 8 + 8] );
    } else {
        LOGCOPY_32( &generation, &data[4 + 4 + 8] );
    }
    return generation;
}

static u_int32_t get_timestamp_from_dist_abort_record(char *data)
{
    u_int64_t timestamp;
    u_int32_t rectype;
    LOGCOPY_32(&rectype, data); 
    if ((rectype < 10000 && rectype > 2000) || rectype > 12000) {
        LOGCOPY_64( &timestamp, &data[4 + 4 + 8 + 4 + 8] );
    } else {
        LOGCOPY_64( &timestamp, &data[4 + 4 + 8 + 4] );
    }
    return timestamp;
}

static u_int64_t get_timestamp_from_regop_rowlocks_record(char *data)
{
    u_int64_t timestamp;
    u_int32_t rectype;
    LOGCOPY_32(&rectype, data); 
    if ((rectype < 10000 && rectype > 2000) || rectype > 12000) {
        LOGCOPY_64( &timestamp, &data[4 + 4 + 8 + 8 + 4 + 8 + 8 + 8 + 8] );
    } else {
        LOGCOPY_64( &timestamp, &data[4 + 4 + 8 + 4 + 8 + 8 + 8 + 8] );
    }
    return timestamp;
}

static u_int32_t get_generation_from_regop_rowlocks_record(char *data)
{
    u_int32_t generation = 0;
    off_t loff;
    u_int32_t lflags;
    u_int32_t rectype;
    LOGCOPY_32(&rectype, data); 
    if ((rectype < 10000 && rectype > 2000) || rectype > 12000) {
        loff = 4 + 4 + 8 + 8 + 4 + 8 + 8 + 8 + 8 + 8;
    } else {
        loff = 4 + 4 + 8 + 4 + 8 + 8 + 8 + 8 + 8;
    }
    LOGCOPY_32( &lflags, &data[loff] );
    if (lflags & DB_TXN_LOGICAL_GEN) {
        LOGCOPY_32(&generation, &data[loff + 4]);
    }

    return generation;
}

static u_int32_t get_timestamp_from_regop_record(char *data)
{
    u_int32_t timestamp;
    u_int32_t rectype;
    LOGCOPY_32(&rectype, data); 
    if ((rectype < 10000 && rectype > 2000) || rectype > 12000) {
        LOGCOPY_32( &timestamp, &data[4 + 4 + 8 + 8 + 4] );
    } else {
        LOGCOPY_32( &timestamp, &data[4 + 4 + 8 + 4] );
    }
    return timestamp;
}

static u_int32_t get_timestamp_from_ckp_record(char *data)
{
    u_int32_t timestamp;
    u_int32_t rectype;
    LOGCOPY_32(&rectype, data); 
    if ((rectype < 10000 && rectype > 2000) || rectype > 12000) {
        LOGCOPY_32( &timestamp, &data[4 + 4 + 8 + 8 + 8 + 8] );
    } else {
        LOGCOPY_32( &timestamp, &data[4 + 4 + 8 + 8 + 8] );
    }
    return timestamp;
}

static u_int32_t get_generation_from_ckp_record(char *data)
{
    u_int32_t generation;
    u_int32_t rectype;
    LOGCOPY_32(&rectype, data); 
    if ((rectype < 10000 && rectype > 2000) || rectype > 12000) {
        LOGCOPY_32( &generation, &data[4 + 4 + 8 + 8 + 8 + 8 + 4] );
    } else {
        LOGCOPY_32( &generation, &data[4 + 4 + 8 + 8 + 8 + 4] );
    }
    return generation;
}

u_int64_t get_timestamp_from_matchable_record(char *data)
{
    u_int32_t rectype = 0;
    u_int32_t dood = 0;
    if (data)
    {
        LOGCOPY_32(&rectype, data); 
        dood = *(uint32_t *)(data);
        logmsg(LOGMSG_DEBUG, "%s rec: %u, dood: %u\n", __func__, rectype,
                dood);
    }
    else
    {
        logmsg(LOGMSG_DEBUG, "No data, so can't get rectype!\n");
    }

    if (rectype == DB___txn_regop_gen || (rectype == DB___txn_regop_gen+2000) ||
       rectype == DB___txn_regop_gen_endianize || (rectype == DB___txn_regop_gen_endianize+2000)) {
        return get_timestamp_from_regop_gen_record(data);
    }

    if (rectype == DB___txn_dist_commit || (rectype == DB___txn_dist_commit+2000)) {
        return get_timestamp_from_dist_commit_record(data);
    }

    if (rectype == DB___txn_dist_abort || (rectype == DB___txn_dist_abort+2000)) {
        return get_timestamp_from_dist_abort_record(data);
    }

    if (rectype == DB___txn_regop_rowlocks || (rectype == DB___txn_regop_rowlocks+2000) ||
        rectype == DB___txn_regop_rowlocks_endianize || (rectype == DB___txn_regop_rowlocks_endianize+2000)) {
        return get_timestamp_from_regop_rowlocks_record(data);
    }

    if (rectype == DB___txn_regop || (rectype == DB___txn_regop+2000)) {
        return get_timestamp_from_regop_record(data);
    }

	if (rectype == DB___txn_ckp || (rectype == DB___txn_ckp+2000) ||
		rectype == DB___txn_ckp_recovery || (rectype == DB___txn_ckp_recovery+2000)) {
		return get_timestamp_from_ckp_record(data);
	}

    return -1;
}

/*
** Return values of columns for the row at which the series_cursor
** is currently pointing.
*/
static int tranlogColumn(
  sqlite3_vtab_cursor *cur,   /* The cursor */
  sqlite3_context *ctx,       /* First argument to sqlite3_result_...() */
  int i                       /* Which column to return */
)
{
  tranlog_cursor *pCur = (tranlog_cursor*)cur;
  u_int32_t rectype = 0;
  u_int32_t generation = 0;
  int64_t timestamp = 0;
  u_int32_t txnid = 0;
  u_int64_t utxnid = 0;
  u_int64_t maxutxnid = 0;
  u_int64_t childutxnid = 0;

  switch( i ){
    case TRANLOG_COLUMN_START:
        if (!pCur->minLsnStr) {
            pCur->minLsnStr = sqlite3_malloc(32);
            tranlog_lsn_to_str(pCur->minLsnStr, &pCur->minLsn);
        }
        sqlite3_result_text(ctx, pCur->minLsnStr, -1, NULL);
        break;

    case TRANLOG_COLUMN_STOP:
        if (!pCur->maxLsnStr) {
            pCur->maxLsnStr = sqlite3_malloc(32);
            tranlog_lsn_to_str(pCur->maxLsnStr, &pCur->maxLsn);
        }
        sqlite3_result_text(ctx, pCur->maxLsnStr, -1, NULL);
        break;
    case TRANLOG_COLUMN_MAXUTXNID: 
        if (pCur->data.data) {
            LOGCOPY_32(&rectype, pCur->data.data); 
            if (rectype == DB___txn_ckp+2000 || rectype == DB___txn_ckp_recovery+2000) {
                LOGCOPY_64(&maxutxnid, &((char*)pCur->data.data)[4 + 4 + 8 + 8 + 8 + 8 + 4 + 4]);
                sqlite3_result_int64(ctx, maxutxnid);
                break;
            } 
        }
        sqlite3_result_null(ctx);
        break;
    case TRANLOG_COLUMN_FLAGS:
        sqlite3_result_int64(ctx, pCur->flags);
        break;
    case TRANLOG_COLUMN_TIMEOUT:
        sqlite3_result_int64(ctx, pCur->timeout);
        break;
    case TRANLOG_COLUMN_BLOCKLSN:
        if (!pCur->blockLsnStr) {
            pCur->blockLsnStr = sqlite3_malloc(32);
        }
        tranlog_lsn_to_str(pCur->blockLsnStr, &pCur->blockLsn);
        sqlite3_result_text(ctx, pCur->blockLsnStr, -1, NULL);
        break;
    case TRANLOG_COLUMN_LSN:
        if (!pCur->curLsnStr) {
            pCur->curLsnStr = sqlite3_malloc(32);
        }
        tranlog_lsn_to_str(pCur->curLsnStr, &pCur->curLsn);
        sqlite3_result_text(ctx, pCur->curLsnStr, -1, NULL);
        break;
    case TRANLOG_COLUMN_RECTYPE:
        if (pCur->data.data)
            LOGCOPY_32(&rectype, pCur->data.data); 
        sqlite3_result_int64(ctx, rectype);
        break;
    case TRANLOG_COLUMN_GENERATION:
        if (pCur->data.data)
            LOGCOPY_32(&rectype, pCur->data.data); 

        normalize_rectype(&rectype);

        if (rectype == DB___txn_regop_gen || rectype == DB___txn_regop_gen_endianize){
            generation = get_generation_from_regop_gen_record(pCur->data.data);
        }

        if (rectype == DB___txn_dist_commit){
            generation = get_generation_from_dist_commit_record(pCur->data.data);
        }

        if (rectype == DB___txn_dist_abort){
            generation = get_generation_from_dist_abort_record(pCur->data.data);
        }

        if (rectype == DB___txn_regop_rowlocks || rectype == DB___txn_regop_rowlocks_endianize) {
            generation = get_generation_from_regop_rowlocks_record(pCur->data.data);
        }

        if (rectype == DB___txn_ckp || rectype == DB___txn_ckp_recovery) {
            generation = get_generation_from_ckp_record(pCur->data.data);
        }

        if (generation > 0) {
            sqlite3_result_int64(ctx, generation);
        } else {
            sqlite3_result_null(ctx);
        }
        break;
    case TRANLOG_COLUMN_TXNID:
        LOGCOPY_32(&txnid, &((char *) pCur->data.data)[4]); 
        sqlite3_result_int64(ctx, txnid);
        break;
    case TRANLOG_COLUMN_UTXNID:
        if (pCur->data.data) {
            LOGCOPY_32(&rectype, pCur->data.data); 
            if ((rectype < 10000 && rectype > 2000) || rectype > 12000) {
                LOGCOPY_64(&utxnid, &((char *) pCur->data.data)[4 + 4 + 8]); 
                sqlite3_result_int64(ctx, utxnid);
                break;
            }
        }
        sqlite3_result_null(ctx);
        break;
    case TRANLOG_COLUMN_TIMESTAMP:
        if (pCur->data.data)
            LOGCOPY_32(&rectype, pCur->data.data); 

        if (rectype == DB___txn_regop_gen || (rectype == DB___txn_regop_gen+2000) ||
            rectype == DB___txn_regop_gen_endianize || (rectype == DB___txn_regop_gen_endianize+2000)) {
            timestamp = get_timestamp_from_regop_gen_record(pCur->data.data);
        }

        if (rectype == DB___txn_dist_commit || (rectype == DB___txn_dist_commit+2000)){
            timestamp = get_timestamp_from_dist_commit_record(pCur->data.data);
        }

        if (rectype == DB___txn_dist_abort || (rectype == DB___txn_dist_abort+2000)){
            timestamp = get_timestamp_from_dist_abort_record(pCur->data.data);
        }

        if (rectype == DB___txn_regop_rowlocks || (rectype == DB___txn_regop_rowlocks+2000) ||
            rectype == DB___txn_regop_rowlocks_endianize || (rectype == DB___txn_regop_rowlocks_endianize+2000)) {
            timestamp = get_timestamp_from_regop_rowlocks_record(pCur->data.data);
        }

        if (rectype == DB___txn_regop || (rectype == DB___txn_regop+2000)) {
            timestamp = get_timestamp_from_regop_record(pCur->data.data);
        }

		if (rectype == DB___txn_ckp || (rectype == DB___txn_ckp+2000) ||
			rectype == DB___txn_ckp_recovery || (rectype == DB___txn_ckp_recovery+2000)) {
			timestamp = get_timestamp_from_ckp_record(pCur->data.data);
		}

        if (timestamp > 0) {
            sqlite3_result_int64(ctx, timestamp);
        } else {
            sqlite3_result_null(ctx);
        }
        break;
    case TRANLOG_COLUMN_LOG:
        sqlite3_result_blob(ctx, pCur->data.data, pCur->data.size, NULL);
        break;
    case TRANLOG_COLUMN_CHILDUTXNID:
        if (pCur->data.data)
                LOGCOPY_32(&rectype, pCur->data.data);

        if (rectype == DB___txn_child+2000 || rectype == DB___txn_child+3000)
        { 
                LOGCOPY_64(&childutxnid, &((char *) pCur->data.data)[4 + 4 + 8 + 8 + 4]); 
        }

        if (childutxnid > 0) {
                sqlite3_result_int64(ctx, childutxnid);
        } else {
                sqlite3_result_null(ctx);
        }
        break;
    case TRANLOG_COLUMN_LSN_FILE:
        sqlite3_result_int(ctx, pCur->curLsn.file);
        break;
    case TRANLOG_COLUMN_LSN_OFFSET:
        sqlite3_result_int(ctx, pCur->curLsn.offset);
        break;
  }
  return SQLITE_OK;
}

/*
** Return the rowid for the current row.  In this implementation, the
** rowid is the same as the output value.
*/
static int tranlogRowid(sqlite3_vtab_cursor *cur, sqlite_int64 *pRowid){
  tranlog_cursor *pCur = (tranlog_cursor*)cur;
  *pRowid = pCur->iRowid;
  return SQLITE_OK;
}

/*
** Return TRUE if the cursor has been moved off of the last
** row of output.
*/
static int tranlogEof(sqlite3_vtab_cursor *cur){
  tranlog_cursor *pCur = (tranlog_cursor*)cur;
  int rc;

  /* If we are not positioned, position now */
  if (pCur->openCursor == 0) {
      if ((rc=tranlogNext(cur)) != SQLITE_OK)
          return rc;
  }
  if (pCur->hitLast || pCur->notDurable)
      return 1;
  if (pCur->maxLsn.file > 0 && log_compare(&pCur->curLsn, &pCur->maxLsn) > 0)
      return 1;
  return 0;
}

static int tranlogFilter(
  sqlite3_vtab_cursor *pVtabCursor, 
  int idxNum, const char *idxStr,
  int argc, sqlite3_value **argv
){
  tranlog_cursor *pCur = (tranlog_cursor *)pVtabCursor;
  int i = 0;

  bzero(&pCur->minLsn, sizeof(pCur->minLsn));
  if( idxNum & 1 ){
    const unsigned char *minLsn = sqlite3_value_text(argv[i++]);
    if (minLsn && parse_lsn(minLsn, &pCur->minLsn)) {
        return SQLITE_CONV_ERROR;
    }
  }
  bzero(&pCur->maxLsn, sizeof(pCur->maxLsn));
  if( idxNum & 2 ){
    const unsigned char *maxLsn = sqlite3_value_text(argv[i++]);
    if (maxLsn && parse_lsn(maxLsn, &pCur->maxLsn)) {
        return SQLITE_CONV_ERROR;
    }
  }
  pCur->flags = 0;
  if( idxNum & 4 ){
    int64_t flags = sqlite3_value_int64(argv[i++]);
    pCur->flags = flags;
  }
  pCur->timeout = gbl_tranlog_default_timeout;
  if( idxNum & 8 ){
    int64_t timeout = sqlite3_value_int64(argv[i++]);
    pCur->timeout = timeout;
  }
  bzero(&pCur->blockLsn, sizeof(pCur->blockLsn));
  if( idxNum & 16 ){
    const unsigned char *blockLsn = sqlite3_value_text(argv[i++]);
    if (blockLsn && parse_lsn(blockLsn, &pCur->blockLsn)) {
        return SQLITE_CONV_ERROR;
    }
  }
  pCur->iRowid = 1;
  return SQLITE_OK;
}

static int tranlogBestIndex(
  sqlite3_vtab *tab,
  sqlite3_index_info *pIdxInfo
){
  int i;                 /* Loop over constraints */
  int idxNum = 0;        /* The query plan bitmask */
  int startIdx = -1;     /* Index of the start= constraint, or -1 if none */
  int stopIdx = -1;      /* Index of the stop= constraint, or -1 if none */
  int flagsIdx = -1;     /* Index of the block= constraint, block waiting if set */
  int timeoutIdx = -1;
  int blockLsnIdx = -1;
  int nArg = 0;          /* Number of arguments that seriesFilter() expects */

  const struct sqlite3_index_constraint *pConstraint;
  pConstraint = pIdxInfo->aConstraint;
  for(i=0; i<pIdxInfo->nConstraint; i++, pConstraint++){
    if( pConstraint->usable==0 ) continue;
    if( pConstraint->op!=SQLITE_INDEX_CONSTRAINT_EQ ) continue;
    switch( pConstraint->iColumn ){
      case TRANLOG_COLUMN_START:
        startIdx = i;
        idxNum |= 1;
        break;
      case TRANLOG_COLUMN_STOP:
        stopIdx = i;
        idxNum |= 2;
        break;
      case TRANLOG_COLUMN_FLAGS:
        flagsIdx = i;
        idxNum |= 4;
        break;
      case TRANLOG_COLUMN_TIMEOUT:
        timeoutIdx = i;
        idxNum |= 8;
        break;
      case TRANLOG_COLUMN_BLOCKLSN:
        blockLsnIdx = i;
        idxNum |= 16;
        break;
    }
  }
  if( startIdx>=0 ){
    pIdxInfo->aConstraintUsage[startIdx].argvIndex = ++nArg;
    pIdxInfo->aConstraintUsage[startIdx].omit = 1;
  }
  if( stopIdx>=0 ){
    pIdxInfo->aConstraintUsage[stopIdx].argvIndex = ++nArg;
    pIdxInfo->aConstraintUsage[stopIdx].omit = 1;
  }
  if( flagsIdx>=0 ){
    pIdxInfo->aConstraintUsage[flagsIdx].argvIndex = ++nArg;
    pIdxInfo->aConstraintUsage[flagsIdx].omit = 1;
  }
  if( timeoutIdx>=0 ){
    pIdxInfo->aConstraintUsage[timeoutIdx].argvIndex = ++nArg;
    pIdxInfo->aConstraintUsage[timeoutIdx].omit = 1;
  }
  if( blockLsnIdx>=0 ){
    pIdxInfo->aConstraintUsage[blockLsnIdx].argvIndex = ++nArg;
    pIdxInfo->aConstraintUsage[blockLsnIdx].omit = 1;
  }
  if( (idxNum & 3)==3 ){
    /* Both start= and stop= boundaries are available.  This is the 
    ** the preferred case */
    pIdxInfo->estimatedCost = (double)1;
  }else{
    /* If either boundary is missing, we have to generate a huge span
    ** of numbers.  Make this case very expensive so that the query
    ** planner will work hard to avoid it. */
    pIdxInfo->estimatedCost = (double)2000000000;
  }
  pIdxInfo->idxNum = idxNum;
  return SQLITE_OK;
}

/*
** This following structure defines all the methods for the 
** generate_series virtual table.
*/
sqlite3_module systblTransactionLogsModule = {
  0,                 /* iVersion */
  0,                 /* xCreate */
  tranlogConnect,    /* xConnect */
  tranlogBestIndex,  /* xBestIndex */
  tranlogDisconnect, /* xDisconnect */
  0,                 /* xDestroy */
  tranlogOpen,       /* xOpen - open a cursor */
  tranlogClose,      /* xClose - close a cursor */
  tranlogFilter,     /* xFilter - configure scan constraints */
  tranlogNext,       /* xNext - advance a cursor */
  tranlogEof,        /* xEof - check for end of scan */
  tranlogColumn,     /* xColumn - read data */
  tranlogRowid,      /* xRowid - read data */
  0,                 /* xUpdate */
  0,                 /* xBegin */
  0,                 /* xSync */
  0,                 /* xCommit */
  0,                 /* xRollback */
  0,                 /* xFindMethod */
  0,                 /* xRename */
  0,                 /* xSavepoint */
  0,                 /* xRelease */
  0,                 /* xRollbackTo */
  0,                 /* xShadowName */
  .access_flag = CDB2_ALLOW_USER
};


