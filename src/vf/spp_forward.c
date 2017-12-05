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

/* Information on the path used for forward. */
struct forward_path {
	char name[SPP_NAME_STR_LEN];	/* component name          */
	volatile enum spp_component_type type;	/* component type          */
	int num;			/* number of receive ports */
	struct forward_rxtx ports[RTE_MAX_ETHPORTS];
					/* port used for transfer  */
};

/* Information for forward. */
struct forward_info {
	volatile int ref_index;		/* index to reference area */
	volatile int upd_index;		/* index to update area    */
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
			"Component[%d] Start update component. (name = %s, type = %d)\n",
			component->component_id, component->name, component->type);

	memcpy(&path->name, component->name, SPP_NAME_STR_LEN);
	path->type = component->type;
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

	RTE_LOG(INFO, FORWARD,
			"Component[%d] Complete update component. (name = %s, type = %d)\n",
			component->component_id, component->name, component->type);

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

/* Merge/Forward iterate component information */
int
spp_forward_core_info_iterate(unsigned int lcore_id, int id,
		struct spp_iterate_core_params *params)
{
	int ret = -1;
	int cnt, num_tx;
	const char *component_type = NULL;
	struct forward_info *info = &g_forward_info[id];
	struct forward_path *path = &info->path[info->ref_index];
	struct spp_port_index rx_ports[RTE_MAX_ETHPORTS];
	struct spp_port_index tx_ports[RTE_MAX_ETHPORTS];

	if (unlikely(path->type == SPP_COMPONENT_UNUSE)) {
		RTE_LOG(ERR, FORWARD,
				"Component[%d] Not used. (status)(core = %d, type = %d)\n",
				id, lcore_id, path->type);
		return -1;
	}

	if (path->type == SPP_COMPONENT_MERGE)
		component_type = SPP_TYPE_MERGE_STR;
	else
		component_type = SPP_TYPE_FORWARD_STR;

	memset(rx_ports, 0x00, sizeof(rx_ports));
	for (cnt = 0; cnt < path->num; cnt++) {
		rx_ports[cnt].if_type = path->ports[cnt].rx.if_type;
		rx_ports[cnt].if_no   = path->ports[cnt].rx.if_no;
	}

	memset(tx_ports, 0x00, sizeof(tx_ports));
	num_tx = (path->num > 0)?1:0;
	tx_ports[0].if_type = path->ports[0].tx.if_type;
	tx_ports[0].if_no   = path->ports[0].tx.if_no;

	/* Set the information with the function specified by the command. */
	ret = (*params->element_proc)(
		params->opaque, lcore_id,
		path->name, component_type,
		path->num, rx_ports, num_tx, tx_ports);
	if (unlikely(ret != 0))
		return -1;

	return 0;
}
