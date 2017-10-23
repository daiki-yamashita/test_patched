#ifndef __SPP_VF_H__
#define __SPP_VF_H__

#include "common.h"
#include "spp_config.h"

#define SPP_CLIENT_MAX 128

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
 * Classifier Type
 */
enum spp_classifier_type {
	SPP_CLASSIFIER_TYPE_NONE,
	SPP_CLASSIFIER_TYPE_MAC
};

/*
 * API Return value
 */
enum spp_return_value {
	SPP_RET_OK = 0,
	SPP_RET_NG = -1,
	SPP_RET_USED_MAC = -2,
	SPP_RET_NOT_ADD_PORT = -3,
	SPP_RET_USED_PORT = -4
};

/*
 * Port info on core
 */
struct spp_core_port_info {
	enum port_type	if_type;
	int		if_no;
	int		dpdk_port;
	uint64_t	mac_addr;
	char		mac_addr_str[SPP_CONFIG_STR_LEN];
};

/*
 * Core info
 */
struct spp_core_info {
	unsigned int lcore_id;
	volatile enum spp_core_status status;
	enum spp_core_type type;
	int	num_rx_port;
	int	num_tx_port;
	struct spp_core_port_info rx_ports[RTE_MAX_ETHPORTS];
	struct spp_core_port_info tx_ports[RTE_MAX_ETHPORTS];
};

/*
 * Get client ID
 * RETURN : CLIENT ID(0~127)
 */
int spp_get_client_id(void);

/*
 * Update Classifier_table
 * OK : SPP_RET_OK(0)
 * NG : SPP_RET_NG(-1)
 *    : SPP_RET_USED_MAC(-2)
 *    : SPP_RET_NOT_ADD_PORT(-3)
 *    : SPP_RET_USED_PORT(-4)
 */
int spp_update_classifier_table(enum spp_classifier_type type, const char *data, const struct spp_config_port_info *port);

/*
 * Flush SPP component
 * OK : SPP_RET_OK(0)
 * NG : SPP_RET_NG(-1)
 */
int spp_flush(void);

/* definition of iterated classifier element procedure function */
typedef int (*spp_iterate_classifier_element_proc)(
		void *opaque,
		enum spp_classifier_type type,
		const char *data,
		const struct spp_config_port_info *port);

/* iterate classifier table parameters */
struct spp_iterate_classifier_table_params {
	void *opaque;
	spp_iterate_classifier_element_proc element_proc;
};

/*
 * Iterate Classifier_table
 */
int spp_iterate_classifier_table(struct spp_iterate_classifier_table_params *params);

#endif /* __SPP_VF_H__ */
