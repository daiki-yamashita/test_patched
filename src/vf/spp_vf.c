#include <netinet/in.h>
#include <arpa/inet.h>
#include <getopt.h>

#include <rte_eth_ring.h>
#include <rte_eth_vhost.h>
#include <rte_memzone.h>

#include "spp_vf.h"
#include "ringlatencystats.h"
#include "classifier_mac.h"
#include "spp_forward.h"
#include "command_proc.h"

/* TODO(yasufum) add desc how there are used */
#define SPP_CORE_STATUS_CHECK_MAX 5
#define SPP_RING_LATENCY_STATS_SAMPLING_INTERVAL 1000000

/* getopt_long return value for long option */
enum SPP_LONGOPT_RETVAL {
	SPP_LONGOPT_RETVAL__ = 127,

	/* add below */
	/* TODO(yasufum) add description what and why add below */
	SPP_LONGOPT_RETVAL_CONFIG,
	SPP_LONGOPT_RETVAL_CLIENT_ID,
	SPP_LONGOPT_RETVAL_VHOST_CLIENT
};

/* Manage given options as global variable */
struct startup_param {
	int client_id;
	char server_ip[INET_ADDRSTRLEN];
	int server_port;
	int vhost_client;
};

/* Status of patch and its cores, mac address assinged for it and port info */
struct patch_info {
	int      use_flg;
	int      dpdk_port;  /* TODO(yasufum) add desc for what is this */
	int      rx_core_no;
	int      tx_core_no;
	char     mac_addr_str[SPP_CONFIG_STR_LEN];
	uint64_t mac_addr;
	struct   spp_core_port_info *rx_core;
	struct   spp_core_port_info *tx_core;
};

/* Manage number of interfaces and patch information  as global variable */
/* TODO(yasufum) refactor, change if to iface */
struct if_info {
	int num_nic;
	int num_vhost;
	int num_ring;
	struct patch_info nic_patchs[RTE_MAX_ETHPORTS];
	struct patch_info vhost_patchs[RTE_MAX_ETHPORTS];
	struct patch_info ring_patchs[RTE_MAX_ETHPORTS];
};

/* Declare global variables */
static struct spp_config_area	g_config;
static struct startup_param	g_startup_param;
static struct if_info		g_if_info;
static struct spp_core_info	g_core_info[SPP_CONFIG_CORE_MAX];
static int 			g_change_core[SPP_CONFIG_CORE_MAX];  /* TODO(yasufum) add desc how it is used and why changed core is kept */

static char config_file_path[PATH_MAX];

/* Print help message */
static void
usage(const char *progname)
{
	RTE_LOG(INFO, APP, "Usage: %s [EAL args] --"
			" --client-id CLIENT_ID"
			" [--config CONFIG_FILE_PATH]"
			" -s SERVER_IP:SERVER_PORT"
			" [--vhost-client]\n"
			" --client-id CLIENT_ID   : My client ID\n"
			" --config CONFIG_FILE_PATH : specific config file path\n"
			" -s SERVER_IP:SERVER_PORT  : Access information to the server\n"
			" --vhost-client            : Run vhost on client\n"
			, progname);
}

static int
add_ring_pmd(int ring_id)
{
	struct rte_ring *ring;
	int ring_port_id;

	/* Lookup ring of given id */
	ring = rte_ring_lookup(get_rx_queue_name(ring_id));
	if (unlikely(ring == NULL)) {
		RTE_LOG(ERR, APP,
			"Cannot get RX ring - is server process running?\n");
		return -1;
	}

	/* Create ring pmd */
	ring_port_id = rte_eth_from_ring(ring);
	RTE_LOG(DEBUG, APP, "ring port id %d\n", ring_port_id);

	return ring_port_id;
}

