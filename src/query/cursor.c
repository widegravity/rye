/*
 * Copyright (C) 2008 Search Solution Corporation. All rights reserved by Search Solution.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

/*
 * cursor.c - cursor manager
 */

#ident "$Id$"

#include "config.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "error_manager.h"
#include "storage_common.h"
#include "memory_alloc.h"
#include "object_primitive.h"
#include "db.h"
#include "locator_cl.h"
#include "server_interface.h"
#include "work_space.h"
#include "set_object.h"
#include "cursor.h"
#include "parser_support.h"
#include "page_buffer.h"
#include "network_interface_cl.h"

/* this must be the last header file included!!! */
#include "dbval.h"

#define CURSOR_BUFFER_SIZE              DB_PAGESIZE
#define CURSOR_BUFFER_AREA_SIZE         IO_MAX_PAGE_SIZE

enum
{
  FIRST_TPL = -1,
  LAST_TPL = -2
};

static void cursor_initialize_current_tuple_value_position (CURSOR_ID * cursor_id_p);
#if defined (ENABLE_UNUSED_FUNCTION)
static int cursor_fixup_vobjs (DB_VALUE * val);
#endif
static int cursor_get_tuple_value_to_dbvalue (OR_BUF * buf, TP_DOMAIN * dom,
                                              QFILE_TUPLE_VALUE_FLAG val_flag, DB_VALUE * db_value, bool copy);
static int cursor_get_tuple_value_from_list (CURSOR_ID * c_id, int index, DB_VALUE * value, char *tuple);
#if defined (ENABLE_UNUSED_FUNCTION)
static int cursor_get_first_tuple_value (char *tuple,
                                         QFILE_TUPLE_VALUE_TYPE_LIST * type_list, DB_VALUE * value, bool copy);
#endif
static char *cursor_peek_tuple (CURSOR_ID * cursor_id);
static int cursor_get_list_file_page (CURSOR_ID * cursor_id, VPID * vpid);
static int cursor_allocate_tuple_area (CURSOR_ID * cursor_id_p, int tuple_length);
static int cursor_construct_tuple_from_overflow_pages (CURSOR_ID * cursor_id_p, VPID * vpid_p);
static int cursor_point_current_tuple (CURSOR_ID * cursor_id_p, int position, int offset);
static int cursor_buffer_last_page (CURSOR_ID * cursor_id_p, VPID * vpid_p);

/*
 * List File routines
 */

static void
cursor_initialize_current_tuple_value_position (CURSOR_ID * cursor_id_p)
{
  if (cursor_id_p == NULL)
    {
      assert (0);
      return;
    }

  cursor_id_p->current_tuple_value_index = -1;
  cursor_id_p->current_tuple_value_p = NULL;
}

/*
 * cursor_copy_list_id () - Copy source list identifier into destination
 *                    list identifier
 *   return: true on ok, false otherwise
 *   dest_list_id(out): Destination list identifier
 *   src_list_id(in): Source list identifier
 */
int
cursor_copy_list_id (QFILE_LIST_ID * dest_list_id_p, const QFILE_LIST_ID * src_list_id_p)
{
  size_t size;
  QFILE_TUPLE_VALUE_TYPE_LIST *dest_type_list_p;
  const QFILE_TUPLE_VALUE_TYPE_LIST *src_type_list_p;

  memcpy (dest_list_id_p, src_list_id_p, DB_SIZEOF (QFILE_LIST_ID));

  src_type_list_p = &(src_list_id_p->type_list);
  dest_type_list_p = &(dest_list_id_p->type_list);

  dest_list_id_p->type_list.domp = NULL;
  if (src_list_id_p->type_list.type_cnt)
    {
      size = src_type_list_p->type_cnt * sizeof (TP_DOMAIN *);
      dest_type_list_p->domp = (TP_DOMAIN **) malloc (size);

      if (dest_type_list_p->domp == NULL)
        {
          return ER_FAILED;
        }
      memcpy (dest_type_list_p->domp, src_type_list_p->domp, size);
    }

  dest_list_id_p->tpl_descr.f_valp = NULL;
  dest_list_id_p->sort_list = NULL;     /* never use sort_list in crs_ level */

  if (src_list_id_p->last_pgptr)
    {
      dest_list_id_p->last_pgptr = (PAGE_PTR) malloc (CURSOR_BUFFER_SIZE);
      if (dest_list_id_p->last_pgptr == NULL)
        {
          return ER_FAILED;
        }

      memcpy (dest_list_id_p->last_pgptr, src_list_id_p->last_pgptr, CURSOR_BUFFER_SIZE);
    }

  return NO_ERROR;
}

/*
 * cursor_free_list_id () - Area allocated for list file identifier is freed
 *   return: nothing
 *   list_id: List file identifier
 */
void
cursor_free_list_id (QFILE_LIST_ID * list_id_p, bool self)
{
  if (list_id_p->last_pgptr)
    {
      free_and_init (list_id_p->last_pgptr);
    }
  if (list_id_p->tpl_descr.f_valp)
    {
      free_and_init (list_id_p->tpl_descr.f_valp);
    }
  if (list_id_p->sort_list)
    {
      free_and_init (list_id_p->sort_list);
    }
  if (list_id_p->type_list.domp)
    {
      free_and_init (list_id_p->type_list.domp);
    }
  if (self)
    {
      free_and_init (list_id_p);
    }
}

