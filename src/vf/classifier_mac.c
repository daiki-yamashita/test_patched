#include <unistd.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>

#include <rte_mbuf.h>
#include <rte_log.h>
#include <rte_cycles.h>
#include <rte_malloc.h>
#include <rte_memcpy.h>
#include <rte_random.h>
#include <rte_byteorder.h>
#include <rte_per_lcore.h>
#include <rte_eal.h>
#include <rte_launch.h>
#include <rte_hash.h>

#include "spp_vf.h"
#include "ringlatencystats.h"
#include "classifier_mac.h"

#define RTE_LOGTYPE_SPP_CLASSIFIER_MAC RTE_LOGTYPE_USER1

#ifdef RTE_MACHINE_CPUFLAG_SSE4_2
#include <rte_hash_crc.h>
#define DEFAULT_HASH_FUNC rte_hash_crc
#else
#include <rte_jhash.h>
#define DEFAULT_HASH_FUNC rte_jhash
#endif

/* number of classifier mac table entry */
#define NUM_CLASSIFIER_MAC_TABLE_ENTRY 128

/* interval that transmit burst packet, if buffer is not filled.
		nano second */
#define DRAIN_TX_PACKET_INTERVAL 100

/* mac address string(xx:xx:xx:xx:xx:xx) buffer size */
static const size_t ETHER_ADDR_STR_BUF_SZ =
		ETHER_ADDR_LEN * 2 + (ETHER_ADDR_LEN - 1) + 1;

/* classified data (destination port, target packets, etc) */
struct classified_data {
	int      if_no;
	uint8_t  tx_port;
	uint16_t num_pkt;
	struct rte_mbuf *pkts[MAX_PKT_BURST];
};

/* initialize classifier. */
static int
init_classifier(const struct spp_core_info *core_info,
		struct rte_hash **classifier_mac_table, struct classified_data *classified_data)
{
	int ret = -1;
	int i;
	struct ether_addr eth_addr;
	char mac_addr_str[ETHER_ADDR_STR_BUF_SZ];

	/* set hash creating parameters */
	struct rte_hash_parameters hash_params = {
			.name      = "classifier_mac_table",
			.entries   = NUM_CLASSIFIER_MAC_TABLE_ENTRY,
			.key_len   = sizeof(struct ether_addr),
			.hash_func = DEFAULT_HASH_FUNC,
			.hash_func_init_val = 0,
			.socket_id = rte_socket_id(),
	};

#ifdef RTE_MACHINE_CPUFLAG_SSE4_2
	RTE_LOG(DEBUG, SPP_CLASSIFIER_MAC, "Enabled SSE4.2. use crc hash.\n");
#else
	RTE_LOG(DEBUG, SPP_CLASSIFIER_MAC, "Disabled SSE4.2. use jenkins hash.\n");
#endif

	/* create classifier mac table (hash table) */
	*classifier_mac_table = rte_hash_create(&hash_params);
	if (unlikely(*classifier_mac_table == NULL)) {
		RTE_LOG(ERR, SPP_CLASSIFIER_MAC, "Cannot create classifier mac table\n");
		return -1;
	}

	/* populate the hash */
	for (i = 0; i < core_info->num_tx_port; i++) {
		rte_memcpy(&eth_addr, &core_info->tx_ports[i].mac_addr, ETHER_ADDR_LEN);

		/* add entry to classifier mac table */
		ret = rte_hash_add_key_data(*classifier_mac_table, (void*)&eth_addr, (void*)(long)i);
		if (unlikely(ret < 0)) {
			ether_format_addr(mac_addr_str, sizeof(mac_addr_str), &eth_addr);
			RTE_LOG(ERR, SPP_CLASSIFIER_MAC,
					"Cannot add entry to classifier mac table. "
					"ret=%d, mac_addr=%s\n", ret, mac_addr_str);
			rte_hash_free(*classifier_mac_table);
			*classifier_mac_table = NULL;
			return -1;
		}

		/* set value */
		classified_data[i].if_no   = i;
		classified_data[i].tx_port = core_info->tx_ports[i].dpdk_port;
		classified_data[i].num_pkt = 0;
	}

	return 0;
}

