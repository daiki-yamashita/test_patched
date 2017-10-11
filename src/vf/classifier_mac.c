#include <unistd.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>

#include <rte_common.h>
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

/* number of classifier information (reference/update) */
#define NUM_CLASSIFIER_MAC_INFO 2

/* interval that wait untill change update index
		micro second */
#define CHANGE_UPDATE_INDEX_WAIT_INTERVAL 10

/* interval that transmit burst packet, if buffer is not filled.
		nano second */
#define DRAIN_TX_PACKET_INTERVAL 100

/* hash table name buffer size
	[reson for value]
		in dpdk's lib/librte_hash/rte_cuckoo_hash.c
			snprintf(ring_name, sizeof(ring_name), "HT_%s", params->name);
			snprintf(hash_name, sizeof(hash_name), "HT_%s", params->name);
		ring_name buffer size is RTE_RING_NAMESIZE
		hash_name buffer size is RTE_HASH_NAMESIZE */
static const size_t HASH_TABLE_NAME_BUF_SZ =
		((RTE_HASH_NAMESIZE < RTE_RING_NAMESIZE) ? 
		RTE_HASH_NAMESIZE: RTE_RING_NAMESIZE) - 3;

/* mac address string(xx:xx:xx:xx:xx:xx) buffer size */
static const size_t ETHER_ADDR_STR_BUF_SZ =
		ETHER_ADDR_LEN * 2 + (ETHER_ADDR_LEN - 1) + 1;

/* classified data (destination port, target packets, etc) */
struct classified_data {
	enum port_type  if_type;
	int             if_no;
	int             if_no_global;
	uint8_t         tx_port;
	uint16_t        num_pkt;
	struct rte_mbuf *pkts[MAX_PKT_BURST];
};

/* classifier information */
struct classifier_mac_info {
	struct rte_hash *classifier_table;
	int num_active_classified;
	int active_classifieds[RTE_MAX_ETHPORTS];
	int default_classified;
};

/* classifier management information */
struct classifier_mac_mng_info {
	struct classifier_mac_info info[NUM_CLASSIFIER_MAC_INFO];
	volatile int ref_index;
	volatile int upd_index;
	struct classified_data classified_data[RTE_MAX_ETHPORTS];
};

/* classifier information per lcore */
static struct classifier_mac_mng_info g_classifier_mng_info[RTE_MAX_LCORE];

/* hash table count. use to make hash table name.
	[reason for value]
		it is incremented at the time of use, 
		but since we want to start at 0. */
static rte_atomic16_t g_hash_table_count = RTE_ATOMIC16_INIT(0xff);

static inline int
is_used_mng_info(const struct classifier_mac_mng_info *mng_info)
{
	return (mng_info != NULL && mng_info->info[0].classifier_table != NULL);
}

/* initialize classifier information. */
static int
init_classifier_info(struct classifier_mac_info *classifier_info,
		const struct spp_core_info *core_info)
{
	int ret = -1;
	int i;
	struct rte_hash **classifier_table = &classifier_info->classifier_table;
	struct ether_addr eth_addr;
	char mac_addr_str[ETHER_ADDR_STR_BUF_SZ];

	rte_hash_reset(*classifier_table);
	classifier_info->num_active_classified = 0;
	classifier_info->default_classified = -1;

	for (i = 0; i < core_info->num_tx_port; i++) {
		if (core_info->tx_ports[i].mac_addr == 0) {
			continue;
		}

		/* store active tx_port that associate with mac address */
		classifier_info->active_classifieds[classifier_info->
				num_active_classified++] = i;

		/* store default classified */
		if (unlikely(core_info->tx_ports[i].mac_addr ==
				SPP_CONFIG_DEFAULT_CLASSIFIED_DMY_ADDR)) {
			classifier_info->default_classified = i;
			RTE_LOG(INFO, SPP_CLASSIFIER_MAC, "default classified. "
					"if_type=%d, if_no=%d, dpdk_port=%d\n",
					core_info->tx_ports[i].if_type,
					core_info->tx_ports[i].if_no,
					core_info->tx_ports[i].dpdk_port);
			continue;
		}

		/* add entry to classifier mac table */
		rte_memcpy(&eth_addr, &core_info->tx_ports[i].mac_addr, ETHER_ADDR_LEN);
		ether_format_addr(mac_addr_str, sizeof(mac_addr_str), &eth_addr);

		ret = rte_hash_add_key_data(*classifier_table,
				(void*)&eth_addr, (void*)(long)i);
		if (unlikely(ret < 0)) {
			RTE_LOG(ERR, SPP_CLASSIFIER_MAC,
					"Cannot add entry to classifier mac table. "
					"ret=%d, mac_addr=%s\n", ret, mac_addr_str);
			rte_hash_free(*classifier_table);
			*classifier_table = NULL;
			return -1;
		}

		RTE_LOG(INFO, SPP_CLASSIFIER_MAC, "Add entry to classifier mac table. "
				"mac_addr=%s, if_type=%d, if_no=%d, dpdk_port=%d\n",
				mac_addr_str, 
				core_info->tx_ports[i].if_type, 
				core_info->tx_ports[i].if_no, 
				core_info->tx_ports[i].dpdk_port);
	}

	return 0;
}

