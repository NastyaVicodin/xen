/*
 * xenpcid.c
 */

#define _GNU_SOURCE  // required for strchrnul()

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

#include <libxenvchan.h>
#include <xenpcid.h>

#include <xenstore.h>
#include <libxl.h>
#include <libxl_json.h>
#include <dirent.h>
#include "pcid.h"

#define BUFSIZE 5000
char inbuf[BUFSIZE];
char outbuf[BUFSIZE];
int insiz = 0;
int outsiz = 0;
struct libxenvchan *ctrl = 0;

libxl_ctx *ctx;

static void *pcid_zalloc(size_t size)
{
    void *ptr = calloc(size, 1);
    if (!ptr) {
        fprintf(stderr, "Memory allocation failed\n");
		exit(1);
	}
    memset(ptr, 0, size);

    return ptr;
}

static void vchan_wr(char *reply)
{
	int ret, len;

    len = strlen(reply);
	ret = libxenvchan_write(ctrl, reply, len);

    if (ret < 0) {
        fprintf(stderr, "vchan write failed\n");
        exit(1);
    }
}

static int set_nonblocking(int fd, int nonblocking)
{
	int flags = fcntl(fd, F_GETFL);
	if (flags == -1)
		return -1;

	if (nonblocking)
		flags |= O_NONBLOCK;
	else
		flags &= ~O_NONBLOCK;

	if (fcntl(fd, F_SETFL, flags) == -1)
		return -1;

	return 0;
}

static yajl_gen_status pcid__yajl_gen_asciiz(yajl_gen hand, const char *str)
{
    return yajl_gen_string(hand, (const unsigned char *)str, strlen(str));
}

static void free_list(struct list_head *head)
{
    struct list_head* tmp;

    while (head != NULL)
    {
        tmp = head;
        head = head->next;
        free(tmp);
    }
}

static void free_list_rsc(struct list_resources *head)
{
    struct list_resources* tmp;

    while (head != NULL)
    {
        tmp = head;
        head = head->next;
        free(tmp);
    }
}
static char *vchan_prepare_cmd(struct pcid__json_object *result,
                               const struct pcid__json_object *args,
                               int id)
{
    yajl_gen hand = NULL;
    /* memory for 'buf' is owned by 'hand' */
    const unsigned char *buf;
    libxl_yajl_length len;
    yajl_gen_status s;
    char *ret = NULL;
    struct list_head *resp_list;
    struct list_resources *resp_rsc_list;

    hand = libxl_yajl_gen_alloc(NULL);
    if ( !hand ) {
    	fprintf(stderr, "Error with hand allocation\n");
        return NULL;
    }

#if HAVE_YAJL_V2
    /* Disable beautify for data sent to QEMU */
    yajl_gen_config(hand, yajl_gen_beautify, 0);
#endif

    yajl_gen_map_open(hand);
    
    if ( !result ) {
        pcid__yajl_gen_asciiz(hand, XENPCID_MSG_ERROR);
    } else {
        pcid__yajl_gen_asciiz(hand, XENPCID_MSG_RETURN);
        if (result->type == JSON_ARRAY) {
            yajl_gen_array_open(hand);
            if (result->u.list) {
                resp_list = result->u.list;
                while (resp_list) {
                    pcid__yajl_gen_asciiz(hand, resp_list->val);
                    free(resp_list->val);
                    resp_list = resp_list->next;
                }
                free_list(result->u.list);
            } else if (result->u.list_rsc) {
                resp_rsc_list = result->u.list_rsc;
                while (resp_rsc_list) {
                    yajl_gen_integer(hand, resp_rsc_list->start);
                    yajl_gen_integer(hand, resp_rsc_list->end);
                    yajl_gen_integer(hand, resp_rsc_list->flags);
                    resp_rsc_list = resp_rsc_list->next;
                }
                free_list_rsc(result->u.list_rsc);
            }
            yajl_gen_array_close(hand);
        } else if (result->type == JSON_STRING) {
            if (result->u.string) {
                fprintf(stderr, "result->u.string = %s\n", result->u.string);
                pcid__yajl_gen_asciiz(hand, result->u.string);
                free(result->u.string);
            } else
                pcid__yajl_gen_asciiz(hand, "success");
        } else if (result->type == JSON_INTEGER) {
            yajl_gen_integer(hand, result->u.i);
        } else {
            fprintf(stderr, "Unknown result type\n");
        }
    }
    pcid__yajl_gen_asciiz(hand, XENPCID_MSG_FIELD_ID);
    yajl_gen_integer(hand, id);
    yajl_gen_map_close(hand);

    s = yajl_gen_get_buf(hand, &buf, &len);
    if (s != yajl_gen_status_ok) {
        goto out;
    }

	ret = pcid_zalloc(sizeof((int)len));
	sprintf(ret, "%*.*s" XENPCID_END_OF_MESSAGE,
            (int)len, (int)len, buf);
    fprintf(stderr, "ret = %s\n", ret);

out:
    return ret;
}

