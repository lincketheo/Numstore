/// Copyright 2026 Theo Lincke
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.

#include "nscore/wal/ns_wal.h"

#include "core/ns_error.h"
#include "core/ns_logging.h"
#include "core/ns_numerics.h"
#include "core/ns_string.h"
#include "core/ns_testing.h"
#include "core/ns_utils.h"
#include "core/os/ns_malloc.h"
#include "core/os/ns_os.h"
#include "nscore/wal/ns_wal_istream.h"
#include "nscore/wal/ns_wal_ostream.h"

#include <stdatomic.h>
#include <string.h>

/******************************************************************************
 * SECTION: WAL
 * ----------------------------------------------------------------------------
 * @brief Main WAL object for writing WAL logs
 ******************************************************************************/

static err_t
wal_init (struct wal *dest, error *e)
{
  dest->ostream = NULL;
  dest->istream = NULL;
  dest->flags   = 0;
  latch_init (&dest->latch);

  dest->ostream = walos_open (dest->fname.data, dest->mem, dest->os, e);
  if (dest->ostream == NULL) {
    return error_trace (e);
  }

  dest->istream = walis_open (dest->fname.data, dest->mem, dest->os, e);
  if (dest->istream == NULL) {
    walos_close (dest->ostream, e);
    dest->ostream = NULL;
    return error_trace (e);
  }

  // Read the start lsn
  bool iseof     = false;
  u32  checksum  = checksum_init ();
  lsn  start_lsn = 0;

  walis_mark_start_log (dest->istream);

  if (walis_read_all (dest->istream, &iseof, NULL, &checksum, &start_lsn, sizeof (start_lsn), e)) {
    walos_close (dest->ostream, e);
    walis_close (dest->istream, e);
    dest->ostream = NULL;
    dest->istream = NULL;
    return error_trace (e);
  }

  walis_mark_end_log (dest->istream);

  if (iseof) {
    dest->flags |= WAL_ISNEW;

    // Truncate the output wal
    if (walos_truncate (dest->ostream, e)) {
      walos_close (dest->ostream, e);
      walis_close (dest->istream, e);
      dest->ostream = NULL;
      dest->istream = NULL;
      return error_trace (e);
    }
  } else {
    dest->start_lsn = start_lsn;
  }

  DBG_ASSERT (wal, dest);

  return error_trace (e);
}

err_t
wal_write_start_lsn (struct wal *w, lsn start_lsn, error *e)
{
  ASSERT (w->flags & WAL_ISNEW);

  WRAP (walos_write_all (w->ostream, NULL, &start_lsn, sizeof (start_lsn), e));

  w->start_lsn = start_lsn;
  w->flags &= ~WAL_ISNEW;

  // Seek the reader to the start too
  WRAP (walis_seek (w->istream, sizeof (start_lsn), e));

  return SUCCESS;
}

static struct wal *
wal_open_internal (const char *fname, struct i_mem mem, struct i_os os, error *e)
{
  struct wal *dest = mem.table->malloc (mem.self, 1, sizeof *dest, e);
  if (dest == NULL) {
    return NULL;
  }

  dest->mem = mem;
  dest->os  = os;

  if (string_copy (&dest->fname, strfcstr (fname), mem, e)) {
    mem.table->free (mem.self, dest);
    return NULL;
  }

  if (wal_init (dest, e)) {
    mem.table->free (mem.self, (char *)dest->fname.data);
    mem.table->free (mem.self, dest);
    return NULL;
  }

  return dest;
}

struct wal *
wal_open (const char *fname, struct i_mem mem, struct i_os os, error *e)
{
  return wal_open_internal (fname, mem, os, e);
}

static inline err_t
wal_destroy (struct wal *w, error *e)
{
  if (w->ostream) {
    wal_flush_all (w, e);
    walos_close (w->ostream, e);
    w->ostream = NULL;
  }
  if (w->istream) {
    walis_close (w->istream, e);
    w->istream = NULL;
  }

  if (w->fname.data) {
    w->mem.table->free (w->mem.self, (void *)w->fname.data);
    w->fname.data = NULL;
  }
  return error_trace (e);
}

err_t
wal_close (struct wal *w, error *e)
{
  struct i_mem mem = w->mem;
  wal_destroy (w, e);
  mem.table->free (mem.self, w);
  return error_trace (e);
}

err_t
wal_close_and_delete (struct wal *w, error *e)
{
  struct i_mem  mem   = w->mem;
  struct i_os   os    = w->os;
  struct string fname = w->fname;
  w->fname.data       = NULL;

  wal_destroy (w, e);
  mem.table->free (mem.self, w);

  os.table->remove_quiet (os.self, fname.data, e);
  mem.table->free (mem.self, (char *)fname.data);

  return error_trace (e);
}

err_t
wal_delete_and_reopen (struct wal *w, error *e)
{
  latch_lock (&w->latch);

  // Copy fname to pass in
  struct string fname = w->fname;

  // Set to NULL so we don't free it
  w->fname.data       = NULL;

  if (wal_destroy (w, e)) {
    latch_unlock (&w->latch);
    return error_trace (e);
  }

  if (w->os.table->remove_quiet (w->os.self, fname.data, e)) {
    latch_unlock (&w->latch);
    return error_trace (e);
  }

  w->fname = fname;

  return wal_init (w, e);
}

bool
wal_isnew (const struct wal *w)
{
  return (w->flags & WAL_ISNEW) != 0;
}

lsn
wal_start_lsn (struct wal *w)
{
  return w->start_lsn;
}

