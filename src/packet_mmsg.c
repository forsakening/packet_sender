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


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <linux/udp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "packet_mmsg.h"



void *mmsg_init(void* thd_opt_p) {

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

    if (mmsg_sock(thd_opt) != EXIT_SUCCESS) {
        pthread_exit((void*)EXIT_FAILURE);
    }

    if (thd_opt->sk_mode == SKT_RX) {
        mmsg_rx(thd_opt_p);
    } else if (thd_opt->sk_mode == SKT_TX) {
        mmsg_tx(thd_opt_p);
    }


    pthread_cleanup_pop(0);
    return NULL;

}



void mmsg_rx(struct thd_opt *thd_opt) {

    int32_t rx_frames = 0;

    struct mmsghdr mmsg_hdr[thd_opt->msgvec_vlen];
    struct iovec iov[thd_opt->msgvec_vlen];
    memset(mmsg_hdr, 0, sizeof(mmsg_hdr));
    memset(iov, 0, sizeof(iov));

    thd_opt->started = 1;

    for (uint32_t i = 0; i < thd_opt->msgvec_vlen; i += 1) {
        iov[i].iov_base = thd_opt->rx_buffer;
        iov[i].iov_len = thd_opt->frame_sz;
        mmsg_hdr[i].msg_hdr.msg_iov = &iov[i];
        mmsg_hdr[i].msg_hdr.msg_iovlen = 1;
        mmsg_hdr[i].msg_hdr.msg_name = NULL;
        mmsg_hdr[i].msg_hdr.msg_control = NULL;
        mmsg_hdr[i].msg_hdr.msg_controllen = 0;
    }

    while(1) {

        /*
         recvmmsg() returns the number of frames sent or -1 on error.
         If some frames have been received in the vector and then an
         error occurs, recvmmsg returns -1. For the received frames
         msg_len is updated.
        */
        rx_frames = recvmmsg(
            thd_opt->sock, mmsg_hdr, thd_opt->msgvec_vlen, 0, NULL
        );
        
        if (rx_frames == -1) {
            thd_opt->sk_err += 1;
        }

        for (uint32_t i = 0; i < thd_opt->msgvec_vlen; i+= 1) {
            if (mmsg_hdr[i].msg_len > 0) {
                thd_opt->rx_bytes += mmsg_hdr[i].msg_len;
                thd_opt->rx_frms += 1;
            }
        }

    }

}



int32_t mmsg_sock(struct thd_opt *thd_opt) {


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


    // Increase the socket Tx queue size so that the entire msg vector can fit
    // into the socket Tx/Rx queue. The Kernel will double the value provided
    // to allow for sk_buff overhead:
    if (sock_op(S_O_QLEN, thd_opt) == -1) {
        tperror(thd_opt, "Can't change the socket Tx queue length");
        return EXIT_FAILURE;
    }



    return EXIT_SUCCESS;

}

// 计算校验和的函数
uint16_t csum(uint16_t *ptr, int nbytes) {
    long sum;
    uint16_t oddbyte;
    short answer;

    sum = 0;
    while (nbytes > 1) {
        sum += *ptr++;
        nbytes -= 2;
    }
    if (nbytes == 1) {
        oddbyte = 0;
        *((u_char *)&oddbyte) = *(u_char *)ptr;
        sum += oddbyte;
    }

    sum = (sum >> 16) + (sum & 0xffff);
    sum = sum + (sum >> 16);
    answer = (short)~sum;

    return answer;
}

// 构建 UDP 报文的函数
void build_udp_packet(char *buffer, int size, uint16_t sport, uint16_t dport, uint32_t saddr, uint32_t daddr) {
    struct iphdr *ip_header = (struct iphdr *)buffer;
    struct udphdr *udp_header = (struct udphdr *)(buffer + sizeof(struct iphdr));

    // 填充 IP 头部
    ip_header->ihl = 5; // IP 头部长度为 20 字节
    ip_header->version = 4; // IP 版本号
    ip_header->tos = 0; // 服务类型
    ip_header->tot_len = htons(size); // 总长度
    ip_header->id = htons(rand()); // 标识
    ip_header->frag_off = 0; // 标志和片偏移
    ip_header->ttl = 64; // 生存时间
    ip_header->protocol = IPPROTO_UDP; // 协议类型
    ip_header->saddr = saddr; // 源地址
    ip_header->daddr = daddr; // 目的地址
    ip_header->check = 0; // 校验和，将被计算

    // 填充 UDP 头部
    udp_header->source = htons(sport); // 源端口
    udp_header->dest = htons(dport); // 目的端口
    udp_header->len = htons(size - sizeof(struct iphdr)); // 长度
    udp_header->check = 0; // 校验和，将被计算

    // 计算 UDP 校验和
    uint32_t udp_data[2] = {(uint32_t)(udp_header->source), (uint32_t)(udp_header->dest)};
    uint32_t *udp_ptr = (uint32_t *)udp_data;
    udp_header->check = csum((uint16_t *)udp_ptr, 2 * sizeof(uint32_t)) << 16;
    udp_header->check += csum((uint16_t *)&ip_header->saddr, sizeof(ip_header->saddr));
    udp_header->check += csum((uint16_t *)&ip_header->daddr, sizeof(ip_header->daddr));
    udp_header->check += htons(size - sizeof(struct iphdr));
    udp_header->check = (udp_header->check >> 16) + (udp_header->check & 0xffff);
    udp_header->check = ~udp_header->check;

    // 计算 IP 校验和
    ip_header->check = csum((uint16_t *)ip_header, sizeof(struct iphdr));

	printf("[build_udp_packet] Sport %d  Dport %d \n", sport, dport);
}