static int handle_ls_command(char *dir_name, struct list_head **result)
{
    struct list_head *dirs = NULL, *head = NULL, *prev = NULL;
    struct dirent *de;
    DIR *dir = NULL;

    head = (struct list_head*)pcid_zalloc(sizeof(struct list_head));
    dirs = head;

    if (strcmp(XENPCID_PCIBACK_DRIVER, dir_name) == 0) {
#if defined(__linux__)
        dir = opendir(SYSFS_PCIBACK_DRIVER);
#endif
    } else if (strcmp(XENPCID_PCI_DEV, dir_name) == 0){
#if defined(__linux__)
        dir = opendir(SYSFS_PCI_DEV);
#endif
    } else {
        fprintf(stderr, "Unknown directory: %s\n", dir_name);
        goto out;
    }

    if (NULL == dir) {
        if (errno == ENOENT) {
            fprintf(stderr,  "Looks like pciback driver not loaded\n");
        } else {
            fprintf(stderr, "Couldn't open\n");
        }
        goto out;
    }

    while((de = readdir(dir))) {
        if (!dirs)
        {
            dirs = (struct list_head*)pcid_zalloc(sizeof(struct list_head));
            prev->next = dirs;
        }
        dirs->val = strdup(de->d_name);
        prev = dirs;
        dirs = dirs->next;
    }

    closedir(dir);

    *result = head;

    return 0;

out:
    fprintf(stderr, "LS command failed\n");
    return 1;
}

static void flexarray_grow(struct flexarray *array, int extents)
{
    int newsize;

    newsize = array->size + extents;
    array->data = realloc(array->data, newsize * sizeof(*(array->data)));
    array->size += extents;
}

static int flexarray_set(struct flexarray *array, unsigned int idx, void *ptr)
{
    if (idx >= array->size) {
        int newsize;
        if (!array->autogrow)
            return 1;
        newsize = (array->size * 2 < idx) ? idx + 1 : array->size * 2;
        flexarray_grow(array, newsize - array->size);
    }
    if ( idx + 1 > array->count )
        array->count = idx + 1;
    array->data[idx] = ptr;
    return 0;
}

static int flexarray_append(struct flexarray *array, void *ptr)
{
    return flexarray_set(array, array->count, ptr);
}

static struct flexarray *flexarray_make(libxl_ctx *ctx, int size, int autogrow)
{
    struct flexarray *array;

    array = pcid_zalloc(sizeof(*array));
    array->size = size;
    array->autogrow = autogrow;
    array->count = 0;
    array->ctx = ctx;
    array->data = calloc(size, sizeof(*(array->data)));
    return array;
}

static int flexarray_get(struct flexarray *array, int idx, void **ptr)
{
    if (idx >= array->size)
        return 1;
    *ptr = array->data[idx];
    return 0;
}

static inline bool pcid__json_object_is_map(const struct pcid__json_object *o)
{
    return o != NULL && o->type == JSON_MAP;
}

static struct pcid__json_object *pcid__json_map_get(const char *key,
                                                    const struct pcid__json_object *o,
                                                    enum pcid__json_node_type expected_type)
{
    struct flexarray *maps = NULL;
    int idx = 0;
    struct pcid__json_map_node *node = NULL;