lsn
wal_size (struct wal *w)
{
  return walos_get_next_lsn (w->ostream);
}

err_t
wal_flush_all (const struct wal *w, error *e)
{
  DBG_ASSERT (wal, w);
  ASSERT (w->ostream);
  return walos_flush_all (w->ostream, e);
}

err_t
wal_crash (struct wal *w, error *e)
{
  DBG_ASSERT (wal, w);

  struct i_mem mem = w->mem;

  if (w->ostream) {
    walos_close (w->ostream, e);
  }
  if (w->istream) {
    walis_close (w->istream, e);
  }
  if (w->fname.data) {
    mem.table->free (mem.self, (void *)w->fname.data);
  }
  mem.table->free (mem.self, w);

  return SUCCESS;
}

/*-----------------------------------------------------------------------------
 * SUBSECTION: WAL Write
 * @brief Write implementation functions
 *----------------------------------------------------------------------------*/

slsn
wal_append_begin_log (struct wal *w, const txid tid, error *e)
{
  latch_lock (&w->latch);
  DBG_ASSERT (wal, w);
  w->whdr.type      = WL_BEGIN;
  w->whdr.begin     = (struct wal_begin){.tid = tid};
  const slsn result = wal_write_locked (w, e);
  latch_unlock (&w->latch);
  return result;
}

slsn
wal_append_commit_log (struct wal *w, const txid tid, const lsn prev, error *e)
{
  latch_lock (&w->latch);
  DBG_ASSERT (wal, w);
  w->whdr.type      = WL_COMMIT;
  w->whdr.commit    = (struct wal_commit){.tid = tid, .prev = prev};
  const slsn result = wal_write_locked (w, e);
  latch_unlock (&w->latch);
  return result;
}

slsn
wal_append_end_log (struct wal *w, const txid tid, const lsn prev, error *e)
{
  latch_lock (&w->latch);
  DBG_ASSERT (wal, w);
  w->whdr.type      = WL_END;
  w->whdr.end       = (struct wal_end){.tid = tid, .prev = prev};
  const slsn result = wal_write_locked (w, e);
  latch_unlock (&w->latch);
  return result;
}

slsn
wal_append_update_log (struct wal *w, const struct wal_update_write update, error *e)
{
  latch_lock (&w->latch);
  DBG_ASSERT (wal, w);
  w->whdr.type      = WL_UPDATE;
  w->whdr.update    = update;
  const slsn result = wal_write_locked (w, e);
  latch_unlock (&w->latch);
  return result;
}

slsn
wal_append_clr_log (struct wal *w, const struct wal_clr_write clr, error *e)
{
  latch_lock (&w->latch);
  DBG_ASSERT (wal, w);
  w->whdr.type      = WL_CLR;
  w->whdr.clr       = clr;
  const slsn result = wal_write_locked (w, e);
  latch_unlock (&w->latch);
  return result;
}

slsn
wal_append_log (struct wal *w, const struct wal_rec_hdr_write *whdr, error *e)
{
  switch (whdr->type) {
    case WL_BEGIN: {
      return wal_append_begin_log (w, whdr->begin.tid, e);
    }
    case WL_COMMIT: {
      return wal_append_commit_log (w, whdr->commit.tid, whdr->commit.prev, e);
    }
    case WL_END: {
      return wal_append_end_log (w, whdr->end.tid, whdr->end.prev, e);
    }
    case WL_UPDATE: {
      return wal_append_update_log (w, whdr->update, e);
    }
    case WL_CLR: {
      return wal_append_clr_log (w, whdr->clr, e);
    }
    case WL_EOF: UNREACHABLE (); // LCOV_EXCL_LINE
  }

  UNREACHABLE (); // LCOV_EXCL_LINE
}

/*-----------------------------------------------------------------------------
 * SUBSECTION: WAL Read
 * @brief Read implementation functions
 *----------------------------------------------------------------------------*/

static int
wal_read_full (
    const struct wal *w,
    u32              *checksum,
    const wlh         type,
    const wlh         second_type,
    u8               *buf,
    const u32         total_len,
    error            *e
)
{
  ASSERT (total_len >= sizeof (wlh) + sizeof (u32));

  u8 *head = buf;

  memcpy (head, &type, sizeof (wlh));
  head += sizeof (wlh);

  if (second_type != WLH_NULL) {
    memcpy (head, &second_type, sizeof (wlh));
    head += sizeof (wlh);
  }

  {
    const u32 toread = total_len - (head - buf) - sizeof (u32);
    if (toread > 0) {
      bool iseof;
      WRAP (walis_read_all (w->istream, &iseof, NULL, checksum, head, toread, e));
      if (iseof) {
        return WL_EOF;
      }
    }

    head += toread;
    bool iseof;
    WRAP (walis_read_all (w->istream, &iseof, NULL, NULL, head, sizeof (u32), e));
    if (iseof) {
      return WL_EOF;
    }
  }

  u32 actual_crc;
  memcpy (&actual_crc, buf + total_len - sizeof (u32), sizeof (u32));
  if (*checksum != actual_crc) {
    return error_causef (e, ERR_CORRUPT, "Invalid CRC");
  }

  return SUCCESS;
}

static err_t
wal_read_physical_update (struct wal *w, u32 *checksum, struct wal_rec_hdr_read *r, error *e)
{
  ASSERT (r->type == WL_UPDATE);
  u8        buf[WL_UPDATE_LEN];
  const int ret = wal_read_full (w, checksum, r->type, r->update.type, buf, WL_UPDATE_LEN, e);
  WRAP (ret);
  if (ret == WL_EOF) {
    r->type = WL_EOF;
    return SUCCESS;
  }
  ASSERT (ret == SUCCESS);
  walf_decode_physical_update (r, buf);
  return SUCCESS;
}

