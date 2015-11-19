/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2015 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License version 2.1 as published by the Free Software Foundation.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

/* TS is an acronym for "Turbo Selector". */

#include "grn_ts.h"

#include "grn_output.h"
#include "grn_str.h"

#include "ts/ts_buf.h"
#include "ts/ts_expr.h"
#include "ts/ts_log.h"
#include "ts/ts_str.h"
#include "ts/ts_types.h"
#include "ts/ts_util.h"

#include <string.h>

/*-------------------------------------------------------------
 * Miscellaneous.
 */

enum { GRN_TS_BATCH_SIZE = 1024 };

/* grn_ts_bool_output() outputs a value. */
static grn_rc
grn_ts_bool_output(grn_ctx *ctx, grn_ts_bool value)
{
  if (value) {
    return grn_bulk_write(ctx, ctx->impl->outbuf, "true", 4);
  } else {
    return grn_bulk_write(ctx, ctx->impl->outbuf, "false", 5);
  }
}

/* grn_ts_int_output() outputs a value. */
static grn_rc
grn_ts_int_output(grn_ctx *ctx, grn_ts_int value)
{
  return grn_text_lltoa(ctx, ctx->impl->outbuf, value);
}

/* grn_ts_float_output() outputs a value. */
static grn_rc
grn_ts_float_output(grn_ctx *ctx, grn_ts_float value)
{
  return grn_text_ftoa(ctx, ctx->impl->outbuf, value);
}

/* grn_ts_time_output() outputs a value. */
static grn_rc
grn_ts_time_output(grn_ctx *ctx, grn_ts_time value)
{
  return grn_text_ftoa(ctx, ctx->impl->outbuf, value * 0.000001);
}

/* grn_ts_text_output() outputs a value. */
static grn_rc
grn_ts_text_output(grn_ctx *ctx, grn_ts_text value)
{
  return grn_text_esc(ctx, ctx->impl->outbuf, value.ptr, value.size);
}

/* grn_ts_geo_output() outputs a value. */
static grn_rc
grn_ts_geo_output(grn_ctx *ctx, grn_ts_geo value)
{
  grn_rc rc = grn_bulk_write(ctx, ctx->impl->outbuf, "\"", 1);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  rc = grn_text_itoa(ctx, ctx->impl->outbuf, value.latitude);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  rc = grn_bulk_write(ctx, ctx->impl->outbuf, "x", 1);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  rc = grn_text_itoa(ctx, ctx->impl->outbuf, value.longitude);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  return grn_bulk_write(ctx, ctx->impl->outbuf, "\"", 1);
}

#define GRN_TS_VECTOR_OUTPUT(kind)\
  size_t i;\
  grn_rc rc = grn_bulk_write(ctx, ctx->impl->outbuf, "[", 1);\
  if (rc != GRN_SUCCESS) {\
    return rc;\
  }\
  for (i = 0; i < value.size; ++i) {\
    if (i) {\
      rc = grn_bulk_write(ctx, ctx->impl->outbuf, ",", 1);\
      if (rc != GRN_SUCCESS) {\
        return rc;\
      }\
    }\
    rc = grn_ts_ ## kind ## _output(ctx, value.ptr[i]);\
    if (rc != GRN_SUCCESS) {\
      return rc;\
    }\
  }\
  return grn_bulk_write(ctx, ctx->impl->outbuf, "]", 1);
/* grn_ts_bool_vector_output() outputs a value. */
static grn_rc
grn_ts_bool_vector_output(grn_ctx *ctx, grn_ts_bool_vector value)
{
  GRN_TS_VECTOR_OUTPUT(bool)
}

/* grn_ts_int_vector_output() outputs a value. */
static grn_rc
grn_ts_int_vector_output(grn_ctx *ctx, grn_ts_int_vector value)
{
  GRN_TS_VECTOR_OUTPUT(int)
}

/* grn_ts_float_vector_output() outputs a value. */
static grn_rc
grn_ts_float_vector_output(grn_ctx *ctx, grn_ts_float_vector value)
{
  GRN_TS_VECTOR_OUTPUT(float)
}