#if defined (ENABLE_UNUSED_FUNCTION)
/*
 * cursor_fixup_vobjs () -
 *   return: NO_ERROR on all ok, ER status( or ER_FAILED) otherwise
 *   value(in/out): a db_value
 * Note: if value is an OID then turn it into an OBJECT type value
 *       if value is a VOBJ then turn it into a vmop
 *       if value is a set/seq then do same fixups on its elements
 */
static int
cursor_fixup_vobjs (DB_VALUE * value_p)
{
  DB_OBJECT *obj = NULL;
  OID *oid;
  int rc;

  switch (DB_VALUE_DOMAIN_TYPE (value_p))
    {
    case DB_TYPE_OID:
      oid = DB_GET_OID (value_p);
      if (oid != NULL && !OID_ISNULL (oid))
        {
          obj = ws_mop (oid, NULL);
        }

      if (obj)
        {
          DB_MAKE_OBJECT (value_p, obj);
          rc = NO_ERROR;
        }
      else
        {
          assert (false);       /* should be avoided */
          rc = ER_FAILED;
        }
      break;

    case DB_TYPE_SEQUENCE:
      rc = set_convert_oids_to_objects (DB_GET_SET (value_p));
      break;

    default:
      rc = NO_ERROR;
      break;
    }

  return rc;
}
#endif

/*
 * cursor_get_tuple_value_to_dbvalue () - The given tuple value which is in disk
 *   representation form is copied/peeked to the db_value structure
 *   return: NO_ERROR on all ok, ER status( or ER_FAILED) otherwise
 *    buf(in)          : Pointer to the tuple value
 *    dom(in)           : Domain for the tpl column
 *    val_flag(in)      : Flag to indicate if tuple value is bound
 *    db_value(out)     : Set to the tuple value
 *    copy(in)          : Indicator for copy/peek
 */
static int
cursor_get_tuple_value_to_dbvalue (OR_BUF * buffer_p, TP_DOMAIN * domain_p,
                                   QFILE_TUPLE_VALUE_FLAG value_flag, DB_VALUE * value_p, bool is_copy)
{
  PR_TYPE *pr_type;
  DB_TYPE type;

  pr_type = domain_p->type;
  if (pr_type == NULL)
    {
      return ER_FAILED;
    }

  type = pr_type->id;
  if (value_flag == V_UNBOUND)
    {
      db_value_domain_init (value_p, type, domain_p->precision, domain_p->scale);
      return NO_ERROR;
    }

  /* for all other types, we can use the prim routines */
  if ((*(pr_type->data_readval)) (buffer_p, value_p, domain_p, -1, is_copy) != NO_ERROR)
    {
      return ER_FAILED;
    }

#if defined (ENABLE_UNUSED_FUNCTION)
  /*
   * OIDs must be turned into objects.
   */
  return cursor_fixup_vobjs (value_p);
#endif

  return NO_ERROR;
}

/*
 * cursor_get_tuple_value_from_list () - The tuple value at the indicated position is
 *   extracted and mapped to given db_value
 *   return: NO_ERROR on all ok, ER status( or ER_FAILED) otherwise
 *   c_id(in)   : Cursor Identifier
 *   index(in)  : Tuple Value index
 *   value(out) : Set to the fetched tuple value
 *   tuple(in)  : List file tuple
 */
static int
cursor_get_tuple_value_from_list (CURSOR_ID * cursor_id_p, int index, DB_VALUE * value_p, char *tuple_p)
{
  QFILE_TUPLE_VALUE_TYPE_LIST *type_list_p;
  QFILE_TUPLE_VALUE_FLAG flag;
  OR_BUF buffer;
  int i;

  if (cursor_id_p == NULL)
    {
      assert (0);
      return ER_FAILED;
    }

  type_list_p = &cursor_id_p->list_id.type_list;

  assert (index >= 0 && index < type_list_p->type_cnt);

  or_init (&buffer, tuple_p, QFILE_GET_TUPLE_LENGTH (tuple_p));

  /* check for saved tplvalue position info */
  if (cursor_id_p->current_tuple_value_index >= 0
      && cursor_id_p->current_tuple_value_index <= index && cursor_id_p->current_tuple_value_p != NULL)
    {
      i = cursor_id_p->current_tuple_value_index;
      tuple_p = cursor_id_p->current_tuple_value_p;
    }
  else
    {
      i = 0;
      tuple_p += QFILE_TUPLE_LENGTH_SIZE;
    }

  for (; i < index; i++)
    {
      tuple_p += (QFILE_TUPLE_VALUE_HEADER_SIZE + QFILE_GET_TUPLE_VALUE_LENGTH (tuple_p));
    }

  /* save index-th tplvalue position info */
  cursor_id_p->current_tuple_value_index = i;
  cursor_id_p->current_tuple_value_p = tuple_p;

  flag = QFILE_GET_TUPLE_VALUE_FLAG (tuple_p);
  tuple_p += QFILE_TUPLE_VALUE_HEADER_SIZE;
  buffer.ptr = tuple_p;

  return cursor_get_tuple_value_to_dbvalue (&buffer, type_list_p->domp[i],
                                            flag, value_p, cursor_id_p->is_copy_tuple_value);
}