/* initialize classifier. */
static int
init_classifier(const struct spp_core_info *core_info,
		struct classifier_mac_mng_info *classifier_mng_info, 
		struct classified_data *classified_data)
{
	int ret = -1;
	int i;
	char hash_table_name[HASH_TABLE_NAME_BUF_SZ];

	struct rte_hash **classifier_mac_table = NULL;

	memset(classifier_mng_info, 0, sizeof(struct classifier_mac_mng_info));
	classifier_mng_info->ref_index = 0;
	classifier_mng_info->upd_index = classifier_mng_info->ref_index + 1;

#ifdef RTE_MACHINE_CPUFLAG_SSE4_2
	RTE_LOG(DEBUG, SPP_CLASSIFIER_MAC, "Enabled SSE4.2. use crc hash.\n");
#else
	RTE_LOG(DEBUG, SPP_CLASSIFIER_MAC, "Disabled SSE4.2. use jenkins hash.\n");
#endif

	for (i = 0; i < NUM_CLASSIFIER_MAC_INFO; ++i) {

		classifier_mac_table = &classifier_mng_info->info[i].classifier_table;

		/* make hash table name(require uniqueness between processes) */
		sprintf(hash_table_name, "cmtab_%07x%02hx%x",
				getpid(), rte_atomic16_add_return(&g_hash_table_count, 1), i);

		RTE_LOG(INFO, SPP_CLASSIFIER_MAC, "Create table. name=%s, bufsz=%lu\n",
				hash_table_name, HASH_TABLE_NAME_BUF_SZ);

		/* set hash creating parameters */
		struct rte_hash_parameters hash_params = {
				.name      = hash_table_name,
				.entries   = NUM_CLASSIFIER_MAC_TABLE_ENTRY,
				.key_len   = sizeof(struct ether_addr),
				.hash_func = DEFAULT_HASH_FUNC,
				.hash_func_init_val = 0,
				.socket_id = rte_socket_id(),
		};

		/* create classifier mac table (hash table) */
		*classifier_mac_table = rte_hash_create(&hash_params);
		if (unlikely(*classifier_mac_table == NULL)) {
			RTE_LOG(ERR, SPP_CLASSIFIER_MAC, "Cannot create classifier mac table. "
					"name=%s\n", hash_table_name);
			return -1;
		}
	}

	/* populate the classifier information at reference */
	ret = init_classifier_info(&classifier_mng_info->
			info[classifier_mng_info->ref_index], core_info);
	if (unlikely(ret != 0)) {
		RTE_LOG(ERR, SPP_CLASSIFIER_MAC,
				"Cannot initialize classifer mac table. ret=%d\n", ret);
		return -1;
	}

	/* store ports information */
	for (i = 0; i < core_info->num_tx_port; i++) {
		classified_data[i].if_type      = core_info->tx_ports[i].if_type;
		classified_data[i].if_no        = i;
		classified_data[i].if_no_global = core_info->tx_ports[i].if_no;
		classified_data[i].tx_port      = core_info->tx_ports[i].dpdk_port;
		classified_data[i].num_pkt      = 0;
	}

