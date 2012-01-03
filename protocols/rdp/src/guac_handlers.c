
/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is libguac-client-rdp.
 *
 * The Initial Developer of the Original Code is
 * Michael Jumper.
 * Portions created by the Initial Developer are Copyright (C) 2011
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#include <stdlib.h>
#include <string.h>

#include <sys/select.h>
#include <errno.h>

#include <freerdp/freerdp.h>
#include <freerdp/channels/channels.h>
#include <freerdp/input.h>

#include <guacamole/socket.h>
#include <guacamole/protocol.h>
#include <guacamole/client.h>

#include "client.h"
#include "rdp_handlers.h"
#include "rdp_keymap.h"

int rdp_guac_client_free_handler(guac_client* client) {

    rdp_guac_client_data* guac_client_data = (rdp_guac_client_data*) client->data;

    /* Disconnect client */
    guac_client_data->rdp_inst->rdp_disconnect(guac_client_data->rdp_inst);

    /* Free RDP client */
    freerdp_free(guac_client_data->rdp_inst);
    freerdp_channels_free(guac_client_data->channels);
    free(guac_client_data->settings);

    /* Free guac client data */
    free(guac_client_data);

    return 0;

}

int rdp_guac_client_handle_messages(guac_client* client) {

    rdp_guac_client_data* guac_client_data = (rdp_guac_client_data*) client->data;
    rdpInst* rdp_inst = guac_client_data->rdp_inst;
    rdpChannels* channels = guac_client_data->channels;

    int index;
    int max_fd, fd;
    void* read_fds[32];
    void* write_fds[32];
    int read_count = 0;
    int write_count = 0;

    fd_set rfds, wfds;

    /* get rdp fds */
    if (rdp_inst->rdp_get_fds(rdp_inst, read_fds, &read_count, write_fds, &write_count) != 0) {
        guac_client_log_error(client, "Unable to read RDP file descriptors.");
        return 1;
    }

    /* get channel fds */
    if (freerdp_channels_get_fds(channels, rdp_inst, read_fds, &read_count, write_fds, &write_count) != 0) {
        guac_client_log_error(client, "Unable to read RDP channel file descriptors.");
        return 1;
    }

    /* Construct read fd_set */
    max_fd = 0;
    FD_ZERO(&rfds);
    for (index = 0; index < read_count; index++) {
        fd = (int)(long) (read_fds[index]);
        if (fd > max_fd)
            max_fd = fd;
        FD_SET(fd, &rfds);
    }

    /* Construct write fd_set */
    FD_ZERO(&wfds);
    for (index = 0; index < write_count; index++) {
        fd = (int)(long) (write_fds[index]);
        if (fd > max_fd)
            max_fd = fd;
        FD_SET(fd, &wfds);
    }

    /* If no file descriptors, error */
    if (max_fd == 0) {
        guac_client_log_error(client, "No file descriptors");
        return 1;
    }

    /* Otherwise, wait for file descriptors given */
    if (select(max_fd + 1, &rfds, &wfds, NULL, NULL) == -1) {
        /* these are not really errors */
        if (!((errno == EAGAIN) ||
            (errno == EWOULDBLOCK) ||
            (errno == EINPROGRESS) ||
            (errno == EINTR))) /* signal occurred */
        {
            guac_client_log_error(client, "Error waiting for file descriptor.");
            return 1;
        }
    }

    /* check the libfreerdp fds */
    if (rdp_inst->rdp_check_fds(rdp_inst) != 0) {
        guac_client_log_error(client, "Error handling RDP file descriptors.");
        return 1;
    }

    /* check channel fds */
    if (freerdp_channels_check_fds(channels, rdp_inst) != 0) {
        guac_client_log_error(client, "Error handling RDP channel file descriptors.");
        return 1;
    }

    /* Success */
    return 0;

}

int rdp_guac_client_mouse_handler(guac_client* client, int x, int y, int mask) {

    rdp_guac_client_data* guac_client_data = (rdp_guac_client_data*) client->data;
    rdpInst* rdp_inst = guac_client_data->rdp_inst;

    /* If button mask unchanged, just send move event */
    if (mask == guac_client_data->mouse_button_mask)
        rdp_inst->rdp_send_input_mouse(rdp_inst, PTR_FLAGS_MOVE, x, y);

    /* Otherwise, send events describing button change */
    else {

        /* Mouse buttons which have JUST become released */
        int released_mask =  guac_client_data->mouse_button_mask & ~mask;

        /* Mouse buttons which have JUST become pressed */
        int pressed_mask  = ~guac_client_data->mouse_button_mask &  mask;

        /* Release event */
        if (released_mask & 0x07) {

            /* Calculate flags */
            int flags = 0;
            if (released_mask & 0x01) flags |= PTR_FLAGS_BUTTON1;
            if (released_mask & 0x02) flags |= PTR_FLAGS_BUTTON3;
            if (released_mask & 0x04) flags |= PTR_FLAGS_BUTTON2;

            rdp_inst->rdp_send_input_mouse(rdp_inst, flags, x, y);

        }

        /* Press event */
        if (pressed_mask & 0x07) {

            /* Calculate flags */
            int flags = PTR_FLAGS_DOWN;
            if (pressed_mask & 0x01) flags |= PTR_FLAGS_BUTTON1;
            if (pressed_mask & 0x02) flags |= PTR_FLAGS_BUTTON3;
            if (pressed_mask & 0x04) flags |= PTR_FLAGS_BUTTON2;
            if (pressed_mask & 0x08) flags |= PTR_FLAGS_WHEEL | 0x78;
            if (pressed_mask & 0x10) flags |= PTR_FLAGS_WHEEL | PTR_FLAGS_WHEEL_NEGATIVE | 0x88;

            /* Send event */
            rdp_inst->rdp_send_input_mouse(rdp_inst, flags, x, y);

        }

        /* Scroll event */
        if (pressed_mask & 0x18) {

            /* Down */
            if (pressed_mask & 0x08)
                rdp_inst->rdp_send_input_mouse(
                        rdp_inst,
                        PTR_FLAGS_WHEEL | 0x78,
                        x, y);

            /* Up */
            if (pressed_mask & 0x10)
                rdp_inst->rdp_send_input_mouse(
                        rdp_inst,
                        PTR_FLAGS_WHEEL | PTR_FLAGS_WHEEL_NEGATIVE | 0x88,
                        x, y);

        }


        guac_client_data->mouse_button_mask = mask;
    }

    return 0;
}

int rdp_guac_client_key_handler(guac_client* client, int keysym, int pressed) {

    rdp_guac_client_data* guac_client_data = (rdp_guac_client_data*) client->data;
    rdpInst* rdp_inst = guac_client_data->rdp_inst;

    /* If keysym can be in lookup table */
    if (keysym <= 0xFFFF) {

        /* Look up scancode */
        const guac_rdp_keymap* keymap = 
            &guac_rdp_keysym_scancode[(keysym & 0xFF00) >> 8][keysym & 0xFF];

        /* If defined, send event */
        if (keymap->scancode != 0)
            rdp_inst->rdp_send_input_scancode(
                    rdp_inst,
                    !pressed,
                    keymap->flags & KBD_FLAGS_EXTENDED,
                    keymap->scancode);
        else
            guac_client_log_info(client, "unmapped keysym: 0x%x", keysym);

    }

    return 0;
}