static int
add_vhost_pmd(int index, int client)
{
	struct rte_eth_conf port_conf = {
		.rxmode = { .max_rx_pkt_len = ETHER_MAX_LEN }
	};
	struct rte_mempool *mp;
	uint8_t vhost_port_id;
	int nr_queues = 1;
	const char *name;
	char devargs[64];
	char *iface;
	uint16_t q;
	int ret;
#define NR_DESCS 128

	mp = rte_mempool_lookup(PKTMBUF_POOL_NAME);
	if (unlikely(mp == NULL)) {
		RTE_LOG(ERR, APP, "Cannot get mempool for mbufs. (name = %s)\n",
				PKTMBUF_POOL_NAME);
		return -1;
	}

	/* eth_vhost0 index 0 iface /tmp/sock0 on numa 0 */
	name = get_vhost_backend_name(index);
	iface = get_vhost_iface_name(index);

	sprintf(devargs, "%s,iface=%s,queues=%d,client=%d",
			name, iface, nr_queues, client);
	ret = rte_eth_dev_attach(devargs, &vhost_port_id);
	if (unlikely(ret < 0)) {
		RTE_LOG(ERR, APP, "rte_eth_dev_attach error. (ret = %d)\n", ret);
		return ret;
	}

	ret = rte_eth_dev_configure(vhost_port_id, nr_queues, nr_queues,
		&port_conf);
	if (unlikely(ret < 0)) {
		RTE_LOG(ERR, APP, "rte_eth_dev_configure error. (ret = %d)\n",
				ret);
		return ret;
	}

	/* Allocate and set up 1 RX queue per Ethernet port. */
	for (q = 0; q < nr_queues; q++) {
		ret = rte_eth_rx_queue_setup(vhost_port_id, q, NR_DESCS,
			rte_eth_dev_socket_id(vhost_port_id), NULL, mp);
		if (unlikely(ret < 0)) {
			RTE_LOG(ERR, APP,
				"rte_eth_rx_queue_setup error. (ret = %d)\n",
				ret);
			return ret;
		}
	}

	/* Allocate and set up 1 TX queue per Ethernet port. */
	for (q = 0; q < nr_queues; q++) {
		ret = rte_eth_tx_queue_setup(vhost_port_id, q, NR_DESCS,
			rte_eth_dev_socket_id(vhost_port_id), NULL);
		if (unlikely(ret < 0)) {
			RTE_LOG(ERR, APP,
				"rte_eth_tx_queue_setup error. (ret = %d)\n",
				ret);
			return ret;
		}
	}

	/* Start the Ethernet port. */
	ret = rte_eth_dev_start(vhost_port_id);
	if (unlikely(ret < 0)) {
		RTE_LOG(ERR, APP, "rte_eth_dev_start error. (ret = %d)\n", ret);
		return ret;
	}

	RTE_LOG(DEBUG, APP, "vhost port id %d\n", vhost_port_id);

	return vhost_port_id;
}

/**
 * Check status of all of cores is same as given
 *
 * It returns -1 as status mismatch if status is not same.
 * If status is SPP_CONFIG_UNUSE, check is skipped.
 */
static int
check_core_status(enum spp_core_status status)
{
	int cnt;  /* increment core id */
	for (cnt = 0; cnt < SPP_CONFIG_CORE_MAX; cnt++) {
		if (g_core_info[cnt].type == SPP_CONFIG_UNUSE) {
			continue;
		}
		if (g_core_info[cnt].status != status) {
			/* Status is mismatched */
			return -1;
		}
	}
	return 0;
}

/**
 * Run check_core_status() for SPP_CORE_STATUS_CHECK_MAX times with
 * interval time (1sec)
 */
static int
check_core_status_wait(enum spp_core_status status)
{
	int cnt = 0;
	for (cnt = 0; cnt < SPP_CORE_STATUS_CHECK_MAX; cnt++) {
		sleep(1);
		int ret = check_core_status(status);
		if (ret == 0) {
			return 0;
		}
	}

	RTE_LOG(ERR, APP, "Status check time out. (status = %d)\n", status);
	return -1;
}

/* Set all core to given status */
static void
set_core_status(enum spp_core_status status)
{
	int core_cnt = 0;  /* increment core id */
	for(core_cnt = 0; core_cnt < SPP_CONFIG_CORE_MAX; core_cnt++) {
		g_core_info[core_cnt].status = status;
	}
}

/**
 * Set all of core status to SPP_CORE_STOP_REQUEST if received signal
 * is SIGTERM or SIGINT
 */
static void
stop_process(int signal) {
	if (unlikely(signal != SIGTERM) &&
			unlikely(signal != SIGINT)) {
		return;
	}

	set_core_status(SPP_CORE_STOP_REQUEST);
}

/**
 * Convert string of given client id to inteter
 *
 * If succeeded, client id of interger is assigned to client_id and
 * reuturn 0. Or return -1 if failed.
 */
static int
parse_app_client_id(const char *client_id_str, int *client_id)
{
	int id = 0;
	char *endptr = NULL;

	id = strtol(client_id_str, &endptr, 0);
	if (unlikely(client_id_str == endptr) || unlikely(*endptr != '\0'))
		return -1;

	if (id >= SPP_CLIENT_MAX)
		return -1;

	*client_id = id;
	RTE_LOG(DEBUG, APP, "Set client id = %d\n", *client_id);
	return 0;
}

/* Parse options for server ip and port */
static int
parse_app_server(const char *server_str, char *server_ip, int *server_port)
{
	const char delim[2] = ":";
	unsigned int pos = 0;
	int port = 0;
	char *endptr = NULL;

	pos = strcspn(server_str, delim);
	if (pos >= strlen(server_str))
		return -1;

	port = strtol(&server_str[pos+1], &endptr, 0);
	if (unlikely(&server_str[pos+1] == endptr) || unlikely(*endptr != '\0'))
		return -1;

	memcpy(server_ip, server_str, pos);
	server_ip[pos] = '\0';
	*server_port = port;
	RTE_LOG(DEBUG, APP, "Set server ip   = %s\n", server_ip);
	RTE_LOG(DEBUG, APP, "Set server port = %d\n", *server_port);
	return 0;
}