	return 0;
}

/* uninitialize classifier. */
static void
uninit_classifier(struct classifier_mac_mng_info *classifier_mng_info)
{
	int i;

	for (i = 0; i < NUM_CLASSIFIER_MAC_INFO; ++i) {
		if (classifier_mng_info->info[i].classifier_table != NULL){
			rte_hash_free(classifier_mng_info->info[i].classifier_table);
			classifier_mng_info->info[i].classifier_table = NULL;
		}
	}
}

/* transmit packet to one destination. */
static inline void
transmit_packet(struct classified_data *classified_data)
{
	int i;
	uint16_t n_tx;

#ifdef SPP_RINGLATENCYSTATS_ENABLE
	if (classified_data->if_type == RING)
		/* if tx-if is ring, set ringlatencystats */
		spp_ringlatencystats_add_time_stamp(classified_data->if_no,
				classified_data->pkts, classified_data->num_pkt);
#endif

	/* transmit packets */
	n_tx = rte_eth_tx_burst(classified_data->tx_port, 0,
			classified_data->pkts, classified_data->num_pkt);

	/* free cannnot transmit packets */
	if (unlikely(n_tx != classified_data->num_pkt)) {
		for (i = n_tx; i < classified_data->num_pkt; i++)
			rte_pktmbuf_free(classified_data->pkts[i]);
		RTE_LOG(DEBUG, SPP_CLASSIFIER_MAC,
				"drop packets(tx). num=%hu, dpdk_port=%hhu\n",
				classified_data->num_pkt - n_tx, classified_data->tx_port);
	}

	classified_data->num_pkt = 0;
}

/* set mbuf pointer to tx buffer
	and transmit packet, if buffer is filled */
static inline void
push_packet(struct rte_mbuf *pkt, struct classified_data *classified_data)
{
	classified_data->pkts[classified_data->num_pkt++] = pkt;

	/* transmit packet, if buffer is filled */
	if (unlikely(classified_data->num_pkt == MAX_PKT_BURST)) {
		RTE_LOG(DEBUG, SPP_CLASSIFIER_MAC,
				"transimit packets (buffer is filled). "
				"if_type=%d, if_no={%d,%d}, tx_port=%hhd, num_pkt=%hu\n",
				classified_data->if_type,
				classified_data->if_no_global,
				classified_data->if_no,
				classified_data->tx_port,
				classified_data->num_pkt);
		transmit_packet(classified_data);
	}
}

/* handle L2 multicast(include broadcast) packet */
static inline void
handle_l2multicast_packet(struct rte_mbuf *pkt,
		struct classifier_mac_info *classifier_info,
		struct classified_data *classified_data)
{
	int i;

	if (unlikely(classifier_info->num_active_classified == 0)) {
		RTE_LOG(ERR, SPP_CLASSIFIER_MAC, "No mac address.(l2multicast packet)\n");
		rte_pktmbuf_free(pkt);
		return;
	}

	rte_mbuf_refcnt_update(pkt, classifier_info->num_active_classified);

	for (i= 0; i < classifier_info->num_active_classified; i++) {
		push_packet(pkt, classified_data + 
				(long)classifier_info->active_classifieds[i]);
	}
}

/* classify packet by destination mac address,
		and transmit packet (conditional). */
static inline void
classify_packet(struct rte_mbuf **rx_pkts, uint16_t n_rx,
		struct classifier_mac_info *classifier_info, 
		struct classified_data *classified_data)
{
	int ret;
	int i;
	struct ether_hdr *eth;
	void *lookup_data;
	char mac_addr_str[ETHER_ADDR_STR_BUF_SZ];