/* grn_ts_time_vector_output() outputs a value. */
static grn_rc
grn_ts_time_vector_output(grn_ctx *ctx, grn_ts_time_vector value)
{
  GRN_TS_VECTOR_OUTPUT(time)
}

/* grn_ts_text_vector_output() outputs a value. */
static grn_rc
grn_ts_text_vector_output(grn_ctx *ctx, grn_ts_text_vector value)
{
  GRN_TS_VECTOR_OUTPUT(text)
}

/* grn_ts_geo_vector_output() outputs a value. */
static grn_rc
grn_ts_geo_vector_output(grn_ctx *ctx, grn_ts_geo_vector value)
{
  GRN_TS_VECTOR_OUTPUT(geo)
}
#undef GRN_TS_VECTOR_OUTPUT

/*-------------------------------------------------------------
 * grn_ts_writer.
 */

typedef struct {
  grn_ts_expr **exprs;
  size_t n_exprs;
  size_t max_n_exprs;
  grn_obj name_buf;
  grn_ts_str *names;
  grn_ts_buf *bufs;
} grn_ts_writer;

/* grn_ts_writer_init() initializes a writer. */
static void
grn_ts_writer_init(grn_ctx *ctx, grn_ts_writer *writer)
{
  memset(writer, 0, sizeof(*writer));
  writer->exprs = NULL;
  GRN_TEXT_INIT(&writer->name_buf, GRN_OBJ_VECTOR);
  writer->names = NULL;
  writer->bufs = NULL;
}

/* grn_ts_writer_fin() finalizes a writer. */
static void
grn_ts_writer_fin(grn_ctx *ctx, grn_ts_writer *writer)
{
  size_t i;
  if (writer->bufs) {
    for (i = 0; i < writer->n_exprs; i++) {
      grn_ts_buf_fin(ctx, &writer->bufs[i]);
    }
    GRN_FREE(writer->bufs);
  }
  if (writer->names) {
    GRN_FREE(writer->names);
  }
  GRN_OBJ_FIN(ctx, &writer->name_buf);
  if (writer->exprs) {
    for (i = 0; i < writer->n_exprs; i++) {
      grn_ts_expr_close(ctx, writer->exprs[i]);
    }
    GRN_FREE(writer->exprs);
  }
}

/*
 * grn_ts_writer_tokenize() extracts the first expression string.
 * If the input is empty, this function returns GRN_END_OF_DATA.
 */
static grn_rc
grn_ts_writer_tokenize(grn_ctx *ctx, grn_ts_writer *writer,
                       grn_ts_str in, grn_ts_str *token, grn_ts_str *rest)
{
  size_t i;
  char stack_top;
  grn_rc rc = GRN_SUCCESS;
  grn_ts_str str = in;
  grn_ts_buf stack;

  /* Find a non-empty token. */
  grn_ts_buf_init(ctx, &stack);
  for ( ; ; ) {
    str = grn_ts_str_trim_left(str);
    if (!str.size) {
      rc = GRN_END_OF_DATA;
      break;
    }
    for (i = 0; i < str.size; i++) {
      if (stack.pos) {
        if (str.ptr[i] == stack_top) {
          if (--stack.pos) {
            stack_top = ((char *)stack.ptr)[stack.pos - 1];
          }
          continue;
        }
        if (stack_top == '"') {
          /* Skip the next byte of an escape character. */
          if ((str.ptr[i] == '\\') && (i < (str.size - 1))) {
            i++;
          }
          continue;
        }
      } else if (str.ptr[i] == ',') {
        break;
      }
      switch (str.ptr[i]) {
        case '(': {
          stack_top = ')';
          rc = grn_ts_buf_write(ctx, &stack, &stack_top, 1);
          break;
        }
        case '[': {
          stack_top = ']';
          rc = grn_ts_buf_write(ctx, &stack, &stack_top, 1);
          break;
        }
        case '{': {
          stack_top = '}';
          rc = grn_ts_buf_write(ctx, &stack, &stack_top, 1);
          break;
        }
        case '"': {
          stack_top = '"';
          rc = grn_ts_buf_write(ctx, &stack, &stack_top, 1);
          break;
        }
      }
      if (rc != GRN_SUCCESS) {
        break;
      }
    }
    if (rc != GRN_SUCCESS) {
      break;
    }
    if (i) {
      /* Output the result. */
      token->ptr = str.ptr;
      token->size = i;
      if (token->size == str.size) {
        rest->ptr = str.ptr + str.size;
        rest->size = 0;
      } else {
        rest->ptr = str.ptr + token->size + 1;
        rest->size = str.size - token->size - 1;
      }
      break;
    }
    str.ptr++;
    str.size--;
  }
  grn_ts_buf_fin(ctx, &stack);
  return rc;
}

