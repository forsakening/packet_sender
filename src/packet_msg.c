/*
 * License: MIT
 *
 * Copyright (c) 2017-2020 James Bensley.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */



#include "packet_msg.h"



void *msg_init(void* thd_opt_p) {

    struct thd_opt *thd_opt = thd_opt_p;


    // Save the thread tid
    pid_t thread_id;
    thread_id = syscall(SYS_gettid);
    thd_opt->thd_id = thread_id;
    

    // Set the thread cancel type and register the cleanup handler
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
    pthread_cleanup_push(thd_cleanup, thd_opt_p);


    if (thd_opt->verbose) {
        if (thd_opt->affinity >= 0) {
            printf(
                "Worker thread %" PRIu32 " started, bound to CPU %" PRId32 "\n",
                thd_opt->thd_id, thd_opt->affinity
            );
        } else {
            printf("Worker thread %" PRIu32 " started\n", thd_opt->thd_id);
        }
    }

    if (msg_sock(thd_opt) != EXIT_SUCCESS) {
        pthread_exit((void*)EXIT_FAILURE);
    }

    if (thd_opt->sk_mode == SKT_RX) {
        msg_rx(thd_opt_p);
    } else if (thd_opt->sk_mode == SKT_TX) {
        msg_tx(thd_opt_p);
    }


    pthread_cleanup_pop(0);
    return NULL;

}



void msg_rx(struct thd_opt *thd_opt) {

    int32_t rx_bytes = 0;

    struct msghdr msg_hdr;
    struct iovec iov;
    memset(&msg_hdr, 0, sizeof(msg_hdr));
    memset(&iov, 0, sizeof(iov));

    iov.iov_base = thd_opt->rx_buffer;
    iov.iov_len = thd_opt->frame_sz;

    msg_hdr.msg_name = NULL;
    msg_hdr.msg_iov = &iov;
    msg_hdr.msg_iovlen = 1;
    msg_hdr.msg_control = NULL;
    msg_hdr.msg_controllen = 0;

    thd_opt->started = 1;


    while(1) {

        rx_bytes = recvmsg(thd_opt->sock, &msg_hdr, 0);
        
        if (rx_bytes == -1) {
            thd_opt->sk_err += 1;
        } else {
            thd_opt->rx_bytes += rx_bytes;
            thd_opt->rx_frms += 1;            
        }

    }

}



int32_t msg_sock(struct thd_opt *thd_opt) {

    // Create a raw socket
    thd_opt->sock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
   
    if (thd_opt->sock == -1) {
        tperror(thd_opt, "Can't create AF_PACKET socket");
        return EXIT_FAILURE;
    }


    // Bind socket to interface
    if (sock_op(S_O_BIND, thd_opt) == -1) {
        tperror(thd_opt, "Can't bind to AF_PACKET socket");
        return EXIT_FAILURE;
    }


    // Bypass the kernel qdisc layer and push frames directly to the driver
    if (sock_op(S_O_QDISC, thd_opt) == -1) {
        tperror(thd_opt, "Can't enable QDISC bypass on socket");
        return EXIT_FAILURE;
    }


    // Enable Tx ring to skip over malformed frames
    if (thd_opt->sk_mode == SKT_TX) {

        if (sock_op(S_O_LOSSY, thd_opt) == -1) {
            tperror(thd_opt, "Can't enable PACKET_LOSS on socket");
            return EXIT_FAILURE;
        }
        
    }


    // Set the socket Rx timestamping settings
    if (sock_op(S_O_TS, thd_opt) == -1) {
        tperror(thd_opt, "Can't set socket Rx timestamp source");
    }


    // Join this socket to the fanout group
    if (thd_opt->thd_nr > 1) {

        if (sock_op(S_O_FANOUT, thd_opt) < 0) {
            tperror(thd_opt, "Can't configure socket fanout");
            return EXIT_FAILURE;
        } else {
            if (thd_opt->verbose)
                printf("%" PRIu32 ":Joint fanout group %" PRIu32 "\n",
                       thd_opt->thd_id, thd_opt->fanout_grp);
        }

    }


    return EXIT_SUCCESS;

}



void msg_tx(struct thd_opt *thd_opt) {

    int32_t tx_bytes;

    struct msghdr msg_hdr;
    struct iovec iov;
    memset(&msg_hdr, 0, sizeof(msg_hdr));
    memset(&iov, 0, sizeof(iov));

    iov.iov_base = thd_opt->tx_buffer;
    iov.iov_len = thd_opt->frame_sz;

    msg_hdr.msg_iov = &iov;
    msg_hdr.msg_iovlen = 1;

    thd_opt->started = 1;

    while (1) {

        tx_bytes = sendmsg(thd_opt->sock, &msg_hdr, 0);

        if (tx_bytes == -1) {
            thd_opt->sk_err += 1;
        } else {
            thd_opt->tx_bytes += tx_bytes;
            thd_opt->tx_frms += 1;
        }

    }

}