#if defined (ENABLE_UNUSED_FUNCTION)
/*
 * cursor_get_first_tuple_value () - First tuple value is extracted and mapped to
 *                             given db_value
 *   return: NO_ERROR on all ok, ER status( or ER_FAILED) otherwise
 *   tuple(in): List file tuple
 *   type_list(in): Type List
 *   value(out): Set to the first tuple value
 *   copy(in): Indicator for copy/peek
 */
static int
cursor_get_first_tuple_value (char *tuple_p,
                              QFILE_TUPLE_VALUE_TYPE_LIST * type_list_p, DB_VALUE * value_p, bool is_copy)
{
  QFILE_TUPLE_VALUE_FLAG flag;
  OR_BUF buffer;

  or_init (&buffer, tuple_p, QFILE_GET_TUPLE_LENGTH (tuple_p));

  tuple_p = (char *) tuple_p + QFILE_TUPLE_LENGTH_SIZE;
  flag = QFILE_GET_TUPLE_VALUE_FLAG (tuple_p);
  tuple_p += QFILE_TUPLE_VALUE_HEADER_SIZE;
  buffer.ptr = tuple_p;

  return cursor_get_tuple_value_to_dbvalue (&buffer, type_list_p->domp[0], flag, value_p, is_copy);
}
#endif

/*
 * cursor_get_list_file_page () -
 *   return:
 *   cursor_id(in/out): Cursor identifier
 *   vpid(in):
 */
static int
cursor_get_list_file_page (CURSOR_ID * cursor_id_p, VPID * vpid_p)
{
  VPID in_vpid;
  int page_size;
  char *page_p;

  if (cursor_id_p == NULL || vpid_p == NULL)
    {
      assert (0);
      return ER_FAILED;
    }

  /* find page at buffer area */
  if (VPID_EQ (vpid_p, &cursor_id_p->current_vpid))
    {
      /*
       * current_vpid can indicate one of pages in buffer area,
       * so do not assign buffer as head of buffer area
       */
      ;
    }
  else
    {
      cursor_id_p->buffer = NULL;
      if (cursor_id_p->buffer_filled_size > 0)
        {
          /* it received a page from server */
          if (VPID_EQ (vpid_p, &cursor_id_p->header_vpid))
            {
              /* in case of header vpid in buffer area */
              cursor_id_p->buffer = cursor_id_p->buffer_area;
            }
          else
            {
              page_p = cursor_id_p->buffer_area;
              page_size = 0;

              while (page_size < (cursor_id_p->buffer_filled_size - CURSOR_BUFFER_SIZE))
                {
                  if (QFILE_GET_OVERFLOW_PAGE_ID (page_p) == NULL_PAGEID)
                    {
                      QFILE_GET_NEXT_VPID (&in_vpid, page_p);
                    }
                  else
                    {
                      QFILE_GET_OVERFLOW_VPID (&in_vpid, page_p);
                    }

                  if (VPID_ISNULL (&in_vpid))
                    {
                      break;
                    }
                  else if (VPID_EQ (vpid_p, &in_vpid))
                    {
                      cursor_id_p->buffer = page_p + CURSOR_BUFFER_SIZE;
                      break;
                    }

                  page_p += CURSOR_BUFFER_SIZE;
                  page_size += CURSOR_BUFFER_SIZE;
                }
            }
        }
    }

  /* if not found, get the page from server */
  if (cursor_id_p->buffer == NULL)
    {
      int ret_val;

      assert (cursor_id_p->buffer_area != NULL);

      ret_val = qfile_get_list_file_page (cursor_id_p->query_id,
                                          vpid_p->volid,
                                          vpid_p->pageid, cursor_id_p->buffer_area, &cursor_id_p->buffer_filled_size);

      if (ret_val != NO_ERROR)
        {
          return ret_val;
        }

      cursor_id_p->buffer = cursor_id_p->buffer_area;
      QFILE_COPY_VPID (&cursor_id_p->header_vpid, vpid_p);
    }

  return NO_ERROR;
}

static int
cursor_allocate_tuple_area (CURSOR_ID * cursor_id_p, int tuple_length)
{
  if (cursor_id_p == NULL)
    {
      assert (0);
      return ER_FAILED;
    }

  if (cursor_id_p->tuple_record.size == 0)
    {
      cursor_id_p->tuple_record.tpl = (char *) malloc (tuple_length);
    }
  else
    {
      cursor_id_p->tuple_record.tpl = (char *) realloc (cursor_id_p->tuple_record.tpl, tuple_length);
    }

  if (cursor_id_p->tuple_record.tpl == NULL)
    {
      return ER_FAILED;
    }

  cursor_id_p->tuple_record.size = tuple_length;
  return NO_ERROR;
}

