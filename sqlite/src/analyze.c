/*
** 2005-07-08
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains code associated with the ANALYZE command.
**
** The ANALYZE command gather statistics about the content of tables
** and indices.  These statistics are made available to the query planner
** to help it make better decisions about how to perform queries.
**
** The following system tables are or have been supported:
**
**    CREATE TABLE sqlite_stat1(tbl, idx, stat);
**    CREATE TABLE sqlite_stat2(tbl, idx, sampleno, sample);
**    CREATE TABLE sqlite_stat3(tbl, idx, nEq, nLt, nDLt, sample);
**    CREATE TABLE sqlite_stat4(tbl, idx, nEq, nLt, nDLt, sample);
**
** Additional tables might be added in future releases of SQLite.
** The sqlite_stat2 table is not created or used unless the SQLite version
** is between 3.6.18 and 3.7.8, inclusive, and unless SQLite is compiled
** with SQLITE_ENABLE_STAT2.  The sqlite_stat2 table is deprecated.
** The sqlite_stat2 table is superseded by sqlite_stat3, which is only
** created and used by SQLite versions 3.7.9 and later and with
** SQLITE_ENABLE_STAT3 defined.  The functionality of sqlite_stat3
** is a superset of sqlite_stat2.  The sqlite_stat4 is an enhanced
** version of sqlite_stat3 and is only available when compiled with
** SQLITE_ENABLE_STAT4 and in SQLite versions 3.8.1 and later.  It is
** not possible to enable both STAT3 and STAT4 at the same time.  If they
** are both enabled, then STAT4 takes precedence.
**
** For most applications, sqlite_stat1 provides all the statistics required
** for the query planner to make good choices.
**
** Format of sqlite_stat1:
**
** There is normally one row per index, with the index identified by the
** name in the idx column.  The tbl column is the name of the table to
** which the index belongs.  In each such row, the stat column will be
** a string consisting of a list of integers.  The first integer in this
** list is the number of rows in the index.  (This is the same as the
** number of rows in the table, except for partial indices.)  The second
** integer is the average number of rows in the index that have the same
** value in the first column of the index.  The third integer is the average
** number of rows in the index that have the same value for the first two
** columns.  The N-th integer (for N>1) is the average number of rows in 
** the index which have the same value for the first N-1 columns.  For
** a K-column index, there will be K+1 integers in the stat column.  If
** the index is unique, then the last integer will be 1.
**
** The list of integers in the stat column can optionally be followed
** by the keyword "unordered".  The "unordered" keyword, if it is present,
** must be separated from the last integer by a single space.  If the
** "unordered" keyword is present, then the query planner assumes that
** the index is unordered and will not use the index for a range query.
** 
** If the sqlite_stat1.idx column is NULL, then the sqlite_stat1.stat
** column contains a single integer which is the (estimated) number of
** rows in the table identified by sqlite_stat1.tbl.
**
** Format of sqlite_stat2:
**
** The sqlite_stat2 is only created and is only used if SQLite is compiled
** with SQLITE_ENABLE_STAT2 and if the SQLite version number is between
** 3.6.18 and 3.7.8.  The "stat2" table contains additional information
** about the distribution of keys within an index.  The index is identified by
** the "idx" column and the "tbl" column is the name of the table to which
** the index belongs.  There are usually 10 rows in the sqlite_stat2
** table for each index.
**
** The sqlite_stat2 entries for an index that have sampleno between 0 and 9
** inclusive are samples of the left-most key value in the index taken at
** evenly spaced points along the index.  Let the number of samples be S
** (10 in the standard build) and let C be the number of rows in the index.
** Then the sampled rows are given by:
**
**     rownumber = (i*C*2 + C)/(S*2)
**
** For i between 0 and S-1.  Conceptually, the index space is divided into
** S uniform buckets and the samples are the middle row from each bucket.
**
** The format for sqlite_stat2 is recorded here for legacy reference.  This
** version of SQLite does not support sqlite_stat2.  It neither reads nor
** writes the sqlite_stat2 table.  This version of SQLite only supports
** sqlite_stat3.
**
** Format for sqlite_stat3:
**
** The sqlite_stat3 format is a subset of sqlite_stat4.  Hence, the
** sqlite_stat4 format will be described first.  Further information
** about sqlite_stat3 follows the sqlite_stat4 description.
**
** Format for sqlite_stat4:
**
** As with sqlite_stat2, the sqlite_stat4 table contains histogram data
** to aid the query planner in choosing good indices based on the values
** that indexed columns are compared against in the WHERE clauses of
** queries.
**
** The sqlite_stat4 table contains multiple entries for each index.
** The idx column names the index and the tbl column is the table of the
** index.  If the idx and tbl columns are the same, then the sample is
** of the INTEGER PRIMARY KEY.  The sample column is a blob which is the
** binary encoding of a key from the index.  The nEq column is a
** list of integers.  The first integer is the approximate number
** of entries in the index whose left-most column exactly matches
** the left-most column of the sample.  The second integer in nEq
** is the approximate number of entries in the index where the
** first two columns match the first two columns of the sample.
** And so forth.  nLt is another list of integers that show the approximate
** number of entries that are strictly less than the sample.  The first
** integer in nLt contains the number of entries in the index where the
** left-most column is less than the left-most column of the sample.
** The K-th integer in the nLt entry is the number of index entries 
** where the first K columns are less than the first K columns of the
** sample.  The nDLt column is like nLt except that it contains the 
** number of distinct entries in the index that are less than the
** sample.
**
** There can be an arbitrary number of sqlite_stat4 entries per index.
** The ANALYZE command will typically generate sqlite_stat4 tables
** that contain between 10 and 40 samples which are distributed across
** the key space, though not uniformly, and which include samples with
** large nEq values.
**
** Format for sqlite_stat3 redux:
**
** The sqlite_stat3 table is like sqlite_stat4 except that it only
** looks at the left-most column of the index.  The sqlite_stat3.sample
** column contains the actual value of the left-most column instead
** of a blob encoding of the complete index key as is found in
** sqlite_stat4.sample.  The nEq, nLt, and nDLt entries of sqlite_stat3
** all contain just a single integer which is the same as the first
** integer in the equivalent columns in sqlite_stat4.
*/
#ifndef SQLITE_OMIT_ANALYZE
#include "sqliteInt.h"

#if defined(SQLITE_BUILDING_FOR_COMDB2)
#include <logmsg.h>
int is_comdb2_index_disableskipscan(const char *);
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */

#if defined(SQLITE_ENABLE_STAT4)
# define IsStat4     1
# define IsStat3     0
#elif defined(SQLITE_ENABLE_STAT3)
# define IsStat4     0
# define IsStat3     1
#else
# define IsStat4     0
# define IsStat3     0
# undef SQLITE_STAT4_SAMPLES
# define SQLITE_STAT4_SAMPLES 1
#endif
#define IsStat34    (IsStat3+IsStat4)  /* 1 for STAT3 or STAT4. 0 otherwise */

#if defined(SQLITE_BUILDING_FOR_COMDB2)
/*
** The number of samples of an index that SQLite takes in order to
** construct a histogram of the table content when running ANALYZE
** and with SQLITE_ENABLE_STAT2
*/
#define SQLITE_INDEX_SAMPLES 10

static __thread int skip4;
int64_t analyze_get_nrecs( int iTable );
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */

/*
** This routine generates code that opens the sqlite_statN tables.
** The sqlite_stat1 table is always relevant.  sqlite_stat2 is now
** obsolete.  sqlite_stat3 and sqlite_stat4 are only opened when
** appropriate compile-time options are provided.
**
** If the sqlite_statN tables do not previously exist, it is created.
**
** Argument zWhere may be a pointer to a buffer containing a table name,
** or it may be a NULL pointer. If it is not NULL, then all entries in
** the sqlite_statN tables associated with the named table are deleted.
** If zWhere==0, then code is generated to delete all stat table entries.
*/
static void openStatTable(
  Parse *pParse,          /* Parsing context */
  int iDb,                /* The database we are looking in */
  int iStatCur,           /* Open the sqlite_stat1 table on this cursor */
  const char *zWhere,     /* Delete entries for this table or index */
  const char *zWhereType  /* Either "tbl" or "idx" */
){
  static const struct {
    const char *zName;
    const char *zCols;
  } aTable[] = {
    { "sqlite_stat1", "tbl,idx,stat" },
#if defined(SQLITE_ENABLE_STAT4)
    { "sqlite_stat4", "tbl,idx,neq,nlt,ndlt,sample" },
    { "sqlite_stat3", 0 },
#elif defined(SQLITE_ENABLE_STAT3)
    { "sqlite_stat3", "tbl,idx,neq,nlt,ndlt,sample" },
    { "sqlite_stat4", 0 },
#else
    { "sqlite_stat3", 0 },
    { "sqlite_stat4", 0 },
#endif
  };
  int i;
  sqlite3 *db = pParse->db;
  Db *pDb;
  Vdbe *v = sqlite3GetVdbe(pParse);
  int aRoot[ArraySize(aTable)];
  u8 aCreateTbl[ArraySize(aTable)];

  if( v==0 ) return;
  assert( sqlite3BtreeHoldsAllMutexes(db) );
  assert( sqlite3VdbeDb(v)==db );
  pDb = &db->aDb[iDb];

  /* Create new statistic tables if they do not exist, or clear them
  ** if they do already exist.
  */
#if defined(SQLITE_BUILDING_FOR_COMDB2)
  if( db->isExpert==0 ){
    skip4 = 0; /* TODO: Flag on connection? */
    for(i=0; i<ArraySize(aTable); i++){
      const char *zTab = aTable[i].zName;
      Table *pStat;
      if( (pStat = sqlite3FindTable(db, zTab, NULL))==0 ){
        if( zTab[11]=='1' || zTab[11]=='3' ){
          /* do nothing */
        }else if( zTab[11]=='4' ){
          skip4 = 1;
        }
      }else{
        aRoot[i] = pStat->tnum;
        sqlite3VdbeAddOp4Int(v, OP_OpenWrite, iStatCur+i, aRoot[i], iDb, 3);
        sqlite3VdbeChangeP5(v, 0);
        VdbeComment((v, zTab));
      }
    }
  }else{ /* db->isExpert==0 */
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
  for(i=0; i<ArraySize(aTable); i++){
    const char *zTab = aTable[i].zName;
    Table *pStat;
    if( (pStat = sqlite3FindTable(db, zTab, pDb->zDbSName))==0 ){
      if( aTable[i].zCols ){
        /* The sqlite_statN table does not exist. Create it. Note that a 
        ** side-effect of the CREATE TABLE statement is to leave the rootpage 
        ** of the new table in register pParse->regRoot. This is important 
        ** because the OpenWrite opcode below will be needing it. */
        sqlite3NestedParse(pParse,
            "CREATE TABLE %Q.%s(%s)", pDb->zDbSName, zTab, aTable[i].zCols
        );
        aRoot[i] = pParse->regRoot;
        aCreateTbl[i] = OPFLAG_P2ISREG;
      }
    }else{
      /* The table already exists. If zWhere is not NULL, delete all entries 
      ** associated with the table zWhere. If zWhere is NULL, delete the
      ** entire contents of the table. */
      aRoot[i] = pStat->tnum;
      aCreateTbl[i] = 0;
      sqlite3TableLock(pParse, iDb, aRoot[i], 1, zTab);
      if( zWhere ){
        sqlite3NestedParse(pParse,
           "DELETE FROM %Q.%s WHERE %s=%Q",
           pDb->zDbSName, zTab, zWhereType, zWhere
        );
#ifdef SQLITE_ENABLE_PREUPDATE_HOOK
      }else if( db->xPreUpdateCallback ){
        sqlite3NestedParse(pParse, "DELETE FROM %Q.%s", pDb->zDbSName, zTab);
#endif
      }else{
#if defined(SQLITE_BUILDING_FOR_COMDB2)
        sqlite3NestedParse(pParse, "DELETE FROM %Q.%s", pDb->zDbSName, zTab);
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
        /* The sqlite_stat[134] table already exists.  Delete all rows. */
        sqlite3VdbeAddOp2(v, OP_Clear, aRoot[i], iDb);
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
      }
    }
  }

  /* Open the sqlite_stat[134] tables for writing. */
  for(i=0; aTable[i].zCols; i++){
    assert( i<ArraySize(aTable) );
    sqlite3VdbeAddOp4Int(v, OP_OpenWrite, iStatCur+i, aRoot[i], iDb, 3);
    sqlite3VdbeChangeP5(v, aCreateTbl[i]);
    VdbeComment((v, aTable[i].zName));
  }
#if defined(SQLITE_BUILDING_FOR_COMDB2)
  } /* db->isExpert==0 */
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
}

/*
** Recommended number of samples for sqlite_stat4
*/
#ifndef SQLITE_STAT4_SAMPLES
# define SQLITE_STAT4_SAMPLES 24
#endif

#if defined(SQLITE_BUILDING_FOR_COMDB2)
# define PACKEDROWSIZE 1024
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */

/*
** Three SQL functions - stat_init(), stat_push(), and stat_get() -
** share an instance of the following structure to hold their state
** information.
*/
typedef struct Stat4Accum Stat4Accum;
typedef struct Stat4Sample Stat4Sample;
struct Stat4Sample {
  tRowcnt *anEq;                  /* sqlite_stat4.nEq */
  tRowcnt *anDLt;                 /* sqlite_stat4.nDLt */
#ifdef SQLITE_ENABLE_STAT3_OR_STAT4
  tRowcnt *anLt;                  /* sqlite_stat4.nLt */
#if !defined(SQLITE_BUILDING_FOR_COMDB2)
  union {
    i64 iRowid;                     /* Rowid in main table of the key */
    u8 *aRowid;                     /* Key for WITHOUT ROWID tables */
  } u;
  u32 nRowid;                     /* Sizeof aRowid[] */
#endif /* !defined(SQLITE_BUILDING_FOR_COMDB2) */
  u8 isPSample;                   /* True if a periodic sample */
  int iCol;                       /* If !isPSample, the reason for inclusion */
  u32 iHash;                      /* Tiebreaker hash */
#if defined(SQLITE_BUILDING_FOR_COMDB2)
  u8 packedRow[PACKEDROWSIZE];
  u32 nPackedRow;
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
#endif
};                                                    
struct Stat4Accum {
#if defined(SQLITE_BUILDING_FOR_COMDB2)
  i64 nActualRow;           /* Number of rows in ??? */
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
  tRowcnt nRow;             /* Number of rows in the entire table */
  tRowcnt nPSample;         /* How often to do a periodic sample */
  int nCol;                 /* Number of columns in index + pk/rowid */
#if !defined(SQLITE_BUILDING_FOR_COMDB2)
  int nKeyCol;              /* Number of index columns w/o the pk/rowid */
#endif /* !defined(SQLITE_BUILDING_FOR_COMDB2) */
  int mxSample;             /* Maximum number of samples to accumulate */
  Stat4Sample current;      /* Current row as a Stat4Sample */
  u32 iPrn;                 /* Pseudo-random number used for sampling */
  Stat4Sample *aBest;       /* Array of nCol best samples */
  int iMin;                 /* Index in a[] of entry with minimum score */
  int nSample;              /* Current number of samples */
  int nMaxEqZero;           /* Max leading 0 in anEq[] for any a[] entry */
  int iGet;                 /* Index of current sample accessed by stat_get() */
  Stat4Sample *a;           /* Array of mxSample Stat4Sample objects */
  sqlite3 *db;              /* Database connection, for malloc() */
};

/* Reclaim memory used by a Stat4Sample
*/
#ifdef SQLITE_ENABLE_STAT3_OR_STAT4
static void sampleClear(sqlite3 *db, Stat4Sample *p){
#if defined(SQLITE_BUILDING_FOR_COMDB2)
  p->nPackedRow = 0;
  memset(p->packedRow, 0, sizeof(p->packedRow));
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
  assert( db!=0 );
  if( p->nRowid ){
    sqlite3DbFree(db, p->u.aRowid);
    p->nRowid = 0;
  }
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
}
#endif

/* Initialize the BLOB value of a ROWID
*/
#ifdef SQLITE_ENABLE_STAT3_OR_STAT4
#if defined(SQLITE_BUILDING_FOR_COMDB2)
static void sampleSetPackedRow(sqlite3 *db, Stat4Sample *p, int n, const u8 *pData){
  assert( db!=0 );
  assert( n<PACKEDROWSIZE );
  p->nPackedRow = n;
  memcpy(p->packedRow, pData, n);
}
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
static void sampleSetRowid(sqlite3 *db, Stat4Sample *p, int n, const u8 *pData){
  assert( db!=0 );
  if( p->nRowid ) sqlite3DbFree(db, p->u.aRowid);
  p->u.aRowid = sqlite3DbMallocRawNN(db, n);
  if( p->u.aRowid ){
    p->nRowid = n;
    memcpy(p->u.aRowid, pData, n);
  }else{
    p->nRowid = 0;
  }
}
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
#endif

/* Initialize the INTEGER value of a ROWID.
*/
#ifdef SQLITE_ENABLE_STAT3_OR_STAT4
#if !defined(SQLITE_BUILDING_FOR_COMDB2)
static void sampleSetRowidInt64(sqlite3 *db, Stat4Sample *p, i64 iRowid){
  assert( db!=0 );
  if( p->nRowid ) sqlite3DbFree(db, p->u.aRowid);
  p->nRowid = 0;
  p->u.iRowid = iRowid;
}
#endif /* !defined(SQLITE_BUILDING_FOR_COMDB2) */
#endif


/*
** Copy the contents of object (*pFrom) into (*pTo).
*/
#ifdef SQLITE_ENABLE_STAT3_OR_STAT4
static void sampleCopy(Stat4Accum *p, Stat4Sample *pTo, Stat4Sample *pFrom){
  pTo->isPSample = pFrom->isPSample;
  pTo->iCol = pFrom->iCol;
  pTo->iHash = pFrom->iHash;
  memcpy(pTo->anEq, pFrom->anEq, sizeof(tRowcnt)*p->nCol);
  memcpy(pTo->anLt, pFrom->anLt, sizeof(tRowcnt)*p->nCol);
  memcpy(pTo->anDLt, pFrom->anDLt, sizeof(tRowcnt)*p->nCol);
#if defined(SQLITE_BUILDING_FOR_COMDB2)
  sampleSetPackedRow(p->db, pTo, pFrom->nPackedRow, pFrom->packedRow);
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
  if( pFrom->nRowid ){
    sampleSetRowid(p->db, pTo, pFrom->nRowid, pFrom->u.aRowid);
  }else{
    sampleSetRowidInt64(p->db, pTo, pFrom->u.iRowid);
  }
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
}
#endif

/*
** Reclaim all memory of a Stat4Accum structure.
*/
static void stat4Destructor(void *pOld){
  Stat4Accum *p = (Stat4Accum*)pOld;
#ifdef SQLITE_ENABLE_STAT3_OR_STAT4
  int i;
  for(i=0; i<p->nCol; i++) sampleClear(p->db, p->aBest+i);
  for(i=0; i<p->mxSample; i++) sampleClear(p->db, p->a+i);
  sampleClear(p->db, &p->current);
#endif
  sqlite3DbFree(p->db, p);
}

#if defined(SQLITE_BUILDING_FOR_COMDB2)
static inline int numRowsToNumSamplesEst(u64 n)
{
  int s;
  if     (n <     10000) s =    24;
  else if(n <     50000) s =    32;
  else if(n <    100000) s =    48;
  else if(n <   1000000) s =  2*32;
  else if(n <  10000000) s =  4*32;
  else if(n < 100000000) s =  8*32;
  else                   s = 16*32;
  return s;
}
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */

/*
** Implementation of the stat_init(N,K,C) SQL function. The three parameters
** are:
**     N:    The number of columns in the index including the rowid/pk (note 1)
**     K:    The number of columns in the index excluding the rowid/pk.
**     C:    The number of rows in the index (note 2)
**
** Note 1:  In the special case of the covering index that implements a
** WITHOUT ROWID table, N is the number of PRIMARY KEY columns, not the
** total number of columns in the table.
**
** Note 2:  C is only used for STAT3 and STAT4.
**
** For indexes on ordinary rowid tables, N==K+1.  But for indexes on
** WITHOUT ROWID tables, N=K+P where P is the number of columns in the
** PRIMARY KEY of the table.  The covering index that implements the
** original WITHOUT ROWID table as N==K as a special case.
**
** This routine allocates the Stat4Accum object in heap memory. The return 
** value is a pointer to the Stat4Accum object.  The datatype of the
** return value is BLOB, but it is really just a pointer to the Stat4Accum
** object.
*/
static void statInit(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  Stat4Accum *p;
  int nCol;                       /* Number of columns in index being sampled */
#if !defined(SQLITE_BUILDING_FOR_COMDB2)
  int nKeyCol;                    /* Number of key columns */
#endif /* !defined(SQLITE_BUILDING_FOR_COMDB2) */
  int nColUp;                     /* nCol rounded up for alignment */
  int n;                          /* Bytes of space to allocate */
  sqlite3 *db;                    /* Database connection */
#ifdef SQLITE_ENABLE_STAT3_OR_STAT4
  int mxSample = SQLITE_STAT4_SAMPLES;
#if defined(SQLITE_BUILDING_FOR_COMDB2)
  u64 nRows = sqlite3_value_int64(argv[1]);
  if( sqlite3_gbl_tunables.stat4_samples_multiplier > 0 ){
      mxSample = numRowsToNumSamplesEst(nRows) * sqlite3_gbl_tunables.stat4_samples_multiplier;
  }
  if( sqlite3_gbl_tunables.stat4_extra_samples > 0 ){
      mxSample += sqlite3_gbl_tunables.stat4_extra_samples;
  }
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
#endif

  /* Decode the three function arguments */
  UNUSED_PARAMETER(argc);
  nCol = sqlite3_value_int(argv[0]);
#if defined(SQLITE_BUILDING_FOR_COMDB2)
  assert( nCol>1 ); /* >1 because it includes the rowid column */
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
  assert( nCol>0 );
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
  nColUp = sizeof(tRowcnt)<8 ? (nCol+1)&~1 : nCol;
#if !defined(SQLITE_BUILDING_FOR_COMDB2)
  nKeyCol = sqlite3_value_int(argv[1]);
  assert( nKeyCol<=nCol );
  assert( nKeyCol>0 );
#endif /* !defined(SQLITE_BUILDING_FOR_COMDB2) */

  /* Allocate the space required for the Stat4Accum object */
  n = sizeof(*p) 
    + sizeof(tRowcnt)*nColUp                  /* Stat4Accum.anEq */
    + sizeof(tRowcnt)*nColUp                  /* Stat4Accum.anDLt */
#ifdef SQLITE_ENABLE_STAT3_OR_STAT4
    + sizeof(tRowcnt)*nColUp                  /* Stat4Accum.anLt */
    + sizeof(Stat4Sample)*(nCol+mxSample)     /* Stat4Accum.aBest[], a[] */
    + sizeof(tRowcnt)*3*nColUp*(nCol+mxSample)
#endif
  ;
  db = sqlite3_context_db_handle(context);
  p = sqlite3DbMallocZero(db, n);
  if( p==0 ){
    sqlite3_result_error_nomem(context);
    return;
  }

  p->db = db;
  p->nRow = 0;
#if defined(SQLITE_BUILDING_FOR_COMDB2)
  p->nActualRow = sqlite3_value_int64(argv[2]);
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
  p->nCol = nCol;
#if !defined(SQLITE_BUILDING_FOR_COMDB2)
  p->nKeyCol = nKeyCol;
#endif /* !defined(SQLITE_BUILDING_FOR_COMDB2) */
  p->current.anDLt = (tRowcnt*)&p[1];
  p->current.anEq = &p->current.anDLt[nColUp];

#ifdef SQLITE_ENABLE_STAT3_OR_STAT4
  {
    u8 *pSpace;                     /* Allocated space not yet assigned */
    int i;                          /* Used to iterate through p->aSample[] */

    p->iGet = -1;
    p->mxSample = mxSample;
#if defined(SQLITE_BUILDING_FOR_COMDB2)
    p->nPSample = (tRowcnt)(nRows/(mxSample/3+1) + 1);
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    p->nPSample = (tRowcnt)(sqlite3_value_int64(argv[2])/(mxSample/3+1) + 1);
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    p->current.anLt = &p->current.anEq[nColUp];
#if defined(SQLITE_BUILDING_FOR_COMDB2)
    p->iPrn = 0x689e962d*(u32)nCol ^ 0xd0944565*(u32)sqlite3_value_int(argv[1]);
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    p->iPrn = 0x689e962d*(u32)nCol ^ 0xd0944565*(u32)sqlite3_value_int(argv[2]);
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
  
    /* Set up the Stat4Accum.a[] and aBest[] arrays */
    p->a = (struct Stat4Sample*)&p->current.anLt[nColUp];
    p->aBest = &p->a[mxSample];
    pSpace = (u8*)(&p->a[mxSample+nCol]);
    for(i=0; i<(mxSample+nCol); i++){
      p->a[i].anEq = (tRowcnt *)pSpace; pSpace += (sizeof(tRowcnt) * nColUp);
      p->a[i].anLt = (tRowcnt *)pSpace; pSpace += (sizeof(tRowcnt) * nColUp);
      p->a[i].anDLt = (tRowcnt *)pSpace; pSpace += (sizeof(tRowcnt) * nColUp);
    }
    assert( (pSpace - (u8*)p)==n );
  
    for(i=0; i<nCol; i++){
      p->aBest[i].iCol = i;
    }
  }
#endif

  /* Return a pointer to the allocated object to the caller.  Note that
  ** only the pointer (the 2nd parameter) matters.  The size of the object
  ** (given by the 3rd parameter) is never used and can be any positive
  ** value. */
  sqlite3_result_blob(context, p, sizeof(*p), stat4Destructor);
}
static const FuncDef statInitFuncdef = {
  2+IsStat34,      /* nArg */
  SQLITE_UTF8,     /* funcFlags */
  0,               /* pUserData */
  0,               /* pNext */
  statInit,        /* xSFunc */
  0,               /* xFinalize */
  0, 0,            /* xValue, xInverse */
  "stat_init",     /* zName */
  {0}
};

#ifdef SQLITE_ENABLE_STAT4
/*
** pNew and pOld are both candidate non-periodic samples selected for 
** the same column (pNew->iCol==pOld->iCol). Ignoring this column and 
** considering only any trailing columns and the sample hash value, this
** function returns true if sample pNew is to be preferred over pOld.
** In other words, if we assume that the cardinalities of the selected
** column for pNew and pOld are equal, is pNew to be preferred over pOld.
**
** This function assumes that for each argument sample, the contents of
** the anEq[] array from pSample->anEq[pSample->iCol+1] onwards are valid. 
*/
static int sampleIsBetterPost(
  Stat4Accum *pAccum, 
  Stat4Sample *pNew, 
  Stat4Sample *pOld
){
  int nCol = pAccum->nCol;
  int i;
  assert( pNew->iCol==pOld->iCol );
  for(i=pNew->iCol+1; i<nCol; i++){
    if( pNew->anEq[i]>pOld->anEq[i] ) return 1;
    if( pNew->anEq[i]<pOld->anEq[i] ) return 0;
  }
  if( pNew->iHash>pOld->iHash ) return 1;
  return 0;
}
#endif