/* Parse options for client app */
static int
parse_app_args(int argc, char *argv[])
{
	int cnt;
	int proc_flg = 0;
	int server_flg = 0;
	int option_index, opt;
	const int argcopt = argc;
	char *argvopt[argcopt];
	const char *progname = argv[0];
	static struct option lgopts[] = { 
			{ "config", required_argument, NULL, SPP_LONGOPT_RETVAL_CONFIG },
			{ "client-id", required_argument, NULL, SPP_LONGOPT_RETVAL_CLIENT_ID },
			{ "vhost-client", no_argument, NULL, SPP_LONGOPT_RETVAL_VHOST_CLIENT },
			{ 0 },
	};

	/**
	 * Save argv to argvopt to aovid loosing the order of options
	 * by getopt_long()
	 */
	for (cnt = 0; cnt < argcopt; cnt++) {
		argvopt[cnt] = argv[cnt];
	}

	/* Clear startup parameters */
	memset(&g_startup_param, 0x00, sizeof(g_startup_param));

	/* Check options of application */
	optind = 0;
	opterr = 0;
	while ((opt = getopt_long(argc, argvopt, "s:", lgopts,
			&option_index)) != EOF) {
		switch (opt) {
		case SPP_LONGOPT_RETVAL_CONFIG:
			if (optarg[0] == '\0' || strlen(optarg) >= sizeof(config_file_path)) {
				usage(progname);
				return -1;
			}
			strcpy(config_file_path, optarg);
			break;
		case SPP_LONGOPT_RETVAL_CLIENT_ID:
			if (parse_app_client_id(optarg, &g_startup_param.client_id) != 0) {
				usage(progname);
				return -1;
			}
			proc_flg = 1;
			break;
		case SPP_LONGOPT_RETVAL_VHOST_CLIENT:
			g_startup_param.vhost_client = 1;
			break;
		case 's':
			if (parse_app_server(optarg, g_startup_param.server_ip,
					&g_startup_param.server_port) != 0) {
				usage(progname);
				return -1;
			}
			server_flg = 1;
			break;
		default:
			usage(progname);
			return -1;
			break;
		}
	}

	/* Check mandatory parameters */
	if ((proc_flg == 0) || (server_flg == 0)) {
		usage(progname);
		return -1;
	}
	RTE_LOG(INFO, APP,
			"app opts (client_id=%d,config=%s,server=%s:%d,vhost_client=%d)\n",
			g_startup_param.client_id,
			config_file_path,
			g_startup_param.server_ip,
			g_startup_param.server_port,
			g_startup_param.vhost_client);
	return 0;
}

/**
 * Return patch info of given type and num of interface
 *
 * It returns NULL value if given type is invalid.
 * TODO(yasufum) refactor name of func to be more understandable (area?)
 * TODO(yasufum) refactor, change if to iface.
 * TODO(yasufum) confirm why it returns not -1 but NULL.
 */
static struct patch_info *
get_if_area(enum port_type if_type, int if_no)
{
	switch (if_type) {
	case PHY:
		return &g_if_info.nic_patchs[if_no];
		break;
	case VHOST:
		return &g_if_info.vhost_patchs[if_no];
		break;
	case RING:
		return &g_if_info.ring_patchs[if_no];
		break;
	default:
		return NULL;
		break;
	}
}

/**
 * Initialize all of patch info by assingning -1
 *
 * TODO(yasufum) refactor, change if to iface.
 */
static void
init_if_info(void)
{
	int port_cnt;  /* increment ether ports */
	memset(&g_if_info, 0x00, sizeof(g_if_info));
	for (port_cnt = 0; port_cnt < RTE_MAX_ETHPORTS; port_cnt++) {
		g_if_info.nic_patchs[port_cnt].rx_core_no   = -1;
		g_if_info.nic_patchs[port_cnt].tx_core_no   = -1;
		g_if_info.vhost_patchs[port_cnt].rx_core_no = -1;
		g_if_info.vhost_patchs[port_cnt].tx_core_no = -1;
		g_if_info.ring_patchs[port_cnt].rx_core_no  = -1;
		g_if_info.ring_patchs[port_cnt].tx_core_no  = -1;
	}
}

/**
 * Initialize g_core_info and its port info
 *
 * Clear g_core_info and set interface type of its port info to UNDEF.
 * TODO(yasufum) refactor, change if to iface.
 */
static void
init_core_info(void)
{
	memset(&g_core_info, 0x00, sizeof(g_core_info));
	int core_cnt, port_cnt;
	for (core_cnt = 0; core_cnt < SPP_CONFIG_CORE_MAX; core_cnt++) {
		g_core_info[core_cnt].lcore_id = core_cnt;
		for (port_cnt = 0; port_cnt < RTE_MAX_ETHPORTS; port_cnt++) {
			g_core_info[core_cnt].rx_ports[port_cnt].if_type = UNDEF;
			g_core_info[core_cnt].tx_ports[port_cnt].if_type = UNDEF;
		}
	}
	memset(g_change_core, 0x00, sizeof(g_change_core));
}

