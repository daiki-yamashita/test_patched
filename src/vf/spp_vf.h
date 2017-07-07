#ifndef __SPP_VF_H__
#define __SPP_VF_H__

#include "common.h"
#include "spp_config.h"

/*
 * State on core
 */
enum spp_core_status {
	SPP_CORE_STOP,
	SPP_CORE_IDLE,
	SPP_CORE_FORWARD,
	SPP_CORE_STOP_REQUEST
};

/*
 * Port info on core
 */
struct spp_core_port_info {
	enum		port_type if_type;
	int		if_no;
	int		dpdk_port;
	uint64_t	mac_addr;
	char		mac_addr_str[SPP_CONFIG_STR_LEN];
};

/*
 * Core info
 */
struct spp_core_info {
	enum	spp_core_status status;
	enum	spp_core_type type;
	int	num_rx_port;
	int	num_tx_port;
	struct spp_core_port_info rx_ports[RTE_MAX_ETHPORTS];
	struct spp_core_port_info tx_ports[RTE_MAX_ETHPORTS];
};

#endif /* __SPP_VF_H__ */