#ifdef SQLITE_ENABLE_STAT3_OR_STAT4
/*
** Return true if pNew is to be preferred over pOld.
**
** This function assumes that for each argument sample, the contents of
** the anEq[] array from pSample->anEq[pSample->iCol] onwards are valid. 
*/
static int sampleIsBetter(
  Stat4Accum *pAccum, 
  Stat4Sample *pNew, 
  Stat4Sample *pOld
){
  tRowcnt nEqNew = pNew->anEq[pNew->iCol];
  tRowcnt nEqOld = pOld->anEq[pOld->iCol];

  assert( pOld->isPSample==0 && pNew->isPSample==0 );
  assert( IsStat4 || (pNew->iCol==0 && pOld->iCol==0) );

  if( (nEqNew>nEqOld) ) return 1;
#ifdef SQLITE_ENABLE_STAT4
  if( nEqNew==nEqOld ){
    if( pNew->iCol<pOld->iCol ) return 1;
    return (pNew->iCol==pOld->iCol && sampleIsBetterPost(pAccum, pNew, pOld));
  }
  return 0;
#else
  return (nEqNew==nEqOld && pNew->iHash>pOld->iHash);
#endif
}

/*
** Copy the contents of sample *pNew into the p->a[] array. If necessary,
** remove the least desirable sample from p->a[] to make room.
*/
static void sampleInsert(Stat4Accum *p, Stat4Sample *pNew, int nEqZero){
  Stat4Sample *pSample = 0;
  int i;

  assert( IsStat4 || nEqZero==0 );

#ifdef SQLITE_ENABLE_STAT4
  /* Stat4Accum.nMaxEqZero is set to the maximum number of leading 0
  ** values in the anEq[] array of any sample in Stat4Accum.a[]. In
  ** other words, if nMaxEqZero is n, then it is guaranteed that there
  ** are no samples with Stat4Sample.anEq[m]==0 for (m>=n). */
  if( nEqZero>p->nMaxEqZero ){
    p->nMaxEqZero = nEqZero;
  }
  if( pNew->isPSample==0 ){
    Stat4Sample *pUpgrade = 0;
    assert( pNew->anEq[pNew->iCol]>0 );

    /* This sample is being added because the prefix that ends in column 
    ** iCol occurs many times in the table. However, if we have already
    ** added a sample that shares this prefix, there is no need to add
    ** this one. Instead, upgrade the priority of the highest priority
    ** existing sample that shares this prefix.  */
    for(i=p->nSample-1; i>=0; i--){
      Stat4Sample *pOld = &p->a[i];
      if( pOld->anEq[pNew->iCol]==0 ){
        if( pOld->isPSample ) return;
        assert( pOld->iCol>pNew->iCol );
        assert( sampleIsBetter(p, pNew, pOld) );
        if( pUpgrade==0 || sampleIsBetter(p, pOld, pUpgrade) ){
          pUpgrade = pOld;
        }
      }
    }
    if( pUpgrade ){
      pUpgrade->iCol = pNew->iCol;
      pUpgrade->anEq[pUpgrade->iCol] = pNew->anEq[pUpgrade->iCol];
      goto find_new_min;
    }
  }
#endif

  /* If necessary, remove sample iMin to make room for the new sample. */
  if( p->nSample>=p->mxSample ){
    Stat4Sample *pMin = &p->a[p->iMin];
    tRowcnt *anEq = pMin->anEq;
    tRowcnt *anLt = pMin->anLt;
    tRowcnt *anDLt = pMin->anDLt;
    sampleClear(p->db, pMin);
    memmove(pMin, &pMin[1], sizeof(p->a[0])*(p->nSample-p->iMin-1));
    pSample = &p->a[p->nSample-1];
#if defined(SQLITE_BUILDING_FOR_COMDB2)
    pSample->nPackedRow = 0;
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    pSample->nRowid = 0;
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    pSample->anEq = anEq;
    pSample->anDLt = anDLt;
    pSample->anLt = anLt;
    p->nSample = p->mxSample-1;
  }

  /* The "rows less-than" for the rowid column must be greater than that
  ** for the last sample in the p->a[] array. Otherwise, the samples would
  ** be out of order. */
#ifdef SQLITE_ENABLE_STAT4
  assert( p->nSample==0 
       || pNew->anLt[p->nCol-1] > p->a[p->nSample-1].anLt[p->nCol-1] );
#endif

  /* Insert the new sample */
  pSample = &p->a[p->nSample];
  sampleCopy(p, pSample, pNew);
  p->nSample++;

  /* Zero the first nEqZero entries in the anEq[] array. */
  memset(pSample->anEq, 0, sizeof(tRowcnt)*nEqZero);

#ifdef SQLITE_ENABLE_STAT4
 find_new_min:
#endif
  if( p->nSample>=p->mxSample ){
    int iMin = -1;
    for(i=0; i<p->mxSample; i++){
      if( p->a[i].isPSample ) continue;
      if( iMin<0 || sampleIsBetter(p, &p->a[iMin], &p->a[i]) ){
        iMin = i;
      }
    }
#if defined(SQLITE_BUILDING_FOR_COMDB2)
    if( iMin==-1 ){
      /* Number of records has increased N times (N > ~200%) since statInit(),
         however we don't know what N is. Assume that N is 2. It takes log3(N)
         iterations to get the right sample frequency. Note that This approach
         will make the sampling distribution more left skewed. */
      p->nPSample *= 3;
      for(i=0; i<p->mxSample; i+=3){
        p->a[i].isPSample = 0;
      }
      goto find_new_min;
    }
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    assert( iMin>=0 );
    p->iMin = iMin;
  }
}
#endif /* SQLITE_ENABLE_STAT3_OR_STAT4 */

/*
** Field iChng of the index being scanned has changed. So at this point
** p->current contains a sample that reflects the previous row of the
** index. The value of anEq[iChng] and subsequent anEq[] elements are
** correct at this point.
*/
static void samplePushPrevious(Stat4Accum *p, int iChng){
#ifdef SQLITE_ENABLE_STAT4
  int i;

  /* Check if any samples from the aBest[] array should be pushed
  ** into IndexSample.a[] at this point.  */
  for(i=(p->nCol-2); i>=iChng; i--){
    Stat4Sample *pBest = &p->aBest[i];
    pBest->anEq[i] = p->current.anEq[i];
    if( p->nSample<p->mxSample || sampleIsBetter(p, pBest, &p->a[p->iMin]) ){
      sampleInsert(p, pBest, i);
    }
  }

  /* Check that no sample contains an anEq[] entry with an index of
  ** p->nMaxEqZero or greater set to zero. */
  for(i=p->nSample-1; i>=0; i--){
    int j;
    for(j=p->nMaxEqZero; j<p->nCol; j++) assert( p->a[i].anEq[j]>0 );
  }

  /* Update the anEq[] fields of any samples already collected. */
  if( iChng<p->nMaxEqZero ){
    for(i=p->nSample-1; i>=0; i--){
      int j;
      for(j=iChng; j<p->nCol; j++){
        if( p->a[i].anEq[j]==0 ) p->a[i].anEq[j] = p->current.anEq[j];
      }
    }
    p->nMaxEqZero = iChng;
  }
#endif

#if defined(SQLITE_ENABLE_STAT3) && !defined(SQLITE_ENABLE_STAT4)
  if( iChng==0 ){
    tRowcnt nLt = p->current.anLt[0];
    tRowcnt nEq = p->current.anEq[0];

    /* Check if this is to be a periodic sample. If so, add it. */
    if( (nLt/p->nPSample)!=(nLt+nEq)/p->nPSample ){
      p->current.isPSample = 1;
      sampleInsert(p, &p->current, 0);
      p->current.isPSample = 0;
    }else 

    /* Or if it is a non-periodic sample. Add it in this case too. */
    if( p->nSample<p->mxSample 
     || sampleIsBetter(p, &p->current, &p->a[p->iMin]) 
    ){
      sampleInsert(p, &p->current, 0);
    }
  }
#endif

#ifndef SQLITE_ENABLE_STAT3_OR_STAT4
  UNUSED_PARAMETER( p );
  UNUSED_PARAMETER( iChng );
#endif
}

/*
** Implementation of the stat_push SQL function:  stat_push(P,C,R)
** Arguments:
**
**    P     Pointer to the Stat4Accum object created by stat_init()
**    C     Index of left-most column to differ from previous row
**    R     Rowid for the current row.  Might be a key record for
**          WITHOUT ROWID tables.
**
** This SQL function always returns NULL.  It's purpose it to accumulate
** statistical data and/or samples in the Stat4Accum object about the
** index being analyzed.  The stat_get() SQL function will later be used to
** extract relevant information for constructing the sqlite_statN tables.
**
** The R parameter is only used for STAT3 and STAT4
*/
static void statPush(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  int i;

  /* The three function arguments */
  Stat4Accum *p = (Stat4Accum*)sqlite3_value_blob(argv[0]);
  int iChng = sqlite3_value_int(argv[1]);

  UNUSED_PARAMETER( argc );
  UNUSED_PARAMETER( context );
#if defined(SQLITE_BUILDING_FOR_COMDB2)
  assert( p->nCol>1 );        /* Includes rowid field */
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
  assert( p->nCol>0 );
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
  assert( iChng<p->nCol );

  if( p->nRow==0 ){
    /* This is the first call to this function. Do initialization. */
    for(i=0; i<p->nCol; i++) p->current.anEq[i] = 1;
  }else{
    /* Second and subsequent calls get processed here */
    samplePushPrevious(p, iChng);

    /* Update anDLt[], anLt[] and anEq[] to reflect the values that apply
    ** to the current row of the index. */
    for(i=0; i<iChng; i++){
      p->current.anEq[i]++;
    }
    for(i=iChng; i<p->nCol; i++){
      p->current.anDLt[i]++;
#ifdef SQLITE_ENABLE_STAT3_OR_STAT4
      p->current.anLt[i] += p->current.anEq[i];
#endif
      p->current.anEq[i] = 1;
    }
  }
  p->nRow++;
#ifdef SQLITE_ENABLE_STAT3_OR_STAT4
#if defined(SQLITE_BUILDING_FOR_COMDB2)
  sampleSetPackedRow(p->db, &p->current, sqlite3_value_bytes(argv[2]),
                                         sqlite3_value_blob(argv[2]));
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
  if( sqlite3_value_type(argv[2])==SQLITE_INTEGER ){
    sampleSetRowidInt64(p->db, &p->current, sqlite3_value_int64(argv[2]));
  }else{
    sampleSetRowid(p->db, &p->current, sqlite3_value_bytes(argv[2]),
                                       sqlite3_value_blob(argv[2]));
  }
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
  p->current.iHash = p->iPrn = p->iPrn*1103515245 + 12345;
#endif

#ifdef SQLITE_ENABLE_STAT4
  {
    tRowcnt nLt = p->current.anLt[p->nCol-1];

    /* Check if this is to be a periodic sample. If so, add it. */
    if( (nLt/p->nPSample)!=(nLt+1)/p->nPSample ){
      p->current.isPSample = 1;
      p->current.iCol = 0;
      sampleInsert(p, &p->current, p->nCol-1);
      p->current.isPSample = 0;
    }

    /* Update the aBest[] array. */
    for(i=0; i<(p->nCol-1); i++){
      p->current.iCol = i;
      if( i>=iChng || sampleIsBetterPost(p, &p->current, &p->aBest[i]) ){
        sampleCopy(p, &p->aBest[i], &p->current);
      }
    }
  }
#endif
}
static const FuncDef statPushFuncdef = {
  2+IsStat34,      /* nArg */
  SQLITE_UTF8,     /* funcFlags */
  0,               /* pUserData */
  0,               /* pNext */
  statPush,        /* xSFunc */
  0,               /* xFinalize */
  0, 0,            /* xValue, xInverse */
  "stat_push",     /* zName */
  {0}
};

#if defined(SQLITE_BUILDING_FOR_COMDB2)
#define STAT_GET_STAT1 0          /* "stat" column of stat1 table */
#define STAT_GET_NEQ   1          /* "neq" column of stat[34] entry */
#define STAT_GET_NLT   2          /* "nlt" column of stat[34] entry */
#define STAT_GET_NDLT  3          /* "ndlt" column of stat[34] entry */
#define STAT_GET_ROW   4
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
#define STAT_GET_STAT1 0          /* "stat" column of stat1 table */
#define STAT_GET_ROWID 1          /* "rowid" column of stat[34] entry */
#define STAT_GET_NEQ   2          /* "neq" column of stat[34] entry */
#define STAT_GET_NLT   3          /* "nlt" column of stat[34] entry */
#define STAT_GET_NDLT  4          /* "ndlt" column of stat[34] entry */
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */

/*
** Implementation of the stat_get(P,J) SQL function.  This routine is
** used to query statistical information that has been gathered into
** the Stat4Accum object by prior calls to stat_push().  The P parameter
** has type BLOB but it is really just a pointer to the Stat4Accum object.
** The content to returned is determined by the parameter J
** which is one of the STAT_GET_xxxx values defined above.
**
** The stat_get(P,J) function is not available to generic SQL.  It is
** inserted as part of a manually constructed bytecode program.  (See
** the callStatGet() routine below.)  It is guaranteed that the P
** parameter will always be a poiner to a Stat4Accum object, never a
** NULL.
**
** If neither STAT3 nor STAT4 are enabled, then J is always
** STAT_GET_STAT1 and is hence omitted and this routine becomes
** a one-parameter function, stat_get(P), that always returns the
** stat1 table entry information.
*/
static void statGet(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  Stat4Accum *p = (Stat4Accum*)sqlite3_value_blob(argv[0]);
#ifdef SQLITE_ENABLE_STAT3_OR_STAT4
  /* STAT3 and STAT4 have a parameter on this routine. */
  int eCall = sqlite3_value_int(argv[1]);
  assert( argc==2 );
#if defined(SQLITE_BUILDING_FOR_COMDB2)
  assert( eCall==STAT_GET_STAT1 || eCall==STAT_GET_NEQ
       || eCall==STAT_GET_ROW   || eCall==STAT_GET_NLT
       || eCall==STAT_GET_NDLT
  );
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
  assert( eCall==STAT_GET_STAT1 || eCall==STAT_GET_NEQ 
       || eCall==STAT_GET_ROWID || eCall==STAT_GET_NLT
       || eCall==STAT_GET_NDLT 
  );
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
  if( eCall==STAT_GET_STAT1 )
#else
  assert( argc==1 );
#endif
  {
    /* Return the value to store in the "stat" column of the sqlite_stat1
    ** table for this index.
    **
    ** The value is a string composed of a list of integers describing 
    ** the index. The first integer in the list is the total number of 
    ** entries in the index. There is one additional integer in the list 
    ** for each indexed column. This additional integer is an estimate of
    ** the number of rows matched by a stabbing query on the index using
    ** a key with the corresponding number of fields. In other words,
    ** if the index is on columns (a,b) and the sqlite_stat1 value is 
    ** "100 10 2", then SQLite estimates that:
    **
    **   * the index contains 100 rows,
    **   * "WHERE a=?" matches 10 rows, and
    **   * "WHERE a=? AND b=?" matches 2 rows.
    **
    ** If D is the count of distinct values and K is the total number of 
    ** rows, then each estimate is computed as:
    **
    **        I = (K+D-1)/D
    */
    char *z;
    int i;

#if defined(SQLITE_BUILDING_FOR_COMDB2)
    char *zRet = sqlite3MallocZero( p->nCol*25 );
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    char *zRet = sqlite3MallocZero( (p->nKeyCol+1)*25 );
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    if( zRet==0 ){
      sqlite3_result_error_nomem(context);
      return;
    }

#if defined(SQLITE_BUILDING_FOR_COMDB2)
    {
      u64 nRow = p->nActualRow > 0 ? p->nActualRow : p->nRow;
      /* Never let the estimated number of rows be less than 1 */
      sqlite3_snprintf(24, zRet, "%llu", MAX(nRow, 1));
      z = zRet + sqlite3Strlen30(zRet);
      for(i=0; i<(p->nCol-1); i++){
        u64 nDistinct = p->current.anDLt[i] + 1;
        u64 iVal = (nRow + nDistinct - 1) / nDistinct;
        sqlite3_snprintf(24, z, " %llu", iVal);
        z += sqlite3Strlen30(z);
      }
    }
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    sqlite3_snprintf(24, zRet, "%llu", (u64)p->nRow);
    z = zRet + sqlite3Strlen30(zRet);
    for(i=0; i<p->nKeyCol; i++){
      u64 nDistinct = p->current.anDLt[i] + 1;
      u64 iVal = (p->nRow + nDistinct - 1) / nDistinct;
      sqlite3_snprintf(24, z, " %llu", iVal);
      z += sqlite3Strlen30(z);
      assert( p->current.anEq[i] );
    }
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    assert( z[0]=='\0' && z>zRet );

    sqlite3_result_text(context, zRet, -1, sqlite3_free);
#if defined(SQLITE_BUILDING_FOR_COMDB2)
    return; /* end of sqlite_stat1 */
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
  }
#ifdef SQLITE_ENABLE_STAT3_OR_STAT4
#if defined(SQLITE_BUILDING_FOR_COMDB2)
  {
    Stat4Sample *pS = NULL;
    tRowcnt *aCnt = NULL;
    int scale = 1;
    int iGet;
    if( p->iGet<0 ){
      samplePushPrevious(p, 0);
      p->iGet = 0;
    }
    iGet = p->iGet;
    if( iGet>=p->nSample ){
      sqlite3_result_null(context);
      return;
    }
    switch( eCall ){
      case STAT_GET_NEQ:  aCnt = p->a[iGet].anEq; break;
      case STAT_GET_NLT:  aCnt = p->a[iGet].anLt; break;
      case STAT_GET_NDLT: aCnt = p->a[iGet].anDLt; break;
      case STAT_GET_ROW:
        pS = p->a + iGet;
        sqlite3_result_blob(context, pS->packedRow, pS->nPackedRow,
                            SQLITE_TRANSIENT);
        ++p->iGet;
        break;
    }
    if( p->nActualRow>0 ){
      scale = (int)((p->nActualRow / (double)p->nRow) + 0.5);
    }
    if( eCall!=STAT_GET_ROW ){
      char *zRet = sqlite3MallocZero( p->nCol*25 );
      if( zRet==0 ){
        sqlite3_result_error_nomem(context);
      }else{
        int i;
        char *z = zRet;
        for(i=0; i<p->nCol; i++){
          if( aCnt[i] > 2 ){
            sqlite3_snprintf(24, z, "%llu ", (u64)aCnt[i] * scale);
          }else{
            sqlite3_snprintf(24, z, "%llu ", (u64)aCnt[i]);
          }
          z += sqlite3Strlen30(z);
        }
        assert( z[0]=='\0' && z>zRet );
        z[-1] = '\0';
        sqlite3_result_text(context, zRet, -1, sqlite3_free);
      }
    }
  }
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
  else if( eCall==STAT_GET_ROWID ){
    if( p->iGet<0 ){
      samplePushPrevious(p, 0);
      p->iGet = 0;
    }
    if( p->iGet<p->nSample ){
      Stat4Sample *pS = p->a + p->iGet;
      if( pS->nRowid==0 ){
        sqlite3_result_int64(context, pS->u.iRowid);
      }else{
        sqlite3_result_blob(context, pS->u.aRowid, pS->nRowid,
                            SQLITE_TRANSIENT);
      }
    }
  }else{
    tRowcnt *aCnt = 0;

    assert( p->iGet<p->nSample );
    switch( eCall ){
      case STAT_GET_NEQ:  aCnt = p->a[p->iGet].anEq; break;
      case STAT_GET_NLT:  aCnt = p->a[p->iGet].anLt; break;
      default: {
        aCnt = p->a[p->iGet].anDLt; 
        p->iGet++;
        break;
      }
    }

    if( IsStat3 ){
      sqlite3_result_int64(context, (i64)aCnt[0]);
    }else{
      char *zRet = sqlite3MallocZero(p->nCol * 25);
      if( zRet==0 ){
        sqlite3_result_error_nomem(context);
      }else{
        int i;
        char *z = zRet;
        for(i=0; i<p->nCol; i++){
          sqlite3_snprintf(24, z, "%llu ", (u64)aCnt[i]);
          z += sqlite3Strlen30(z);
        }
        assert( z[0]=='\0' && z>zRet );
        z[-1] = '\0';
        sqlite3_result_text(context, zRet, -1, sqlite3_free);
      }
    }
  }
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
#endif /* SQLITE_ENABLE_STAT3_OR_STAT4 */
#ifndef SQLITE_DEBUG
  UNUSED_PARAMETER( argc );
#endif
}
static const FuncDef statGetFuncdef = {
  1+IsStat34,      /* nArg */
  SQLITE_UTF8,     /* funcFlags */
  0,               /* pUserData */
  0,               /* pNext */
  statGet,         /* xSFunc */
  0,               /* xFinalize */
  0, 0,            /* xValue, xInverse */
  "stat_get",      /* zName */
  {0}
};

static void callStatGet(Vdbe *v, int regStat4, int iParam, int regOut){
  assert( regOut!=regStat4 && regOut!=regStat4+1 );
#ifdef SQLITE_ENABLE_STAT3_OR_STAT4
  sqlite3VdbeAddOp2(v, OP_Integer, iParam, regStat4+1);
#elif SQLITE_DEBUG
  assert( iParam==STAT_GET_STAT1 );
#else
  UNUSED_PARAMETER( iParam );
#endif
  sqlite3VdbeAddOp4(v, OP_Function0, 0, regStat4, regOut,
                    (char*)&statGetFuncdef, P4_FUNCDEF);
  sqlite3VdbeChangeP5(v, 1 + IsStat34);
}

