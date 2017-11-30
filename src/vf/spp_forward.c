#include <rte_cycles.h>

#include "spp_vf.h"
#include "ringlatencystats.h"
#include "spp_forward.h"

#define RTE_LOGTYPE_FORWARD RTE_LOGTYPE_USER1

/* A set of port info of rx and tx */
struct forward_rxtx {
	struct spp_port_info rx;
	struct spp_port_info tx;
};

struct forward_path {
	int num;
	struct forward_rxtx ports[RTE_MAX_ETHPORTS];
};

struct forward_info {
	enum spp_component_type type;
	volatile int ref_index;
	volatile int upd_index;
	struct forward_path path[SPP_INFO_AREA_MAX];
};

struct forward_info g_forward_info[RTE_MAX_LCORE];

/* Clear info */
void
spp_forward_init(void)
{
	int cnt = 0;
	memset(&g_forward_info, 0x00, sizeof(g_forward_info));
	for (cnt = 0; cnt < RTE_MAX_LCORE; cnt++) {
		g_forward_info[cnt].ref_index = 0;
		g_forward_info[cnt].upd_index = 1;
	}
}

/* Clear info for one element. */
static void
clear_forward_info(int id)
{
	struct forward_info *info = &g_forward_info[id];
	info->type = SPP_COMPONENT_UNUSE;
	memset(&g_forward_info[id].path, 0x00, sizeof(struct forward_path));
}

/* Update forward info */
int
spp_forward_update(struct spp_component_info *component)
{
	int cnt = 0;
	int num_rx = component->num_rx_port;
	int num_tx = component->num_tx_port;
	int max = (num_rx > num_tx)?num_rx*num_tx:num_tx;
	struct forward_info *info = &g_forward_info[component->component_id];
	struct forward_path *path = &info->path[info->upd_index];

	/* Forward component allows only one receiving port. */
	if ((component->type == SPP_COMPONENT_FORWARD) && unlikely(num_rx > 1)) {
		RTE_LOG(ERR, FORWARD, "Component[%d] Setting error. (type = %d, rx = %d)\n",
			component->component_id, component->type, num_rx);
		return -1;
	}

	/* Component allows only one transmit port. */
	if (unlikely(num_tx != 0) && unlikely(num_tx != 1)) {
		RTE_LOG(ERR, FORWARD, "Component[%d] Setting error. (type = %d, tx = %d)\n",
			component->component_id, component->type, num_tx);
		return -1;
	}

	clear_forward_info(component->component_id);

	RTE_LOG(INFO, FORWARD,
			"Component[%d] Start update component. (type = %d)\n",
			component->component_id, component->type);

	info->type = component->type;
	path->num = component->num_rx_port;
	for (cnt = 0; cnt < num_rx; cnt++)
		memcpy(&path->ports[cnt].rx, component->rx_ports[cnt],
				sizeof(struct spp_port_info));

	/* Transmit port is set according with larger num_rx / num_tx. */
	for (cnt = 0; cnt < max; cnt++)
		memcpy(&path->ports[cnt].tx, component->tx_ports[0],
				sizeof(struct spp_port_info));

	info->upd_index = info->ref_index;
	while(likely(info->ref_index == info->upd_index))
		rte_delay_us_block(SPP_CHANGE_UPDATE_INTERVAL);

	RTE_LOG(INFO, FORWARD, "Component[%d] Complete update component. (type = %d)\n",
			component->component_id, component->type);

	return 0;
}

/* Change index of forward info */
static inline void
change_forward_index(int id)
{
	struct forward_info *info = &g_forward_info[id];
	if (info->ref_index == info->upd_index)
		info->ref_index = (info->upd_index+1)%SPP_INFO_AREA_MAX;
}
/**
 * Forwarding packets as forwarder or merger
 *
 * Behavior of forwarding is defined as core_info->type which is given
 * as an argument of void and typecasted to spp_config_info.
 */
int
spp_forward(int id)
{
	int cnt, num, buf;
	int nb_rx = 0;
	int nb_tx = 0;
	struct forward_info *info = &g_forward_info[id];
	struct forward_path *path = NULL;
	struct spp_port_info *rx;
	struct spp_port_info *tx;
	struct rte_mbuf *bufs[MAX_PKT_BURST];

	change_forward_index(id);
	path = &info->path[info->ref_index];
	num = path->num;

	for (cnt = 0; cnt < num; cnt++) {
		rx = &path->ports[cnt].rx;
		tx = &path->ports[cnt].tx;

		/* Receive packets */
		nb_rx = rte_eth_rx_burst(rx->dpdk_port, 0, bufs, MAX_PKT_BURST);
		if (unlikely(nb_rx == 0))
			continue;

#ifdef SPP_RINGLATENCYSTATS_ENABLE
		if (rx->if_type == RING)
			spp_ringlatencystats_calculate_latency(rx->if_no,
					bufs, nb_rx);

		if (tx->if_type == RING)
			spp_ringlatencystats_add_time_stamp(tx->if_no,
					bufs, nb_rx);
#endif /* SPP_RINGLATENCYSTATS_ENABLE */

		/* Send packets */
		if (tx->dpdk_port >= 0)
			nb_tx = rte_eth_tx_burst(tx->dpdk_port, 0, bufs, nb_rx);

		/* Discard remained packets to release mbuf */
		if (unlikely(nb_tx < nb_rx)) {
			for (buf = nb_tx; buf < nb_rx; buf++)
				rte_pktmbuf_free(bufs[buf]);
		}
	}
	return 0;
}