static int
cursor_construct_tuple_from_overflow_pages (CURSOR_ID * cursor_id_p, VPID * vpid_p)
{
  VPID overflow_vpid;
  char *buffer_p;
  char *tmp_tuple_p, *tuple_p;
  int tuple_length, offset, tuple_page_size;

  if (cursor_id_p == NULL || vpid_p == NULL)
    {
      assert (0);
      return ER_FAILED;
    }

  /* get tuple length and allocate space for the tuple */
  tmp_tuple_p = cursor_id_p->buffer + QFILE_PAGE_HEADER_SIZE;
  tuple_length = QFILE_GET_TUPLE_LENGTH (tmp_tuple_p);

  if (cursor_id_p->tuple_record.size < tuple_length)
    {
      if (cursor_allocate_tuple_area (cursor_id_p, tuple_length) != NO_ERROR)
        {
          return ER_FAILED;
        }
    }

  tuple_p = cursor_id_p->tuple_record.tpl;
  offset = 0;

  do
    {
      buffer_p = cursor_id_p->buffer;

      QFILE_GET_OVERFLOW_VPID (&overflow_vpid, buffer_p);
      tuple_page_size = MIN (tuple_length - offset, QFILE_MAX_TUPLE_SIZE_IN_PAGE);
      memcpy (tuple_p, buffer_p + QFILE_PAGE_HEADER_SIZE, tuple_page_size);
      tuple_p += tuple_page_size;
      offset += tuple_page_size;

      if (overflow_vpid.pageid != NULL_PAGEID)
        {
          if (cursor_get_list_file_page (cursor_id_p, &overflow_vpid) != NO_ERROR)
            {
              return ER_FAILED;
            }
          QFILE_COPY_VPID (&cursor_id_p->current_vpid, &overflow_vpid);
        }
    }
  while (overflow_vpid.pageid != NULL_PAGEID);

  /* reset buffer as a head page of overflow page */
  if (!VPID_EQ (vpid_p, &overflow_vpid) && cursor_get_list_file_page (cursor_id_p, vpid_p) != NO_ERROR)
    {
      return ER_FAILED;
    }

  cursor_id_p->current_tuple_p = cursor_id_p->tuple_record.tpl;

  return NO_ERROR;
}

static int
cursor_point_current_tuple (CURSOR_ID * cursor_id_p, int position, int offset)
{
  if (cursor_id_p == NULL || cursor_id_p->buffer == NULL)
    {
      assert (0);
      return ER_FAILED;
    }

  cursor_id_p->buffer_tuple_count = QFILE_GET_TUPLE_COUNT (cursor_id_p->buffer);
  cursor_id_p->current_tuple_length = QFILE_GET_TUPLE_LENGTH ((cursor_id_p->buffer + QFILE_PAGE_HEADER_SIZE));

  if (position == LAST_TPL)
    {
      cursor_id_p->current_tuple_no = cursor_id_p->buffer_tuple_count - 1;
      cursor_id_p->current_tuple_offset = QFILE_GET_LAST_TUPLE_OFFSET (cursor_id_p->buffer);
    }
  else if (position == FIRST_TPL)
    {
      cursor_id_p->current_tuple_no = 0;
      cursor_id_p->current_tuple_offset = QFILE_PAGE_HEADER_SIZE;
    }
  else if (position < cursor_id_p->buffer_tuple_count)
    {
      cursor_id_p->current_tuple_no = position;
      cursor_id_p->current_tuple_offset = offset;
    }
  else
    {
      return ER_FAILED;
    }

  return NO_ERROR;
}

static int
cursor_buffer_last_page (CURSOR_ID * cursor_id_p, VPID * vpid_p)
{
  if (cursor_id_p == NULL || vpid_p == NULL)
    {
      assert (0);
      return ER_FAILED;
    }

  if (cursor_id_p->list_id.last_pgptr && VPID_EQ (&(cursor_id_p->list_id.first_vpid), vpid_p))
    {
      if (cursor_id_p->buffer == NULL)
        {
          return ER_FAILED;
        }

      memcpy (cursor_id_p->buffer, cursor_id_p->list_id.last_pgptr, CURSOR_BUFFER_SIZE);
    }
  else
    {
      if (cursor_get_list_file_page (cursor_id_p, vpid_p) != NO_ERROR)
        {
          return ER_FAILED;
        }
    }

  return NO_ERROR;
}

/*
 * cursor_fetch_page_having_tuple () - A request is made to the server side to
 *   bring the specified list file page and copy the page to the cursor buffer
 *   area
 *   return: NO_ERROR on all ok, ER status( or ER_FAILED) otherwise
 *   cursor_id(in): Cursor identifier
 *   vpid(in): List File Real Page Identifier
 *   position(in):
 *   offset(in):
 * Note: For performance reasons, this routine checks the cursor identifier
 *       and if the cursor LIST FILE has a hidden OID column (for update)
 *       or has preceding hidden OID columns, vector fetches those referred
 *       objects from the server.
 *
 *       It also positions the tuple pointer to the desired tuple position.
 *       If position = LAST_TPL, then the cursor is positioned to the LAST
 *       tuple on the page.  If position = FIRST_TPL, then the cursor is
 *       positioned to the FIRST tuple on the page.  Otherwise, position is
 *       the tuple position in the fetched page and offset is used as the
 *       byte offset to the tuple.  If positioning to the first or last tuple
 *       on the page, the offset is ignored.
 */