    if (pcid__json_object_is_map(o)) {
        maps = o->u.map;
        for (idx = 0; idx < maps->count; idx++) {
            if (flexarray_get(maps, idx, (void**)&node) != 0) {
                return NULL;
            }
            if (strcmp(key, node->map_key) == 0) {
                if (expected_type == JSON_ANY
                    || (node->obj && (node->obj->type & expected_type))) {
                    return node->obj;
                } else {
                    return NULL;
                }
            }
        }
    }
    return NULL;
}

static int handle_write_cmd(char *sysfs_path, char *pci_info)
{
    int rc, fd;
    fprintf(stderr, "handle_write_cmd %s\n", sysfs_path);
    fd = open(sysfs_path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "Couldn't open %s\n", sysfs_path);
        return ERROR_FAIL;
    }

    rc = write(fd, pci_info, strlen(pci_info));
    /* Annoying to have two if's, but we need the errno */
    if (rc < 0)
        fprintf(stderr, "write to %s returned %d\n", sysfs_path, rc);
    close(fd);

    if (rc < 0)
        return ERROR_FAIL;

    return 0;
}

static long long handle_read_cmd(char *sysfs_path)
{
    uint16_t read_items;
    long long result;
    FILE *f = fopen(sysfs_path, "r");

    if (!f) {
        fprintf(stderr, "PCI device %s does not have vendor attribute\n",
                sysfs_path);
        return -1;
    }
    read_items = fscanf(f, "0x%llx\n", &result);
    fclose(f);
    if (read_items != 1) {
        fprintf(stderr, "Cannot read vendor of pci device %s\n", sysfs_path);
        return -1;
    }

    return result;
}

static int handle_read_resources_cmd(char *pci_path,
                                     struct list_resources **result)
{
    unsigned long long start, end, flags;
    struct list_resources *list = NULL, *head = NULL, *prev = NULL;
    int i;
    char *sysfs_path;
    FILE *f;

    head = (struct list_resources*)pcid_zalloc(sizeof(struct list_resources));
    list = head;

    sysfs_path = (char *)pcid_zalloc(sizeof(strlen(SYSFS_PCI_DEV) + strlen(pci_path) + 1));
    sprintf(sysfs_path, SYSFS_PCI_DEV"%s", pci_path);
    f = fopen(sysfs_path, "r");
    if (!f) {
        fprintf(stderr, "Failed to open %s\n", sysfs_path);
        free(sysfs_path);
        return -1;
    }
    for (i = 0; i < PROC_PCI_NUM_RESOURCES; i++) {
        if (fscanf(f, "0x%llx 0x%llx 0x%llx\n", &start, &end, &flags) != 3)
            continue;

        if (!list)
        {
            list = (struct list_resources*)pcid_zalloc(sizeof(struct list_resources));
            prev->next = list;
        }
        list->start = start;
        list->end = end;
        list->flags = flags;
        prev = list;
        list = list->next;
    }
    fclose(f);
    free(sysfs_path);
    *result = head;

    return 0;
}

static int handle_unbind_cmd(char *pci_path, char *pci_info, char **result)
{
    char *spath, *new_path, *dp = NULL;
    struct stat st;

    spath = (char *)pcid_zalloc(strlen(SYSFS_PCI_DEV) + strlen(pci_path) + 1);
    sprintf(spath, SYSFS_PCI_DEV"%s", pci_path);
    free(pci_path);

    if ( !lstat(spath, &st) ) {
        /* Find the canonical path to the driver. */
        dp = (char *)pcid_zalloc(PATH_MAX);
        if ( !(realpath(spath, dp)) ) {
            fprintf(stderr, "realpath() failed\n");
            goto fail;
        }
        *result = dp;
        new_path = (char *)pcid_zalloc(strlen(dp) + strlen("/unbind") + 1);
        /* Unbind from the old driver */
        sprintf(new_path, "%s/unbind", dp);

        if ( handle_write_cmd(new_path, pci_info) != 0 ) {
            fprintf(stderr, "Couldn't unbind device\n");
            goto fail_write;
        }
        free(new_path);
    } else {
        *result = (char *)pcid_zalloc(strlen("nolstat") + 1);
        sprintf(*result, "nolstat");
    }
    free(spath);
    free(pci_info);

    return 0;

fail_write:
    free(new_path);
fail:
    free(spath);
    free(pci_info);

    return -1;
}