static err_t
wal_read_fsm_update (struct wal *w, u32 *checksum, struct wal_rec_hdr_read *r, error *e)
{
  ASSERT (r->type == WL_UPDATE);
  u8        buf[WL_FSM_UPDATE_LEN];
  const int ret = wal_read_full (w, checksum, r->type, r->update.type, buf, WL_FSM_UPDATE_LEN, e);
  WRAP (ret);
  if (ret == WL_EOF) {
    r->type = WL_EOF;
    return SUCCESS;
  }
  ASSERT (ret == SUCCESS);
  walf_decode_fsm_update (r, buf);
  return SUCCESS;
}

static err_t
wal_read_file_extend_update (struct wal *w, u32 *checksum, struct wal_rec_hdr_read *r, error *e)
{
  ASSERT (r->type == WL_UPDATE);
  u8        buf[WL_FILE_EXT_LEN];
  const int ret = wal_read_full (w, checksum, r->type, r->update.type, buf, WL_FILE_EXT_LEN, e);
  WRAP (ret);
  if (ret == WL_EOF) {
    r->type = WL_EOF;
    return SUCCESS;
  }
  ASSERT (ret == SUCCESS);
  walf_decode_file_extend_update (r, buf);
  return SUCCESS;
}

static err_t
wal_read_physical_clr (struct wal *w, u32 *checksum, struct wal_rec_hdr_read *r, error *e)
{
  ASSERT (r->type == WL_CLR);
  u8        buf[WL_CLR_LEN];
  const int ret = wal_read_full (w, checksum, r->type, r->clr.type, buf, WL_CLR_LEN, e);
  WRAP (ret);
  if (ret == WL_EOF) {
    r->type = WL_EOF;
    return SUCCESS;
  }
  ASSERT (ret == SUCCESS);
  walf_decode_physical_clr (r, buf);
  return SUCCESS;
}

static err_t
wal_read_fsm_clr (struct wal *w, u32 *checksum, struct wal_rec_hdr_read *r, error *e)
{
  ASSERT (r->type == WL_CLR);
  u8        buf[WL_FSM_CLR_LEN];
  const int ret = wal_read_full (w, checksum, r->type, r->clr.type, buf, WL_FSM_CLR_LEN, e);
  WRAP (ret);
  if (ret == WL_EOF) {
    r->type = WL_EOF;
    return SUCCESS;
  }
  ASSERT (ret == SUCCESS);
  walf_decode_fsm_clr (r, buf);
  return SUCCESS;
}

static err_t
wal_read_dummy_clr (struct wal *w, u32 *checksum, struct wal_rec_hdr_read *r, error *e)
{
  ASSERT (r->type == WL_CLR);
  u8        buf[WL_DUMMY_CLR_LEN];
  const int ret = wal_read_full (w, checksum, r->type, r->clr.type, buf, WL_DUMMY_CLR_LEN, e);
  WRAP (ret);
  if (ret == WL_EOF) {
    r->type = WL_EOF;
    return SUCCESS;
  }
  ASSERT (ret == SUCCESS);
  walf_decode_dummy_clr (r, buf);
  return SUCCESS;
}

static err_t
wal_read_begin (struct wal *w, u32 *checksum, struct wal_rec_hdr_read *r, error *e)
{
  ASSERT (r->type == WL_BEGIN);
  u8        buf[WL_BEGIN_LEN];
  const int ret = wal_read_full (w, checksum, r->type, WLH_NULL, buf, WL_BEGIN_LEN, e);
  WRAP (ret);
  if (ret == WL_EOF) {
    r->type = WL_EOF;
    return SUCCESS;
  }
  ASSERT (ret == SUCCESS);
  walf_decode_begin (r, buf);
  return SUCCESS;
}

static err_t
wal_read_commit (struct wal *w, u32 *checksum, struct wal_rec_hdr_read *r, error *e)
{
  ASSERT (r->type == WL_COMMIT);
  u8        buf[WL_COMMIT_LEN];
  const int ret = wal_read_full (w, checksum, r->type, WLH_NULL, buf, WL_COMMIT_LEN, e);
  WRAP (ret);
  if (ret == WL_EOF) {
    r->type = WL_EOF;
    return SUCCESS;
  }
  ASSERT (ret == SUCCESS);
  walf_decode_commit (r, buf);
  return SUCCESS;
}

static err_t
wal_read_end (struct wal *w, u32 *checksum, struct wal_rec_hdr_read *r, error *e)
{
  ASSERT (r->type == WL_END);
  u8        buf[WL_END_LEN];
  const int ret = wal_read_full (w, checksum, r->type, WLH_NULL, buf, WL_END_LEN, e);
  WRAP (ret);
  if (ret == WL_EOF) {
    r->type = WL_EOF;
    return SUCCESS;
  }
  ASSERT (ret == SUCCESS);
  walf_decode_end (r, buf);
  return SUCCESS;
}