	for (i = 0; i < n_rx; i++) {
		eth = rte_pktmbuf_mtod(rx_pkts[i], struct ether_hdr *);

		/* find in table (by destination mac address)*/
		ret = rte_hash_lookup_data(classifier_info->classifier_table,
				(const void*)&eth->d_addr, &lookup_data);
		if (ret < 0) {
			/* L2 multicast(include broadcast) ? */
			if (unlikely(is_multicast_ether_addr(&eth->d_addr))) {
				RTE_LOG(DEBUG, SPP_CLASSIFIER_MAC,
						"multicast mac address.\n");
				handle_l2multicast_packet(rx_pkts[i],
						classifier_info, classified_data);
				continue;
			}

			/* if no default, drop packet */
			if (unlikely(classifier_info->default_classified == -1)) {
				ether_format_addr(mac_addr_str,
						sizeof(mac_addr_str), &eth->d_addr);
				RTE_LOG(ERR, SPP_CLASSIFIER_MAC,
						"unknown mac address. "
						"ret=%d, mac_addr=%s\n",
						ret, mac_addr_str);
				rte_pktmbuf_free(rx_pkts[i]);
				continue;
			}

			/* to default classifed */
			RTE_LOG(DEBUG, SPP_CLASSIFIER_MAC,
					"to default classified.\n");
			lookup_data = (void *)(long)classifier_info->default_classified;
		}

		/* set mbuf pointer to tx buffer
			and transmit packet, if buffer is filled */
		push_packet(rx_pkts[i], classified_data + (long)lookup_data);
	}
}

/* change update index at classifier management information */
static inline void
change_update_index(struct classifier_mac_mng_info *classifier_mng_info, unsigned int lcore_id)
{
	if (unlikely(classifier_mng_info->ref_index == 
			classifier_mng_info->upd_index)) {
		RTE_LOG(DEBUG, SPP_CLASSIFIER_MAC,
				"Core[%u] Change update index.\n", lcore_id);
		classifier_mng_info->upd_index = 
				(classifier_mng_info->upd_index + 1) % 
				NUM_CLASSIFIER_MAC_INFO;
	}
}

/* classifier(mac address) initialize globals. */
int
spp_classifier_mac_init(void)
{
	memset(g_classifier_mng_info, 0, sizeof(g_classifier_mng_info));

	return 0;
}

/* classifier(mac address) update component info. */
int
spp_classifier_mac_update(struct spp_core_info *core_info)
{
	int ret = -1;
	unsigned int lcore_id = core_info->lcore_id;

	struct classifier_mac_mng_info *classifier_mng_info =
			g_classifier_mng_info + lcore_id;

	struct classifier_mac_info *classifier_info =
			classifier_mng_info->info + classifier_mng_info->upd_index;

	RTE_LOG(INFO, SPP_CLASSIFIER_MAC,
			"Core[%u] Start update component.\n", lcore_id);

	/* initialize update side classifier information */
	ret = init_classifier_info(classifier_info, core_info);
	if (unlikely(ret != 0)) {
		RTE_LOG(ERR, SPP_CLASSIFIER_MAC,
				"Cannot update classifer mac. ret=%d\n", ret);
		return ret;
	}

	/* change index of reference side */
	classifier_mng_info->ref_index = classifier_mng_info->upd_index;

	/* wait until no longer access the new update side */
	while(likely(classifier_mng_info->ref_index == classifier_mng_info->upd_index))
		rte_delay_us_block(CHANGE_UPDATE_INDEX_WAIT_INTERVAL);

	RTE_LOG(INFO, SPP_CLASSIFIER_MAC,
			"Core[%u] Complete update component.\n", lcore_id);

	return 0;
}