/* grn_ts_writer_expand() expands a wildcard. */
static grn_rc
grn_ts_writer_expand(grn_ctx *ctx, grn_ts_writer *writer,
                     grn_obj *table, grn_ts_str str)
{
  grn_rc rc = GRN_SUCCESS;
  grn_hash_cursor *cursor;
  grn_hash *hash = grn_hash_create(ctx, NULL, sizeof(grn_ts_id), 0,
                                   GRN_OBJ_TABLE_HASH_KEY | GRN_HASH_TINY);
  if (!hash) {
    return GRN_INVALID_ARGUMENT;
  }
  grn_table_columns(ctx, table, str.ptr, str.size - 1, (grn_obj *)hash);
  if (ctx->rc != GRN_SUCCESS) {
    return ctx->rc;
  }
  cursor = grn_hash_cursor_open(ctx, hash, NULL, 0, NULL, 0, 0, -1, 0);
  if (!cursor) {
    rc = GRN_INVALID_ARGUMENT;
  } else {
    while (grn_hash_cursor_next(ctx, cursor) != GRN_ID_NIL) {
      char name_buf[GRN_TABLE_MAX_KEY_SIZE];
      size_t name_size;
      grn_obj *column;
      grn_ts_id *column_id;
      if (!grn_hash_cursor_get_key(ctx, cursor, (void **)&column_id)) {
        rc = GRN_INVALID_ARGUMENT;
        break;
      }
      column = grn_ctx_at(ctx, *column_id);
      if (!column) {
        rc = GRN_INVALID_ARGUMENT;
        break;
      }
      name_size = grn_column_name(ctx, column, name_buf, sizeof(name_buf));
      grn_obj_unlink(ctx, column);
      rc = grn_vector_add_element(ctx, &writer->name_buf,
                                  name_buf, name_size, 0, GRN_DB_TEXT);
      if (rc != GRN_SUCCESS) {
        break;
      }
    }
    grn_hash_cursor_close(ctx, cursor);
  }
  grn_hash_close(ctx, hash);
  return rc;
}

/* grn_ts_writer_parse() parses output expressions. */
static grn_rc
grn_ts_writer_parse(grn_ctx *ctx, grn_ts_writer *writer,
                    grn_obj *table, grn_ts_str str)
{
  grn_rc rc;
  grn_ts_str rest = str;
  for ( ; ; ) {
    grn_ts_str token = { NULL, 0 };
    rc = grn_ts_writer_tokenize(ctx, writer, rest, &token, &rest);
    if (rc != GRN_SUCCESS) {
      return (rc == GRN_END_OF_DATA) ? GRN_SUCCESS : rc;
    }
    if ((token.ptr[token.size - 1] == '*') &&
        grn_ts_str_is_name_prefix((grn_ts_str){ token.ptr, token.size - 1 })) {
      rc = grn_ts_writer_expand(ctx, writer, table, token);
      if (rc != GRN_SUCCESS) {
        return rc;
      }
    } else if (grn_ts_str_is_key_name(token) &&
               !grn_ts_table_has_key(ctx, table)) {
      /*
       * Skip _key if the table has no _key, because the default output_columns
       * option contains _key.
       */
      GRN_TS_DEBUG("skip \"_key\" because the table has no _key");
    } else {
      rc = grn_vector_add_element(ctx, &writer->name_buf,
                                  token.ptr, token.size, 0, GRN_DB_TEXT);
      if (rc != GRN_SUCCESS) {
        return rc;
      }
    }
  }
  return GRN_SUCCESS;
}