/*
** Generate code to do an analysis of all indices associated with
** a single table.
*/
static void analyzeOneTable(
  Parse *pParse,   /* Parser context */
  Table *pTab,     /* Table whose indices are to be analyzed */
  Index *pOnlyIdx, /* If not NULL, only analyze this one index */
  int iStatCur,    /* Index of VdbeCursor that writes the sqlite_stat1 table */
  int iMem,        /* Available memory locations begin here */
  int iTab         /* Next available cursor */
){
  sqlite3 *db = pParse->db;    /* Database handle */
  Index *pIdx;                 /* An index to being analyzed */
  int iIdxCur;                 /* Cursor open on index being analyzed */
  int iTabCur;                 /* Table cursor */
  Vdbe *v;                     /* The virtual machine being built up */
  int i;                       /* Loop counter */
#if !defined(SQLITE_BUILDING_FOR_COMDB2)
  int jZeroRows = -1;          /* Jump from here if number of rows is zero */
#endif /* !defined(SQLITE_BUILDING_FOR_COMDB2) */
  int iDb;                     /* Index of database containing pTab */
  u8 needTableCnt = 1;         /* True to count the table */
  int regNewRowid = iMem++;    /* Rowid for the inserted record */
  int regStat4 = iMem++;       /* Register to hold Stat4Accum object */
  int regChng = iMem++;        /* Index of changed index field */
#ifdef SQLITE_ENABLE_STAT3_OR_STAT4
#if defined(SQLITE_BUILDING_FOR_COMDB2)
  int regSampleRow = iMem++;   /* Register to hold sample row */
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
  int regRowid = iMem++;       /* Rowid argument passed to stat_push() */
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
#endif
  int regTemp = iMem++;        /* Temporary use register */
  int regTabname = iMem++;     /* Register containing table name */
  int regIdxname = iMem++;     /* Register containing index name */
  int regStat1 = iMem++;       /* Value for the stat column of sqlite_stat1 */
  int regPrev = iMem;          /* MUST BE LAST (see below) */
#ifdef SQLITE_ENABLE_PREUPDATE_HOOK
  Table *pStat1 = 0; 
#endif

  pParse->nMem = MAX(pParse->nMem, iMem);
  v = sqlite3GetVdbe(pParse);
  if( v==0 || NEVER(pTab==0) ){
    return;
  }
  if( pTab->tnum==0 ){
    /* Do not gather statistics on views or virtual tables */
    return;
  }
  if( sqlite3_strlike("sqlite\\_%", pTab->zName, '\\')==0 ){
    /* Do not gather statistics on system tables */
    return;
  }
  assert( sqlite3BtreeHoldsAllMutexes(db) );
  iDb = sqlite3SchemaToIndex(db, pTab->pSchema);
  assert( iDb>=0 );
#if defined(SQLITE_BUILDING_FOR_COMDB2)
  assert( sqlite3SchemaMutexHeld(db, iDb, 0) );
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
#ifndef SQLITE_OMIT_AUTHORIZATION
  if( sqlite3AuthCheck(pParse, SQLITE_ANALYZE, pTab->zName, 0,
      db->aDb[iDb].zDbSName ) ){
    return;
  }
#endif

#ifdef SQLITE_ENABLE_PREUPDATE_HOOK
  if( db->xPreUpdateCallback ){
    pStat1 = (Table*)sqlite3DbMallocZero(db, sizeof(Table) + 13);
    if( pStat1==0 ) return;
    pStat1->zName = (char*)&pStat1[1];
    memcpy(pStat1->zName, "sqlite_stat1", 13);
    pStat1->nCol = 3;
    pStat1->iPKey = -1;
    sqlite3VdbeAddOp4(pParse->pVdbe, OP_Noop, 0, 0, 0,(char*)pStat1,P4_DYNBLOB);
  }
#endif

  /* Establish a read-lock on the table at the shared-cache level. 
  ** Open a read-only cursor on the table. Also allocate a cursor number
  ** to use for scanning indexes (iIdxCur). No index cursor is opened at
  ** this time though.  */
  sqlite3TableLock(pParse, iDb, pTab->tnum, 0, pTab->zName);
  iTabCur = iTab++;
  iIdxCur = iTab++;
  pParse->nTab = MAX(pParse->nTab, iTab);
  sqlite3OpenTable(pParse, iTabCur, iDb, pTab, OP_OpenRead);
  sqlite3VdbeLoadString(v, regTabname, pTab->zName);

  for(pIdx=pTab->pIndex; pIdx; pIdx=pIdx->pNext){
    int nCol;                     /* Number of columns in pIdx. "N" */
    int addrRewind;               /* Address of "OP_Rewind iIdxCur" */
    int addrNextRow;              /* Address of "next_row:" */
    const char *zIdxName;         /* Name of the index */
    int nColTest;                 /* Number of columns to test for changes */

    if( pOnlyIdx && pOnlyIdx!=pIdx ) continue;
    if( pIdx->pPartIdxWhere==0 ) needTableCnt = 0;
#if defined(SQLITE_BUILDING_FOR_COMDB2)
    nCol = pIdx->nKeyCol;
    for(i=0; i < nCol; ++i){
      if( strcmp(pIdx->azColl[i], "DATACOPY")==0 ){
        nCol = i;
        break;
      }
    }
    nColTest = nCol;
    if( !HasRowid(pTab) && IsPrimaryKeyIndex(pIdx) ){
      zIdxName = pTab->zName;
    }else{
      zIdxName = pIdx->zName;
    }
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    if( !HasRowid(pTab) && IsPrimaryKeyIndex(pIdx) ){
      nCol = pIdx->nKeyCol;
      zIdxName = pTab->zName;
      nColTest = nCol - 1;
    }else{
      nCol = pIdx->nColumn;
      zIdxName = pIdx->zName;
      nColTest = pIdx->uniqNotNull ? pIdx->nKeyCol-1 : nCol-1;
    }
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */

    /* Populate the register containing the index name. */
    sqlite3VdbeLoadString(v, regIdxname, zIdxName);
    VdbeComment((v, "Analysis for %s.%s", pTab->zName, zIdxName));

    /*
    ** Pseudo-code for loop that calls stat_push():
    **
    **   Rewind csr
    **   if eof(csr) goto end_of_scan;
    **   regChng = 0
    **   goto chng_addr_0;
    **
    **  next_row:
    **   regChng = 0
    **   if( idx(0) != regPrev(0) ) goto chng_addr_0
    **   regChng = 1
    **   if( idx(1) != regPrev(1) ) goto chng_addr_1
    **   ...
    **   regChng = N
    **   goto chng_addr_N
    **
    **  chng_addr_0:
    **   regPrev(0) = idx(0)
    **  chng_addr_1:
    **   regPrev(1) = idx(1)
    **  ...
    **
    **  endDistinctTest:
    **   regRowid = idx(rowid)
    **   stat_push(P, regChng, regRowid)
    **   Next csr
    **   if !eof(csr) goto next_row;
    **
    **  end_of_scan:
    */

    /* Make sure there are enough memory cells allocated to accommodate 
    ** the regPrev array and a trailing rowid (the rowid slot is required
    ** when building a record to insert into the sample column of 
    ** the sqlite_stat4 table.  */
    pParse->nMem = MAX(pParse->nMem, regPrev+nColTest);

    /* Open a read-only cursor on the index being analyzed. */
    assert( iDb==sqlite3SchemaToIndex(db, pIdx->pSchema) );
    sqlite3VdbeAddOp3(v, OP_OpenRead, iIdxCur, pIdx->tnum, iDb);
    sqlite3VdbeSetP4KeyInfo(pParse, pIdx);
    VdbeComment((v, "%s", pIdx->zName));

    /* Invoke the stat_init() function. The arguments are:
    ** 
    **    (1) the number of columns in the index including the rowid
    **        (or for a WITHOUT ROWID table, the number of PK columns),
    **    (2) the number of columns in the key without the rowid/pk
    **    (3) the number of rows in the index,
    **
    **
    ** The third argument is only used for STAT3 and STAT4
    */
#if defined(SQLITE_BUILDING_FOR_COMDB2)
#ifdef SQLITE_ENABLE_STAT3_OR_STAT4
    /* we only care about our shared tables */
    if( iDb==0 ){
      i64 actualCount = analyze_get_nrecs(pIdx->tnum);
      sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, regStat4+3, 0,
                            (const u8*)&actualCount, P4_INT64);
    }else{
      /* TODO: Is there a better default value here? */
      sqlite3VdbeAddOp2(v, OP_Integer, 0, regStat4+3);
    }
#endif
    sqlite3VdbeAddOp2(v, OP_Integer, nCol+1, regStat4+1);
    sqlite3VdbeAddOp2(v, OP_Count, iIdxCur, regStat4+2);
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
#ifdef SQLITE_ENABLE_STAT3_OR_STAT4
    sqlite3VdbeAddOp2(v, OP_Count, iIdxCur, regStat4+3);
#endif
    sqlite3VdbeAddOp2(v, OP_Integer, nCol, regStat4+1);
    sqlite3VdbeAddOp2(v, OP_Integer, pIdx->nKeyCol, regStat4+2);
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    sqlite3VdbeAddOp4(v, OP_Function0, 0, regStat4+1, regStat4,
                     (char*)&statInitFuncdef, P4_FUNCDEF);
    sqlite3VdbeChangeP5(v, 2+IsStat34);

    /* Implementation of the following:
    **
    **   Rewind csr
    **   if eof(csr) goto end_of_scan;
    **   regChng = 0
    **   goto next_push_0;
    **
    */
    addrRewind = sqlite3VdbeAddOp1(v, OP_Rewind, iIdxCur);
    VdbeCoverage(v);
    sqlite3VdbeAddOp2(v, OP_Integer, 0, regChng);
    addrNextRow = sqlite3VdbeCurrentAddr(v);

    if( nColTest>0 ){
      int endDistinctTest = sqlite3VdbeMakeLabel(pParse);
      int *aGotoChng;               /* Array of jump instruction addresses */
      aGotoChng = sqlite3DbMallocRawNN(db, sizeof(int)*nColTest);
      if( aGotoChng==0 ) continue;

      /*
      **  next_row:
      **   regChng = 0
      **   if( idx(0) != regPrev(0) ) goto chng_addr_0
      **   regChng = 1
      **   if( idx(1) != regPrev(1) ) goto chng_addr_1
      **   ...
      **   regChng = N
      **   goto endDistinctTest
      */
      sqlite3VdbeAddOp0(v, OP_Goto);
      addrNextRow = sqlite3VdbeCurrentAddr(v);
      if( nColTest==1 && pIdx->nKeyCol==1 && IsUniqueIndex(pIdx) ){
        /* For a single-column UNIQUE index, once we have found a non-NULL
        ** row, we know that all the rest will be distinct, so skip 
        ** subsequent distinctness tests. */
        sqlite3VdbeAddOp2(v, OP_NotNull, regPrev, endDistinctTest);
        VdbeCoverage(v);
      }
      for(i=0; i<nColTest; i++){
        char *pColl = (char*)sqlite3LocateCollSeq(pParse, pIdx->azColl[i]);
        sqlite3VdbeAddOp2(v, OP_Integer, i, regChng);
        sqlite3VdbeAddOp3(v, OP_Column, iIdxCur, i, regTemp);
        aGotoChng[i] = 
        sqlite3VdbeAddOp4(v, OP_Ne, regTemp, 0, regPrev+i, pColl, P4_COLLSEQ);
        sqlite3VdbeChangeP5(v, SQLITE_NULLEQ);
        VdbeCoverage(v);
      }
      sqlite3VdbeAddOp2(v, OP_Integer, nColTest, regChng);
      sqlite3VdbeGoto(v, endDistinctTest);
  
  
      /*
      **  chng_addr_0:
      **   regPrev(0) = idx(0)
      **  chng_addr_1:
      **   regPrev(1) = idx(1)
      **  ...
      */
      sqlite3VdbeJumpHere(v, addrNextRow-1);
      for(i=0; i<nColTest; i++){
        sqlite3VdbeJumpHere(v, aGotoChng[i]);
        sqlite3VdbeAddOp3(v, OP_Column, iIdxCur, i, regPrev+i);
      }
      sqlite3VdbeResolveLabel(v, endDistinctTest);
      sqlite3DbFree(db, aGotoChng);
    }
  
    /*
    **  chng_addr_N:
    **   regRowid = idx(rowid)            // STAT34 only
    **   stat_push(P, regChng, regRowid)  // 3rd parameter STAT34 only
    **   Next csr
    **   if !eof(csr) goto next_row;
    */
#if !defined(SQLITE_BUILDING_FOR_COMDB2)
#ifdef SQLITE_ENABLE_STAT3_OR_STAT4
    assert( regRowid==(regStat4+2) );
    if( HasRowid(pTab) ){
      sqlite3VdbeAddOp2(v, OP_IdxRowid, iIdxCur, regRowid);
    }else{
      Index *pPk = sqlite3PrimaryKeyIndex(pIdx->pTable);
      int j, k, regKey;
      regKey = sqlite3GetTempRange(pParse, pPk->nKeyCol);
      for(j=0; j<pPk->nKeyCol; j++){
        k = sqlite3ColumnOfIndex(pIdx, pPk->aiColumn[j]);
        assert( k>=0 && k<pIdx->nColumn );
        sqlite3VdbeAddOp3(v, OP_Column, iIdxCur, k, regKey+j);
        VdbeComment((v, "%s", pTab->aCol[pPk->aiColumn[j]].zName));
      }
      sqlite3VdbeAddOp3(v, OP_MakeRecord, regKey, pPk->nKeyCol, regRowid);
      sqlite3ReleaseTempRange(pParse, regKey, pPk->nKeyCol);
    }
#endif
#endif /* !defined(SQLITE_BUILDING_FOR_COMDB2) */
    assert( regChng==(regStat4+1) );
#if defined(SQLITE_BUILDING_FOR_COMDB2)
#ifdef SQLITE_ENABLE_STAT3_OR_STAT4
    assert( regSampleRow==(regStat4+2) );
    sqlite3VdbeAddOp3(v, OP_MakeRecord, regPrev, nCol, regSampleRow);
#endif
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    sqlite3VdbeAddOp4(v, OP_Function0, 1, regStat4, regTemp,
                     (char*)&statPushFuncdef, P4_FUNCDEF);
    sqlite3VdbeChangeP5(v, 2+IsStat34);
    sqlite3VdbeAddOp2(v, OP_Next, iIdxCur, addrNextRow); VdbeCoverage(v);

    /* Add the entry to the stat1 table. */
#if defined(SQLITE_BUILDING_FOR_COMDB2)
    if( sqlite3_gbl_tunables.analyze_empty_tables ){
      sqlite3VdbeJumpHere(v, addrRewind);
    }
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    callStatGet(v, regStat4, STAT_GET_STAT1, regStat1);
    assert( "BBB"[0]==SQLITE_AFF_TEXT );
    sqlite3VdbeAddOp4(v, OP_MakeRecord, regTabname, 3, regTemp, "BBB", 0);
    sqlite3VdbeAddOp2(v, OP_NewRowid, iStatCur, regNewRowid);
    sqlite3VdbeAddOp3(v, OP_Insert, iStatCur, regTemp, regNewRowid);
#ifdef SQLITE_ENABLE_PREUPDATE_HOOK
    sqlite3VdbeChangeP4(v, -1, (char*)pStat1, P4_TABLE);
#endif
    sqlite3VdbeChangeP5(v, OPFLAG_APPEND);

    /* Add the entries to the stat3 or stat4 table. */
#ifdef SQLITE_ENABLE_STAT3_OR_STAT4
#if defined(SQLITE_BUILDING_FOR_COMDB2)
    if( skip4==0 ){
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    {
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
      int regEq = regStat1;
      int regLt = regStat1+1;
      int regDLt = regStat1+2;
      int regSample = regStat1+3;
      int regCol = regStat1+4;
#if !defined(SQLITE_BUILDING_FOR_COMDB2)
      int regSampleRowid = regCol + nCol;
#endif /* !defined(SQLITE_BUILDING_FOR_COMDB2) */
      int addrNext;
      int addrIsNull;
#if !defined(SQLITE_BUILDING_FOR_COMDB2)
      u8 seekOp = HasRowid(pTab) ? OP_NotExists : OP_NotFound;
#endif /* !defined(SQLITE_BUILDING_FOR_COMDB2) */

      pParse->nMem = MAX(pParse->nMem, regCol+nCol);

#if defined(SQLITE_BUILDING_FOR_COMDB2)
      if( sqlite3_gbl_tunables.analyze_empty_tables ){
        addrRewind = sqlite3VdbeAddOp1(v, OP_Rewind, iIdxCur);
        VdbeCoverage(v);
      }
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
      addrNext = sqlite3VdbeCurrentAddr(v);
#if !defined(SQLITE_BUILDING_FOR_COMDB2)
      callStatGet(v, regStat4, STAT_GET_ROWID, regSampleRowid);
      addrIsNull = sqlite3VdbeAddOp1(v, OP_IsNull, regSampleRowid);
      VdbeCoverage(v);
#endif /* !defined(SQLITE_BUILDING_FOR_COMDB2) */
      callStatGet(v, regStat4, STAT_GET_NEQ, regEq);
#if defined(SQLITE_BUILDING_FOR_COMDB2)
      addrIsNull = sqlite3VdbeAddOp1(v, OP_IsNull, regEq);
      VdbeCoverage(v);
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
      callStatGet(v, regStat4, STAT_GET_NLT, regLt);
      callStatGet(v, regStat4, STAT_GET_NDLT, regDLt);
#if defined(SQLITE_BUILDING_FOR_COMDB2)
      callStatGet(v, regStat4, STAT_GET_ROW, regSample);
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
      sqlite3VdbeAddOp4Int(v, seekOp, iTabCur, addrNext, regSampleRowid, 0);
      VdbeCoverage(v);
#ifdef SQLITE_ENABLE_STAT3
      sqlite3ExprCodeLoadIndexColumn(pParse, pIdx, iTabCur, 0, regSample);
#else
      for(i=0; i<nCol; i++){
        sqlite3ExprCodeLoadIndexColumn(pParse, pIdx, iTabCur, i, regCol+i);
      }
      sqlite3VdbeAddOp3(v, OP_MakeRecord, regCol, nCol, regSample);
#endif
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
      sqlite3VdbeAddOp3(v, OP_MakeRecord, regTabname, 6, regTemp);
      sqlite3VdbeAddOp2(v, OP_NewRowid, iStatCur+1, regNewRowid);
      sqlite3VdbeAddOp3(v, OP_Insert, iStatCur+1, regTemp, regNewRowid);
      sqlite3VdbeAddOp2(v, OP_Goto, 1, addrNext); /* P1==1 for end-of-loop */
      sqlite3VdbeJumpHere(v, addrIsNull);
#if defined(SQLITE_BUILDING_FOR_COMDB2)
      if( sqlite3_gbl_tunables.analyze_empty_tables ){
        sqlite3VdbeJumpHere(v, addrRewind);
      }
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    }
#endif /* SQLITE_ENABLE_STAT3_OR_STAT4 */

    /* End of analysis */
#if defined(SQLITE_BUILDING_FOR_COMDB2)
    if( !sqlite3_gbl_tunables.analyze_empty_tables ){
      sqlite3VdbeJumpHere(v, addrRewind);
    }
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    sqlite3VdbeJumpHere(v, addrRewind);
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
  }


#if !defined(SQLITE_BUILDING_FOR_COMDB2)
  /* Create a single sqlite_stat1 entry containing NULL as the index
  ** name and the row count as the content.
  */
  if( pOnlyIdx==0 && needTableCnt ){
    VdbeComment((v, "%s", pTab->zName));
    sqlite3VdbeAddOp2(v, OP_Count, iTabCur, regStat1);
    jZeroRows = sqlite3VdbeAddOp1(v, OP_IfNot, regStat1); VdbeCoverage(v);
    sqlite3VdbeAddOp2(v, OP_Null, 0, regIdxname);
    assert( "BBB"[0]==SQLITE_AFF_TEXT );
    sqlite3VdbeAddOp4(v, OP_MakeRecord, regTabname, 3, regTemp, "BBB", 0);
    sqlite3VdbeAddOp2(v, OP_NewRowid, iStatCur, regNewRowid);
    sqlite3VdbeAddOp3(v, OP_Insert, iStatCur, regTemp, regNewRowid);
    sqlite3VdbeChangeP5(v, OPFLAG_APPEND);
#ifdef SQLITE_ENABLE_PREUPDATE_HOOK
    sqlite3VdbeChangeP4(v, -1, (char*)pStat1, P4_TABLE);
#endif
    sqlite3VdbeJumpHere(v, jZeroRows);
  }
#else /* !defined(SQLITE_BUILDING_FOR_COMDB2) */
  UNUSED_PARAMETER(needTableCnt);
#endif /* !defined(SQLITE_BUILDING_FOR_COMDB2) */
}