int
cursor_fetch_page_having_tuple (CURSOR_ID * cursor_id_p, VPID * vpid_p, int position, int offset)
{
  if (cursor_id_p == NULL || vpid_p == NULL)
    {
      assert (0);
      return ER_FAILED;
    }

  cursor_initialize_current_tuple_value_position (cursor_id_p);

  if (!VPID_EQ (&(cursor_id_p->current_vpid), vpid_p))
    {
      if (cursor_buffer_last_page (cursor_id_p, vpid_p) != NO_ERROR)
        {
          return ER_FAILED;
        }
    }

  if (cursor_id_p->buffer == NULL)
    {
      return ER_FAILED;
    }

  if (cursor_point_current_tuple (cursor_id_p, position, offset) != NO_ERROR)
    {
      return ER_FAILED;
    }

  if (QFILE_GET_OVERFLOW_PAGE_ID (cursor_id_p->buffer) != NULL_PAGEID)
    {
      if (cursor_construct_tuple_from_overflow_pages (cursor_id_p, vpid_p) != NO_ERROR)
        {
          return ER_FAILED;
        }
    }
  else
    {
      cursor_id_p->current_tuple_p = cursor_id_p->buffer + cursor_id_p->current_tuple_offset;
    }

  /* If there is only one tuple, don't prefetch objects because
   * prefetching a small set of objects is slower than fetching
   * them individually.
   */
  if (cursor_id_p->buffer_tuple_count < 2)
    {
      return NO_ERROR;
    }

  return NO_ERROR;
}

#if defined (RYE_DEBUG)
/*
 * cursor_print_list () - Dump the content of the list file to the standard output
 *   return:
 *   query_id(in):
 *   list_id(in): List File Identifier
 */
void
cursor_print_list (QUERY_ID query_id, QFILE_LIST_ID * list_id_p)
{
  CURSOR_ID cursor_id;
  DB_VALUE *value_list_p, *value_p;
  int count, i, status;

  if (list_id_p == NULL)
    {
      assert (0);
      return;
    }

  count = list_id_p->type_list.type_cnt;
  value_list_p = (DB_VALUE *) malloc (count * sizeof (DB_VALUE));
  if (value_list_p == NULL)
    {
      return;
    }

  fprintf (stdout, "\n=================   Q U E R Y   R E S U L T S   =================\n\n");

  if (cursor_open (&cursor_id, list_id_p) == false)
    {
      free_and_init (value_list_p);
      return;
    }

  cursor_id.query_id = query_id;

  while (true)
    {
      status = cursor_next_tuple (&cursor_id);
      if (status != DB_CURSOR_SUCCESS)
        {
          break;
        }

      if (cursor_get_tuple_value_list (&cursor_id, count, value_list_p) != NO_ERROR)
        {
          goto cleanup;
        }

      fprintf (stdout, "\n ");

      for (i = 0, value_p = value_list_p; i < count; i++, value_p++)
        {
          fprintf (stdout, "  ");

          if (TP_IS_SET_TYPE (DB_VALUE_TYPE (value_p)))
            {
              db_set_print (DB_GET_SET (value_p));
            }
          else
            {
              db_value_print (value_p);
            }

          db_value_clear (value_p);
          fprintf (stdout, "  ");
        }
    }

  fprintf (stdout, "\n");

cleanup:

  cursor_close (&cursor_id);

  free_and_init (value_list_p);
  return;
}
#endif

/*
 * Cursor Management routines
 */

/*
 * cursor_open () -
 *   return: true on all ok, false otherwise
 *   cursor_id(out): Cursor identifier
 *   list_id: List file identifier
 *
 * Note: A cursor is opened to scan through the tuples of the given
 *       list file. The cursor identifier is initialized and memory
 *       buffer for the cursor identifier is allocated.
 */
bool
cursor_open (CURSOR_ID * cursor_id_p, QFILE_LIST_ID * list_id_p)
{
  static QFILE_LIST_ID empty_list_id;   /* TODO: remove static empty_list_id */

  if (cursor_id_p == NULL)
    {
      assert (0);
      return false;
    }

  QFILE_CLEAR_LIST_ID (&empty_list_id);

  cursor_id_p->position = C_BEFORE;
  cursor_id_p->tuple_no = -1;
  VPID_SET_NULL (&cursor_id_p->current_vpid);
  VPID_SET_NULL (&cursor_id_p->next_vpid);
  VPID_SET_NULL (&cursor_id_p->header_vpid);
  cursor_id_p->tuple_record.size = 0;
  cursor_id_p->tuple_record.tpl = NULL;
  cursor_id_p->on_overflow = false;
  cursor_id_p->buffer_tuple_count = 0;
  cursor_id_p->current_tuple_no = -1;
  cursor_id_p->current_tuple_offset = -1;
  cursor_id_p->current_tuple_p = NULL;
  cursor_id_p->current_tuple_length = -1;
  cursor_id_p->buffer = NULL;
  cursor_id_p->buffer_area = NULL;
  cursor_id_p->buffer_filled_size = 0;
  cursor_id_p->list_id = empty_list_id;
  cursor_id_p->is_copy_tuple_value = true;      /* copy */
  cursor_initialize_current_tuple_value_position (cursor_id_p);

  if (cursor_copy_list_id (&cursor_id_p->list_id, list_id_p) != NO_ERROR)
    {
      return false;
    }

  cursor_id_p->query_id = list_id_p->query_id;

  if (cursor_id_p->list_id.type_list.type_cnt)
    {
      cursor_id_p->buffer_area = (char *) malloc (CURSOR_BUFFER_AREA_SIZE);
      cursor_id_p->buffer = cursor_id_p->buffer_area;

      if (cursor_id_p->buffer == NULL)
        {
          return false;
        }
    }

  return true;
}