/**
 * Set properties of g_core_info from config
 *
 * TODO(yasufum) refactor, change if to iface.
 * TODO(yasufum) confirm meaning of funciton name and is typo ?
 */
static int
set_form_proc_info(struct spp_config_area *config)
{
	int core_cnt, rx_start, rx_cnt, tx_start, tx_cnt;
	enum port_type if_type;
	int if_no;
	struct spp_config_functions *core_func = NULL;
	struct spp_core_info *core_info = NULL;
	struct patch_info *patch_info = NULL;
	for (core_cnt = 0; core_cnt < config->proc.num_func; core_cnt++) {
		core_func = &config->proc.functions[core_cnt];
		core_info = &g_core_info[core_func->core_no];

		if (core_func->type == SPP_CONFIG_UNUSE) {
			continue;
		}

    /* Forwardをまとめる事は可、他種別は不可 */
    /* TODO(yasufum) confirm what is the purpose and meaning */
    if ((core_info->type != SPP_CONFIG_UNUSE) &&
        ((core_info->type != SPP_CONFIG_FORWARD) ||
         (core_func->type != SPP_CONFIG_FORWARD))) {
      RTE_LOG(ERR, APP, "Core in use. (core = %d, type = %d/%d)\n",
          core_func->core_no,
          core_func->type, core_info->type);
      return -1;
    }

		core_info->type = core_func->type;
		if (!rte_lcore_is_enabled(core_func->core_no)) {
			/* CPU mismatch */
			RTE_LOG(ERR, APP, "CPU mismatch (cpu = %u)\n",
					core_func->core_no);
			return -1;
		}

		rx_start = core_info->num_rx_port;
		core_info->num_rx_port += core_func->num_rx_port;
		for (rx_cnt = 0; rx_cnt < core_func->num_rx_port; rx_cnt++) {
			if_type = core_func->rx_ports[rx_cnt].if_type;
			if_no   = core_func->rx_ports[rx_cnt].if_no;

			core_info->rx_ports[rx_start + rx_cnt].if_type = if_type;
			core_info->rx_ports[rx_start + rx_cnt].if_no   = if_no;

			/* Retrieve patch corresponding to type and number of the interface */
			patch_info = get_if_area(if_type, if_no);

			patch_info->use_flg = 1;
			if (unlikely(patch_info->rx_core != NULL)) {
				RTE_LOG(ERR, APP, "Used RX port (core = %d, if_type = %d, if_no = %d)\n",
						core_func->core_no, if_type, if_no);
				return -1;
			}

			/* IF情報からCORE情報を変更する場合用に設定 */
      /* TODO(yasufum) confirm the meaning of this comment */
			patch_info->rx_core_no = core_func->core_no;
			patch_info->rx_core    = &core_info->rx_ports[rx_start + rx_cnt];
		}

		/* Set TX port */
		tx_start = core_info->num_tx_port;
		core_info->num_tx_port += core_func->num_tx_port;
		for (tx_cnt = 0; tx_cnt < core_func->num_tx_port; tx_cnt++) {
			if_type = core_func->tx_ports[tx_cnt].if_type;
			if_no   = core_func->tx_ports[tx_cnt].if_no;

			core_info->tx_ports[tx_start + tx_cnt].if_type = if_type;
			core_info->tx_ports[tx_start + tx_cnt].if_no   = if_no;

			patch_info = get_if_area(if_type, if_no);

			patch_info->use_flg = 1;
			if (unlikely(patch_info->tx_core != NULL)) {
				RTE_LOG(ERR, APP, "Used TX port (core = %d, if_type = %d, if_no = %d)\n",
						core_func->core_no, if_type, if_no);
				return -1;
			}

			/* IF情報からCORE情報を変更する場合用に設定 */
      /* TODO(yasufum) confirm the meaning of this comment */
			patch_info->tx_core_no = core_func->core_no;
			patch_info->tx_core    = &core_info->tx_ports[tx_start + tx_cnt];
		}
	}

	return 0;
}

/**
 * Load mac table entries from config and setup patches
 *
 * TODO(yasufum) refactor, change if to iface.
 * TODO(yasufum) confirm if additional description for the structure of mac table is needed.
 */