/* grn_ts_writer_build() builds output expresions. */
static grn_rc
grn_ts_writer_build(grn_ctx *ctx, grn_ts_writer *writer, grn_obj *table)
{
  size_t i, n_names = grn_vector_size(ctx, &writer->name_buf);
  if (!n_names) {
    return GRN_SUCCESS;
  }
  writer->names = GRN_MALLOCN(grn_ts_str, n_names);
  if (!writer->names) {
    GRN_TS_ERR_RETURN(GRN_NO_MEMORY_AVAILABLE,
                      "GRN_MALLOCN failed: %" GRN_FMT_SIZE " x %" GRN_FMT_SIZE,
                      sizeof(grn_ts_str), n_names);
  }
  writer->exprs = GRN_MALLOCN(grn_ts_expr *, n_names);
  if (!writer->exprs) {
    GRN_TS_ERR_RETURN(GRN_NO_MEMORY_AVAILABLE,
                      "GRN_MALLOCN failed: %" GRN_FMT_SIZE " x %" GRN_FMT_SIZE,
                      sizeof(grn_ts_expr *), n_names);
  }
  for (i = 0; i < n_names; i++) {
    grn_rc rc;
    grn_ts_expr *new_expr;
    const char *name_ptr;
    size_t name_size = grn_vector_get_element(ctx, &writer->name_buf, i,
                                              &name_ptr, NULL, NULL);
    rc = grn_ts_expr_parse(ctx, table, (grn_ts_str){ name_ptr, name_size },
                           &new_expr);
    if (rc != GRN_SUCCESS) {
      return rc;
    }
    writer->names[i].ptr = name_ptr;
    writer->names[i].size = name_size;
    writer->exprs[i] = new_expr;
    writer->n_exprs++;
  }
  return GRN_SUCCESS;
}

/* grn_ts_writer_open() creates a writer. */
static grn_rc
grn_ts_writer_open(grn_ctx *ctx, grn_obj *table, grn_ts_str str,
                   grn_ts_writer **writer)
{
  grn_rc rc;
  grn_ts_writer *new_writer = GRN_MALLOCN(grn_ts_writer, 1);
  if (!new_writer) {
    GRN_TS_ERR_RETURN(GRN_NO_MEMORY_AVAILABLE,
                      "GRN_MALLOCN failed: %" GRN_FMT_SIZE " x 1",
                      sizeof(grn_ts_writer));
  }
  grn_ts_writer_init(ctx, new_writer);
  rc = grn_ts_writer_parse(ctx, new_writer, table, str);
  if (rc == GRN_SUCCESS) {
    rc = grn_ts_writer_build(ctx, new_writer, table);
  }
  if (rc != GRN_SUCCESS) {
    grn_ts_writer_fin(ctx, new_writer);
    GRN_FREE(new_writer);
    return rc;
  }
  *writer = new_writer;
  return GRN_SUCCESS;
}

/* grn_ts_writer_close() destroys a writer. */
static void
grn_ts_writer_close(grn_ctx *ctx, grn_ts_writer *writer)
{
  grn_ts_writer_fin(ctx, writer);
  GRN_FREE(writer);
}

/* TODO: Errors of output macros, such as GRN_TEXT_*(), are ignored. */

#define GRN_TS_WRITER_OUTPUT_HEADER_CASE(TYPE, name)\
  case GRN_DB_ ## TYPE: {\
    GRN_TEXT_PUTS(ctx, ctx->impl->outbuf, name);\
    break;\
  }