/*
 * cursor_set_copy_tuple_value () - Record the indicator for copy/peek tplvalue.
 *   return: It returns the previous indicator.
 *   cursor_id(in/out): Cursor identifier
 *   copy(in):
 */
bool
cursor_set_copy_tuple_value (CURSOR_ID * cursor_id_p, bool is_copy)
{
  bool old;

  if (cursor_id_p == NULL)
    {
      assert (0);
      return false;
    }

  old = cursor_id_p->is_copy_tuple_value;

  cursor_id_p->is_copy_tuple_value = is_copy;

  return old;
}

/*
 * cursor_free () - Free the area allocated for the cursor identifier.
 *   return:
 *   cursor_id(in/out): Cursor Identifier
 */
void
cursor_free (CURSOR_ID * cursor_id_p)
{
  if (cursor_id_p == NULL)
    {
      assert (false);
      return;
    }

  cursor_free_list_id (&cursor_id_p->list_id, false);

  if (cursor_id_p->buffer_area != NULL)
    {
      free_and_init (cursor_id_p->buffer_area);
      cursor_id_p->buffer_filled_size = 0;
      cursor_id_p->buffer = NULL;
    }

  free_and_init (cursor_id_p->tuple_record.tpl);
}

/*
 * cursor_close () - Free the area allocated for the cursor identifier and
 *                       invalidate the cursor identifier.
 *   return:
 *   cursor_id(in/out): Cursor Identifier
 */
void
cursor_close (CURSOR_ID * cursor_id_p)
{
  if (cursor_id_p == NULL)
    {
      assert (0);
      return;
    }

  /* free the cursor allocated area */
  cursor_free (cursor_id_p);

  /* invalidate the cursor_id */
  cursor_id_p->position = C_BEFORE;
  cursor_id_p->tuple_no = -1;
  cursor_id_p->current_vpid.pageid = NULL_PAGEID;
  cursor_id_p->next_vpid.pageid = NULL_PAGEID;
  cursor_id_p->header_vpid.pageid = NULL_PAGEID;
  cursor_id_p->buffer_tuple_count = 0;
  cursor_id_p->current_tuple_no = -1;
  cursor_id_p->current_tuple_offset = -1;
  cursor_id_p->current_tuple_p = NULL;
  cursor_id_p->current_tuple_length = -1;
  cursor_id_p->query_id = NULL_QUERY_ID;
  cursor_initialize_current_tuple_value_position (cursor_id_p);
}

/*
 * crs_peek_tuple () - peek the current cursor tuple
 *   return: NULL on error
 *   cursor_id(in): Cursor Identifier
 * Note: A pointer to the beginning of the current cursor tuple is
 *       returned. The pointer directly points to inside the cursor memory
 *       buffer.
 */
static char *
cursor_peek_tuple (CURSOR_ID * cursor_id_p)
{
  if (cursor_id_p == NULL)
    {
      assert (0);
      return NULL;
    }

  if (cursor_id_p->position != C_ON)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_QPROC_INVALID_CRSPOS, 0);
      return NULL;
    }

  /* tuple is contained in the cursor buffer */
  return cursor_id_p->current_tuple_p;
}

/*
 * cursor_next_tuple () -
 *   return: DB_CURSOR_SUCCESS, DB_CURSOR_END, error_code
 *   cursor_id(in/out): Cursor Identifier
 * Note: Makes the next tuple in the LIST FILE referred by the cursor
 *       identifier the current active tuple of the cursor and returns
 *       DB_CURSOR_SUCCESS.
 *
 * Note: if end_of_scan: DB_CURSOR_END, otherwise an error code is returned.
 */
