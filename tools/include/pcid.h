/*
    Common definitions for Xen PCI client-server protocol.
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

#ifndef PCID_H
#define PCID_H

#define PCID_XS_PATH             "/local/domain/%d/data/pcid-vchan"

#define PCID_END_OF_MESSAGE      "\r\n"

#define PCID_MSG_EXECUTE         "execute"
#define PCID_MSG_RETURN          "return"
#define PCID_MSG_ERROR           "error"

#define PCID_MSG_FIELD_ID        "id"
#define PCID_MSG_FIELD_ARGS      "arguments"

#define PCID_CMD_LIST            "ls"
#define PCID_CMD_UNBIND          "unbind"
#define PCID_CMD_RESET           "reset"
#define PCID_CMD_DIR_ID          "dir_id"

#define PCID_CMD_WRITE           "write"
#define PCID_CMD_READ_HEX        "read_hex"
#define PCID_CMD_EXISTS          "exists"
#define PCID_CMD_READ_RESOURCES  "read_resources"
#define PCID_CMD_PCI_PATH        "pci_path"
#define PCID_CMD_PCI_INFO        "pci_info"

#define PCID_PCIBACK_DRIVER      "pciback_driver"
#define PCID_PCI_DEV             "pci_dev"

#define SYSFS_DRIVER_PATH        "driver_path"

#if defined(__linux__)
#define SYSFS_PCIBACK_DRIVER   "/sys/bus/pci/drivers/pciback"
#define SYSFS_PCI_DEV          "/sys/bus/pci/devices"
#endif

#define PCI_INFO_PATH "/libxl/pci"
#define PCI_BDF_XSPATH         "%04x-%02x-%02x-%01x"
#define PCI_BDF                "%04x:%02x:%02x.%01x"

/*
 * TODO: Avoid code duplicates
 *
 * The presentation of the structures and some of the functions were taken from
 * Libxl internal files. It will be necessary to made this code common to avoid
 * duplicates.
 */

struct vchan_state {
    /* Server domain ID. */
    libxl_domid domid;
    /* XenStore path of the server with the ring buffer and event channel. */
    char *xs_path;

    struct libxenvchan *ctrl;
    int select_fd;
    /* receive buffer */
    char *rx_buf;
    size_t rx_buf_size; /* current allocated size */
    size_t rx_buf_used; /* actual data in the buffer */
};

enum pcid__json_resp_type {
    PCID_JSON_LIST = (1 << 0),
    PCID_JSON_WRITE = (1 << 1),
    PCID_JSON_READ_HEX = (1 << 2),
    PCID_JSON_EXISTS = (1 << 3),
    PCID_JSON_LIST_RSC = (1 << 4),
    PCID_JSON_UNBIND = (1 << 5),
    PCID_JSON_RESET = (1 << 6),
    PCID_JSON_ANY = 255 /* this is a mask of all values above, adjust as needed */
};

struct pcid_list {
    LIBXL_LIST_HEAD(, struct pcid_list) head;
    LIBXL_LIST_ENTRY(struct pcid_list) entry;
    char *val;
};

struct pcid_list_resources {
    LIBXL_LIST_HEAD(, struct pcid_list_resources) head;
    LIBXL_LIST_ENTRY(struct pcid_list_resources) entry;
    long long start;
    long long end;
    long long flags;
};

struct pcid__json_object {
    enum pcid__json_resp_type type;
    long long i;
    char *string;
    char *info;
    struct pcid_list *list;
    struct pcid_list_resources *list_rsc;
};

int vchan_process_message(struct vchan_state *state,
                          struct pcid__json_object **result);
char *vchan_prepare_reply(struct pcid__json_object *result, int id);

#endif /* PCID_H */

/*
 * Local variables:
 *  mode: C
 *  c-file-style: "linux"
 *  indent-tabs-mode: t
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