static int vchan_handle_message(libxl_ctx *ctx,
                                struct vchan_state *state,
                                struct pcid__json_object **resp)
{
    long long read_result;
    int ret;
    struct list_head *dir_list = NULL;
    struct list_resources *resources_list = NULL;
    struct pcid__json_object *command_obj, *args, *dir_id, *pci_info;
    struct pcid__json_object *pci_path;
    char *dir_name, *command_name, *full_path = NULL;

    command_obj = pcid__json_map_get(XENPCID_MSG_EXECUTE, *resp, JSON_ANY);
    command_name = command_obj->u.string;

    if (strcmp(XENPCID_CMD_LIST, command_name) == 0) {
        args = pcid__json_map_get(XENPCID_MSG_FIELD_ARGS, *resp, JSON_MAP);
        dir_id = pcid__json_map_get(XENPCID_CMD_DIR_ID, args, JSON_ANY);
        dir_name = dir_id->u.string;

        ret = handle_ls_command(dir_name, &dir_list);
        if (ret)
            goto out;

        (*resp)->type = JSON_ARRAY;
        (*resp)->u.list = dir_list;
    } else if (strcmp(XENPCID_CMD_WRITE, command_name) == 0) {
        args = pcid__json_map_get(XENPCID_MSG_FIELD_ARGS, *resp, JSON_MAP);
        dir_id = pcid__json_map_get(XENPCID_CMD_DIR_ID, args, JSON_ANY);
        pci_path = pcid__json_map_get(XENPCID_CMD_PCI_PATH, args, JSON_ANY);
        pci_info = pcid__json_map_get(XENPCID_CMD_PCI_INFO, args, JSON_ANY);

        if (strcmp(dir_id->u.string, XENPCID_PCI_DEV) == 0) {
            full_path = (char *)pcid_zalloc(strlen(SYSFS_PCI_DEV) +
                                            strlen(pci_path->u.string) + 1);
            sprintf(full_path, SYSFS_PCI_DEV"%s", pci_path->u.string);
        } else if (strcmp(dir_id->u.string, XENPCID_PCIBACK_DRIVER) == 0){
            full_path = (char *)pcid_zalloc(strlen(SYSFS_PCIBACK_DRIVER) +
                                            strlen(pci_path->u.string) + 1);
            sprintf(full_path, SYSFS_PCIBACK_DRIVER"%s", pci_path->u.string);
        } else if (strcmp(dir_id->u.string, SYSFS_DRIVER_PATH) == 0){
            full_path = pci_path->u.string;
        } else {
            fprintf(stderr, "Unknown write directory %s\n", dir_id->u.string);
            goto out;
        }
        ret = handle_write_cmd(full_path, pci_info->u.string);
        full_path ? free(full_path) : 0;
        if (ret != 0)
            goto out;
        (*resp)->type = JSON_STRING;
        (*resp)->u.string = NULL;
    } else if (strcmp(XENPCID_CMD_READ, command_name) == 0) {
        args = pcid__json_map_get(XENPCID_MSG_FIELD_ARGS, *resp, JSON_MAP);
        pci_info = pcid__json_map_get(XENPCID_CMD_PCI_INFO, args, JSON_ANY);
        dir_id = pcid__json_map_get(XENPCID_CMD_DIR_ID, args, JSON_ANY);

        if (strcmp(XENPCID_PCI_DEV, dir_id->u.string) == 0) {
            full_path = (char *)pcid_zalloc(strlen(SYSFS_PCI_DEV) +
                                            strlen(pci_info->u.string) + 1);
            sprintf(full_path, SYSFS_PCI_DEV"%s", pci_info->u.string);
        } else {
            full_path = pci_info->u.string;
        }
        read_result = handle_read_cmd(full_path);
        full_path ? free(full_path) : 0;
        if (read_result < 0)
            goto out;
        (*resp)->type = JSON_INTEGER;
        (*resp)->u.i = read_result;
    } else if (strcmp(XENPCID_CMD_READ_RESOURCES, command_name) == 0) {
        args = pcid__json_map_get(XENPCID_MSG_FIELD_ARGS, *resp, JSON_MAP);
        pci_info = pcid__json_map_get(XENPCID_CMD_PCI_INFO, args, JSON_ANY);

        ret = handle_read_resources_cmd(pci_info->u.string, &resources_list);
        if (ret < 0)
            goto out;
        (*resp)->type = JSON_ARRAY;
        (*resp)->u.list_rsc = resources_list;
    } else if (strcmp(XENPCID_CMD_UNBIND, command_name) == 0) {
        args = pcid__json_map_get(XENPCID_MSG_FIELD_ARGS, *resp, JSON_MAP);
        pci_path = pcid__json_map_get(XENPCID_CMD_PCI_PATH, args, JSON_ANY);
        pci_info = pcid__json_map_get(XENPCID_CMD_PCI_INFO, args, JSON_ANY);

        ret = handle_unbind_cmd(pci_path->u.string, pci_info->u.string, &full_path);
        if ( ret < 0 )
            goto out;
        (*resp)->type = JSON_STRING;
        (*resp)->u.string = full_path;
        free(args);
        free(pci_path);
        free(pci_info);
    } else {
        fprintf(stderr, "Unknown command\n");
        goto out;
    }
    free(command_name);
    fprintf(stderr, "vchan_handle_message end\n");
    return 0;

out:
    fprintf(stderr, "Message handling failed\n");
    return 1;
}