int get_eth0_ip_mac(uint8_t *mac_address, uint32_t *ip) {
	const char* interface_name = "eth0"; // 指定网卡的名称

    struct ifreq ifr;
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);

    if (sockfd == -1) {
        perror("socket");
        return -1;
    }

    strncpy(ifr.ifr_name, interface_name, IFNAMSIZ - 1);
    ifr.ifr_name[IFNAMSIZ - 1] = '\0';

    // 获取 IP 地址
    if (ioctl(sockfd, SIOCGIFADDR, &ifr) == -1) {
        perror("ioctl");
        close(sockfd);
        return -1;
    }

    struct sockaddr_in* addr = (struct sockaddr_in*)&ifr.ifr_addr;
    //char ip_address[INET_ADDRSTRLEN];
    //inet_ntop(AF_INET, &(addr->sin_addr), ip_address, INET_ADDRSTRLEN);
    //printf("IP Address: %x\n", addr->sin_addr.s_addr);
	*ip = addr->sin_addr.s_addr;

    // 获取 MAC 地址
    if (ioctl(sockfd, SIOCGIFHWADDR, &ifr) == -1) {
        perror("ioctl");
        close(sockfd);
        return -1;
    }

    memcpy(mac_address, ifr.ifr_hwaddr.sa_data, 6);
    //printf("MAC Address: %02X:%02X:%02X:%02X:%02X:%02X\n",
    //       mac_address[0], mac_address[1], mac_address[2],
    //       mac_address[3], mac_address[4], mac_address[5]);

    close(sockfd);
    return 0;
}

void make_udp_packet(uint16_t sport, uint16_t dport, uint8_t *ether_packet, int ether_len) {
	uint8_t *dst_mac = ether_packet;
	uint8_t *src_mac = dst_mac + 6;
	uint8_t *type = src_mac + 6;
	dst_mac[0] = 0xf2;
	dst_mac[1] = 0x01;
	dst_mac[2] = 0x0a;
	dst_mac[3] = 0x32;
	dst_mac[4] = 0xc8;
	dst_mac[5] = 0x01;

	//src_mac[0] = 0xfa;
	//src_mac[1] = 0x16;
	//src_mac[2] = 0x90;
	//src_mac[3] = 0x56;
	//src_mac[4] = 0x40;
	//src_mac[5] = 0x4e;

	uint32_t src_ip = 0;
	get_eth0_ip_mac(src_mac, &src_ip);
	

	type[0] = 0x08;
	type[1] = 0x00;

	build_udp_packet((char*)(ether_packet+14), ether_len-14, sport, dport, src_ip, 0x6482320a);	
}

void mmsg_tx(struct thd_opt *thd_opt) {

    int32_t tx_frames = 0;

    struct mmsghdr mmsg_hdr[thd_opt->msgvec_vlen];
    struct iovec iov[thd_opt->msgvec_vlen];
    memset(mmsg_hdr, 0, sizeof(mmsg_hdr));
    memset(iov, 0, sizeof(iov));

	uint8_t* pkt_buffers = (uint8_t*)malloc(thd_opt->msgvec_vlen * thd_opt->frame_sz);
	if (NULL == pkt_buffers) {
		printf("pkt_buffers malloc error\n");
		return;
	}
	uint8_t* pkt_buffer = NULL;

    thd_opt->started = 1;
	int src_port,dst_port;
	int base = 12345 + thd_opt->thread_num + 10;
    for (uint32_t i = 0; i < thd_opt->msgvec_vlen; i += 1) {
		pkt_buffer = pkt_buffers + (i * thd_opt->frame_sz);

		src_port = 10000 + i % 50000;
		dst_port = base + i / 50000;

		make_udp_packet(src_port, dst_port, pkt_buffer, thd_opt->frame_sz);
		iov[i].iov_base = pkt_buffer;
        iov[i].iov_len = thd_opt->frame_sz;
	
        //iov[i].iov_base = thd_opt->tx_buffer;
        //iov[i].iov_len = thd_opt->frame_sz;
        
        mmsg_hdr[i].msg_hdr.msg_iov = &iov[i];
        mmsg_hdr[i].msg_hdr.msg_iovlen = 1;
    }

	int loop_cnt = 0;
	int last_cnts = 0;
	if (thd_opt->msgvec_vlen > 1024) {
		last_cnts = thd_opt->msgvec_vlen % 1024;
		loop_cnt = thd_opt->msgvec_vlen / 1024;
	}

	printf("LoopCnt %d LastCnt %d \n", loop_cnt, last_cnts);
	sleep(1);

	
	int index = 0;
	struct mmsghdr *msgvec = NULL;
	int msglen = 0;
	int i ;
    while (1) {
        /*
         When using sendmmsg() to send a batch of frames, unlike PACKET_MMAP,
         an error is only returned if no datagrams were sent, rather than the
         PACKET_MMAP approach to return an error if any one frame failed to
         send.

         sendmmsg() returns the number of frames sent or -1 and sets errno if
         0 frames were sent.
        */

		msgvec = mmsg_hdr + index * 1024;
		msglen = 1024;
		if (index >= loop_cnt) {
			msglen = last_cnts;
			index = 0;
		}else {
			index++;
		}
		
		
		
        tx_frames = sendmmsg(thd_opt->sock, msgvec, msglen, 0);

        if (tx_frames == -1) {
            thd_opt->sk_err += 1;
        } else {
            ///// TODO
            ///// No need to check frame size if all frames are the same size?
            for (i = 0; i < msglen; i+= 1) {
                thd_opt->tx_bytes += mmsg_hdr[i].msg_len;
            }
            thd_opt->tx_frms += tx_frames;
        }

    }

}