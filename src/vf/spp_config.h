#ifndef __SPP_CONFIG_H__
#define __SPP_CONFIG_H__

#include <jansson.h>
#include "common.h"

#define SPP_CONFIG_FILE_PATH "/usr/local/etc/spp/spp.json"

#define SPP_CONFIG_IFTYPE_NIC   "nic"
#define SPP_CONFIG_IFTYPE_VHOST "vhost"
#define SPP_CONFIG_IFTYPE_RING  "ring"

#define SPP_CONFIG_STR_LEN 32
#define SPP_CONFIG_MAC_TABLE_MAX 16
#define SPP_CONFIG_CORE_MAX 64
#define SPP_CONFIG_PATH_LEN 1024

#define SPP_CONFIG_DEFAULT_CLASSIFIED_SPEC_STR     "default"
#define SPP_CONFIG_DEFAULT_CLASSIFIED_DMY_ADDR_STR "00:00:00:00:00:01"
#define SPP_CONFIG_DEFAULT_CLASSIFIED_DMY_ADDR     0x010000000000

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
 * Instead of json_path_get
 * OK : Json object address
 * NG : NULL
 */
json_t *spp_config_get_path_obj(const json_t *json, const char *path);

/*
 * Change mac address string to int64
 * OK : int64 that store mac address
 * NG : -1
 */
int64_t spp_config_change_mac_str_to_int64(const char *mac);

/*
 * Extract if-type/if-number from port string
 *
 * OK : 0
 * NG : -1
 */
int spp_config_get_if_info(const char *port, enum port_type *if_type, int *if_no);

/*
 * Load config file
 * OK : 0
 * NG : -1
 */
int spp_config_load_file(const char* config_file_path, int node_id, struct spp_config_area *config);

#endif /* __SPP_CONFIG_H__ */