static err_t
wal_read_sequential (struct wal *w, struct wal_rec_hdr_read *dest, lsn *rlsn, error *e)
{
  u32  checksum = checksum_init ();
  wlh  t;
  bool iseof;

  walis_mark_start_log (w->istream);

  WRAP (walis_read_all (w->istream, &iseof, rlsn, &checksum, &t, sizeof (t), e));
  if (rlsn) {
    *rlsn += w->start_lsn;
  }
  if (iseof) {
    dest->type = WL_EOF;
    return SUCCESS;
  }

  dest->type = -1;

  switch (t) {
    case WL_UPDATE: {
      dest->type        = t;
      dest->update.type = -1;
      WRAP (walis_read_all (w->istream, &iseof, rlsn, &checksum, &t, sizeof (t), e));
      if (rlsn) {
        *rlsn += w->start_lsn;
      }
      if (iseof) {
        dest->type = WL_EOF;
        return SUCCESS;
      }
      switch (t) {
        case WUP_PHYSICAL:
          dest->update.type = t;
          WRAP (wal_read_physical_update (w, &checksum, dest, e));
          break;
        case WUP_FEXT:
          dest->update.type = t;
          WRAP (wal_read_file_extend_update (w, &checksum, dest, e));
          break;
        case WUP_FSM:
          dest->update.type = t;
          WRAP (wal_read_fsm_update (w, &checksum, dest, e));
          break;
      }
      if ((int)dest->update.type == -1) {
        dest->type = -1;
      }
      break;
    }
    case WL_CLR: {
      dest->type     = t;
      dest->clr.type = -1;
      WRAP (walis_read_all (w->istream, &iseof, rlsn, &checksum, &t, sizeof (t), e));
      if (rlsn) {
        *rlsn += w->start_lsn;
      }
      if (iseof) {
        dest->type = WL_EOF;
        return SUCCESS;
      }
      switch (t) {
        case WCLR_PHYSICAL:
          dest->clr.type = t;
          WRAP (wal_read_physical_clr (w, &checksum, dest, e));
          break;
        case WCLR_FSM:
          dest->clr.type = t;
          WRAP (wal_read_fsm_clr (w, &checksum, dest, e));
          break;
        case WCLR_DUMMY:
          dest->clr.type = t;
          WRAP (wal_read_dummy_clr (w, &checksum, dest, e));
          break;
      }
      if ((int)dest->clr.type == -1) {
        dest->type = -1;
      }
      break;
    }
    case WL_BEGIN: {
      dest->type = t;
      WRAP (wal_read_begin (w, &checksum, dest, e));
      break;
    }
    case WL_COMMIT: {
      dest->type = t;
      WRAP (wal_read_commit (w, &checksum, dest, e));
      break;
    }
    case WL_END: {
      dest->type = t;
      WRAP (wal_read_end (w, &checksum, dest, e));
      break;
    }
  }

  if ((int)dest->type == -1) {
    return error_causef (e, ERR_CORRUPT, "Invalid wal header type");
  }

  walis_mark_end_log (w->istream);

  return SUCCESS;
}

struct wal_rec_hdr_read *
wal_read_next (struct wal *w, lsn *rlsn, error *e)
{
  latch_lock (&w->latch);
  DBG_ASSERT (wal, w);

  ASSERT (w->istream);
  if (wal_read_sequential (w, &w->rhdr, rlsn, e)) {
    latch_unlock (&w->latch);
    return NULL;
  }

  latch_unlock (&w->latch);
  return &w->rhdr;
}

struct wal_rec_hdr_read *
wal_read_first (struct wal *w, error *e)
{
  return wal_read_entry (w, sizeof (lsn), e);
}

struct wal_rec_hdr_read *
wal_read_entry (struct wal *w, const lsn id, error *e)
{
  latch_lock (&w->latch);
  DBG_ASSERT (wal, w);

  ASSERT (w->istream);
  if (id < w->start_lsn) {
    error_causef (
        e,
        ERR_CORRUPT,
        "Tried to read previous deleted log %" PRlsn " %" PRlsn,
        id,
        w->start_lsn
    );
    latch_unlock (&w->latch);
    return NULL;
  }

  if (walis_seek (w->istream, id - w->start_lsn, e)) {
    latch_unlock (&w->latch);
    return NULL;
  }

  lsn rlsn;
  if (wal_read_sequential (w, &w->rhdr, &rlsn, e)) {
    latch_unlock (&w->latch);
    return NULL;
  }

  latch_unlock (&w->latch);
  return &w->rhdr;
}

/******************************************************************************
 * SECTION: WAL Write Logic
 * ----------------------------------------------------------------------------
 * @brief Write WAL entries
 ******************************************************************************/

static err_t
wal_write_begin (const struct wal *w, const struct wal_rec_hdr_write *r, error *e)
{
  ASSERT (r->type == WL_BEGIN);

  ASSERT (w->ostream);

  u32       checksum = checksum_init ();
  const wlh t        = r->type;
  WRAP (walos_write_all (w->ostream, &checksum, &t, sizeof (wlh), e));
  WRAP (walos_write_all (w->ostream, &checksum, &r->begin.tid, sizeof (txid), e));
  WRAP (walos_write_all (w->ostream, NULL, &checksum, sizeof (u32), e));

  return SUCCESS;
}

static err_t
wal_write_commit (const struct wal *w, const struct wal_rec_hdr_write *r, error *e)
{
  ASSERT (r->type == WL_COMMIT);

  ASSERT (w->ostream);

  u32       checksum = checksum_init ();
  const wlh t        = r->type;
  WRAP (walos_write_all (w->ostream, &checksum, &t, sizeof (wlh), e));
  WRAP (walos_write_all (w->ostream, &checksum, &r->commit.tid, sizeof (txid), e));
  WRAP (walos_write_all (w->ostream, &checksum, &r->commit.prev, sizeof (lsn), e));
  WRAP (walos_write_all (w->ostream, NULL, &checksum, sizeof (u32), e));

  return SUCCESS;
}

