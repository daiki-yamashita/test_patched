/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2015-2016 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <limits.h>

#include <rte_cycles.h>
#include <rte_malloc.h>
#include <rte_memzone.h>

#include "args.h"
#include "common.h"
#include "init.h"

#define CLIENT_QUEUE_RINGSIZE 128

/* array of info/queues for clients */
struct client *clients;

/* The mbuf pool for packet rx */
static struct rte_mempool *pktmbuf_pool;

/* the port details */
struct port_info *ports;

/**
 * Initialise the mbuf pool for packet reception for the NIC, and any other
 * buffer pools needed by the app - currently none.
 */
static int
init_mbuf_pools(void)
{
	const unsigned int num_mbufs = (num_clients * MBUFS_PER_CLIENT)
		+ (ports->num_ports * MBUFS_PER_PORT);

	/*
	 * don't pass single-producer/single-consumer flags to mbuf create as
	 * it seems faster to use a cache instead
	 */
	RTE_LOG(DEBUG, APP, "Creating mbuf pool '%s' [%u mbufs] ...\n",
		PKTMBUF_POOL_NAME, num_mbufs);

	if (rte_eal_process_type() == RTE_PROC_SECONDARY) {
		pktmbuf_pool = rte_mempool_lookup(PKTMBUF_POOL_NAME);
		if (pktmbuf_pool == NULL)
			rte_exit(EXIT_FAILURE,
				"Cannot get mempool for mbufs\n");
	} else {
		pktmbuf_pool = rte_mempool_create(PKTMBUF_POOL_NAME, num_mbufs,
			MBUF_SIZE, MBUF_CACHE_SIZE,
			sizeof(struct rte_pktmbuf_pool_private),
			rte_pktmbuf_pool_init, NULL, rte_pktmbuf_init, NULL,
			rte_socket_id(), NO_FLAGS);
	}

	return (pktmbuf_pool == NULL); /* 0  on success */
}

/**
 * Set up the DPDK rings which will be used to pass packets, via
 * pointers, between the multi-process server and client processes.
 * Each client needs one RX queue.
 */
static int
init_shm_rings(void)
{
	const unsigned int ringsize = CLIENT_QUEUE_RINGSIZE;
	unsigned int socket_id;
	const char *q_name;
	unsigned int i;

	clients = rte_malloc("client details",
		sizeof(*clients) * num_clients, 0);
	if (clients == NULL)
		rte_exit(EXIT_FAILURE,
			"Cannot allocate memory for client program details\n");

	for (i = 0; i < num_clients; i++) {
		/* Create an RX queue for each client */
		socket_id = rte_socket_id();
		q_name = get_rx_queue_name(i);
		if (rte_eal_process_type() == RTE_PROC_SECONDARY) {
			clients[i].rx_q = rte_ring_lookup(q_name);
		} else {
			clients[i].rx_q = rte_ring_create(q_name,
				ringsize, socket_id,
				/* single prod, single cons */
				RING_F_SP_ENQ | RING_F_SC_DEQ);
		}
		if (clients[i].rx_q == NULL)
			rte_exit(EXIT_FAILURE,
				"Cannot create rx ring queue for client %u\n",
				i);
	}

	return 0;
}

/**
 * Main init function for the multi-process server app,
 * calls subfunctions to do each stage of the initialisation.
 */
int
init(int argc, char *argv[])
{
	int retval;
	const struct rte_memzone *mz;
	uint16_t count, total_ports;

	/* init EAL, parsing EAL args */
	retval = rte_eal_init(argc, argv);
	if (retval < 0)
		return -1;

	argc -= retval;
	argv += retval;

	/* get total number of ports */
	total_ports = rte_eth_dev_count();

	/* set up array for port data */
	if (rte_eal_process_type() == RTE_PROC_SECONDARY) {
		mz = rte_memzone_lookup(MZ_PORT_INFO);
		if (mz == NULL)
			rte_exit(EXIT_FAILURE,
				"Cannot get port info structure\n");
		ports = mz->addr;
	} else { /* RTE_PROC_PRIMARY */
		mz = rte_memzone_reserve(MZ_PORT_INFO, sizeof(*ports),
			rte_socket_id(), NO_FLAGS);
		if (mz == NULL)
			rte_exit(EXIT_FAILURE,
				"Cannot reserve memory zone for port information\n");
		memset(mz->addr, 0, sizeof(*ports));
		ports = mz->addr;
	}

	/* parse additional, application arguments */
	retval = parse_app_args(total_ports, argc, argv);
	if (retval != 0)
		return -1;

	/* initialise mbuf pools */
	retval = init_mbuf_pools();
	if (retval != 0)
		rte_exit(EXIT_FAILURE, "Cannot create needed mbuf pools\n");

	/* now initialise the ports we will use */
	if (rte_eal_process_type() == RTE_PROC_PRIMARY) {
		for (count = 0; count < ports->num_ports; count++) {
			retval = init_port(ports->id[count], pktmbuf_pool);
			if (retval != 0)
				rte_exit(EXIT_FAILURE,
					"Cannot initialise port %d\n", count);
		}
	}
	check_all_ports_link_status(ports, ports->num_ports, (~0x0));

	/* initialise the client queues/rings for inter-eu comms */
	init_shm_rings();

	return 0;
}