/* classifier(mac address) thread function. */
int
spp_classifier_mac_do(void *arg)
{
	int ret = -1;
	int i;
	int n_rx;
	unsigned int lcore_id = rte_lcore_id();
	struct spp_core_info *core_info = (struct spp_core_info *)arg;
	struct classifier_mac_mng_info *classifier_mng_info =
			g_classifier_mng_info + rte_lcore_id();

	struct classifier_mac_info *classifier_info = NULL;
	struct rte_mbuf *rx_pkts[MAX_PKT_BURST];

	const int n_classified_data = core_info->num_tx_port;
	struct classified_data *classified_data = classifier_mng_info->classified_data;

	uint64_t cur_tsc, prev_tsc = 0;
	const uint64_t drain_tsc = (rte_get_tsc_hz() + US_PER_S - 1) /
			US_PER_S * DRAIN_TX_PACKET_INTERVAL;

	/* initialize */
	ret = init_classifier(core_info, classifier_mng_info, classified_data);
	if (unlikely(ret != 0))
		return ret;

	/* to idle  */
	core_info->status = SPP_CORE_IDLE;
	RTE_LOG(INFO, SPP_CLASSIFIER_MAC, "Core[%u] Start. (type = %d)\n",
			lcore_id, core_info->type);

	while(likely(core_info->status == SPP_CORE_IDLE) ||
			likely(core_info->status == SPP_CORE_FORWARD)) {

		while(likely(core_info->status == SPP_CORE_FORWARD)) {
			/* change index of update side */
			change_update_index(classifier_mng_info, lcore_id);

			/* decide classifier infomation of the current cycle */
			classifier_info = classifier_mng_info->info + 
					classifier_mng_info->ref_index;

			/* drain tx packets, if buffer is not filled for interval */
			cur_tsc = rte_rdtsc();
			if (unlikely(cur_tsc - prev_tsc > drain_tsc)) {
				for (i = 0; i < n_classified_data; i++) {
					if (unlikely(classified_data[i].num_pkt != 0)) {
						RTE_LOG(DEBUG, SPP_CLASSIFIER_MAC,
                        					"transimit packets (drain). "
								"index=%d, "
								"num_pkt=%hu, "
								"interval=%lu\n",
								i,
								classified_data[i].num_pkt,
								cur_tsc - prev_tsc);
						transmit_packet(&classified_data[i]);
					}
				}
				prev_tsc = cur_tsc;
			}

			/* retrieve packets */
			n_rx = rte_eth_rx_burst(core_info->rx_ports[0].dpdk_port, 0,
					rx_pkts, MAX_PKT_BURST);
			if (unlikely(n_rx == 0))
				continue;

#ifdef SPP_RINGLATENCYSTATS_ENABLE
			if (core_info->rx_ports[0].if_type == RING)
				spp_ringlatencystats_calculate_latency(
						core_info->rx_ports[0].if_no, rx_pkts, n_rx);
#endif

			/* classify and transmit (filled) */
			classify_packet(rx_pkts, n_rx, classifier_info, classified_data);
		}
	}

	/* just in case */
	change_update_index(classifier_mng_info, lcore_id);

	/* uninitialize */
	uninit_classifier(classifier_mng_info);

	core_info->status = SPP_CORE_STOP;

	return 0;
}

/* classifier(mac address) iterate classifier table. */
int spp_classifier_mac_iterate_table(
		struct spp_iterate_classifier_table_params *params)
{
	int ret, i;
	const void *key;
	void *data;
	uint32_t next = 0;
	struct classifier_mac_mng_info *classifier_mng_info;
	struct classifier_mac_info *classifier_info;
	struct classified_data *classified_data;
	struct spp_config_port_info port;
	char mac_addr_str[ETHER_ADDR_STR_BUF_SZ];

	for (i = 0; i < RTE_MAX_LCORE; i++) {
		classifier_mng_info = g_classifier_mng_info + i;
		if (! is_used_mng_info(classifier_mng_info))
			continue;

		classifier_info = classifier_mng_info->info + 
				classifier_mng_info->ref_index;

		classified_data = classifier_mng_info->classified_data;

		RTE_LOG(DEBUG, SPP_CLASSIFIER_MAC,
			"Core[%u] Start iterate classifier table.\n", i);

		while(1) {
			ret = rte_hash_iterate(classifier_info->classifier_table,
					&key, &data, &next);

			if (unlikely(ret < 0))
				break;

			ether_format_addr(mac_addr_str, sizeof(mac_addr_str),
					(const struct ether_addr *)key);

			port.if_type = (classified_data + (long)data)->if_type;
			port.if_no   = (classified_data + (long)data)->if_no_global;

			(*params->element_proc)(
					params->opaque,
					SPP_CLASSIFIER_TYPE_MAC,
					mac_addr_str,
					&port);
		}
	}

	return 0;
}