/* transimit packet to one destination. */
static inline void
transimit_packet(struct classified_data *classified_data)
{
	int i;
	uint16_t n_tx;

	/* set ringlatencystats */
	spp_ringlatencystats_add_time_stamp(classified_data->if_no,
			classified_data->pkts, classified_data->num_pkt);

	/* transimit packets */
	n_tx = rte_eth_tx_burst(classified_data->tx_port, 0,
			classified_data->pkts, classified_data->num_pkt);

	/* free cannnot transiit packets */
	if (unlikely(n_tx != classified_data->num_pkt)) {
		for (i = n_tx; i < classified_data->num_pkt; i++)
			rte_pktmbuf_free(classified_data->pkts[i]);
		RTE_LOG(DEBUG, SPP_CLASSIFIER_MAC,
				"drop packets(tx). num=%hu, dpdk_port=%hhu\n",
				classified_data->num_pkt - n_tx, classified_data->tx_port);
	}

	classified_data->num_pkt = 0;
}

/* classify packet by destination mac address,
		and transimit packet (conditional). */
static inline void
classify_packet(struct rte_mbuf **rx_pkts, uint16_t n_rx,
		struct rte_hash *classifier_mac_table, struct classified_data *classified_data)
{
	int ret;
	int i;
	struct ether_hdr *eth;
	struct classified_data *cd;
	void *lookup_data;
	char mac_addr_str[ETHER_ADDR_STR_BUF_SZ];

	for (i = 0; i < n_rx; i++) {
		eth = rte_pktmbuf_mtod(rx_pkts[i], struct ether_hdr *);

		/* find in table (by destination mac address)*/
		ret = rte_hash_lookup_data(classifier_mac_table,
				(const void*)&eth->d_addr, &lookup_data);
		if (unlikely(ret < 0)) {
			ether_format_addr(mac_addr_str, sizeof(mac_addr_str), &eth->d_addr);
			RTE_LOG(ERR, SPP_CLASSIFIER_MAC,
					"unknown mac address. ret=%d, mac_addr=%s\n", ret, mac_addr_str);
			rte_pktmbuf_free(rx_pkts[i]);
			continue;
		}

		/* set mbuf pointer to tx buffer */
		cd = classified_data + (long)lookup_data;
		cd->pkts[cd->num_pkt++] = rx_pkts[i];

		/* transimit packet, if buffer is filled */
		if (unlikely(cd->num_pkt == MAX_PKT_BURST))
			transimit_packet(cd);
	}
}

/* classifier(mac address) thread function. */
int
spp_classifier_mac_do(void *arg)
{
	int ret = -1;
	int i;
	int n_rx;
	struct spp_core_info *core_info = (struct spp_core_info *)arg;
	struct rte_mbuf *rx_pkts[MAX_PKT_BURST];

	struct rte_hash *classifier_mac_table = NULL;
	const int n_classified_data = core_info->num_tx_port;
	struct classified_data classified_data[n_classified_data];

	uint64_t cur_tsc, prev_tsc = 0;
	const uint64_t drain_tsc = (rte_get_tsc_hz() + US_PER_S - 1) /
			US_PER_S * DRAIN_TX_PACKET_INTERVAL;

	/* initialize */
	ret = init_classifier(core_info, &classifier_mac_table, classified_data);
	if (unlikely(ret != 0))
		return ret;

	/* to idle  */
	core_info->status = SPP_CORE_IDLE;
	while(likely(core_info->status == SPP_CORE_IDLE) ||
			likely(core_info->status == SPP_CORE_FORWARD)) {

		while(likely(core_info->status == SPP_CORE_FORWARD)) {
			/* drain tx packets, if buffer is not filled for interval */
			cur_tsc = rte_rdtsc();
			if (unlikely(cur_tsc - prev_tsc > drain_tsc)) {
				for (i = 0; i < n_classified_data; i++) {
					if (unlikely(classified_data[i].num_pkt != 0))
						transimit_packet(&classified_data[i]);
				}
				prev_tsc = cur_tsc;
			}

			/* retrieve packets */
			n_rx = rte_eth_rx_burst(core_info->rx_ports[0].dpdk_port, 0,
					rx_pkts, MAX_PKT_BURST);
			if (unlikely(n_rx == 0))
				continue;

			/* classify and transimit (filled) */
			classify_packet(rx_pkts, n_rx, classifier_mac_table, classified_data);
		}
	}

	/* uninitialize */
	if (classifier_mac_table != NULL) {
		rte_hash_free(classifier_mac_table);
		classifier_mac_table = NULL;
	}
	core_info->status = SPP_CORE_STOP;

	return 0;
}
