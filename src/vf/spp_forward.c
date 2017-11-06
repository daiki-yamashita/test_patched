#include "spp_vf.h"
#include "ringlatencystats.h"
#include "spp_forward.h"

#define RTE_LOGTYPE_FORWARD RTE_LOGTYPE_USER1

/* A set of port info of rx and tx */
struct rxtx {
	struct spp_core_port_info rx;
	struct spp_core_port_info tx;
};

/* Set destination port as source */
static void
set_use_interface(struct spp_core_port_info *dst,
		struct spp_core_port_info *src)
{
	dst->if_type   = src->if_type;
	dst->if_no     = src->if_no;
	dst->dpdk_port = src->dpdk_port;
}

/**
 * Forwarding packets as forwarder or merger
 *
 * Behavior of forwarding is defined as core_info->type which is given
 * as an argument of void and typecasted to spp_config_info.
 */
int
spp_forward(void *arg)
{
	unsigned int lcore_id = rte_lcore_id();
	struct spp_core_info *core_info = (struct spp_core_info *)arg;
	int if_cnt, rxtx_num = 0;
	struct rxtx patch[RTE_MAX_ETHPORTS];

  /* Decide the destination of forwarding */
	rxtx_num = core_info->num_rx_port;
	for (if_cnt = 0; if_cnt < rxtx_num; if_cnt++) {
		set_use_interface(&patch[if_cnt].rx,
				&core_info->rx_ports[if_cnt]);
    /* Forwarding type is supposed to forwarder or merger */
		if (core_info->type == SPP_CONFIG_FORWARD) {
			set_use_interface(&patch[if_cnt].tx,
					&core_info->tx_ports[if_cnt]);
		} else {
			set_use_interface(&patch[if_cnt].tx,
					&core_info->tx_ports[0]);
		}
	}

	core_info->status = SPP_CORE_IDLE;
	RTE_LOG(INFO, FORWARD, "Core[%d] Start. (type = %d)\n", lcore_id,
			core_info->type);

	int cnt, nb_rx, nb_tx, buf;
	struct spp_core_port_info *rx;
	struct spp_core_port_info *tx;
	struct rte_mbuf *bufs[MAX_PKT_BURST];
	while (likely(core_info->status == SPP_CORE_IDLE) ||
			likely(core_info->status == SPP_CORE_FORWARD)) {
		while (likely(core_info->status == SPP_CORE_FORWARD)) {
			for (cnt = 0; cnt < rxtx_num; cnt++) {
				rx = &patch[cnt].rx;
				tx = &patch[cnt].tx;

				/* Receive packets */
				nb_rx = rte_eth_rx_burst(rx->dpdk_port, 0, bufs, MAX_PKT_BURST);
				if (unlikely(nb_rx == 0)) {
					continue;
				}

#ifdef SPP_RINGLATENCYSTATS_ENABLE
				if (rx->if_type == RING) {
					spp_ringlatencystats_calculate_latency(rx->if_no,
							bufs, nb_rx);
				}
				if (tx->if_type == RING) {
					spp_ringlatencystats_add_time_stamp(tx->if_no,
							bufs, nb_rx);
				}
#endif /* SPP_RINGLATENCYSTATS_ENABLE */

				/* Send packets */
				nb_tx = rte_eth_tx_burst(tx->dpdk_port, 0, bufs, nb_rx);

				/* Discard remained packets to release mbuf */
				if (unlikely(nb_tx < nb_rx)) {
					for (buf = nb_tx; buf < nb_rx; buf++) {
						rte_pktmbuf_free(bufs[buf]);
					}
				}
			}
		}
	}

	RTE_LOG(INFO, FORWARD, "Core[%d] End. (type = %d)\n", lcore_id,
			core_info->type);
	core_info->status = SPP_CORE_STOP;
	return 0;
}
