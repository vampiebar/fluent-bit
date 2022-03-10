/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Fluent Bit
 *  ==========
 *  Copyright (C) 2015-2022 The Fluent Bit Authors
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <fluent-bit/flb_input_plugin.h>
#include <fluent-bit/flb_version.h>
#include <fluent-bit/flb_error.h>
#include <fluent-bit/flb_pack.h>

#include <monkey/monkey.h>
#include <monkey/mk_core.h>
#include <cmetrics/cmt_decode_opentelemetry.h>

#include "opentelemetry.h"
#include "http_conn.h"

#define HTTP_CONTENT_JSON  0

static int send_response(struct http_conn *conn, int http_status, char *message)
{
    int len;
    flb_sds_t out;

    out = flb_sds_create_size(256);
    if (!out) {
        return -1;
    }

    if (message) {
        len = strlen(message);
    }
    else {
        len = 0;
    }

    if (http_status == 201) {
        flb_sds_printf(&out,
                       "HTTP/1.1 201 Created \r\n"
                       "Server: Fluent Bit v%s\r\n"
                       "Content-Length: 0\r\n\r\n",
                       FLB_VERSION_STR);
    }
    else if (http_status == 200) {
        flb_sds_printf(&out,
                       "HTTP/1.1 200 OK\r\n"
                       "Server: Fluent Bit v%s\r\n"
                       "Content-Length: 0\r\n\r\n",
                       FLB_VERSION_STR);
    }
    else if (http_status == 204) {
        flb_sds_printf(&out,
                       "HTTP/1.1 204 No Content\r\n"
                       "Server: Fluent Bit v%s\r\n"
                       "Content-Length: 0\r\n\r\n",
                       FLB_VERSION_STR);
    }
    else if (http_status == 400) {
        flb_sds_printf(&out,
                       "HTTP/1.1 400 Forbidden\r\n"
                       "Server: Fluent Bit v%s\r\n"
                       "Content-Length: %i\r\n\r\n%s",
                       FLB_VERSION_STR,
                       len, message);
    }

    write(conn->fd, out, flb_sds_len(out));
    flb_sds_destroy(out);
    return 0;
}


static int process_payload(struct flb_opentelemetry *ctx, struct http_conn *conn,
                           flb_sds_t tag,
                           struct mk_http_session *session,
                           struct mk_http_request *request)
{
    struct mk_list        *header_iterator;
    struct                *decoded_context;
    size_t                 payload_length;
    unsigned char         *payload_buffer;
    int                    gzip_payload;
    struct mk_http_header *header;
    size_t                 offset;
    int                    result;

    gzip_payload = FLB_FALSE;

    mk_list_foreach(header_iterator, &request->header_list) {
        header = mk_list_entry(head, struct mk_http_header, _head);

        if (strcasecmp(header->key.data, "Content-Encoding") == 0) {
            if (strcasecmp(header->val.data, "gzip") == 0) {
                gzip_payload = FLB_TRUE;
            }

            break;
        }
    }

    if (gzip_payload) {
        result = flb_gzip_uncompress(request->data.data, request->data.len,
                                     &payload_buffer, &payload_length);

        if (result) {
            send_response(conn, 400, "error: decompression error\n");
            return -1;
        }
    }
    else {
        payload_buffer = request->data.data;
        payload_length = request->data.len;
    }

    offset = 0;

    result = cmt_decode_opentelemetry_create(&decoded_context,
                                             payload_buffer,
                                             payload_length,
                                             &offset);

    if (result == CMT_DECODE_OPENTELEMETRY_SUCCESS) {
        result = flb_input_metrics_append(ctx->ins, NULL, 0, decoded_context);

        cmt_decode_opentelemetry_destroy(decoded_context);
    }

    if (gzip_payload) {
        free(payload_buffer);
    }

    return 0;
}

static inline int mk_http_point_header(mk_ptr_t *h,
                                       struct mk_http_parser *parser, int key)
{
    struct mk_http_header *header;

    header = &parser->headers[key];
    if (header->type == key) {
        h->data = header->val.data;
        h->len  = header->val.len;
        return 0;
    }
    else {
        h->data = NULL;
        h->len  = -1;
    }

    return -1;
}

/*
 * Handle an incoming request. It perform extra checks over the request, if
 * everything is OK, it enqueue the incoming payload.
 */
int opentelemetry_prot_handle(struct flb_opentelemetry *ctx, struct http_conn *conn,
                              struct mk_http_session *session,
                              struct mk_http_request *request)
{
    int i;
    int ret;
    int len;
    char *uri;
    char *qs;
    off_t diff;
    flb_sds_t tag;
    struct mk_http_header *header;

    if (request->uri.data[0] != '/') {
        send_response(conn, 400, "error: invalid request\n");
        return -1;
    }

    /* Decode URI */
    uri = mk_utils_url_decode(request->uri);
    if (!uri) {
        uri = mk_mem_alloc_z(request->uri.len + 1);
        if (!uri) {
            return -1;
        }
        memcpy(uri, request->uri.data, request->uri.len);
        uri[request->uri.len] = '\0';
    }

    /* Try to match a query string so we can remove it */
    qs = strchr(uri, '?');
    if (qs) {
        /* remove the query string part */
        diff = qs - uri;
        uri[diff] = '\0';
    }

    /* Compose the query string using the URI */
    len = strlen(uri);

    if (len == 1) {
        tag = NULL; /* use default tag */
    }
    else {
        tag = flb_sds_create_size(len);
        if (!tag) {
            mk_mem_free(uri);
            return -1;
        }

        /* New tag skipping the URI '/' */
        flb_sds_cat(tag, uri + 1, len - 1);

        /* Sanitize, only allow alphanum chars */
        for (i = 0; i < flb_sds_len(tag); i++) {
            if (!isalnum(tag[i]) && tag[i] != '_' && tag[i] != '.') {
                tag[i] = '_';
            }
        }
    }

    mk_mem_free(uri);

    /* Check if we have a Host header: Hostname ; port */
    mk_http_point_header(&request->host, &session->parser, MK_HEADER_HOST);

    /* Header: Connection */
    mk_http_point_header(&request->connection, &session->parser,
                         MK_HEADER_CONNECTION);

    /* HTTP/1.1 needs Host header */
    if (!request->host.data && request->protocol == MK_HTTP_PROTOCOL_11) {
        flb_sds_destroy(tag);
        return -1;
    }

    /* Should we close the session after this request ? */
    mk_http_keepalive_check(session, request, ctx->server);

    /* Content Length */
    header = &session->parser.headers[MK_HEADER_CONTENT_LENGTH];
    if (header->type == MK_HEADER_CONTENT_LENGTH) {
        request->_content_length.data = header->val.data;
        request->_content_length.len  = header->val.len;
    }
    else {
        request->_content_length.data = NULL;
    }

    if (request->method != MK_METHOD_POST) {
        flb_sds_destroy(tag);
        send_response(conn, 400, "error: invalid HTTP method\n");
        return -1;
    }

    ret = process_payload(ctx, conn, tag, session, request);
    flb_sds_destroy(tag);
    send_response(conn, ctx->successful_response_code, NULL);
    return ret;
}

/*
 * Handle an incoming request which has resulted in an http parser error.
 */
int opentelemetry_prot_handle_error(struct flb_opentelemetry *ctx, struct http_conn *conn,
                                    struct mk_http_session *session,
                                    struct mk_http_request *request)
{
    send_response(conn, 400, "error: invalid request\n");
    return -1;
}