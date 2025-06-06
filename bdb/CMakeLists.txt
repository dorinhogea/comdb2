add_library(bdb
  add.c
  attr.c
  bdb.c
  bdb_access.c
  bdb_blkseq.c
  bdb_llops.c
  bdb_net.c
  bdb_osqlbkfill.c
  bdb_osqlcur.c
  bdb_osqllog.c
  bdb_osqltrn.c
  bdb_schemachange.c
  bdb_thd_io.c
  bdb_verify.c
  bdblock.c
  berktest.c
  callback.c
  compress.c
  count.c
  cursor.c
  cursor_ll.c
  cursor_rowlocks.c
  custom_recover.c
  del.c
  error.c
  fetch.c
  file.c
  fstdump.c
  genid.c
  import.c
  info.c
  lite.c
  ll.c
  llmeta.c
  llog_auto.c
  locks.c
  locktest.c
  log_queue_dump.c
  log_queue_trigger.c
  log_trigger.c
  odh.c
  phys.c
  phys_rep_lsn.c
  queue.c
  queuedb.c
  read.c
  rep.c
  rep_qstat.c
  rowlocks.c
  rowlocks_util.c
  serializable.c
  summarize.c
  temphash.c
  temptable.c
  threads.c
  tran.c
  tranread.c
  upd.c
  util.c
)

set(module bdb)
set(MODULE BDB)
configure_file(${PROJECT_SOURCE_DIR}/mem/mem.h.in mem_bdb.h @ONLY)
set(BERK_DIST ${PROJECT_SOURCE_DIR}/berkdb/dist)
configure_file(${BERK_DIST}/RELEASE dist/RELEASE COPYONLY)
configure_file(${BERK_DIST}/genrec.sh dist/genrec.sh COPYONLY)
configure_file(${BERK_DIST}/geninc.sh dist/geninc.sh COPYONLY)
configure_file(${BERK_DIST}/gen_rec.awk dist/gen_rec.awk COPYONLY)
configure_file(${BERK_DIST}/gen_inc.awk dist/gen_inc.awk COPYONLY)
configure_file(${BERK_DIST}/template/rec_ctemp dist/template/rec_ctemp COPYONLY)
add_custom_command(
  OUTPUT llog_auto.c llog_auto.h llog_ext.h
  DEPENDS llog.src
  COMMAND dist/genrec.sh ${CMAKE_CURRENT_SOURCE_DIR}/llog.src llog_auto.c llog_auto.h rec_llog
  COMMAND dist/geninc.sh llog llog_ext.h llog_auto.c
)
if (${CMAKE_C_COMPILER_ID} STREQUAL Clang OR ${CMAKE_C_COMPILER_ID} STREQUAL AppleClang)
  set_source_files_properties(llog_auto.c PROPERTIES COMPILE_FLAGS "-Wno-deprecated-non-prototype")
endif()

include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
  ${PROJECT_SOURCE_DIR}/util
  ${PROJECT_SOURCE_DIR}/bbinc
  ${PROJECT_BINARY_DIR}/bbinc
  ${PROJECT_SOURCE_DIR}/sqlite/src
  ${PROJECT_SOURCE_DIR}/sqlite/ext/comdb2
  ${PROJECT_SOURCE_DIR}/berkdb
  ${PROJECT_SOURCE_DIR}/berkdb/build
  ${PROJECT_BINARY_DIR}/berkdb
  ${PROJECT_SOURCE_DIR}/comdb2rle
  ${PROJECT_SOURCE_DIR}/crc32c
  ${PROJECT_SOURCE_DIR}/csc2
  ${PROJECT_SOURCE_DIR}/cson
  ${PROJECT_SOURCE_DIR}/datetime
  ${PROJECT_SOURCE_DIR}/db
  ${PROJECT_BINARY_DIR}/db
  ${PROJECT_SOURCE_DIR}/dfp/decNumber
  ${PROJECT_SOURCE_DIR}/dfp/dfpal
  ${PROJECT_SOURCE_DIR}/dlmalloc
  ${PROJECT_SOURCE_DIR}/mem
  ${PROJECT_BINARY_DIR}/mem
  ${PROJECT_SOURCE_DIR}/net
  ${PROJECT_SOURCE_DIR}/cdb2api
  ${PROJECT_BINARY_DIR}/protobuf
  ${PROJECT_SOURCE_DIR}/schemachange
  ${LIBEVENT_INCLUDE_DIR}
  ${OPENSSL_INCLUDE_DIR}
  ${PROTOBUF-C_INCLUDE_DIR}
)
add_dependencies(bdb db mem proto)
target_link_libraries(bdb PUBLIC db)
if (COMDB2_BBCMAKE)
  comdb2_lib_target(bdb)
endif()
