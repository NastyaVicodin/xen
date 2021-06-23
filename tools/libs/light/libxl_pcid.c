/*
    Utils for xl pcid daemon

    Copyright (C) 2021 EPAM Systems Inc.

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; If not, see <http://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE  // required for strchrnul()

#include "libxl_osdeps.h" /* must come before any other headers */

#include "libxl_internal.h"
#include "libxl_pcid.h"

#include <libxl_utils.h>
#include <libxlutil.h>

#include <libxenvchan.h>
#include <xenstore.h>

#include <libxl.h>
#include <libxl_json.h>
#include <dirent.h>

static int vchan_get_next_msg(libxl__gc *gc, struct vchan_state *state,
                              libxl__json_object **o_r)
{
    libxl__json_object *o = NULL;
    o = libxl__json_parse(gc, state->rx_buf);
    if (!o) {
        LOGD(ERROR, state->domid, "Parse error");
        return ERROR_PROTOCOL_ERROR_PCID;
    }

    *o_r = o;

    return 0;
}

static int vchan_handle_message(libxl__gc *gc, struct vchan_state *state,
                                struct libxl__json_object *resp,
                                struct pcid__json_object **result)
{
    const struct libxl__json_object *command_obj;
    char *command_name;

    command_obj = libxl__json_map_get(PCID_MSG_EXECUTE, resp, JSON_ANY);
    command_name = command_obj->u.string;

    return 0;
}

int vchan_process_message(struct vchan_state *state,
                          struct pcid__json_object **result)
{
    libxl_ctx *ctx = NULL;
    GC_INIT(ctx);
    int rc;
    struct libxl__json_object *o = NULL;
    struct pcid__json_object *reply = NULL;

    /* parse rx buffer to find one json object */
    rc = vchan_get_next_msg(gc, state, &o);
    if (rc == ERROR_NOTFOUND) {
        perror("Message not found\n");
        return rc;
    }

    rc = vchan_handle_message(gc, state, o, &reply);
    if (rc == 0)
        *result = reply;

    return 0;
}

char *vchan_prepare_reply(struct pcid__json_object *result,
                          int id)
{
    libxl_ctx *ctx = NULL;
    GC_INIT(ctx);
    yajl_gen hand = NULL;
    /* memory for 'buf' is owned by 'hand' */
    const unsigned char *buf;
    libxl_yajl_length len;
    yajl_gen_status s;
    char *ret = NULL;

    hand = libxl_yajl_gen_alloc(NULL);
    if (!hand) {
        fprintf(stderr, "Error with hand allocation\n");
        goto out;
    }

#if HAVE_YAJL_V2
    /* Disable beautify for data */
    yajl_gen_config(hand, yajl_gen_beautify, 0);
#endif

    yajl_gen_map_open(hand);
    if ( !result )
        libxl__yajl_gen_asciiz(hand, PCID_MSG_ERROR);
    else
        libxl__yajl_gen_asciiz(hand, PCID_MSG_RETURN);
    yajl_gen_array_open(hand);
    yajl_gen_array_close(hand);
    libxl__yajl_gen_asciiz(hand, PCID_MSG_FIELD_ID);
    yajl_gen_integer(hand, id);
    yajl_gen_map_close(hand);

    s = yajl_gen_get_buf(hand, &buf, &len);
    if (s != yajl_gen_status_ok) {
        goto out;
    }

    ret = libxl__sprintf(gc, "%*.*s" PCID_END_OF_MESSAGE,
                         (int)len, (int)len, buf);

    yajl_gen_free(hand);

out:
    return ret;
}
