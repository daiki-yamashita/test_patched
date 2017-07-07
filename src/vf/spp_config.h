#ifndef __SPP_CONFIG_H__
#define __SPP_CONFIG_H__

#include "common.h"

#define SPP_CONFIG_FILE_PATH "/usr/local/etc/spp/spp.json"

#define SPP_CONFIG_IFTYPE_NIC   "nic"
#define SPP_CONFIG_IFTYPE_VHOST "vhost"
#define SPP_CONFIG_IFTYPE_RING  "ring"

#define SPP_CONFIG_STR_LEN 32
#define SPP_CONFIG_MAC_TABLE_MAX 16
#define SPP_CONFIG_CORE_MAX 64

/*
 * Process type for each CORE
 */
enum spp_core_type {
	SPP_CONFIG_UNUSE,
	SPP_CONFIG_CLASSIFIER_MAC,
	SPP_CONFIG_MERGE,
	SPP_CONFIG_FORWARD,
};

/*
 * Interface information structure
 */
struct spp_config_port_info {
	enum port_type	if_type;
	int		if_no;
};

/*
 * MAC Table information structure
 */
struct spp_config_mac_table_element {
	struct		spp_config_port_info port;
	char		mac_addr_str[SPP_CONFIG_STR_LEN];
	uint64_t	mac_addr;
};

/*
 * Classifier Table information structure
 */
struct spp_config_classifier_table {
	char	name[SPP_CONFIG_STR_LEN];
	int	num_table;
	struct spp_config_mac_table_element mac_tables[SPP_CONFIG_MAC_TABLE_MAX];
};

/*
 * Functions information structure
 */
struct spp_config_functions {
	int	core_no;
	enum	spp_core_type type;
	int	num_rx_port;
	int	num_tx_port;
	struct spp_config_port_info rx_ports[RTE_MAX_ETHPORTS];
	struct spp_config_port_info tx_ports[RTE_MAX_ETHPORTS];
};

/*
 * Process information structure
 */
struct spp_config_proc_info {
	char	name[SPP_CONFIG_STR_LEN];
	int	num_vhost;
	int	num_ring;
	int	num_func;
	struct spp_config_functions functions[SPP_CONFIG_CORE_MAX];
};

/*
 * Config information structure
 */
struct spp_config_area {
	struct spp_config_proc_info proc;
	struct spp_config_classifier_table classifier_table;
};

/*
 * Load config file
 * OK : 0
 * NG : -1
 */
int spp_config_load_file(const char* config_file_path, int node_id, struct spp_config_area *config);

#endif /* __SPP_CONFIG_H__ */