static inline bool pcid__json_object_is_array(const struct pcid__json_object *o)
{
    return o != NULL && o->type == JSON_ARRAY;
}

static int pcid__json_object_append_to(libxl_ctx *ctx,
                                       struct pcid__json_object *obj,
                                       struct pcid__yajl_ctx *ctx_yajl)
{
    struct pcid__json_object *dst = ctx_yajl->current;

    if (dst) {
        switch (dst->type) {
        case JSON_MAP: {
            struct pcid__json_map_node *last = NULL;

            if (dst->u.map->count == 0) {
                fprintf(stderr,
                        "Try to add a value to an empty map (with no key)\n");
                return ERROR_FAIL;
            }
            flexarray_get(dst->u.map, dst->u.map->count - 1, (void**)&last);
            last->obj = obj;
            break;
        }
        case JSON_ARRAY:
            flexarray_append(dst->u.array, obj);
            break;
        default:
            fprintf(stderr,
                    "Try append an object is not a map/array (%i)\n",
                    dst->type);
            return ERROR_FAIL;
        }
    }

    obj->parent = dst;

    if (pcid__json_object_is_map(obj) || pcid__json_object_is_array(obj))
        ctx_yajl->current = obj;
    if (ctx_yajl->head == NULL)
        ctx_yajl->head = obj;

    return 0;
}

static int json_callback_string(void *opaque, const unsigned char *str,
                                long unsigned int len)
{
    struct pcid__yajl_ctx *ctx = opaque;
    char *t = NULL;
    struct pcid__json_object *obj = NULL;

    t = pcid_zalloc(len + 1);
    strncpy(t, (const char *) str, len);
    t[len] = 0;

    obj = pcid_zalloc(sizeof(*obj));
    obj->u.string = t;

    if (pcid__json_object_append_to(ctx->ctx, obj, ctx))
        return 1;

    return 1;
}

static struct pcid__json_object *pcid__json_object_alloc(libxl_ctx *ctx,
                                                         enum pcid__json_node_type type)
{
    struct pcid__json_object *obj;

    obj = pcid_zalloc(sizeof(*obj));
    obj->type = type;

    if (type == JSON_MAP || type == JSON_ARRAY) {
        struct flexarray *array = flexarray_make(ctx, 1, 1);
        if (type == JSON_MAP)
            obj->u.map = array;
        else
            obj->u.array = array;
    }

    return obj;
}

static int json_callback_map_key(void *opaque, const unsigned char *str,
                                 libxl_yajl_length len)
{
    struct pcid__yajl_ctx *ctx_yajl = opaque;
    char *t = NULL;
    struct pcid__json_object *obj = ctx_yajl->current;

    t = pcid_zalloc(len + 1);

    strncpy(t, (const char *) str, len);
    t[len] = 0;

    if (pcid__json_object_is_map(obj)) {
        struct pcid__json_map_node *node;

        node = pcid_zalloc(sizeof(*node));
        node->map_key = t;
        node->obj = NULL;;
        flexarray_append(obj->u.map, node);
    } else {
        fprintf(stderr, "Current json object is not a map\n");
        return 0;
    }

    return 1;
}

