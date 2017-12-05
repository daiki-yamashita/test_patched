#ifndef __SPP_VF_H__
#define __SPP_VF_H__

#include "common.h"

#define SPP_TYPE_CLASSIFIER_MAC_STR "classifier_mac"
#define SPP_TYPE_MERGE_STR          "merge"
#define SPP_TYPE_FORWARD_STR        "forward"

#define SPP_IFTYPE_NIC_STR   "phy"
#define SPP_IFTYPE_VHOST_STR "vhost"
#define SPP_IFTYPE_RING_STR  "ring"

#define SPP_CLIENT_MAX    128
#define SPP_INFO_AREA_MAX 2
#define SPP_MIN_STR_LEN   32
#define SPP_NAME_STR_LEN  128

#define SPP_CHANGE_UPDATE_INTERVAL 10

#define SPP_DEFAULT_CLASSIFIED_SPEC_STR     "default"
#define SPP_DEFAULT_CLASSIFIED_DMY_ADDR_STR "00:00:00:00:00:01"
#define SPP_DEFAULT_CLASSIFIED_DMY_ADDR     0x010000000000

/*
 * State on component
 */
enum spp_core_status {
	SPP_CORE_UNUSE,
	SPP_CORE_STOP,
	SPP_CORE_IDLE,
	SPP_CORE_FORWARD,
	SPP_CORE_STOP_REQUEST,
	SPP_CORE_IDLE_REQUEST
};

/*
 * Process type for each component
 */
enum spp_component_type {
	SPP_COMPONENT_UNUSE,
	SPP_COMPONENT_CLASSIFIER_MAC,
	SPP_COMPONENT_MERGE,
	SPP_COMPONENT_FORWARD,
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
};

/* Port type (rx or tx) */
enum spp_port_rxtx {
	SPP_PORT_RXTX_NONE,
	SPP_PORT_RXTX_RX,
	SPP_PORT_RXTX_TX,
	SPP_PORT_RXTX_ALL,
};

/* command setting type */
enum spp_command_action {
	SPP_CMD_ACTION_NONE,
	SPP_CMD_ACTION_START,
	SPP_CMD_ACTION_STOP,
	SPP_CMD_ACTION_ADD,
	SPP_CMD_ACTION_DEL,
};

/*
 * Interface information structure
 */
struct spp_port_index {
	enum port_type  if_type;
	int             if_no;
};

/*
 * Port info
 */
struct spp_port_info {
	enum port_type	if_type;
	int		if_no;
	int		dpdk_port;
	uint64_t	mac_addr;
	char		mac_addr_str[SPP_MIN_STR_LEN];
};

/*
 * Component info
 */
struct spp_component_info {
	char name[SPP_NAME_STR_LEN];
	enum spp_component_type type;
	unsigned int lcore_id;
	int component_id;
	int num_rx_port;
	int num_tx_port;
	struct spp_port_info *rx_ports[RTE_MAX_ETHPORTS];
	struct spp_port_info *tx_ports[RTE_MAX_ETHPORTS];
};

#if 0
/*
 * Core info
 */
struct spp_core_info {
	enum spp_core_status status;
	int num_component;
	int component_id[SPP_CONFIG_CORE_MAX];
};
#endif

/*
 * Get client ID
 * RETURN : CLIENT ID(0~127)
 */
int spp_get_client_id(void);

/*
 * Update Classifier_table
 * OK : SPP_RET_OK(0)
 * NG : SPP_RET_NG(-1)
 */
int spp_update_classifier_table(
		enum spp_command_action action,
		enum spp_classifier_type type,
		const char *data,
		const struct spp_port_index *port);

/*
 * Update component
 * OK : SPP_RET_OK(0)
 * NG : SPP_RET_NG(-1)
 */
int spp_update_component(
		enum spp_command_action action,
		const char *name, unsigned int lcore_id,
		enum spp_component_type type);

/*
 * Update port
 * OK : SPP_RET_OK(0)
 * NG : SPP_RET_NG(-1)
 */
int spp_update_port(
		enum spp_command_action action,
		const struct spp_port_index *port,
		enum spp_port_rxtx rxtx,
		const char *name);

/*
 * Flush SPP component
 * OK : SPP_RET_OK(0)
 * NG : SPP_RET_NG(-1)
 */
int spp_flush(void);

/* definition of iterated core element procedure function */
typedef int (*spp_iterate_core_element_proc)(
		void *opaque,
		const unsigned int lcore_id,
		const char *name,
		const char *type,
		const int num_rx,
		const struct spp_port_index *rx_ports,
		const int num_tx,
		const struct spp_port_index *tx_ports);

/* iterate core information  parameters */
struct spp_iterate_core_params {
	void *opaque;
	spp_iterate_core_element_proc element_proc;
};

/* Iterate core infomartion */
int spp_iterate_core_info(struct spp_iterate_core_params *params);

/* definition of iterated classifier element procedure function */
typedef int (*spp_iterate_classifier_element_proc)(
		void *opaque,
		enum spp_classifier_type type,
		const char *data,
		const struct spp_port_index *port);

/* iterate classifier table parameters */
struct spp_iterate_classifier_table_params {
	void *opaque;
	spp_iterate_classifier_element_proc element_proc;
};

/*
 * Iterate Classifier_table
 */
int spp_iterate_classifier_table(struct spp_iterate_classifier_table_params *params);

/* Get core status */
enum spp_core_status spp_get_core_status(unsigned int lcore_id);

/* Get component type of target core */
enum spp_component_type spp_get_component_type(unsigned int lcore_id);

/* Get component type being updated on target core */
enum spp_component_type spp_get_component_type_update(unsigned int lcore_id);

/*
 * Get core ID of target component
 * RETURN : core ID
 */
unsigned int spp_get_component_core(int component_id);

/*
 * Check core index change
 * RETURN : True if index has changed.
 */
int spp_check_core_index(unsigned int lcore_id);

/*
 * Get name matching component ID
 * OK : component ID
 * NG : SPP_RET_NG
 */
int spp_get_component_id(const char *name);

/**
 * Check mac address used on the port for registering or removing
 * RETURN : True if target MAC address matches MAC address of port.
 */
int spp_check_mac_used_port(uint64_t mac_addr, enum port_type if_type, int if_no);

/*
 * Check if port has been added.
 * RETURN : True if port has been added.
 */
int spp_check_added_port(enum port_type if_type, int if_no);

/*
 * Check if port has been flushed.
 * RETURN : True if port has been flushed.
 */
int spp_check_flush_port(enum port_type if_type, int if_no);

/*
 * Check if component is using port.
 * OK : match component ID
 * NG : SPP_RET_NG
 */
int spp_check_used_port(enum port_type if_type, int if_no, enum spp_port_rxtx rxtx);

/*
 * Change mac address string to int64
 * OK : int64 that store mac address
 * NG : -1
 */
int64_t spp_change_mac_str_to_int64(const char *mac);

/*
 * Extract if-type/if-number from port string
 *
 * OK : 0
 * NG : -1
 */
int spp_get_if_info(const char *port, enum port_type *if_type, int *if_no);

/*
 * Format port string form if-type/if-number
 *
 * OK : 0
 * NG : -1
 */
int spp_format_port_string(char *port, enum port_type if_type, int if_no);

/*
 * Change component type from string to type value.
 */
enum spp_component_type spp_change_component_type(const char *type_str);

#endif /* __SPP_VF_H__ */