/*
** Generate code that will cause the most recent index analysis to
** be loaded into internal hash tables where is can be used.
*/
static void loadAnalysis(Parse *pParse, int iDb){
  Vdbe *v = sqlite3GetVdbe(pParse);
  if( v ){
    sqlite3VdbeAddOp1(v, OP_LoadAnalysis, iDb);
  }
}

/*
** Generate code that will do an analysis of an entire database
*/
static void analyzeDatabase(Parse *pParse, int iDb){
  sqlite3 *db = pParse->db;
  Schema *pSchema = db->aDb[iDb].pSchema;    /* Schema of database iDb */
  HashElem *k;
  int iStatCur;
  int iMem;
  int iTab;

  sqlite3BeginWriteOperation(pParse, 0, iDb);
  iStatCur = pParse->nTab;
  pParse->nTab += 3;
  openStatTable(pParse, iDb, iStatCur, 0, 0);
  iMem = pParse->nMem+1;
  iTab = pParse->nTab;
  assert( sqlite3SchemaMutexHeld(db, iDb, 0) );
  for(k=sqliteHashFirst(&pSchema->tblHash); k; k=sqliteHashNext(k)){
    Table *pTab = (Table*)sqliteHashData(k);
    analyzeOneTable(pParse, pTab, 0, iStatCur, iMem, iTab);
  }
  loadAnalysis(pParse, iDb);
}

/*
** Generate code that will do an analysis of a single table in
** a database.  If pOnlyIdx is not NULL then it is a single index
** in pTab that should be analyzed.
*/
static void analyzeTable(Parse *pParse, Table *pTab, Index *pOnlyIdx){
  int iDb;
  int iStatCur;

  assert( pTab!=0 );
  assert( sqlite3BtreeHoldsAllMutexes(pParse->db) );
  iDb = sqlite3SchemaToIndex(pParse->db, pTab->pSchema);
  sqlite3BeginWriteOperation(pParse, 0, iDb);
  iStatCur = pParse->nTab;
  pParse->nTab += 3;
  if( pOnlyIdx ){
    openStatTable(pParse, iDb, iStatCur, pOnlyIdx->zName, "idx");
  }else{
    openStatTable(pParse, iDb, iStatCur, pTab->zName, "tbl");
  }
  analyzeOneTable(pParse, pTab, pOnlyIdx, iStatCur,pParse->nMem+1,pParse->nTab);
#if !defined(SQLITE_BUILDING_FOR_COMDB2)
  loadAnalysis(pParse, iDb);
#endif /* !defined(SQLITE_BUILDING_FOR_COMDB2) */
}

/*
** Generate code for the ANALYZE command.  The parser calls this routine
** when it recognizes an ANALYZE command.
**
**        ANALYZE                            -- 1
**        ANALYZE  <database>                -- 2
**        ANALYZE  ?<database>.?<tablename>  -- 3
**
** Form 1 causes all indices in all attached databases to be analyzed.
** Form 2 analyzes all indices the single database named.
** Form 3 analyzes all indices associated with the named table.
*/
void sqlite3Analyze(Parse *pParse, Token *pName1, Token *pName2){
  sqlite3 *db = pParse->db;
  int iDb;
  int i;
  char *z, *zDb;
  Table *pTab;
  Index *pIdx;
  Token *pTableName;
#if !defined(SQLITE_BUILDING_FOR_COMDB2)
  Vdbe *v;
#endif /* !defined(SQLITE_BUILDING_FOR_COMDB2) */

  /* Read the database schema. If an error occurs, leave an error message
  ** and code in pParse and return NULL. */
  assert( sqlite3BtreeHoldsAllMutexes(pParse->db) );
  if( SQLITE_OK!=sqlite3ReadSchema(pParse) ){
    return;
  }

  assert( pName2!=0 || pName1==0 );
  if( pName1==0 ){
    /* Form 1:  Analyze everything */
    for(i=0; i<db->nDb; i++){
      if( i==1 ) continue;  /* Do not analyze the TEMP database */
      analyzeDatabase(pParse, i);
    }
  }else if( pName2->n==0 && (iDb = sqlite3FindDb(db, pName1))>=0 ){
    /* Analyze the schema named as the argument */
    analyzeDatabase(pParse, iDb);
  }else{
    /* Form 3: Analyze the table or index named as an argument */
    iDb = sqlite3TwoPartName(pParse, pName1, pName2, &pTableName);
    if( iDb>=0 ){
      zDb = pName2->n ? db->aDb[iDb].zDbSName : 0;
      z = sqlite3NameFromToken(db, pTableName);
      if( z ){
        if( (pIdx = sqlite3FindIndex(db, z, zDb))!=0 ){
          analyzeTable(pParse, pIdx->pTable, pIdx);
        }else if( (pTab = sqlite3LocateTable(pParse, 0, z, zDb))!=0 ){
          analyzeTable(pParse, pTab, 0);
        }
        sqlite3DbFree(db, z);
      }
    }
  }
#if !defined(SQLITE_BUILDING_FOR_COMDB2)
  if( db->nSqlExec==0 && (v = sqlite3GetVdbe(pParse))!=0 ){
    sqlite3VdbeAddOp0(v, OP_Expire);
  }
#endif /* !defined(SQLITE_BUILDING_FOR_COMDB2) */
}

/*
** Used to pass information from the analyzer reader through to the
** callback routine.
*/
typedef struct analysisInfo analysisInfo;
struct analysisInfo {
  sqlite3 *db;
  const char *zDatabase;
};

