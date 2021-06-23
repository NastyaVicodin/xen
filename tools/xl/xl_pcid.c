/*
    Pcid daemon that acts as a server for the client in the libxl PCI

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

#include <libxl_utils.h>
#include <libxlutil.h>

#include "xl.h"
#include "xl_utils.h"
#include "xl_parse.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <libxenvchan.h>
#include <pcid.h>
#include <xenstore.h>

#include <libxl.h>
#include <dirent.h>
#include "xl_pcid.h"

#define BUFSIZE 4096
/*
 * TODO: Running this code in multi-threaded environment
 * Now the code is designed so that only one request to the server
 * from the client is made in one domain. In the future, it is necessary
 * to take into account cases when from different domains there can be
 * several requests from a client at the same time. Therefore, it will be
 * necessary to regulate the multithreading of processes for global variables.
 */
char inbuf[BUFSIZE];
char outbuf[BUFSIZE];
int insiz = 0;
int outsiz = 0;
struct libxenvchan *ctrl;

static void *pcid_zalloc(size_t size)
{
    void *ptr = calloc(size, 1);

    if (!ptr) {
        fprintf(stderr, "Memory allocation failed\n");
        exit(1);
    }

    return ptr;
}

static void vchan_wr(char *msg)
{
    int ret, len;

    len = strlen(msg);
    while (len) {
        ret = libxenvchan_write(ctrl, msg, len);
        if (ret < 0) {
            fprintf(stderr, "vchan write failed\n");
            return ;
        }
        msg += ret;
        len -= ret;
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

static struct vchan_state *vchan_get_instance(void)
{
    static struct vchan_state *state = NULL;

    if (state)
        return state;

    state = pcid_zalloc(sizeof(*state));

    return state;
}

static int handle_ls_command(char *dir_name, struct pcid_list **result)
{
    struct pcid_list *dir_list, *node = NULL;
    struct dirent *de;
    DIR *dir = NULL;

    if (strcmp(PCID_PCIBACK_DRIVER, dir_name) == 0)
        dir = opendir(SYSFS_PCIBACK_DRIVER);
    else {
        fprintf(stderr, "Unknown directory: %s\n", dir_name);
        goto out;
    }

    if (dir == NULL) {
        if (errno == ENOENT)
            fprintf(stderr, "Looks like pciback driver not loaded\n");
        else
            fprintf(stderr, "Couldn't open\n");
        goto out;
    }

    dir_list = pcid_zalloc(sizeof(*dir_list));
    if (!dir_list)
        goto out_mem_fail;
    LIBXL_LIST_INIT(&dir_list->head);
    while ((de = readdir(dir))) {
        node = pcid_zalloc(sizeof(*node));
        if (!node)
            goto out_mem_fail;
        node->val = strdup(de->d_name);
        LIBXL_LIST_INSERT_HEAD(&dir_list->head, node, entry);
    }

    closedir(dir);

    *result = dir_list;

    return 0;

out_mem_fail:
    closedir(dir);
    fprintf(stderr, "Memory allocation failed\n");
out:
    fprintf(stderr, "LS command failed\n");
    return 1;
}

static int handle_write_cmd(char *sysfs_path, char *pci_info)
{
    int rc, fd;

    fd = open(sysfs_path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "Couldn't open %s\n", sysfs_path);
        return ERROR_FAIL;
    }

    rc = write(fd, pci_info, strlen(pci_info));
    close(fd);
    if (rc < 0) {
        fprintf(stderr, "write to %s returned %d\n", sysfs_path, rc);
        return ERROR_FAIL;
    }

    return 0;
}

static int pcid_handle_cmd(struct pcid__json_object **result)
{
    struct pcid_list *dir_list = NULL;
    int ret;
    int command_name = (*result)->type;

    if (command_name == PCID_JSON_LIST) {
        ret = handle_ls_command((*result)->string, &dir_list);
        if (ret)
            goto fail;
        (*result)->list = dir_list;
    } else if (command_name == PCID_JSON_WRITE) {
        ret = handle_write_cmd((*result)->string, (*result)->info);
        if (ret != 0)
            goto fail;
        (*result)->string = NULL;
    } else {
        fprintf(stderr, "Unknown command\n");
        goto fail;
    }

    return 0;

fail:
    return 1;
}

static void free_pcid_list(struct pcid_list *list)
{
    struct pcid_list *ent, *next;

    if (!(LIBXL_LIST_EMPTY(&list->head))) {
        LIBXL_LIST_FOREACH(ent, &list->head, entry) {
            free(ent->val);
            next = LIBXL_LIST_NEXT(ent, entry);
            free(ent);
            ent = next;
        }
    }
}

static void pcid_vchan_receive_command(struct vchan_state *state)
{
    struct pcid__json_object *result = NULL;
    char *reply;
    size_t len;
    int ret;
    char *end = NULL;
    const char eom[] = PCID_END_OF_MESSAGE;
    const size_t eoml = sizeof(eom) - 1;

    state->rx_buf = outbuf;
    state->rx_buf_size = outsiz;
    state->rx_buf_used = outsiz;

    if (!state->rx_buf_used) {
        fprintf(stderr, "Buffer is empty\n");
        return;
    }

    /* Search for the end of a message: "\r\n" */
    end = memmem(state->rx_buf, state->rx_buf_size, eom, eoml);
    if (!end) {
        fprintf(stderr, "End of the message not found\n");
        return;
    }
    len = (end - state->rx_buf) + eoml;

    fprintf(stderr, "parsing %zuB: '%.*s'\n", len, (int)len,
            state->rx_buf);

    /* Replace \r by \0 so that pcid__json_parse can use strlen */
    state->rx_buf[len - eoml] = '\0';

    ret = vchan_process_message(state, &result);
    if (ret != 0)
        return;

    ret = pcid_handle_cmd(&result);
    if (ret)
        return;

    reply = vchan_prepare_reply(result, 0);
    if (!reply) {
        fprintf(stderr, "Reply preparing failed\n");
        return;
    }

    outsiz -= len;
    if (outsiz) {
        memmove(outbuf, outbuf + len, outsiz);
        insiz += len;
    } else
        memset(outbuf, 0, BUFSIZE);

    vchan_wr(reply);

    if (result->type == PCID_JSON_LIST)
        free_pcid_list(result->list);
}

int main_pcid(int argc, char *argv[])
{
    int ret, rsiz, wsiz;
    int libxenvchan_fd;
    uint32_t domid;
    char *domid_str, vchan_path[100];
    struct xs_handle *xs;
    struct vchan_state *vchan;

    int opt = 0, daemonize = 1;
    const char *pidfile = NULL;
    static const struct option opts[] = {
        {"pidfile", 1, 0, 'p'},
        COMMON_LONG_OPTS,
        {0, 0, 0, 0}
    };

    SWITCH_FOREACH_OPT(opt, "Fp:", opts, "pcid", 0) {
    case 'F':
        daemonize = 0;
        break;
    case 'p':
        pidfile = optarg;
        break;
    }

    if (daemonize) {
        ret = do_daemonize("xlpcid", pidfile);
        if (ret) {
            ret = (ret == 1) ? 0 : ret;
            goto out_daemon;
        }
    }

    xs = xs_open(0);
    if (!xs)
        perror("XS opening ERROR");;

    domid_str = xs_read(xs, XBT_NULL, "domid", NULL);
    domid = atoi(domid_str);
    free(domid_str);
    free(xs);

    rsiz = 0;
    wsiz = 0;
    sprintf(vchan_path, PCID_XS_PATH, domid);
    ctrl = libxenvchan_server_init(NULL, DOM0_ID, vchan_path, rsiz, wsiz);
    if (!ctrl) {
        perror("Libxenvchan server init failed\n");
        exit(1);
    }

    vchan = vchan_get_instance();
    if (!vchan)
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
        FD_SET(libxenvchan_fd, &rfds);
        ret = select(libxenvchan_fd + 1, &rfds, &wfds, NULL, NULL);
        if (ret < 0) {
            fprintf(stderr, "Error occured during the libxenvchan fd monitoring\n");
            goto exit;
        }
        if (FD_ISSET(0, &rfds)) {
            ret = read(0, inbuf + insiz, BUFSIZE - insiz);
            if (ret < 0 && errno != EAGAIN)
                goto exit;
            if (ret == 0) {
                while (insiz) {
                    libxenvchan_wait(ctrl);
                }
                goto out;
            }
            if (ret)
                insiz += ret;
        }
        if (FD_ISSET(libxenvchan_fd, &rfds))
            libxenvchan_wait(ctrl);
        if (FD_ISSET(1, &wfds))
            pcid_vchan_receive_command(vchan);
        while (libxenvchan_data_ready(ctrl) && outsiz < BUFSIZE) {
            ret = libxenvchan_read(ctrl, outbuf + outsiz, BUFSIZE - outsiz);
            if (ret < 0)
                goto exit;
            outsiz += ret;
            pcid_vchan_receive_command(vchan);
            while (!libxenvchan_data_ready(ctrl))
                libxenvchan_wait(ctrl);
        }
        if (!libxenvchan_is_open(ctrl)) {
            if (set_nonblocking(1, 0))
                goto exit;
            while (outsiz)
                pcid_vchan_receive_command(vchan);
            goto out;
        }
    }

out:
    free(vchan);
    return 0;

exit:
    free(vchan);
out_daemon:
    exit(1);
}