static err_t
wal_write_end (const struct wal *w, const struct wal_rec_hdr_write *r, error *e)
{
  ASSERT (r->type == WL_END);

  ASSERT (w->ostream);

  u32       checksum = checksum_init ();
  const wlh t        = r->type;
  WRAP (walos_write_all (w->ostream, &checksum, &t, sizeof (wlh), e));
  WRAP (walos_write_all (w->ostream, &checksum, &r->end.tid, sizeof (txid), e));
  WRAP (walos_write_all (w->ostream, &checksum, &r->end.prev, sizeof (lsn), e));
  WRAP (walos_write_all (w->ostream, NULL, &checksum, sizeof (u32), e));

  return SUCCESS;
}

static err_t
wal_write_physical_update (const struct wal *w, const struct wal_rec_hdr_write *r, error *e)
{
  ASSERT (w->ostream);

  u32       checksum = checksum_init ();
  const wlh t        = (wlh)r->type;
  const wlh ut       = (wlh)r->update.type;
  WRAP (walos_write_all (w->ostream, &checksum, &t, sizeof (wlh), e));
  WRAP (walos_write_all (w->ostream, &checksum, &ut, sizeof (wlh), e));
  WRAP (walos_write_all (w->ostream, &checksum, &r->update.tid, sizeof (txid), e));
  WRAP (walos_write_all (w->ostream, &checksum, &r->update.prev, sizeof (lsn), e));
  WRAP (walos_write_all (w->ostream, &checksum, &r->update.phys.pg, sizeof (pgno), e));
  WRAP (walos_write_all (w->ostream, &checksum, r->update.phys.undo, NS_PAGE_SIZE, e));
  WRAP (walos_write_all (w->ostream, &checksum, r->update.phys.redo, NS_PAGE_SIZE, e));
  WRAP (walos_write_all (w->ostream, NULL, &checksum, sizeof (u32), e));

  return SUCCESS;
}

static err_t
wal_write_fsm_update (const struct wal *w, const struct wal_rec_hdr_write *r, error *e)
{
  ASSERT (w->ostream);

  u32       checksum = checksum_init ();
  const wlh t        = (wlh)r->type;
  const wlh ut       = (wlh)r->update.type;
  WRAP (walos_write_all (w->ostream, &checksum, &t, sizeof (wlh), e));
  WRAP (walos_write_all (w->ostream, &checksum, &ut, sizeof (wlh), e));
  WRAP (walos_write_all (w->ostream, &checksum, &r->update.tid, sizeof (txid), e));
  WRAP (walos_write_all (w->ostream, &checksum, &r->update.prev, sizeof (lsn), e));
  WRAP (walos_write_all (w->ostream, &checksum, &r->update.fsm.pg, sizeof (pgno), e));
  WRAP (walos_write_all (w->ostream, &checksum, &r->update.fsm.bit, sizeof (p_size), e));
  WRAP (walos_write_all (w->ostream, &checksum, &r->update.fsm.undo, sizeof (u8), e));
  WRAP (walos_write_all (w->ostream, &checksum, &r->update.fsm.redo, sizeof (u8), e));
  WRAP (walos_write_all (w->ostream, NULL, &checksum, sizeof (u32), e));

  return SUCCESS;
}

static err_t
wal_write_file_extend_update (const struct wal *w, const struct wal_rec_hdr_write *r, error *e)
{
  ASSERT (w->ostream);

  u32       checksum = checksum_init ();
  const wlh t        = (wlh)r->type;
  const wlh ut       = (wlh)r->update.type;
  WRAP (walos_write_all (w->ostream, &checksum, &t, sizeof (wlh), e));
  WRAP (walos_write_all (w->ostream, &checksum, &ut, sizeof (wlh), e));
  WRAP (walos_write_all (w->ostream, &checksum, &r->update.tid, sizeof (txid), e));
  WRAP (walos_write_all (w->ostream, &checksum, &r->update.prev, sizeof (lsn), e));
  WRAP (walos_write_all (w->ostream, &checksum, &r->update.fext.undo, sizeof (pgno), e));
  WRAP (walos_write_all (w->ostream, &checksum, &r->update.fext.redo, sizeof (pgno), e));
  WRAP (walos_write_all (w->ostream, NULL, &checksum, sizeof (u32), e));

  return SUCCESS;
}

static err_t
wal_write_physical_clr (const struct wal *w, const struct wal_rec_hdr_write *r, error *e)
{
  ASSERT (r->type == WL_CLR);

  ASSERT (w->ostream);

  u32       checksum = checksum_init ();
  const wlh t        = r->type;
  const wlh ut       = r->clr.type;
  WRAP (walos_write_all (w->ostream, &checksum, &t, sizeof (wlh), e));
  WRAP (walos_write_all (w->ostream, &checksum, &ut, sizeof (wlh), e));
  WRAP (walos_write_all (w->ostream, &checksum, &r->clr.tid, sizeof (txid), e));
  WRAP (walos_write_all (w->ostream, &checksum, &r->clr.prev, sizeof (lsn), e));
  WRAP (walos_write_all (w->ostream, &checksum, &r->clr.phys.pg, sizeof (pgno), e));
  WRAP (walos_write_all (w->ostream, &checksum, &r->clr.undo_next, sizeof (lsn), e));
  WRAP (walos_write_all (w->ostream, &checksum, r->clr.phys.redo, NS_PAGE_SIZE, e));
  WRAP (walos_write_all (w->ostream, NULL, &checksum, sizeof (u32), e));

  return SUCCESS;
}