static int
set_from_classifier_table(struct spp_config_area *config)
{
	enum port_type if_type;
	int if_no = 0;
	int mac_cnt = 0;
	struct spp_config_mac_table_element *mac_table = NULL;
	struct patch_info *patch_info = NULL;
	for (mac_cnt = 0; mac_cnt < config->classifier_table.num_table; mac_cnt++) {
		mac_table = &config->classifier_table.mac_tables[mac_cnt];

		if_type = mac_table->port.if_type;
		if_no   = mac_table->port.if_no;

    /* Retrieve patch corresponding to type and number of the interface */
		patch_info = get_if_area(if_type, if_no);

		if (unlikely(patch_info->use_flg == 0)) {
			RTE_LOG(ERR, APP, "Not used interface (if_type = %d, if_no = %d)\n",
					if_type, if_no);
			return -1;
		}

    /* Set mac address from the table for destination tx, not need for rx */
		patch_info->mac_addr = mac_table->mac_addr;
		strcpy(patch_info->mac_addr_str, mac_table->mac_addr_str);
		if (unlikely(patch_info->tx_core != NULL)) {
			patch_info->tx_core->mac_addr = mac_table->mac_addr;
			strcpy(patch_info->tx_core->mac_addr_str, mac_table->mac_addr_str);
		}
	}
	return 0;
}

/**
 * Setup patch info of port on host
 *
 * TODO(yasufum) refactor, change if to iface.
 */
static int
set_nic_interface(struct spp_config_area *config __attribute__ ((unused)))
{
	/* NIC Setting */
	g_if_info.num_nic = rte_eth_dev_count();
	if (g_if_info.num_nic > RTE_MAX_ETHPORTS) {
		g_if_info.num_nic = RTE_MAX_ETHPORTS;
	}

	int nic_cnt, nic_num = 0;
	struct patch_info *patch_info = NULL;
	for (nic_cnt = 0; nic_cnt < RTE_MAX_ETHPORTS; nic_cnt++) {
		patch_info = &g_if_info.nic_patchs[nic_cnt];
		patch_info->dpdk_port = nic_cnt;

    /* TODO(yasufum) confirm why it is needed */
		if (patch_info->use_flg == 0) {
			/* Not Used */
			continue;
		}

		if (patch_info->rx_core != NULL) {
			patch_info->rx_core->dpdk_port = nic_cnt;
		}
		if (patch_info->tx_core != NULL) {
			patch_info->tx_core->dpdk_port = nic_cnt;
		}

		nic_num++;
	}

	if (unlikely(nic_num > g_if_info.num_nic)) {
		RTE_LOG(ERR, APP, "NIC Setting mismatch. (IF = %d, config = %d)\n",
				nic_num, g_if_info.num_nic);
		return -1;
	}

	return 0;
}

/**
 * Setup vhost interfaces from config
 *
 * TODO(yasufum) refactor, change if to iface.
 */
static int
set_vhost_interface(struct spp_config_area *config)
{
	int vhost_cnt, vhost_num = 0;
	g_if_info.num_vhost = config->proc.num_vhost;
	struct patch_info *patch_info = NULL;
	for (vhost_cnt = 0; vhost_cnt < RTE_MAX_ETHPORTS; vhost_cnt++) {
		patch_info = &g_if_info.vhost_patchs[vhost_cnt];
		if (patch_info->use_flg == 0) {
			/* Not Used */
			continue;
		}

		int dpdk_port = add_vhost_pmd(vhost_cnt, g_startup_param.vhost_client);
		if (unlikely(dpdk_port < 0)) {
			RTE_LOG(ERR, APP, "VHOST add failed. (no = %d)\n",
					vhost_cnt);
			return -1;
		}
		patch_info->dpdk_port = dpdk_port;

		if (patch_info->rx_core != NULL) {
			patch_info->rx_core->dpdk_port = dpdk_port;
		}
		if (patch_info->tx_core != NULL) {
			patch_info->tx_core->dpdk_port = dpdk_port;
		}
		vhost_num++;
	}
	if (unlikely(vhost_num > g_if_info.num_vhost)) {
		RTE_LOG(ERR, APP, "VHOST Setting mismatch. (IF = %d, config = %d)\n",
				vhost_num, g_if_info.num_vhost);
		return -1;
	}
	return 0;
}

/**
 * Setup ring interfaces from config
 *
 * TODO(yasufum) refactor, change if to iface.
 */
static int
set_ring_interface(struct spp_config_area *config)
{
	int ring_cnt, ring_num = 0;
	g_if_info.num_ring = config->proc.num_ring;
	struct patch_info *patch_info = NULL;
	for (ring_cnt = 0; ring_cnt < RTE_MAX_ETHPORTS; ring_cnt++) {
		patch_info = &g_if_info.ring_patchs[ring_cnt];

    /* TODO(yasufum) confirm why it is needed */
		if (patch_info->use_flg == 0) {
			/* Not Used */
			continue;
		}

		int dpdk_port = add_ring_pmd(ring_cnt);
		if (unlikely(dpdk_port < 0)) {
			RTE_LOG(ERR, APP, "RING add failed. (no = %d)\n",
					ring_cnt);
			return -1;
		}
		patch_info->dpdk_port = dpdk_port;

		if (patch_info->rx_core != NULL) {
			patch_info->rx_core->dpdk_port = dpdk_port;
		}
		if (patch_info->tx_core != NULL) {
			patch_info->tx_core->dpdk_port = dpdk_port;
		}
		ring_num++;
	}
	if (unlikely(ring_num > g_if_info.num_ring)) {
		RTE_LOG(ERR, APP, "RING Setting mismatch. (IF = %d, config = %d)\n",
				ring_num, g_if_info.num_ring);
		return -1;
	}
	return 0;
}