static int json_callback_start_map(void *opaque)
{
    struct pcid__yajl_ctx *ctx = opaque;
    struct pcid__json_object *obj = NULL;

    obj = pcid__json_object_alloc(ctx->ctx, JSON_MAP);

    if (pcid__json_object_append_to(ctx->ctx, obj, ctx))
        return 0;

    return 1;
}

static int json_callback_end_map(void *opaque)
{
    struct pcid__yajl_ctx *ctx = opaque;

    if (ctx->current) {
        ctx->current = ctx->current->parent;
    } else {
        fprintf(stderr,
                "No current pcid__json_object, cannot use his parent\n");
        return 0;
    }

    return 1;
}

static yajl_callbacks callbacks = {
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    json_callback_string,
    json_callback_start_map,
    json_callback_map_key,
    json_callback_end_map,
    NULL,
    NULL,
};

static void yajl_ctx_free(struct pcid__yajl_ctx *yajl_ctx)
{
    if (yajl_ctx->hand) {
        yajl_free(yajl_ctx->hand);
        yajl_ctx->hand = NULL;
    }
}

static struct pcid__json_object *pcid__json_parse(libxl_ctx *ctx, const char *s)
{
    yajl_status status;
    struct pcid__yajl_ctx yajl_ctx;
    struct pcid__json_object *o = NULL;
    unsigned char *str = NULL;

    memset(&yajl_ctx, 0, sizeof (yajl_ctx));
    yajl_ctx.ctx = ctx;

    if (yajl_ctx.hand == NULL) {
        yajl_ctx.hand = libxl__yajl_alloc(&callbacks, NULL, &yajl_ctx);
    }

    status = yajl_parse(yajl_ctx.hand, (const unsigned char *)s, strlen(s));
    if (status != yajl_status_ok){
    	fprintf(stderr, "yajl_parse error\n");
        goto out;
    }

    status = yajl_complete_parse(yajl_ctx.hand);
    if (status != yajl_status_ok){
    	fprintf(stderr, "yajl_complete_parse error\n");
        goto out;
    }

    o = yajl_ctx.head;

    yajl_ctx.head = NULL;

    yajl_ctx_free(&yajl_ctx);
    return o;

out:
    str = yajl_get_error(yajl_ctx.hand, 1, (const unsigned char*)s, strlen(s));
    yajl_free_error(yajl_ctx.hand, str);
    yajl_ctx_free(&yajl_ctx);
    return NULL;
}

static int vchan_get_next_msg(libxl_ctx *ctx, struct vchan_state *state,
                              struct pcid__json_object **o_r)
    /* Find a JSON object and store it in o_r.
     * return ERROR_NOTFOUND if no object is found.
     */
{
    size_t len;
    char *end = NULL;
    const char eom[] = XENPCID_END_OF_MESSAGE;
    const size_t eoml = sizeof(eom) - 1;
    struct pcid__json_object *o = NULL;

    if ( !state->rx_buf_used ){
        fprintf(stderr, "Buffer is empty\n");
        return ERROR_NOTFOUND;
    }

    /* Search for the end of a message: "\r\n" */
    end = memmem(state->rx_buf, state->rx_buf_size, eom, eoml);
    if ( !end ) {
        fprintf(stderr, "End of the message not found\n");
        return ERROR_NOTFOUND;
    }
    len = (end - state->rx_buf) + eoml;

    fprintf(stderr, "parsing %luB: '%.*s'\n", len, (int)len,
         state->rx_buf);

    if (len) {
        outsiz -= len;
        memmove(outbuf, outbuf + len, outsiz);
    }

    /* Replace \r by \0 so that pcid__json_parse can use strlen */
    state->rx_buf[len - eoml] = '\0';
    o = pcid__json_parse(ctx, state->rx_buf);

    if ( !o ) {
        fprintf(stderr, "Parse error\n");
        return ERROR_PROTOCOL_ERROR_PCID;
    }
    state->rx_buf_used = len;
    state->rx_buf_used -= len;
    memmove(state->rx_buf, state->rx_buf + len, state->rx_buf_used);

    *o_r = o;

    return 0;
}