int
cursor_next_tuple (CURSOR_ID * cursor_id_p)
{
  if (cursor_id_p == NULL || cursor_id_p->query_id == NULL_QUERY_ID)
    {
      assert (0);
      return DB_CURSOR_ERROR;
    }

  cursor_initialize_current_tuple_value_position (cursor_id_p);

  if (cursor_id_p->position == C_BEFORE)
    {
      if (VPID_ISNULL (&(cursor_id_p->list_id.first_vpid)))
        {
          return DB_CURSOR_END;
        }

      if (cursor_fetch_page_having_tuple (cursor_id_p, &cursor_id_p->list_id.first_vpid, FIRST_TPL, 0) != NO_ERROR)
        {
          return DB_CURSOR_ERROR;
        }

      QFILE_COPY_VPID (&cursor_id_p->current_vpid, &cursor_id_p->list_id.first_vpid);
      /*
       * Setup the cursor so that we can proceed through the next "if"
       * statement w/o code duplication.
       */
      cursor_id_p->position = C_ON;
      cursor_id_p->tuple_no = -1;
      cursor_id_p->current_tuple_no = -1;
      cursor_id_p->current_tuple_length = 0;
    }

  if (cursor_id_p->position == C_ON)
    {
      VPID next_vpid;

      if (cursor_id_p->current_tuple_no < cursor_id_p->buffer_tuple_count - 1)
        {
          cursor_id_p->tuple_no++;
          cursor_id_p->current_tuple_no++;
          cursor_id_p->current_tuple_offset += cursor_id_p->current_tuple_length;
          cursor_id_p->current_tuple_p += cursor_id_p->current_tuple_length;
          cursor_id_p->current_tuple_length = QFILE_GET_TUPLE_LENGTH (cursor_id_p->current_tuple_p);
        }
      else if (QFILE_GET_NEXT_PAGE_ID (cursor_id_p->buffer) != NULL_PAGEID)
        {
          QFILE_GET_NEXT_VPID (&next_vpid, cursor_id_p->buffer);
          if (cursor_fetch_page_having_tuple (cursor_id_p, &next_vpid, FIRST_TPL, 0) != NO_ERROR)
            {
              return DB_CURSOR_ERROR;
            }
          QFILE_COPY_VPID (&cursor_id_p->current_vpid, &next_vpid);
          cursor_id_p->tuple_no++;
        }
      else
        {
          cursor_id_p->position = C_AFTER;
          cursor_id_p->tuple_no = cursor_id_p->list_id.tuple_cnt;
          return DB_CURSOR_END;
        }
    }
  else if (cursor_id_p->position == C_AFTER)
    {
      return DB_CURSOR_END;
    }
  else
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_QPROC_UNKNOWN_CRSPOS, 0);
      return ER_QPROC_UNKNOWN_CRSPOS;
    }

  return DB_CURSOR_SUCCESS;
}

/*
 * cursor_prev_tuple () -
 *   return: DB_CURSOR_SUCCESS, DB_CURSOR_END, error_code
 *   cursor_id(in/out): Cursor Identifier
 * Note: Makes the previous tuple in the LIST FILE referred by cursor
 *       identifier the current active tuple of the cursor and returns
 *       DB_CURSOR_SUCCESS.
 *
 * Note: if end_of_scan: DB_CURSOR_END, otherwise an error code is returned.
 */
int
cursor_prev_tuple (CURSOR_ID * cursor_id_p)
{
  if (cursor_id_p == NULL)
    {
      assert (0);
      return DB_CURSOR_ERROR;
    }

  cursor_initialize_current_tuple_value_position (cursor_id_p);

  if (cursor_id_p->position == C_BEFORE)
    {
      return DB_CURSOR_END;
    }
  else if (cursor_id_p->position == C_ON)
    {
      VPID prev_vpid;

      if (cursor_id_p->current_tuple_no > 0)
        {
          cursor_id_p->tuple_no--;
          cursor_id_p->current_tuple_no--;
          cursor_id_p->current_tuple_offset -= QFILE_GET_PREV_TUPLE_LENGTH (cursor_id_p->current_tuple_p);
          cursor_id_p->current_tuple_p -= QFILE_GET_PREV_TUPLE_LENGTH (cursor_id_p->current_tuple_p);
          cursor_id_p->current_tuple_length = QFILE_GET_TUPLE_LENGTH (cursor_id_p->current_tuple_p);
        }
      else if (QFILE_GET_PREV_PAGE_ID (cursor_id_p->buffer) != NULL_PAGEID)
        {
          QFILE_GET_PREV_VPID (&prev_vpid, cursor_id_p->buffer);

          if (cursor_fetch_page_having_tuple (cursor_id_p, &prev_vpid, LAST_TPL, 0) != NO_ERROR)
            {
              return DB_CURSOR_ERROR;
            }

          QFILE_COPY_VPID (&cursor_id_p->current_vpid, &prev_vpid);
          cursor_id_p->tuple_no--;
        }
      else
        {
          cursor_id_p->position = C_BEFORE;
          cursor_id_p->tuple_no = -1;
          return DB_CURSOR_END;
        }
    }
  else if (cursor_id_p->position == C_AFTER)
    {
      if (VPID_ISNULL (&(cursor_id_p->list_id.first_vpid)))
        {
          return DB_CURSOR_END;
        }

      if (cursor_fetch_page_having_tuple (cursor_id_p, &cursor_id_p->list_id.last_vpid, LAST_TPL, 0) != NO_ERROR)
        {
          return DB_CURSOR_ERROR;
        }

      QFILE_COPY_VPID (&cursor_id_p->current_vpid, &cursor_id_p->list_id.last_vpid);
      cursor_id_p->position = C_ON;
      cursor_id_p->tuple_no--;
    }
  else
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_QPROC_UNKNOWN_CRSPOS, 0);
      return ER_QPROC_UNKNOWN_CRSPOS;
    }

  return DB_CURSOR_SUCCESS;
}

