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

#define PCID_MSG_FIELD_ID        "id"
#define PCID_MSG_FIELD_ARGS      "arguments"

#define PCID_CMD_LIST            "ls"
#define PCID_CMD_DIR_ID          "dir_id"

#define PCID_PCIBACK_DRIVER      "pciback_driver"

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