/* grn_ts_writer_output_header() outputs names and data types. */
static grn_rc
grn_ts_writer_output_header(grn_ctx *ctx, grn_ts_writer *writer)
{
  grn_rc rc;
  GRN_OUTPUT_ARRAY_OPEN("COLUMNS", writer->n_exprs);
  for (size_t i = 0; i < writer->n_exprs; ++i) {
    GRN_OUTPUT_ARRAY_OPEN("COLUMN", 2);
    rc = grn_text_esc(ctx, ctx->impl->outbuf,
                      writer->names[i].ptr, writer->names[i].size);
    if (rc != GRN_SUCCESS) {
      return rc;
    }
    GRN_TEXT_PUT(ctx, ctx->impl->outbuf, ",\"", 2);
    switch (writer->exprs[i]->data_type) {
      case GRN_DB_VOID: {
        if (writer->exprs[i]->data_kind == GRN_TS_GEO) {
          GRN_TEXT_PUTS(ctx, ctx->impl->outbuf, "GeoPoint");
        } else {
          GRN_TEXT_PUTS(ctx, ctx->impl->outbuf, "Void");
        }
        break;
      }
      GRN_TS_WRITER_OUTPUT_HEADER_CASE(BOOL, "Bool")
      GRN_TS_WRITER_OUTPUT_HEADER_CASE(INT8, "Int8")
      GRN_TS_WRITER_OUTPUT_HEADER_CASE(INT16, "Int16")
      GRN_TS_WRITER_OUTPUT_HEADER_CASE(INT32, "Int32")
      GRN_TS_WRITER_OUTPUT_HEADER_CASE(INT64, "Int64")
      GRN_TS_WRITER_OUTPUT_HEADER_CASE(UINT8, "UInt8")
      GRN_TS_WRITER_OUTPUT_HEADER_CASE(UINT16, "UInt16")
      GRN_TS_WRITER_OUTPUT_HEADER_CASE(UINT32, "UInt32")
      GRN_TS_WRITER_OUTPUT_HEADER_CASE(UINT64, "UInt64")
      GRN_TS_WRITER_OUTPUT_HEADER_CASE(FLOAT, "Float")
      GRN_TS_WRITER_OUTPUT_HEADER_CASE(TIME, "Time")
      GRN_TS_WRITER_OUTPUT_HEADER_CASE(SHORT_TEXT, "ShortText")
      GRN_TS_WRITER_OUTPUT_HEADER_CASE(TEXT, "Text")
      GRN_TS_WRITER_OUTPUT_HEADER_CASE(LONG_TEXT, "LongText")
      GRN_TS_WRITER_OUTPUT_HEADER_CASE(TOKYO_GEO_POINT, "TokyoGeoPoint")
      GRN_TS_WRITER_OUTPUT_HEADER_CASE(WGS84_GEO_POINT, "WGS84GeoPoint")
      default: {
        char name_buf[GRN_TABLE_MAX_KEY_SIZE];
        size_t name_size;
        grn_obj *obj = grn_ctx_at(ctx, writer->exprs[i]->data_type);
        if (!obj) {
          GRN_TS_ERR_RETURN(GRN_UNKNOWN_ERROR, "grn_ctx_at failed: %d",
                            writer->exprs[i]->data_type);
        }
        if (!grn_ts_obj_is_table(ctx, obj)) {
          grn_obj_unlink(ctx, obj);
          GRN_TS_ERR_RETURN(GRN_UNKNOWN_ERROR, "not table: %d",
                            writer->exprs[i]->data_type);
        }
        name_size = grn_obj_name(ctx, obj, name_buf, sizeof(name_buf));
        GRN_TEXT_PUT(ctx, ctx->impl->outbuf, name_buf, name_size);
        grn_obj_unlink(ctx, obj);
        break;
      }
    }
    GRN_TEXT_PUTC(ctx, ctx->impl->outbuf, '"');
    GRN_OUTPUT_ARRAY_CLOSE();
  }
  GRN_OUTPUT_ARRAY_CLOSE(); /* COLUMNS. */
  return GRN_SUCCESS;
}
#undef GRN_TS_WRITER_OUTPUT_HEADER_CASE