static err_t
wal_write_fsm_clr (const struct wal *w, const struct wal_rec_hdr_write *r, error *e)
{
  ASSERT (r->type == WL_CLR);

  ASSERT (w->ostream);

  u32       checksum = checksum_init ();
  const wlh t        = r->type;
  const wlh ut       = r->clr.type;
  WRAP (walos_write_all (w->ostream, &checksum, &t, sizeof (wlh), e));
  WRAP (walos_write_all (w->ostream, &checksum, &ut, sizeof (wlh), e));
  WRAP (walos_write_all (w->ostream, &checksum, &r->clr.tid, sizeof (txid), e));
  WRAP (walos_write_all (w->ostream, &checksum, &r->clr.prev, sizeof (lsn), e));
  WRAP (walos_write_all (w->ostream, &checksum, &r->clr.fsm.pg, sizeof (pgno), e));
  WRAP (walos_write_all (w->ostream, &checksum, &r->clr.undo_next, sizeof (lsn), e));
  WRAP (walos_write_all (w->ostream, &checksum, &r->clr.fsm.bit, sizeof (p_size), e));
  WRAP (walos_write_all (w->ostream, &checksum, &r->clr.fsm.redo, sizeof (u8), e));
  WRAP (walos_write_all (w->ostream, NULL, &checksum, sizeof (u32), e));

  return SUCCESS;
}

static err_t
wal_write_dummy_clr (const struct wal *w, const struct wal_rec_hdr_write *r, error *e)
{
  ASSERT (r->type == WL_CLR);

  ASSERT (w->ostream);

  u32       checksum = checksum_init ();
  const wlh t        = r->type;
  const wlh ut       = r->clr.type;
  WRAP (walos_write_all (w->ostream, &checksum, &t, sizeof (wlh), e));
  WRAP (walos_write_all (w->ostream, &checksum, &ut, sizeof (wlh), e));
  WRAP (walos_write_all (w->ostream, &checksum, &r->clr.tid, sizeof (txid), e));
  WRAP (walos_write_all (w->ostream, &checksum, &r->clr.prev, sizeof (lsn), e));
  WRAP (walos_write_all (w->ostream, &checksum, &r->clr.undo_next, sizeof (lsn), e));
  WRAP (walos_write_all (w->ostream, NULL, &checksum, sizeof (u32), e));

  return SUCCESS;
}

slsn
wal_write_locked (struct wal *w, error *e)
{
  ASSERT (w->ostream);
  ASSERT (!(w->flags & WAL_ISNEW));

  const lsn ret = walos_get_next_lsn (w->ostream) + w->start_lsn;

  switch (w->whdr.type) {
    case WL_BEGIN: {
      WRAP (wal_write_begin (w, &w->whdr, e));
      break;
    }
    case WL_COMMIT: {
      WRAP (wal_write_commit (w, &w->whdr, e));
      break;
    }
    case WL_END: {
      WRAP (wal_write_end (w, &w->whdr, e));
      break;
    }
    case WL_UPDATE: {
      switch (w->whdr.update.type) {
        case WUP_PHYSICAL: {
          WRAP (wal_write_physical_update (w, &w->whdr, e));
          break;
        }
        case WUP_FSM: {
          WRAP (wal_write_fsm_update (w, &w->whdr, e));
          break;
        }
        case WUP_FEXT: {
          WRAP (wal_write_file_extend_update (w, &w->whdr, e));
          break;
        }
      }
      break;
    }
    case WL_CLR: {
      switch (w->whdr.clr.type) {
        case WCLR_PHYSICAL: {
          WRAP (wal_write_physical_clr (w, &w->whdr, e));
          break;
        }
        case WCLR_FSM: {
          WRAP (wal_write_fsm_clr (w, &w->whdr, e));
          break;
        }
        case WCLR_DUMMY: {
          WRAP (wal_write_dummy_clr (w, &w->whdr, e));
          break;
        }
      }
      break;
    }
    case WL_EOF: {
      UNREACHABLE (); // LCOV_EXCL_LINE
    }
  }

  return ret;
}

#ifndef NDEBUG

/**
 * @struct wal_queue
 * @brief Parameters for the wal_multi_threaded test
 *
 * @var wal_queue::sync
 * @brief The "start" signal so all threads start at the same time
 *
 * @var wal_queue::ww
 * @brief The shared wal writer
 *
 * @var wal_queue::idx
 * @brief The next log to write - this is atomically added to for each writer
 *
 * @var wal_queue::read
 * @brief The list of log records to write (as read to make it easier to build)
 *
 * @var wal_queue::len
 * @brief Length of [read]
 */
struct wal_queue
{
  _Atomic u32              sync;
  struct wal              *ww;
  atomic_int               idx;

  struct wal_rec_hdr_read *read;
  const int                len;
};

static void *
wal_thread (void *ctx)
{
  error             e = error_create ();
  struct wal_queue *q = ctx;

  while (atomic_load (&q->sync) > 0) {
    spin_pause ();
  }

  while (true) {
    const int idx = atomic_fetch_add (&q->idx, 1);
    if (idx >= q->len) {
      return NULL;
    }

    struct wal_rec_hdr_write write = wrhw_from_wrhr (&q->read[idx]);

    const slsn               l     = wal_append_log (q->ww, &write, &e);
    if (l < 0) {
      panic ("Failed to write log");
    }

    if (wal_flush_all (q->ww, &e)) {
      panic ("Failed to flush wal");
    }
  }
}