/*
** The first argument points to a nul-terminated string containing a
** list of space separated integers. Read the first nOut of these into
** the array aOut[].
*/
static void decodeIntArray(
  char *zIntArray,       /* String containing int array to decode */
  int nOut,              /* Number of slots in aOut[] */
  tRowcnt *aOut,         /* Store integers here */
  LogEst *aLog,          /* Or, if aOut==0, here */
  Index *pIndex          /* Handle extra flags for this index, if not NULL */
){
  char *z = zIntArray;
  int c;
  int i;
  tRowcnt v;

#ifdef SQLITE_ENABLE_STAT3_OR_STAT4
  if( z==0 ) z = "";
#else
  assert( z!=0 );
#endif
  for(i=0; *z && i<nOut; i++){
    v = 0;
    while( (c=z[0])>='0' && c<='9' ){
      v = v*10 + c - '0';
      z++;
    }
#ifdef SQLITE_ENABLE_STAT3_OR_STAT4
    if( aOut ) aOut[i] = v;
#if !defined(SQLITE_BUILDING_FOR_COMDB2)
    if( aLog ) aLog[i] = sqlite3LogEst(v);
#endif /* !defined(SQLITE_BUILDING_FOR_COMDB2) */
#else
    assert( aOut==0 );
    UNUSED_PARAMETER(aOut);
#if !defined(SQLITE_BUILDING_FOR_COMDB2)
    assert( aLog!=0 );
    aLog[i] = sqlite3LogEst(v);
#endif /* !defined(SQLITE_BUILDING_FOR_COMDB2) */
#endif
#if defined(SQLITE_BUILDING_FOR_COMDB2)
    if( aLog ) aLog[i] = sqlite3LogEst(v);
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    if( *z==' ' ) z++;
  }
#ifndef SQLITE_ENABLE_STAT3_OR_STAT4
  assert( pIndex!=0 ); {
#else
  if( pIndex ){
#endif
    pIndex->bUnordered = 0;
    pIndex->noSkipScan = 0;
#if defined(SQLITE_BUILDING_FOR_COMDB2)
    /* assign noSkipScan, only if not foreign table */ 
    if( pIndex->pTable ) {
      if( pIndex->pTable->iDb==0 ){
        pIndex->noSkipScan = is_comdb2_index_disableskipscan(
                pIndex->pTable->zName
                );
      } else if( pIndex->pTable->iDb > 1 ){ /* foreign */
        pIndex->noSkipScan = 1;
      }
    }
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    while( z[0] ){
      if( sqlite3_strglob("unordered*", z)==0 ){
        pIndex->bUnordered = 1;
      }else if( sqlite3_strglob("sz=[0-9]*", z)==0 ){
        pIndex->szIdxRow = sqlite3LogEst(sqlite3Atoi(z+3));
      }else if( sqlite3_strglob("noskipscan*", z)==0 ){
        pIndex->noSkipScan = 1;
      }
#ifdef SQLITE_ENABLE_COSTMULT
      else if( sqlite3_strglob("costmult=[0-9]*",z)==0 ){
        pIndex->pTable->costMult = sqlite3LogEst(sqlite3Atoi(z+9));
      }
#endif
      while( z[0]!=0 && z[0]!=' ' ) z++;
      while( z[0]==' ' ) z++;
    }
  }
}

char *mapped_index(const char *oldname);

/*
** This callback is invoked once for each index when reading the
** sqlite_stat1 table.  
**
**     argv[0] = name of the table
**     argv[1] = name of the index (might be NULL)
**     argv[2] = results of analysis - on integer for each column
**
** Entries for which argv[1]==NULL simply record the number of rows in
** the table.
*/
static int analysisLoader(void *pData, int argc, char **argv, char **NotUsed){
  analysisInfo *pInfo = (analysisInfo*)pData;
  Index *pIndex;
  Table *pTable;
  const char *z;

  assert( argc==3 );
  UNUSED_PARAMETER2(NotUsed, argc);

  if( argv==0 || argv[0]==0 || argv[2]==0 ){
    return 0;
  }
#if defined(SQLITE_BUILDING_FOR_COMDB2)
  pTable = sqlite3FindTableCheckOnly(pInfo->db, argv[0], pInfo->zDatabase);
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
  pTable = sqlite3FindTable(pInfo->db, argv[0], pInfo->zDatabase);
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
  if( pTable==0 ){
    return 0;
  }
  if( argv[1]==0 ){
    pIndex = 0;
  }else if( sqlite3_stricmp(argv[0],argv[1])==0 ){
    pIndex = sqlite3PrimaryKeyIndex(pTable);
  }else{
    pIndex = sqlite3FindIndex(pInfo->db, argv[1], pInfo->zDatabase);
  }
#if defined(SQLITE_BUILDING_FOR_COMDB2)
  if ( pIndex==0 ) {
      char *newName = mapped_index(argv[1]);
      if ( newName!=0 ) {
          pIndex = sqlite3FindIndex(pInfo->db, newName, pInfo->zDatabase);
      }
  }
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
  z = argv[2];

  if( pIndex ){
    tRowcnt *aiRowEst = 0;
    int nCol = pIndex->nKeyCol+1;
#ifdef SQLITE_ENABLE_STAT3_OR_STAT4
    /* Index.aiRowEst may already be set here if there are duplicate 
    ** sqlite_stat1 entries for this index. In that case just clobber
    ** the old data with the new instead of allocating a new array.  */
    if( pIndex->aiRowEst==0 ){
      pIndex->aiRowEst = (tRowcnt*)sqlite3MallocZero(sizeof(tRowcnt) * nCol);
      if( pIndex->aiRowEst==0 ) sqlite3OomFault(pInfo->db);
    }
    aiRowEst = pIndex->aiRowEst;
#endif
    pIndex->bUnordered = 0;
    decodeIntArray((char*)z, nCol, aiRowEst, pIndex->aiRowLogEst, pIndex);
    pIndex->hasStat1 = 1;
    if( pIndex->pPartIdxWhere==0 ){
      pTable->nRowLogEst = pIndex->aiRowLogEst[0];
      pTable->tabFlags |= TF_HasStat1;
    }
#if defined(SQLITE_BUILDING_FOR_COMDB2)
  /*
   * stale entries in sqlite_stat1 will have pIndex null
   * and result in a potential bad estimate for table
   * if stale index entry was processed last
   */
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
  }else{
    Index fakeIdx;
    fakeIdx.szIdxRow = pTable->szTabRow;
#ifdef SQLITE_ENABLE_COSTMULT
    fakeIdx.pTable = pTable;
#endif
    decodeIntArray((char*)z, 1, 0, &pTable->nRowLogEst, &fakeIdx);
    pTable->szTabRow = fakeIdx.szIdxRow;
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    pTable->tabFlags |= TF_HasStat1;
  }

  return 0;
}

/*
** If the Index.aSample variable is not NULL, delete the aSample[] array
** and its contents.
*/
void sqlite3DeleteIndexSamples(sqlite3 *db, Index *pIdx){
#ifdef SQLITE_ENABLE_STAT3_OR_STAT4
  if( pIdx->aSample ){
    int j;
#if defined(SQLITE_BUILDING_FOR_COMDB2)
    for(j=0; j<pIdx->nAlloc; j++){
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    for(j=0; j<pIdx->nSample; j++){
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
      IndexSample *p = &pIdx->aSample[j];
      sqlite3DbFree(db, p->p);
#if defined(SQLITE_BUILDING_FOR_COMDB2)
      sqlite3DbFree(db, p->anEq); p->anEq = 0;
      sqlite3DbFree(db, p->anLt); p->anLt = 0;
      sqlite3DbFree(db, p->anDLt); p->anDLt = 0;
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    }
#if defined(SQLITE_BUILDING_FOR_COMDB2)
    sqlite3DbFree(db, pIdx->aAvgEq); pIdx->aAvgEq = 0;
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    sqlite3DbFree(db, pIdx->aSample);
#if defined(SQLITE_BUILDING_FOR_COMDB2)
    pIdx->nAlloc = 0;
    pIdx->nSample = 0;
    pIdx->aSample = 0;
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
  }
  if( db && db->pnBytesFreed==0 ){
    pIdx->nSample = 0;
    pIdx->aSample = 0;
  }
#else
  UNUSED_PARAMETER(db);
  UNUSED_PARAMETER(pIdx);
#endif /* SQLITE_ENABLE_STAT3_OR_STAT4 */
}

#ifdef SQLITE_ENABLE_STAT3_OR_STAT4
/*
** Populate the pIdx->aAvgEq[] array based on the samples currently
** stored in pIdx->aSample[]. 
*/
static void initAvgEq(Index *pIdx){
  if( pIdx ){
    IndexSample *aSample = pIdx->aSample;
#if defined(SQLITE_BUILDING_FOR_COMDB2)
    if( aSample==0 ) return;
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    IndexSample *pFinal = &aSample[pIdx->nSample-1];
    int iCol;
#if defined(SQLITE_BUILDING_FOR_COMDB2)
    int nCol = pIdx->nSampleCol;
    {
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    int nCol = 1;
    if( pIdx->nSampleCol>1 ){
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
      /* If this is stat4 data, then calculate aAvgEq[] values for all
      ** sample columns except the last. The last is always set to 1, as
      ** once the trailing PK fields are considered all index keys are
      ** unique.  */
#if !defined(SQLITE_BUILDING_FOR_COMDB2)
      nCol = pIdx->nSampleCol-1;
#endif /* !defined(SQLITE_BUILDING_FOR_COMDB2) */
      pIdx->aAvgEq[nCol] = 1;
    }
    for(iCol=0; iCol<nCol; iCol++){
      int nSample = pIdx->nSample;
      int i;                    /* Used to iterate through samples */
      tRowcnt sumEq = 0;        /* Sum of the nEq values */
      tRowcnt avgEq = 0;
      tRowcnt nRow;             /* Number of rows in index */
      i64 nSum100 = 0;          /* Number of terms contributing to sumEq */
      i64 nDist100;             /* Number of distinct values in index */

      if( !pIdx->aiRowEst || iCol>=pIdx->nKeyCol || pIdx->aiRowEst[iCol+1]==0 ){
        nRow = pFinal->anLt[iCol];
        nDist100 = (i64)100 * pFinal->anDLt[iCol];
        nSample--;
      }else{
        nRow = pIdx->aiRowEst[0];
        nDist100 = ((i64)100 * pIdx->aiRowEst[0]) / pIdx->aiRowEst[iCol+1];
      }
      pIdx->nRowEst0 = nRow;

      /* Set nSum to the number of distinct (iCol+1) field prefixes that
      ** occur in the stat4 table for this index. Set sumEq to the sum of 
      ** the nEq values for column iCol for the same set (adding the value 
      ** only once where there exist duplicate prefixes).  */
      for(i=0; i<nSample; i++){
        if( i==(pIdx->nSample-1)
         || aSample[i].anDLt[iCol]!=aSample[i+1].anDLt[iCol] 
        ){
          sumEq += aSample[i].anEq[iCol];
          nSum100 += 100;
        }
      }

      if( nDist100>nSum100 && sumEq<nRow ){
        avgEq = ((i64)100 * (nRow - sumEq))/(nDist100 - nSum100);
      }
      if( avgEq==0 ) avgEq = 1;
      pIdx->aAvgEq[iCol] = avgEq;
    }
  }
}

/*
** Look up an index by name.  Or, if the name of a WITHOUT ROWID table
** is supplied instead, find the PRIMARY KEY index for that table.
*/
static Index *findIndexOrPrimaryKey(
  sqlite3 *db,
  const char *zName,
  const char *zDb
){
  Index *pIdx = sqlite3FindIndex(db, zName, zDb);
  if( pIdx==0 ){
#if defined(SQLITE_BUILDING_FOR_COMDB2)
    Table *pTab = sqlite3FindTableCheckOnly(db, zName, zDb);
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    Table *pTab = sqlite3FindTable(db, zName, zDb);
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    if( pTab && !HasRowid(pTab) ) pIdx = sqlite3PrimaryKeyIndex(pTab);
  }
  return pIdx;
}

#if !defined(SQLITE_BUILDING_FOR_COMDB2)
/*
** Load the content from either the sqlite_stat4 or sqlite_stat3 table 
** into the relevant Index.aSample[] arrays.
**
** Arguments zSql1 and zSql2 must point to SQL statements that return
** data equivalent to the following (statements are different for stat3,
** see the caller of this function for details):
**
**    zSql1: SELECT idx,count(*) FROM %Q.sqlite_stat4 GROUP BY idx
**    zSql2: SELECT idx,neq,nlt,ndlt,sample FROM %Q.sqlite_stat4
**
** where %Q is replaced with the database name before the SQL is executed.
*/
static int loadStatTbl(
  sqlite3 *db,                  /* Database handle */
  int bStat3,                   /* Assume single column records only */
  const char *zSql1,            /* SQL statement 1 (see above) */
  const char *zSql2,            /* SQL statement 2 (see above) */
  const char *zDb               /* Database name (e.g. "main") */
){
  int rc;                       /* Result codes from subroutines */
  sqlite3_stmt *pStmt = 0;      /* An SQL statement being run */
  char *zSql;                   /* Text of the SQL statement */
  Index *pPrevIdx = 0;          /* Previous index in the loop */
  IndexSample *pSample;         /* A slot in pIdx->aSample[] */

  assert( db->lookaside.bDisable );
  zSql = sqlite3MPrintf(db, zSql1, zDb);
  if( !zSql ){
    return SQLITE_NOMEM_BKPT;
  }
  rc = sqlite3_prepare(db, zSql, -1, &pStmt, 0);
  sqlite3DbFree(db, zSql);
  if( rc ) return rc;

  while( sqlite3_step(pStmt)==SQLITE_ROW ){
    int nIdxCol = 1;              /* Number of columns in stat4 records */

    char *zIndex;   /* Index name */
    Index *pIdx;    /* Pointer to the index object */
    int nSample;    /* Number of samples */
    int nByte;      /* Bytes of space required */
    int i;          /* Bytes of space required */
    tRowcnt *pSpace;

    zIndex = (char *)sqlite3_column_text(pStmt, 0);
    if( zIndex==0 ) continue;
    nSample = sqlite3_column_int(pStmt, 1);
    pIdx = findIndexOrPrimaryKey(db, zIndex, zDb);
    assert( pIdx==0 || bStat3 || pIdx->nSample==0 );
    /* Index.nSample is non-zero at this point if data has already been
    ** loaded from the stat4 table. In this case ignore stat3 data.  */
    if( pIdx==0 || pIdx->nSample ) continue;
    if( bStat3==0 ){
      assert( !HasRowid(pIdx->pTable) || pIdx->nColumn==pIdx->nKeyCol+1 );
      if( !HasRowid(pIdx->pTable) && IsPrimaryKeyIndex(pIdx) ){
        nIdxCol = pIdx->nKeyCol;
      }else{
        nIdxCol = pIdx->nColumn;
      }
    }
    pIdx->nSampleCol = nIdxCol;
    nByte = sizeof(IndexSample) * nSample;
    nByte += sizeof(tRowcnt) * nIdxCol * 3 * nSample;
    nByte += nIdxCol * sizeof(tRowcnt);     /* Space for Index.aAvgEq[] */

    pIdx->aSample = sqlite3DbMallocZero(db, nByte);
    if( pIdx->aSample==0 ){
      sqlite3_finalize(pStmt);
      return SQLITE_NOMEM_BKPT;
    }
    pSpace = (tRowcnt*)&pIdx->aSample[nSample];
    pIdx->aAvgEq = pSpace; pSpace += nIdxCol;
    pIdx->pTable->tabFlags |= TF_HasStat4;
    for(i=0; i<nSample; i++){
      pIdx->aSample[i].anEq = pSpace; pSpace += nIdxCol;
      pIdx->aSample[i].anLt = pSpace; pSpace += nIdxCol;
      pIdx->aSample[i].anDLt = pSpace; pSpace += nIdxCol;
    }
    assert( ((u8*)pSpace)-nByte==(u8*)(pIdx->aSample) );
  }
  rc = sqlite3_finalize(pStmt);
  if( rc ) return rc;

  zSql = sqlite3MPrintf(db, zSql2, zDb);
  if( !zSql ){
    return SQLITE_NOMEM_BKPT;
  }
  rc = sqlite3_prepare(db, zSql, -1, &pStmt, 0);
  sqlite3DbFree(db, zSql);
  if( rc ) return rc;

  while( sqlite3_step(pStmt)==SQLITE_ROW ){
    char *zIndex;                 /* Index name */
    Index *pIdx;                  /* Pointer to the index object */
    int nCol = 1;                 /* Number of columns in index */

    zIndex = (char *)sqlite3_column_text(pStmt, 0);
    if( zIndex==0 ) continue;
    pIdx = findIndexOrPrimaryKey(db, zIndex, zDb);
    if( pIdx==0 ) continue;
    /* This next condition is true if data has already been loaded from 
    ** the sqlite_stat4 table. In this case ignore stat3 data.  */
    nCol = pIdx->nSampleCol;
    if( bStat3 && nCol>1 ) continue;
    if( pIdx!=pPrevIdx ){
      initAvgEq(pPrevIdx);
      pPrevIdx = pIdx;
    }
    pSample = &pIdx->aSample[pIdx->nSample];
    decodeIntArray((char*)sqlite3_column_text(pStmt,1),nCol,pSample->anEq,0,0);
    decodeIntArray((char*)sqlite3_column_text(pStmt,2),nCol,pSample->anLt,0,0);
    decodeIntArray((char*)sqlite3_column_text(pStmt,3),nCol,pSample->anDLt,0,0);

    /* Take a copy of the sample. Add two 0x00 bytes the end of the buffer.
    ** This is in case the sample record is corrupted. In that case, the
    ** sqlite3VdbeRecordCompare() may read up to two varints past the
    ** end of the allocated buffer before it realizes it is dealing with
    ** a corrupt record. Adding the two 0x00 bytes prevents this from causing
    ** a buffer overread.  */
    pSample->n = sqlite3_column_bytes(pStmt, 4);
    pSample->p = sqlite3DbMallocZero(db, pSample->n + 2);
    if( pSample->p==0 ){
      sqlite3_finalize(pStmt);
      return SQLITE_NOMEM_BKPT;
    }
    if( pSample->n ){
      memcpy(pSample->p, sqlite3_column_blob(pStmt, 4), pSample->n);
    }
    pIdx->nSample++;
  }
  rc = sqlite3_finalize(pStmt);
  if( rc==SQLITE_OK ) initAvgEq(pPrevIdx);
  return rc;
}
#endif /* !defined(SQLITE_BUILDING_FOR_COMDB2) */
#if defined(SQLITE_BUILDING_FOR_COMDB2)
#include <vdbecompare.c>
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */

/*
** Load content from the sqlite_stat4 and sqlite_stat3 tables into 
** the Index.aSample[] arrays of all indices.
*/
static int loadStat4(sqlite3 *db, const char *zDb){
#if defined(SQLITE_BUILDING_FOR_COMDB2)
  int rc;                   /* Result codes from subroutines */
  sqlite3_stmt *pStmt = 0;  /* An SQL statement being run */
  char *zSql;
  const char *zSql2 = "SELECT idx,neq,nlt,ndlt,sample,tbl FROM %Q.sqlite_stat4"
                      " WHERE tbl not like 'cdb2.%%.sav';";

  if( sqlite3FindTableCheckOnly(db, "sqlite_stat4", zDb)==0 ){
    return SQLITE_OK;
  }
  zSql = sqlite3MPrintf(db, zSql2, zDb);
  if( !zSql ) return SQLITE_NOMEM_BKPT;
  rc = sqlite3_prepare(db, zSql, -1, &pStmt, 0);
  sqlite3DbFree(db, zSql);
  if( rc!=SQLITE_OK ) return rc;

  while( sqlite3_step(pStmt)==SQLITE_ROW ){
    char *zTable;
    char *zIndex;
    Index *pIdx;
    int nCol, iPos, nAlloc;
    IndexSample *pSample;

    zTable = (char *)sqlite3_column_text(pStmt, 5);
    if( strncmp("cdb2.", zTable, 5)==0 ) continue;
    zIndex = (char *)sqlite3_column_text(pStmt, 0);
    if( zIndex==0 ) continue;
    pIdx = findIndexOrPrimaryKey(db, zIndex, zDb);
#if defined(SQLITE_BUILDING_FOR_COMDB2)
    if( pIdx==0 ) {
        char *newName = mapped_index(zIndex);
        if ( newName!=0 ) {
            pIdx = sqlite3FindIndex(db, newName, zDb);
        }
    }
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    if( pIdx==0 ) continue;
    if( pIdx->pKeyInfo==0 ){
      Parse parse;
      int j;
      memset(&parse, 0, sizeof(Parse));
      parse.db = db;
      pIdx->pKeyInfo = sqlite3KeyInfoOfIndex(&parse, pIdx);
      if( pIdx->pKeyInfo==0 ) continue;
      for(j=0; j<pIdx->nKeyCol; j++){
        if( pIdx->pKeyInfo->aColl[j] ) break; /* DATACOPY */
      }
      pIdx->nSampleCol = j;
    }
    nCol = pIdx->nSampleCol;
    nAlloc = pIdx->nAlloc;
    if( pIdx->nSample>=nAlloc ){
      int i, bOom;
      int nSize = (nCol + 1) * sizeof(tRowcnt);
      if( nAlloc==0 ){
        nAlloc = MAX(pIdx->nSample, SQLITE_STAT4_SAMPLES);
        assert( pIdx->aAvgEq==0 );
        pIdx->aAvgEq = sqlite3DbMallocZero(db, nSize);
        if( pIdx->aAvgEq==0 ){
          sqlite3_finalize(pStmt);
          return SQLITE_NOMEM_BKPT;
        }
      }else{
        nAlloc = pIdx->nSample * 2;
      }
      assert( nAlloc>=pIdx->nSample );
      pIdx->aSample = sqlite3DbRealloc(db, pIdx->aSample,
                                       nAlloc * sizeof(IndexSample));
      if( pIdx->aSample==0 ){
        sqlite3_finalize(pStmt);
        return SQLITE_NOMEM_BKPT;
      }
      /* Zero out the new slots */
      memset(&pIdx->aSample[pIdx->nSample], 0,
             (nAlloc - pIdx->nSample) * sizeof(IndexSample));
      /* TODO: Make this whole loop use one big malloc. */
      bOom = 0;
      for(i=pIdx->nSample; i<nAlloc; i++){
        pIdx->aSample[i].anEq  = sqlite3DbMallocZero(db, nSize);
        if( pIdx->aSample[i].anEq==0 ){ bOom = 1; break; }
        pIdx->aSample[i].anLt  = sqlite3DbMallocZero(db, nSize);
        if( pIdx->aSample[i].anLt==0 ){ bOom = 1; break; }
        pIdx->aSample[i].anDLt = sqlite3DbMallocZero(db, nSize);
        if( pIdx->aSample[i].anDLt==0 ){ bOom = 1; break; }
      }
      if( bOom ){
        sqlite3_finalize(pStmt);
        return SQLITE_NOMEM_BKPT;
      }
      pIdx->nAlloc = nAlloc;
    }

    /* Find position for this sample */
    iPos = pIdx->nSample;
    if( iPos>0 ){
      int x, cmp;
      int iPosGreater = iPos;
      int iPosSmaller = 0;
      UnpackedRecord *pUnpacked = sqlite3VdbeAllocUnpackedRecord(pIdx->pKeyInfo);
      if( pUnpacked==0 ){
        sqlite3_finalize(pStmt);
        return SQLITE_NOMEM_BKPT;
      }
      sqlite3VdbeRecordUnpack(pIdx->pKeyInfo, sqlite3_column_bytes(pStmt, 4),
                              sqlite3_column_blob(pStmt, 4), pUnpacked);
      /* find first less than the new item */
      {
        /* Most of the time, last slot is the correct position. */
        x = iPosGreater - 1;
        cmp = sqlite3VdbeRecordCompare(pIdx->aSample[x].n,
                                       pIdx->aSample[x].p, pUnpacked);
        if( cmp<0 ) iPosSmaller = iPosGreater;
      }
      /* TODO: What is special about 32 here? */
      while( iPosGreater-iPosSmaller>32 ){ /* binary search */
        x = (iPosSmaller + iPosGreater) / 2;
        cmp = sqlite3VdbeRecordCompare(pIdx->aSample[x].n,
                                       pIdx->aSample[x].p, pUnpacked);
        if( cmp<0 ){
          iPosSmaller = x;
        }else{
          iPosGreater = x;
        }
      }
      for(iPos=iPosGreater; iPos>iPosSmaller; iPos--){
        x = iPos - 1;
        cmp = sqlite3VdbeRecordCompare(pIdx->aSample[x].n,
                                       pIdx->aSample[x].p, pUnpacked);
        if( cmp<0 ) break;
      }
      sqlite3DbFree(db, pUnpacked);
      /* Make room for sample @pos */
      if( iPos<pIdx->nSample ){
        int k;
        IndexSample tmp = pIdx->aSample[pIdx->nSample];
        for(k=pIdx->nSample-1; k>=iPos; k--){
          pIdx->aSample[k+1] = pIdx->aSample[k];
        }
        pIdx->aSample[iPos] = tmp;
      }
    }
    assert( iPos>=0 && iPos<pIdx->nAlloc );
    pSample = &pIdx->aSample[iPos];
    decodeIntArray((char*)sqlite3_column_text(pStmt,1),nCol,pSample->anEq,0,0);
    decodeIntArray((char*)sqlite3_column_text(pStmt,2),nCol,pSample->anLt,0,0);
    decodeIntArray((char*)sqlite3_column_text(pStmt,3),nCol,pSample->anDLt,0,0);

    /* Take a copy of the sample. Add two 0x00 bytes the end of the buffer.
    ** This is in case the sample record is corrupted. In that case, the
    ** sqlite3VdbeRecordCompare() may read up to two varints past the
    ** end of the allocated buffer before it realizes it is dealing with
    ** a corrupt record. Adding the two 0x00 bytes prevents this from causing
    ** a buffer overread.  */
    pSample->n = sqlite3_column_bytes(pStmt, 4);
    pSample->p = sqlite3DbMallocZero(db, pSample->n + 2);
    if( pSample->p==0 ){
      sqlite3_finalize(pStmt);
      return SQLITE_NOMEM_BKPT;
    }
    memcpy(pSample->p, sqlite3_column_blob(pStmt, 4), pSample->n);
    memset((u8*)pSample->p + pSample->n, 0, 2);
    pIdx->nSample++;
    pIdx->pTable->tabFlags |= TF_HasStat4;
  }
  return sqlite3_finalize(pStmt);
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
  int rc = SQLITE_OK;             /* Result codes from subroutines */

  assert( db->lookaside.bDisable );
  if( sqlite3FindTable(db, "sqlite_stat4", zDb) ){
    rc = loadStatTbl(db, 0,
      "SELECT idx,count(*) FROM %Q.sqlite_stat4 GROUP BY idx", 
      "SELECT idx,neq,nlt,ndlt,sample FROM %Q.sqlite_stat4",
      zDb
    );
  }

  if( rc==SQLITE_OK && sqlite3FindTable(db, "sqlite_stat3", zDb) ){
    rc = loadStatTbl(db, 1,
      "SELECT idx,count(*) FROM %Q.sqlite_stat3 GROUP BY idx", 
      "SELECT idx,neq,nlt,ndlt,sqlite_record(sample) FROM %Q.sqlite_stat3",
      zDb
    );
  }

  return rc;
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
}
#endif /* SQLITE_ENABLE_STAT3_OR_STAT4 */

/*
** Load the content of the sqlite_stat1 and sqlite_stat3/4 tables. The
** contents of sqlite_stat1 are used to populate the Index.aiRowEst[]
** arrays. The contents of sqlite_stat3/4 are used to populate the
** Index.aSample[] arrays.
**
** If the sqlite_stat1 table is not present in the database, SQLITE_ERROR
** is returned. In this case, even if SQLITE_ENABLE_STAT3/4 was defined 
** during compilation and the sqlite_stat3/4 table is present, no data is 
** read from it.
**
** If SQLITE_ENABLE_STAT3/4 was defined during compilation and the 
** sqlite_stat4 table is not present in the database, SQLITE_ERROR is
** returned. However, in this case, data is read from the sqlite_stat1
** table (if it is present) before returning.
**
** If an OOM error occurs, this function always sets db->mallocFailed.
** This means if the caller does not care about other errors, the return
** code may be ignored.
*/
int sqlite3AnalysisLoad(sqlite3 *db, int iDb){
  analysisInfo sInfo;
  HashElem *i;
  char *zSql;
  int rc = SQLITE_OK;
  Schema *pSchema = db->aDb[iDb].pSchema;

  assert( iDb>=0 && iDb<db->nDb );
  assert( db->aDb[iDb].pBt!=0 );

  /* Clear any prior statistics */
  assert( sqlite3SchemaMutexHeld(db, iDb, 0) );
  for(i=sqliteHashFirst(&pSchema->tblHash); i; i=sqliteHashNext(i)){
    Table *pTab = sqliteHashData(i);
    pTab->tabFlags &= ~TF_HasStat1;
  }
  for(i=sqliteHashFirst(&pSchema->idxHash); i; i=sqliteHashNext(i)){
    Index *pIdx = sqliteHashData(i);
#if defined(SQLITE_BUILDING_FOR_COMDB2)
    /* if this is a dynamic attach, don't wipe all the data! */
    if( iDb <= 1
     || db->init.busy==0
     || db->init.zTblName == NULL
     || pIdx->pTable == NULL
     || pIdx->pTable->zName == NULL
     || (strlen(db->init.zTblName) == strlen(pIdx->pTable->zName)
     && strncasecmp(db->init.zTblName,
              pIdx->pTable->zName, strlen(db->init.zTblName)) == 0)
    ){
       pIdx->hasStat1 = 0;
       sqlite3DefaultRowEst(pIdx);
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    pIdx->hasStat1 = 0;
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
#ifdef SQLITE_ENABLE_STAT3_OR_STAT4
    sqlite3DeleteIndexSamples(db, pIdx);
    pIdx->aSample = 0;
#endif
#if defined(SQLITE_BUILDING_FOR_COMDB2)
    }else{
      /* We are running InitOne on an attached table, but this one is
      ** a index for an already attached table */
    }
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
  }

  /* Load new statistics out of the sqlite_stat1 table */
  sInfo.db = db;
  sInfo.zDatabase = db->aDb[iDb].zDbSName;
#if defined(SQLITE_BUILDING_FOR_COMDB2)
  if( sqlite3FindTableByAnalysisLoad(db, "sqlite_stat1", sInfo.zDatabase)!=0 ){
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
  if( sqlite3FindTable(db, "sqlite_stat1", sInfo.zDatabase)!=0 ){
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    zSql = sqlite3MPrintf(db, 
#if defined(SQLITE_BUILDING_FOR_COMDB2)
        "SELECT tbl,idx,stat FROM %Q.sqlite_stat1 WHERE tbl not like 'cdb2.%%.sav'", sInfo.zDatabase);
#else /* defined(SQLITE_BUILDING_FOR_COMDB2) */
        "SELECT tbl,idx,stat FROM %Q.sqlite_stat1", sInfo.zDatabase);
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    if( zSql==0 ){
      rc = SQLITE_NOMEM_BKPT;
    }else{
      rc = sqlite3_exec(db, zSql, analysisLoader, &sInfo, 0);
      sqlite3DbFree(db, zSql);
    }
  }

  /* Set appropriate defaults on all indexes not in the sqlite_stat1 table */
  assert( sqlite3SchemaMutexHeld(db, iDb, 0) );
  for(i=sqliteHashFirst(&pSchema->idxHash); i; i=sqliteHashNext(i)){
    Index *pIdx = sqliteHashData(i);
    if( !pIdx->hasStat1 ) sqlite3DefaultRowEst(pIdx);
  }

  /* Load the statistics from the sqlite_stat4 table. */
#ifdef SQLITE_ENABLE_STAT3_OR_STAT4
  if( rc==SQLITE_OK ){
    db->lookaside.bDisable++;
    rc = loadStat4(db, sInfo.zDatabase);
    db->lookaside.bDisable--;
  }
  for(i=sqliteHashFirst(&pSchema->idxHash); i; i=sqliteHashNext(i)){
    Index *pIdx = sqliteHashData(i);
#if defined(SQLITE_BUILDING_FOR_COMDB2)
    initAvgEq(pIdx);
    if( pIdx->pKeyInfo ){
      int nRef = pIdx->pKeyInfo->nRef;
      sqlite3KeyInfoUnref(pIdx->pKeyInfo);
      if( nRef==1 ){ /* was just us -- no one else using */
        pIdx->pKeyInfo = NULL;
      }
    }
#endif /* defined(SQLITE_BUILDING_FOR_COMDB2) */
    sqlite3_free(pIdx->aiRowEst);
    pIdx->aiRowEst = 0;
  }
#endif

  if( rc==SQLITE_NOMEM ){
    sqlite3OomFault(db);
  }
  return rc;
}


#endif /* SQLITE_OMIT_ANALYZE */