/**
 * Setup management info for spp_vf
 *
 * TODO(yasufum) refactor, change if to iface.
 * TODO(yasufum) refactor, change function name from manage to mng or management
 */
static int
init_manage_data(struct spp_config_area *config)
{
	/* Initialize interface and core infomation */
	init_if_info();
	init_core_info();

  /* Load config for resource assingment and network configuration */
	int ret_proc = set_form_proc_info(config);
	if (unlikely(ret_proc != 0)) {
		return -1;
	}
	int ret_classifier = set_from_classifier_table(config);
	if (unlikely(ret_classifier != 0)) {
		return -1;
	}

	int ret_nic = set_nic_interface(config);
	if (unlikely(ret_nic != 0)) {
		return -1;
	}

	int ret_vhost = set_vhost_interface(config);
	if (unlikely(ret_vhost != 0)) {
		return -1;
	}

	int ret_ring = set_ring_interface(config);
	if (unlikely(ret_ring != 0)) {
		return -1;
	}

	return 0;
}

#ifdef SPP_RINGLATENCYSTATS_ENABLE
/**
 * Print statistics of time for packet processing in ring interface
 *
 * TODO(yasufum) refactor, change if to iface.
 */
static void
print_ring_latency_stats(void)
{
	/* Clear screen and move cursor to top left */
	const char topLeft[] = { 27, '[', '1', ';', '1', 'H', '\0' };
	const char clr[] = { 27, '[', '2', 'J', '\0' };
	printf("%s%s", clr, topLeft);

	int ring_cnt, stats_cnt;
	struct spp_ringlatencystats_ring_latency_stats stats[RTE_MAX_ETHPORTS];
	memset(&stats, 0x00, sizeof(stats));

	printf("RING Latency\n");
	printf(" RING");
	for (ring_cnt = 0; ring_cnt < RTE_MAX_ETHPORTS; ring_cnt++) {
		if (g_if_info.ring_patchs[ring_cnt].use_flg == 0) {
			continue;
		}
		spp_ringlatencystats_get_stats(ring_cnt, &stats[ring_cnt]);
		printf(", %-18d", ring_cnt);
	}
	printf("\n");

	for (stats_cnt = 0; stats_cnt < SPP_RINGLATENCYSTATS_STATS_SLOT_COUNT; stats_cnt++) {
		printf("%3dns", stats_cnt);
		for (ring_cnt = 0; ring_cnt < RTE_MAX_ETHPORTS; ring_cnt++) {
			if (g_if_info.ring_patchs[ring_cnt].use_flg == 0) {
				continue;
			}

			printf(", 0x%-16lx", stats[ring_cnt].slot[stats_cnt]);
		}
		printf("\n");
	}

	return;
}
#endif /* SPP_RINGLATENCYSTATS_ENABLE */

/**
 * Remove sock file
 */
static void
del_vhost_sockfile(struct patch_info *vhost_patchs)
{
	int cnt;

	/* Do not rmeove for if it is running in vhost-client mode. */
	if (g_startup_param.vhost_client != 0)
		return;

	for (cnt = 0; cnt < RTE_MAX_ETHPORTS; cnt++) {
		if (likely(vhost_patchs[cnt].use_flg == 0)) {
			/* Skip removing if it is not using vhost */
			continue;
		}

		remove(get_vhost_iface_name(cnt));
	}
}