TEST (wal_multi_threaded)
{
  ctx->os.table->remove_quiet (ctx->os.self, "test.wal", &ctx->e);
  struct wal *ww = wal_open ("test.wal", ctx->mem, ctx->os, &ctx->e);
  wal_write_start_lsn (ww, 0, &ctx->e);

  const u32                N    = 5000;

  struct wal_rec_hdr_read *read = ctx->mem.table->malloc (ctx->mem.self, N, sizeof *read, &ctx->e);

  for (u32 i = 0; i < N; ++i) {
    wal_rec_hdr_read_random (&read[i]);
  }

  struct wal_queue queue = {
      .sync = 1,
      .ww   = ww,
      .idx  = 0,
      .read = read,
      .len  = N,
  };

  u32      nthreads;
  i_thread threads[10];
  for (nthreads = 0; nthreads < arrlen (threads); ++nthreads) {
    ctx->os.table->thread_create (ctx->os.self, &threads[nthreads], wal_thread, &queue, &ctx->e);
  }

  // launch
  atomic_store (&queue.sync, 0);

  i_log_info ("Threads active\n");

  for (; nthreads > 0; --nthreads) {
    ctx->os.table->thread_join (ctx->os.self, threads[nthreads - 1].self);
  }

  // To speed up searches, keep a "finger" which is "near" the
  // most recent found log
  u32 finger   = 0;
  lsn read_lsn = 0;

  for (u32 i = 0; i < N; ++i) {
    struct wal_rec_hdr_read *actual = wal_read_next (ww, &read_lsn, &ctx->e);
    i_print_wal_rec_hdr_read_light (LOG_INFO, actual, read_lsn);
    test_assert (actual->type != WL_EOF);

    // Search through all records to ensure it's there
    bool found = false;
    for (u32 k = 0; k < N; ++k) {
      const u32 idx = (finger + k) % N;
      if (wal_rec_hdr_read_equal (actual, &read[idx])) {
        finger         = (idx + 1) % N;
        read[idx].type = WL_EOF;
        found          = true;
        break;
      }
    }
    test_assert (found);
  }

  const struct wal_rec_hdr_read *actual = wal_read_next (ww, &read_lsn, &ctx->e);
  test_assert_int_equal (actual->type, WL_EOF);

  wal_close (ww, &ctx->e);
  ctx->mem.table->free (ctx->mem.self, read);
}

struct wal_test_params
{
  const char              *fname;
  struct wal_rec_hdr_read *batch1;
  u32                      batch1_len;
  struct wal_rec_hdr_read *batch2;
  u32                      batch2_len;
};

static void
wal_test_fill_batch (struct wal_rec_hdr_read *batch, const u32 len)
{
  // I just removed error for this because it wasn't being used -
  // no other reason - maybe breaks

  for (u32 i = 0; i < len; i++) {
    struct wal_rec_hdr_read *r = &batch[i];

    switch (r->type) {
      case WL_UPDATE: {
        rand_bytes (r->update.phys.undo, NS_PAGE_SIZE);
        rand_bytes (r->update.phys.redo, NS_PAGE_SIZE);
        break;
      }
      case WL_CLR: {
        rand_bytes (r->clr.phys.redo, NS_PAGE_SIZE);
        break;
      }
      default: {
        break;
      }
    }
  }
}

static void
wal_test_free_batch (const struct wal_rec_hdr_read *batch, const u32 len)
{
  for (u32 i = 0; i < len; i++) {
    const struct wal_rec_hdr_read *r = &batch[i];
    test_assert (r != NULL);
  }
}

static void
run_wal_test (const struct wal_test_params *p, const struct i_os os)
{
  error        e   = error_create ();
  struct i_mem mem = default_mem ();

  os.table->remove_quiet (os.self, p->fname, &e);
  struct wal *ww = wal_open (p->fname, mem, os, &e);
  wal_write_start_lsn (ww, 0, &e);
  /**
   * Write all the input logs
   */
  {
    slsn l = -1;
    for (u32 i = 0; i < p->batch1_len; i++) {
      struct wal_rec_hdr_write out   = wrhw_from_wrhr (&p->batch1[i]);
      slsn                     nextl = wal_append_log (ww, &out, &e);
      test_assert (nextl >= 0);
      test_assert (nextl > l);
      l = nextl;
    }
    wal_flush_all (ww, &e);
  }

  /**
   * Read all the input logs and expect
   * that they are the same as the
   * first batch written ones
   */
  {
    for (u32 i = 0; i < p->batch1_len; i++) {
      lsn                      read_lsn;
      struct wal_rec_hdr_read *next = NULL;
      if (i == 0) {
        next = wal_read_first (ww, &e);
      } else {
        next = wal_read_next (ww, &read_lsn, &e);
      }
      test_assert (wal_rec_hdr_read_equal (next, &p->batch1[i]));
    }
  }

  /**
   * Write a second batch of input logs
   */
  {
    slsn l = 0;
    for (u32 i = 0; i < p->batch2_len; i++) {
      struct wal_rec_hdr_write out = wrhw_from_wrhr (&p->batch2[i]);
      l                            = wal_append_log (ww, &out, &e);
      test_assert (l > 0);
    }
    wal_flush_all (ww, &e);
  }

  /**
   * Read from the start and confirm all the logs
   */
  {
    for (u32 i = 0; i < p->batch1_len; i++) {
      lsn                      read_lsn;
      struct wal_rec_hdr_read *next = NULL;
      if (i == 0) {
        next = wal_read_first (ww, &e);
      } else {
        next = wal_read_next (ww, &read_lsn, &e);
      }
      test_assert (wal_rec_hdr_read_equal (next, &p->batch1[i]));
    }

    for (u32 i = 0; i < p->batch2_len; i++) {
      lsn                      read_lsn;
      struct wal_rec_hdr_read *next = wal_read_next (ww, &read_lsn, &e);
      test_assert (wal_rec_hdr_read_equal (next, &p->batch2[i]));
    }
  }

  wal_close (ww, &e);
}