#define GRN_TS_WRITER_OUTPUT_BODY_CASE(KIND, kind)\
  case GRN_TS_ ## KIND: {\
    grn_ts_ ## kind *value = (grn_ts_ ## kind *)writer->bufs[j].ptr;\
    grn_ts_ ## kind ## _output(ctx, value[i]);\
    break;\
  }
#define GRN_TS_WRITER_OUTPUT_BODY_VECTOR_CASE(KIND, kind)\
  GRN_TS_WRITER_OUTPUT_BODY_CASE(KIND ## _VECTOR, kind ## _vector)
/*
 * grn_ts_writer_output_body() evaluates expressions and outputs the results.
 */
static grn_rc
grn_ts_writer_output_body(grn_ctx *ctx, grn_ts_writer *writer,
                          const grn_ts_record *in, size_t n_in)
{
  size_t i, j, count = 0;
  writer->bufs = GRN_MALLOCN(grn_ts_buf, writer->n_exprs);
  if (!writer->bufs) {
    GRN_TS_ERR_RETURN(GRN_NO_MEMORY_AVAILABLE,
                      "GRN_MALLOCN failed: %" GRN_FMT_SIZE " x %" GRN_FMT_SIZE,
                      sizeof(grn_ts_buf), writer->n_exprs);
  }
  for (i = 0; i < writer->n_exprs; i++) {
    grn_ts_buf_init(ctx, &writer->bufs[i]);
  }
  while (count < n_in) {
    size_t batch_size = GRN_TS_BATCH_SIZE;
    if (batch_size > (n_in - count)) {
      batch_size = n_in - count;
    }
    for (i = 0; i < writer->n_exprs; ++i) {
      grn_rc rc = grn_ts_expr_evaluate_to_buf(ctx, writer->exprs[i], in + count,
                                              batch_size, &writer->bufs[i]);
      if (rc != GRN_SUCCESS) {
        return rc;
      }
    }
    for (i = 0; i < batch_size; ++i) {
      GRN_OUTPUT_ARRAY_OPEN("HIT", writer->n_exprs);
      for (j = 0; j < writer->n_exprs; ++j) {
        if (j) {
          GRN_TEXT_PUTC(ctx, ctx->impl->outbuf, ',');
        }
        switch (writer->exprs[j]->data_kind) {
          GRN_TS_WRITER_OUTPUT_BODY_CASE(BOOL, bool);
          GRN_TS_WRITER_OUTPUT_BODY_CASE(INT, int);
          GRN_TS_WRITER_OUTPUT_BODY_CASE(FLOAT, float);
          GRN_TS_WRITER_OUTPUT_BODY_CASE(TIME, time);
          GRN_TS_WRITER_OUTPUT_BODY_CASE(TEXT, text);
          GRN_TS_WRITER_OUTPUT_BODY_CASE(GEO, geo);
          GRN_TS_WRITER_OUTPUT_BODY_VECTOR_CASE(BOOL, bool);
          GRN_TS_WRITER_OUTPUT_BODY_VECTOR_CASE(INT, int);
          GRN_TS_WRITER_OUTPUT_BODY_VECTOR_CASE(FLOAT, float);
          GRN_TS_WRITER_OUTPUT_BODY_VECTOR_CASE(TIME, time);
          GRN_TS_WRITER_OUTPUT_BODY_VECTOR_CASE(TEXT, text);
          GRN_TS_WRITER_OUTPUT_BODY_VECTOR_CASE(GEO, geo);
          default: {
            break;
          }
        }
      }
      GRN_OUTPUT_ARRAY_CLOSE(); /* HITS. */
    }
    count += batch_size;
  }
  return GRN_SUCCESS;
}
#undef GRN_TS_WRITER_OUTPUT_BODY_VECTOR_CASE
#undef GRN_TS_WRITER_OUTPUT_BODY_CASE

/* grn_ts_writer_output() outputs search results into the output buffer. */
static grn_rc
grn_ts_writer_output(grn_ctx *ctx, grn_ts_writer *writer,
                     const grn_ts_record *in, size_t n_in, size_t n_hits)
{
  grn_rc rc;
  GRN_OUTPUT_ARRAY_OPEN("RESULT", 1);
  GRN_OUTPUT_ARRAY_OPEN("RESULTSET", 2 + n_in);
  GRN_OUTPUT_ARRAY_OPEN("NHITS", 1);
  rc = grn_text_ulltoa(ctx, ctx->impl->outbuf, n_hits);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  GRN_OUTPUT_ARRAY_CLOSE(); /* NHITS. */
  rc = grn_ts_writer_output_header(ctx, writer);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  rc = grn_ts_writer_output_body(ctx, writer, in, n_in);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  GRN_OUTPUT_ARRAY_CLOSE(); /* RESULTSET. */
  GRN_OUTPUT_ARRAY_CLOSE(); /* RESET. */
  return GRN_SUCCESS;
}

/* grn_ts_select_filter() applies a filter to all the records of a table. */
static grn_rc
grn_ts_select_filter(grn_ctx *ctx, grn_obj *table, grn_ts_str str,
                     size_t offset, size_t limit,
                     grn_ts_record **out, size_t *n_out, size_t *n_hits)
{
  grn_rc rc;
  grn_table_cursor *cursor;
  grn_ts_expr *expr;
  grn_ts_record *buf = NULL;
  size_t buf_size = 0;

  *out = NULL;
  *n_out = 0;
  *n_hits = 0;

  cursor = grn_table_cursor_open(ctx, table, NULL, 0, NULL, 0, 0, -1,
                                 GRN_CURSOR_ASCENDING | GRN_CURSOR_BY_ID);
  if (!cursor) {
    return (ctx->rc != GRN_SUCCESS) ? ctx->rc : GRN_UNKNOWN_ERROR;
  }

  rc = grn_ts_expr_parse(ctx, table, str, &expr);
  if (rc == GRN_SUCCESS) {
    for ( ; ; ) {
      size_t i, batch_size;
      grn_ts_record *batch;

      /* Extend the record buffer. */
      if (buf_size < (*n_out + GRN_TS_BATCH_SIZE)) {
        size_t new_size = buf_size ? (buf_size * 2) : GRN_TS_BATCH_SIZE;
        size_t n_bytes = sizeof(grn_ts_record) * new_size;
        grn_ts_record *new_buf = (grn_ts_record *)GRN_REALLOC(buf, n_bytes);
        if (!new_buf) {
          GRN_TS_ERR(GRN_NO_MEMORY_AVAILABLE,
                     "GRN_REALLOC failed: %" GRN_FMT_SIZE,
                     n_bytes);
          rc = ctx->rc;
          break;
        }
        buf = new_buf;
        buf_size = new_size;
      }

      /* Read records from the cursor. */
      batch = buf + *n_out;
      for (i = 0; i < GRN_TS_BATCH_SIZE; i++) {
        batch[i].id = grn_table_cursor_next(ctx, cursor);
        if (batch[i].id == GRN_ID_NIL) {
          break;
        }
        batch[i].score = 0.0;
      }
      batch_size = i;
      if (!batch_size) {
        break;
      }

      /* Apply the filter. */
      rc = grn_ts_expr_filter(ctx, expr, batch, batch_size,
                              batch, &batch_size);
      if (rc != GRN_SUCCESS) {
        break;
      }
      *n_hits += batch_size;

      /* Apply the offset and the limit. */
      if (offset) {
        if (batch_size <= offset) {
          offset -= batch_size;
          batch_size = 0;
        } else {
          size_t n_bytes = sizeof(grn_ts_record) * (batch_size - offset);
          grn_memmove(batch, batch + offset, n_bytes);
          batch_size -= offset;
          offset = 0;
        }
      }
      if (batch_size <= limit) {
        limit -= batch_size;
      } else {
        batch_size = limit;
        limit = 0;
      }
      *n_out += batch_size;
    }
    /* Ignore a failure of destruction. */
    grn_ts_expr_close(ctx, expr);
  }
  /* Ignore a failure of  destruction. */
  grn_table_cursor_close(ctx, cursor);

  if (rc != GRN_SUCCESS) {
    if (buf) {
      GRN_FREE(buf);
    }
    *n_out = 0;
    *n_hits = 0;
    return rc;
  }
  *out = buf;
  return GRN_SUCCESS;
}

/* grn_ts_select_scorer() adjust scores. */
static grn_rc
grn_ts_select_scorer(grn_ctx *ctx, grn_obj *table, grn_ts_str str,
                     grn_ts_record *records, size_t n_records)
{
  grn_rc rc;
  grn_ts_str rest;
  grn_ts_expr *expr;
  rest = grn_ts_str_trim_left(str);
  if (!rest.size) {
    return GRN_SUCCESS;
  }
  /* If a string starts with "_score =", skip it. */
  if (grn_ts_str_starts_with(str, (grn_ts_str){ "_score", 6 })) {
    rest = grn_ts_str_trim_left((grn_ts_str){ rest.ptr + 6, rest.size - 6 });
    if (!rest.size) {
      return GRN_SUCCESS;
    }
    if ((rest.size >= 2) && (rest.ptr[0] == '=') && (rest.ptr[1] != '=')) {
      /* Use the rest of a string. */
      rest.ptr += 1;
      rest.size -= 1;
    } else {
      /* Use the whole string. */
      rest = str;
    }
  }
  rc = grn_ts_expr_parse(ctx, table, rest, &expr);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  rc = grn_ts_expr_adjust(ctx, expr, records, n_records);
  grn_ts_expr_close(ctx, expr);
  return rc;
}

/* grn_ts_select_output() outputs the results. */
static grn_rc
grn_ts_select_output(grn_ctx *ctx, grn_obj *table, grn_ts_str str,
                     const grn_ts_record *in, size_t n_in, size_t n_hits)
{
  grn_ts_writer *writer;
  grn_rc rc = grn_ts_writer_open(ctx, table, str, &writer);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  rc = grn_ts_writer_output(ctx, writer, in, n_in, n_hits);
  grn_ts_writer_close(ctx, writer);
  return rc;
}

/*-------------------------------------------------------------
 * API.
 */

grn_rc
grn_ts_select(grn_ctx *ctx, grn_obj *table,
              const char *filter_ptr, size_t filter_len,
              const char *scorer_ptr, size_t scorer_len,
              const char *output_columns_ptr, size_t output_columns_len,
              size_t offset, size_t limit)
{
  grn_rc rc;
  grn_ts_str filter = { filter_ptr, filter_len };
  grn_ts_str scorer = { scorer_ptr, scorer_len };
  grn_ts_str output_columns = { output_columns_ptr, output_columns_len };
  grn_ts_record *records = NULL;
  size_t n_records, n_hits;
  if (!ctx || !table || !grn_ts_obj_is_table(ctx, table) ||
      (!filter_ptr && filter_len) || (!scorer_ptr && scorer_len) ||
      (!output_columns_ptr && output_columns_len)) {
    return GRN_INVALID_ARGUMENT;
  }
  rc = grn_ts_select_filter(ctx, table, filter, offset, limit,
                            &records, &n_records, &n_hits);
  if (rc == GRN_SUCCESS) {
    rc = grn_ts_select_scorer(ctx, table, scorer, records, n_records);
    if (rc == GRN_SUCCESS) {
      rc = grn_ts_select_output(ctx, table, output_columns,
                                records, n_records, n_hits);
    }
  }
  if (records) {
    GRN_FREE(records);
  }
  if (rc != GRN_SUCCESS) {
    GRN_BULK_REWIND(ctx->impl->outbuf);
    if ((ctx->rc == GRN_SUCCESS) || !ctx->errbuf[0]) {
      ERR(rc, "error message is missing");
    } else if (ctx->errlvl < GRN_LOG_ERROR) {
      ctx->errlvl = GRN_LOG_ERROR;
    }
  }
  return rc;
}