/* TODO(yasufum) refactor, change if to iface. */
/* TODO(yasufum) change test using ut_main(), or add desccription for what and why use it */
int
#ifndef USE_UT_SPP_VF
main(int argc, char *argv[])
#else /* ifndef USE_UT_SPP_VF */
ut_main(int argc, char *argv[])
#endif  /* ifndef USE_UT_SPP_VF */
{
	int ret = -1;
#ifdef SPP_DEMONIZE
	/* Daemonize process */
	int ret_daemon = daemon(0, 0);
	if (unlikely(ret_daemon != 0)) {
		RTE_LOG(ERR, APP, "daemonize is faild. (ret = %d)\n", ret_daemon);
		return ret_daemon;
	}
#endif

	/* Signal handler registration (SIGTERM/SIGINT) */
	signal(SIGTERM, stop_process);
	signal(SIGINT,  stop_process);

	/* Setup config wiht default file path */
	strcpy(config_file_path, SPP_CONFIG_FILE_PATH);

	unsigned int main_lcore_id = 0xffffffff;
	while(1) {
		int ret_dpdk = rte_eal_init(argc, argv);
		if (unlikely(ret_dpdk < 0)) {
			break;
		}

		argc -= ret_dpdk;
		argv += ret_dpdk;

		/* Set log level  */
		rte_log_set_global_level(RTE_LOG_LEVEL);

		/* Parse spp_vf specific parameters */
		int ret_parse = parse_app_args(argc, argv);
		if (unlikely(ret_parse != 0)) {
			break;
		}

		RTE_LOG(INFO, APP, "Load config file(%s)\n", config_file_path);

		int ret_config = spp_config_load_file(config_file_path, 0, &g_config);
		if (unlikely(ret_config != 0)) {
			break;
		}

		/* Get lcore id of main thread to set its status after */
		main_lcore_id = rte_lcore_id();

		int ret_manage = init_manage_data(&g_config);
		if (unlikely(ret_manage != 0)) {
			break;
		}

		int ret_classifier_mac_init = spp_classifier_mac_init();
		if (unlikely(ret_classifier_mac_init != 0)) {
			break;
		}

    /* Setup connection for accepting commands from controller */
		int ret_command_init = spp_command_proc_init(
				g_startup_param.server_ip,
				g_startup_param.server_port);
		if (unlikely(ret_command_init != 0)) {
			break;
		}

#ifdef SPP_RINGLATENCYSTATS_ENABLE
		int ret_ringlatency = spp_ringlatencystats_init(
				SPP_RING_LATENCY_STATS_SAMPLING_INTERVAL, g_if_info.num_ring);
		if (unlikely(ret_ringlatency != 0)) {
			break;
		}
#endif /* SPP_RINGLATENCYSTATS_ENABLE */

		/* Start worker threads of classifier and forwarder */
		unsigned int lcore_id = 0;
		RTE_LCORE_FOREACH_SLAVE(lcore_id) {
			if (g_core_info[lcore_id].type == SPP_CONFIG_CLASSIFIER_MAC) {
				rte_eal_remote_launch(spp_classifier_mac_do,
						(void *)&g_core_info[lcore_id],
						lcore_id);
			} else {
				rte_eal_remote_launch(spp_forward,
						(void *)&g_core_info[lcore_id],
						lcore_id);
			}
		}

    /* Set the status of main thread to idle */
		g_core_info[main_lcore_id].status = SPP_CORE_IDLE;
		int ret_wait = check_core_status_wait(SPP_CORE_IDLE);
		if (unlikely(ret_wait != 0)) {
			break;
		}

		/* Start forwarding */
		set_core_status(SPP_CORE_FORWARD);
		RTE_LOG(INFO, APP, "My ID %d start handling message\n", 0);
		RTE_LOG(INFO, APP, "[Press Ctrl-C to quit ...]\n");

		/* Enter loop for accepting commands */
		int ret_do = 0;
#ifndef USE_UT_SPP_VF
		while(likely(g_core_info[main_lcore_id].status != SPP_CORE_STOP_REQUEST)) {
#else
		{
#endif
      /* Receive command */
			ret_do = spp_command_proc_do();
			if (unlikely(ret_do != 0)) {
				break;
			}

			sleep(1);

#ifdef SPP_RINGLATENCYSTATS_ENABLE
			print_ring_latency_stats();
#endif /* SPP_RINGLATENCYSTATS_ENABLE */
		}

    /* TODO(yasufum) confirm, add why this check is needed because it is same the case of "ret = 0", or remove */
		if (unlikely(ret_do != 0)) {
			break;
		}

		ret = 0;
		break;
	}

	/* Finalize to exit */
	if (main_lcore_id == rte_lcore_id())
	{
		g_core_info[main_lcore_id].status = SPP_CORE_STOP;
		int ret_core_end = check_core_status_wait(SPP_CORE_STOP);
		if (unlikely(ret_core_end != 0)) {
			RTE_LOG(ERR, APP, "Core did not stop.\n");
		}

		/* Remove vhost sock file if it is not running in vhost-client mode */
		del_vhost_sockfile(g_if_info.vhost_patchs);
	}

#ifdef SPP_RINGLATENCYSTATS_ENABLE
	spp_ringlatencystats_uninit();
#endif /* SPP_RINGLATENCYSTATS_ENABLE */

	RTE_LOG(INFO, APP, "spp_vf exit.\n");
	return ret;
}

int
spp_get_client_id(void)
{
	return g_startup_param.client_id;
}

/**
 * Check mac address used on the interface
 *
 * TODO(yasufum) refactor, change if to iface.
 * TODO(yasufum) confirm, add the reason why this check is needed
 */
static int
check_mac_used_interface(uint64_t mac_addr, enum port_type *if_type, int *if_no)
{
	int cnt = 0;
	for (cnt = 0; cnt < RTE_MAX_ETHPORTS; cnt++) {
		if (unlikely(g_if_info.nic_patchs[cnt].mac_addr == mac_addr)) {
			*if_type = PHY;
			*if_no = cnt;
			return 0;
		}
		if (unlikely(g_if_info.vhost_patchs[cnt].mac_addr == mac_addr)) {
			*if_type = VHOST;
			*if_no = cnt;
			return 0;
		}
		if (unlikely(g_if_info.ring_patchs[cnt].mac_addr == mac_addr)) {
			*if_type = RING;
			*if_no = cnt;
			return 0;
		}
	}
	return -1;
}

int
spp_update_classifier_table(
		enum spp_classifier_type type,
		const char *data,
		const struct spp_config_port_info *port)
{
	enum port_type if_type = UNDEF;
        int if_no = 0;
	struct patch_info *patch_info = NULL;
	int64_t ret_mac = 0;
	uint64_t mac_addr = 0;
	int ret_used = 0;

	if (type == SPP_CLASSIFIER_TYPE_MAC) {
		RTE_LOG(DEBUG, APP, "update_classifier_table ( type = mac, data = %s, port = %d:%d )\n",
				data, port->if_type, port->if_no);

		ret_mac = spp_config_change_mac_str_to_int64(data);
		if (unlikely(ret_mac == -1)) {
			RTE_LOG(ERR, APP, "MAC address format error. ( mac = %s )\n", data);
			return SPP_RET_NG;
		}

		mac_addr = (uint64_t)ret_mac;

		ret_used = check_mac_used_interface(mac_addr, &if_type, &if_no);
		if (port->if_type == UNDEF) {
			/* Delete(unuse) */
			if (ret_used < 0) {
				RTE_LOG(DEBUG, APP, "No MAC address. ( mac = %s )\n", data);
				return SPP_RET_OK;
			}

			patch_info = get_if_area(if_type, if_no);
			if (unlikely(patch_info == NULL)) {
				RTE_LOG(ERR, APP, "No port. ( port = %d:%d )\n", port->if_type, port->if_no);
				return SPP_RET_NG;
			}

			patch_info->mac_addr = 0;
			memset(patch_info->mac_addr_str, 0x00, SPP_CONFIG_STR_LEN);
			if (patch_info->tx_core != NULL) {
				patch_info->tx_core->mac_addr = 0;
				memset(patch_info->tx_core->mac_addr_str, 0x00, SPP_CONFIG_STR_LEN);
			}
		}
		else
		{
			/* Setting */
			if (unlikely(ret_used == 0)) {
				if (likely(port->if_type == if_type) && likely(port->if_no == if_no)) {
					RTE_LOG(DEBUG, APP, "Same MAC address and port. ( mac = %s, port = %d:%d )\n",
							data, if_type, if_no);
					return SPP_RET_OK;
				}
				else
				{
					RTE_LOG(ERR, APP, "MAC address in used. ( mac = %s )\n", data);
					return SPP_RET_USED_MAC;
				}
			}

			patch_info = get_if_area(port->if_type, port->if_no);
			if (unlikely(patch_info == NULL)) {
				RTE_LOG(ERR, APP, "No port. ( port = %d:%d )\n", port->if_type, port->if_no);
				return SPP_RET_NG;
			}

			if (unlikely(patch_info->use_flg == 0)) {
				RTE_LOG(ERR, APP, "Port not added. ( port = %d:%d )\n", port->if_type, port->if_no);
				return SPP_RET_NOT_ADD_PORT;
			}

			if (unlikely(patch_info->mac_addr != 0)) {
				RTE_LOG(ERR, APP, "Port in used. ( port = %d:%d )\n", port->if_type, port->if_no);
				return SPP_RET_USED_PORT;
			}

			patch_info->mac_addr = mac_addr;
			strcpy(patch_info->mac_addr_str, data);
			if (patch_info->tx_core != NULL) {
				patch_info->tx_core->mac_addr = mac_addr;
				strcpy(patch_info->tx_core->mac_addr_str, data);
			}
		}
	}

  /* TODO(yasufum) add desc how it is used and why changed core is kept */
	g_change_core[patch_info->tx_core_no] = 1;
	return SPP_RET_OK;
}

/* Flush command to execute it */
int
spp_flush(void)
{
	int core_cnt = 0;  /* increment core id */
	int ret_classifier = 0;
	struct spp_core_info *core_info = NULL;

	for(core_cnt = 0; core_cnt < SPP_CONFIG_CORE_MAX; core_cnt++) {
		if (g_change_core[core_cnt] == 0)
			continue;

		core_info = &g_core_info[core_cnt];
		if (core_info->type == SPP_CONFIG_CLASSIFIER_MAC) {
			ret_classifier = spp_classifier_mac_update(core_info);
			if (unlikely(ret_classifier < 0)) {
				RTE_LOG(ERR, APP, "Flush error. ( component = classifier_mac)\n");
				return SPP_RET_NG;
			}
		}
	}

	/* Finally, zero-clear g_change_core */
	memset(g_change_core, 0x00, sizeof(g_change_core));
	return SPP_RET_OK;
}

int
spp_iterate_classifier_table(
		struct spp_iterate_classifier_table_params *params)
{
	int ret;

	ret = spp_classifier_mac_iterate_table(params);
	if (unlikely(ret != 0)) {
		RTE_LOG(ERR, APP, "Cannot iterate classfier_mac_table.\n");
		return SPP_RET_NG;
	}

	return SPP_RET_OK;
}