/*
 * cursor_first_tuple () -
 *
 * arguments:
 *   return: DB_CURSOR_SUCCESS, DB_CURSOR_END, error_code
 *   cursor_id(in/out): Cursor Identifier
 * Note: Makes the first tuple in the LIST FILE referred by the cursor
 *       identifier the current active tuple of the cursor and returns
 *       DB_CURSOR_SUCCESS. If there are no tuples in the list file,
 *       end_of_scan condition is reached.
 *
 * Note: if end_of_scan: DB_CURSOR_END, otherwise an error code is returned.
 */
int
cursor_first_tuple (CURSOR_ID * cursor_id_p)
{
  if (cursor_id_p == NULL)
    {
      assert (0);
      return DB_CURSOR_ERROR;
    }

  if (VPID_ISNULL (&(cursor_id_p->list_id.first_vpid)))
    {
      return DB_CURSOR_END;
    }

  if (cursor_fetch_page_having_tuple (cursor_id_p, &cursor_id_p->list_id.first_vpid, FIRST_TPL, 0) != NO_ERROR)
    {
      return DB_CURSOR_ERROR;
    }

  QFILE_COPY_VPID (&cursor_id_p->current_vpid, &cursor_id_p->list_id.first_vpid);
  cursor_id_p->position = C_ON;
  cursor_id_p->tuple_no = 0;

  if (cursor_id_p->buffer_tuple_count == 0)
    {
      cursor_id_p->position = C_AFTER;
      cursor_id_p->tuple_no = cursor_id_p->list_id.tuple_cnt;
      return DB_CURSOR_END;
    }

  return DB_CURSOR_SUCCESS;
}

/*
 * cursor_last_tuple () -
 *   return: DB_CURSOR_SUCCESS, DB_CURSOR_END, error_code
 *   cursor_id(in/out): Cursor Identifier
 * Note: Makes the last tuple in the LIST FILE referred by the cursor
 *       identifier the current active tuple of the cursor and returns
 *       DB_CURSOR_SUCCESS. If there are no tuples in the list file,
 *       end_of_scan condition is reached.
 *
 * Note: if end_of_scan: DB_CURSOR_END, otherwise an error code is returned.
 */
int
cursor_last_tuple (CURSOR_ID * cursor_id_p)
{
  if (cursor_id_p == NULL)
    {
      assert (0);
      return DB_CURSOR_ERROR;
    }

  if (VPID_ISNULL (&(cursor_id_p->list_id.first_vpid)))
    {
      return DB_CURSOR_END;
    }

  if (cursor_fetch_page_having_tuple (cursor_id_p, &cursor_id_p->list_id.last_vpid, LAST_TPL, 0) != NO_ERROR)
    {
      return DB_CURSOR_ERROR;
    }

  QFILE_COPY_VPID (&cursor_id_p->current_vpid, &cursor_id_p->list_id.last_vpid);
  cursor_id_p->position = C_ON;
  cursor_id_p->tuple_no = cursor_id_p->list_id.tuple_cnt - 1;

  return DB_CURSOR_SUCCESS;
}

/*
 * cursor_get_tuple_value () -
 *   return: NO_ERROR on all ok, ER status( or ER_FAILED) otherwise
 *   c_id(in): Cursor Identifier
 *   index(in):
 *   value(in/out):
 * Note: The data value of the current cursor tuple at the position
 *       pecified is fetched. If the position specified by index is
 *       not a valid position number, or if the cursor is not
 *       currently pointing to a tuple, then necessary error codes are
 *       returned.
 */
int
cursor_get_tuple_value (CURSOR_ID * cursor_id_p, int index, DB_VALUE * value_p)
{
  char *tuple_p;

  if (cursor_id_p == NULL)
    {
      assert (0);
      return ER_FAILED;
    }

  if (index < 0 || index >= cursor_id_p->list_id.type_list.type_cnt)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_QPROC_INVALID_TPLVAL_INDEX, 1, index);
      return ER_FAILED;
    }

  tuple_p = cursor_peek_tuple (cursor_id_p);
  if (tuple_p == NULL)
    {
      return ER_FAILED;
    }

  return cursor_get_tuple_value_from_list (cursor_id_p, index, value_p, tuple_p);
}

/*
 * cursor_get_tuple_value_list () -
 *   return: NO_ERROR on all ok, ER status( or ER_FAILED) otherwise
 *   cursor_id(in): Cursor Identifier
 *   size(in): Number of values in the value list
 *   value_list(in/out): Set to the values fetched from the current tuple
 * Note: The data values of the current cursor tuple are fetched
 *       and put the value_list in their original order. The size
 *       parameter must be equal to the number of values in the tuple
 *       and the caller should allocate necessary space for the value
 *       list. If the cursor is not currently pointing to tuple, an
 *       error code is returned.
 */
int
cursor_get_tuple_value_list (CURSOR_ID * cursor_id_p, int size, DB_VALUE * value_list_p)
{
  DB_VALUE *value_p;
  int index;

  if (cursor_id_p == NULL)
    {
      assert (0);
      return ER_FAILED;
    }

  index = 0;
  value_p = value_list_p;

  while (index < size)
    {
      if (cursor_get_tuple_value (cursor_id_p, index, value_p) != NO_ERROR)
        {
          return ER_FAILED;
        }

      index++;
      value_p++;
    }

  return NO_ERROR;
}