static int vchan_read_message(libxl_ctx *ctx,
                              struct vchan_state *state,
                              struct pcid__json_object **result)
{
    int rc;
    struct pcid__json_object *o = NULL;

    /* parse rx buffer to find one json object */
    rc = vchan_get_next_msg(ctx, state, &o);
    if ( rc == ERROR_NOTFOUND ){
        perror("Message not found\n");
        return rc;
    }

    rc = vchan_handle_message(ctx, state, &o);
    if ( rc == 0 )
        *result = o;
    else
        *result = NULL;
    return 0;
}

static struct vchan_state *vchan_get_instance(void)
{
    static struct vchan_state *state = NULL;

    if ( state )
        return state;

    state = pcid_zalloc(sizeof(*state));

    return state;
}

static void vchan_receive_command(libxl_ctx *ctx,
								  struct vchan_state *state,
                                  const struct pcid__json_object *args)
{
    struct pcid__json_object *result = NULL;
    char *reply;
    int ret;

    state->rx_buf = outbuf;
    state->rx_buf_size = outsiz;
    state->rx_buf_used = outsiz;
	ret = vchan_read_message(ctx, state, &result);

    reply = vchan_prepare_cmd(result, args, 0);
    if ( !reply ){
    	fprintf(stderr, "Reply preparing failed\n");
        return;
    }
	
    vchan_wr(reply);
    //free(reply);
}

int main(int argc, char *argv[])
{
	int ret, rsiz, wsiz;
	int libxenvchan_fd;
	uint32_t domid;
	char *domid_str, vchan_path[100];
	struct xs_handle *xs;
	struct pcid__json_object *args = NULL;
	struct vchan_state *vchan;

	xs = xs_open(0);
	if (!xs)
		perror("XS opening ERROR");;

	domid_str = xs_read(xs, XBT_NULL, "domid", NULL);
	domid = atoi(domid_str);

	rsiz = 0;
	wsiz = 0;
	sprintf(vchan_path, XENPCID_XS_PATH, domid);
	ctrl = libxenvchan_server_init(NULL, DOM0_ID, vchan_path, rsiz, wsiz);
	if (!ctrl) {
		perror("Libxenvchan server init failed\n");
		exit(1);
	}

	vchan = vchan_get_instance();
	if ( !vchan )
        perror("Vchan creation failed\n");

	vchan->domid = DOM0_ID;
    vchan->xs_path = vchan_path;
    vchan->ctrl = ctrl;

    libxenvchan_fd = libxenvchan_fd_for_select(ctrl);
    vchan->select_fd = libxenvchan_fd;
	
	for (;;) {
		fd_set rfds;
		fd_set wfds;
		FD_ZERO(&rfds);
		FD_ZERO(&wfds);
		if (insiz != BUFSIZE)
			FD_SET(0, &rfds);
		if (outsiz)
			FD_SET(1, &wfds);
		FD_SET(libxenvchan_fd, &rfds);
		ret = select(libxenvchan_fd + 1, &rfds, &wfds, NULL, NULL);
		if (ret < 0) {
			exit(1);
		}
		if (FD_ISSET(0, &rfds)) {
			ret = read(0, inbuf + insiz, BUFSIZE - insiz);
			if (ret < 0 && errno != EAGAIN)
				exit(1);
			if (ret == 0) {
				while (insiz) {
					libxenvchan_wait(ctrl);
				}
				return 0;
			}
			if (ret)
				insiz += ret;
		}
		if (FD_ISSET(libxenvchan_fd, &rfds)) {
			libxenvchan_wait(ctrl);
		}
		if (FD_ISSET(1, &wfds))
		{
			vchan_receive_command(ctx, vchan, args);
		}
		while (libxenvchan_data_ready(ctrl) && outsiz < BUFSIZE) {
			ret = libxenvchan_read(ctrl, outbuf + outsiz, BUFSIZE - outsiz);
			if (ret < 0)
				exit(1);
			outsiz += ret;
			vchan_receive_command(ctx, vchan, args);
		}
		if (!libxenvchan_is_open(ctrl)) {
			if (set_nonblocking(1, 0)) {
				exit(1);
			}
			while (outsiz)
			{
				vchan_receive_command(ctx, vchan, args);
			}
			return 0;
		}
	}
    return 0;
}