////////////////////////////////////////////////////////////
// WAL test cases

TEST (wal)
{
  struct wal_rec_hdr_read batch1_full[] = {
      {.type = WL_BEGIN, .begin = {.tid = 1}},
      {.type = WL_COMMIT, .commit = {.tid = 3, .prev = 20}},
      {.type = WL_END, .end = {.tid = 4, .prev = 30}},
      {
          .type = WL_UPDATE,
          .update =
              {
                  .type = WUP_PHYSICAL,
                  .tid  = 5,
                  .prev = 40,
                  .phys = {.pg = 111},
              },
      },
      {
          .type = WL_CLR,
          .clr  = {
              .type      = WCLR_PHYSICAL,
              .tid       = 6,
              .prev      = 50,
              .undo_next = 42,
              .phys      = {.pg = 222},
          },
      },
  };

  struct wal_rec_hdr_read batch2_full[] = {
      {.type = WL_BEGIN, .begin = {.tid = 2}},
      {
          .type   = WL_UPDATE,
          .update = {
              .type = WUP_PHYSICAL,
              .tid  = 6,
              .prev = 41,
              .phys = {.pg = 112},
          },
      },
  };

  struct wal_rec_hdr_read batch1_begin_only[] = {
      {.type = WL_BEGIN, .begin = {.tid = 1}},
  };

  struct wal_rec_hdr_read batch2_begin_only[] = {
      {.type = WL_BEGIN, .begin = {.tid = 2}},
  };

  struct wal_rec_hdr_read batch1_no_ckpt[] = {
      {.type = WL_BEGIN, .begin = {.tid = 1}},
      {.type = WL_COMMIT, .commit = {.tid = 3, .prev = 20}},
      {.type = WL_END, .end = {.tid = 4, .prev = 30}},
      {
          .type = WL_UPDATE,
          .update =
              {
                  .type = WUP_PHYSICAL,
                  .tid  = 5,
                  .prev = 40,
                  .phys = {.pg = 111},
              },
      },
      {
          .type = WL_CLR,
          .clr  = {
              .type      = WCLR_PHYSICAL,
              .tid       = 6,
              .prev      = 50,
              .undo_next = 42,
              .phys      = {.pg = 222},
          },
      },
  };

  struct wal_test_params cases[] = {
      {
          .fname      = "test_full.wal",
          .batch1     = batch1_full,
          .batch1_len = arrlen (batch1_full),
          .batch2     = batch2_full,
          .batch2_len = arrlen (batch2_full),
      },
      {
          .fname      = "test_begin_only.wal",
          .batch1     = batch1_begin_only,
          .batch1_len = arrlen (batch1_begin_only),
          .batch2     = batch2_begin_only,
          .batch2_len = arrlen (batch2_begin_only),
      },
      {
          .fname      = "test_no_ckpt.wal",
          .batch1     = batch1_no_ckpt,
          .batch1_len = arrlen (batch1_no_ckpt),
          .batch2     = batch2_full,
          .batch2_len = arrlen (batch2_full),
      },
  };

  for (u32 i = 0; i < arrlen (cases); i++) {
    TEST_CASE ("Wal: %d", i)
    {
      const struct wal_test_params *c = &cases[i];

      wal_test_fill_batch (c->batch1, c->batch1_len);
      wal_test_fill_batch (c->batch2, c->batch2_len);

      run_wal_test (c, ctx->os);

      wal_test_free_batch (c->batch1, c->batch1_len);
      wal_test_free_batch (c->batch2, c->batch2_len);
    }
  }
}

TEST (wal_single_entry)
{
  struct wal_rec_hdr_read cases[] = {
      {.type = WL_BEGIN, .begin = {.tid = 1}},
      {.type = WL_COMMIT, .commit = {.tid = 2, .prev = 10}},
      {.type = WL_END, .end = {.tid = 3, .prev = 20}},
      {.type   = WL_UPDATE,
       .update = {.type = WUP_PHYSICAL, .tid = 4, .prev = 30, .phys = {.pg = 111}}},
      {.type = WL_CLR,
       .clr  = {.type = WCLR_PHYSICAL, .tid = 5, .prev = 40, .undo_next = 42, .phys = {.pg = 222}}},
  };

  for (u32 i = 0; i < arrlen (cases); i++) {
    TEST_CASE ("wal_single_entry: %d", i)
    {
      struct wal_rec_hdr_read *c = &cases[i];

      wal_test_fill_batch (c, 1);

      ctx->os.table->remove_quiet (ctx->os.self, "test_single_entry.wal", &ctx->e);
      struct wal *ww = wal_open ("test_single_entry.wal", ctx->mem, ctx->os, &ctx->e);
      wal_write_start_lsn (ww, 0, &ctx->e);

      // WRITE
      struct wal_rec_hdr_write out = wrhw_from_wrhr (c);
      const slsn               l   = wal_append_log (ww, &out, &ctx->e);
      test_assert (l >= 0);

      wal_flush_all (ww, &ctx->e);

      // READ
      struct wal_rec_hdr_read *next = wal_read_first (ww, &ctx->e);
      test_assert (wal_rec_hdr_read_equal (next, c));

      wal_close (ww, &ctx->e);

      wal_test_free_batch (c, 1);
    }
  }
}

#endif
