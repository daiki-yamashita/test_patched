#include <netinet/in.h>
#include <arpa/inet.h>
#include <getopt.h>

#include <rte_eth_ring.h>
#include <rte_eth_vhost.h>
#include <rte_memzone.h>
#include <rte_cycles.h>

#include "spp_vf.h"
#include "ringlatencystats.h"
#include "classifier_mac.h"
#include "spp_forward.h"
#include "command_proc.h"

/* TODO(yasufum) add desc how there are used */
#define SPP_CORE_STATUS_CHECK_MAX 5
#define SPP_RING_LATENCY_STATS_SAMPLING_INTERVAL 1000000

#define CORE_TYPE_CLASSIFIER_MAC_STR "classifier_mac"
#define CORE_TYPE_MERGE_STR          "merge"
#define CORE_TYPE_FORWARD_STR        "forward"

/* getopt_long return value for long option */
enum SPP_LONGOPT_RETVAL {
	SPP_LONGOPT_RETVAL__ = 127,

	/* add below */
	/* TODO(yasufum) add description what and why add below */
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

/* Manage number of interfaces  and port information as global variable */
/* TODO(yasufum) refactor, change if to iface */
struct if_info {
	int num_nic;
	int num_vhost;
	int num_ring;
	struct spp_port_info nic[RTE_MAX_ETHPORTS];
	struct spp_port_info vhost[RTE_MAX_ETHPORTS];
	struct spp_port_info ring[RTE_MAX_ETHPORTS];
};

/* Manage component running in core as global variable */
struct core_info {
	volatile enum spp_component_type type;
	int num;
	int id[RTE_MAX_LCORE];
};

/* Manage core status and component information as global variable */
struct core_mng_info {
	volatile enum spp_core_status status;
	volatile int ref_index;
	volatile int upd_index;
	struct core_info core[SPP_INFO_AREA_MAX];
};

/* Declare global variables */
static unsigned int g_main_lcore_id = 0xffffffff;
static struct startup_param		g_startup_param;
static struct if_info			g_if_info;
static struct spp_component_info	g_component_info[RTE_MAX_LCORE];
static struct core_mng_info		g_core_info[RTE_MAX_LCORE];

static int 				g_change_core[RTE_MAX_LCORE];  /* TODO(yasufum) add desc how it is used and why changed component is kept */
static int 				g_change_component[RTE_MAX_LCORE];

/* Print help message */
static void
usage(const char *progname)
{
	RTE_LOG(INFO, APP, "Usage: %s [EAL args] --"
			" --client-id CLIENT_ID"
			" -s SERVER_IP:SERVER_PORT"
			" [--vhost-client]\n"
			" --client-id CLIENT_ID   : My client ID\n"
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
	RTE_LOG(INFO, APP, "ring port add. (no = %d / port = %d)\n",
			ring_id, ring_port_id);
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

	RTE_LOG(INFO, APP, "vhost port add. (no = %d / port = %d)\n",
			index, vhost_port_id);
	return vhost_port_id;
}

/* Get core status */
enum spp_core_status
spp_get_core_status(unsigned int lcore_id)
{
	return g_core_info[lcore_id].status;
}

/**
 * Check status of all of cores is same as given
 *
 * It returns -1 as status mismatch if status is not same.
 * If core is in use, status will be checked.
 */
static int
check_core_status(enum spp_core_status status)
{
	unsigned int lcore_id = 0;
	RTE_LCORE_FOREACH_SLAVE(lcore_id) {
		if (g_core_info[lcore_id].status != status) {
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

/* Set core status */
static void
set_core_status(unsigned int lcore_id,
		enum spp_core_status status)
{
	g_core_info[lcore_id].status = status;
}

/* Set all core to given status */
static void
set_all_core_status(enum spp_core_status status)
{
	unsigned int lcore_id = 0;
	RTE_LCORE_FOREACH_SLAVE(lcore_id) {
		g_core_info[lcore_id].status = status;
	}
}

/**
 * Set all of component status to SPP_CORE_STOP_REQUEST if received signal
 * is SIGTERM or SIGINT
 */
static void
stop_process(int signal) {
	if (unlikely(signal != SIGTERM) &&
			unlikely(signal != SIGINT)) {
		return;
	}

	g_core_info[g_main_lcore_id].status = SPP_CORE_STOP_REQUEST;
	set_all_core_status(SPP_CORE_STOP_REQUEST);
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

	if (id >= RTE_MAX_LCORE)
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
			"app opts (client_id=%d,server=%s:%d,vhost_client=%d)\n",
			g_startup_param.client_id,
			g_startup_param.server_ip,
			g_startup_param.server_port,
			g_startup_param.vhost_client);
	return 0;
}

/**
 * Return port info of given type and num of interface
 *
 * It returns NULL value if given type is invalid.
 *
 * TODO(yasufum) refactor name of func to be more understandable (area?)
 * TODO(yasufum) refactor, change if to iface.
 */
static struct spp_port_info *
get_if_area(enum port_type if_type, int if_no)
{
	switch (if_type) {
	case PHY:
		return &g_if_info.nic[if_no];
		break;
	case VHOST:
		return &g_if_info.vhost[if_no];
		break;
	case RING:
		return &g_if_info.ring[if_no];
		break;
	default:
		return NULL;
		break;
	}
}

/**
 * Initialize g_if_info
 *
 * Clear g_if_info and set initial value.
 * TODO(yasufum) refactor, change if to iface.
 */
static void
init_if_info(void)
{
	int port_cnt;  /* increment ether ports */
	memset(&g_if_info, 0x00, sizeof(g_if_info));
	for (port_cnt = 0; port_cnt < RTE_MAX_ETHPORTS; port_cnt++) {
		g_if_info.nic[port_cnt].if_type   = UNDEF;
		g_if_info.nic[port_cnt].if_no     = port_cnt;
		g_if_info.nic[port_cnt].dpdk_port = -1;
		g_if_info.vhost[port_cnt].if_type   = UNDEF;
		g_if_info.vhost[port_cnt].if_no     = port_cnt;
		g_if_info.vhost[port_cnt].dpdk_port = -1;
		g_if_info.ring[port_cnt].if_type   = UNDEF;
		g_if_info.ring[port_cnt].if_no     = port_cnt;
		g_if_info.ring[port_cnt].dpdk_port = -1;
	}
}

/**
 * Initialize g_component_info
 */
static void
init_component_info(void)
{
	int cnt;
	memset(&g_component_info, 0x00, sizeof(g_component_info));
	for (cnt = 0; cnt < RTE_MAX_LCORE; cnt++) {
		g_component_info[cnt].component_id = cnt;
	}
	memset(g_change_component, 0x00, sizeof(g_change_component));
}

/**
 * Initialize g_core_info
 */
static void
init_core_info(void)
{
	int cnt = 0;
	memset(&g_core_info, 0x00, sizeof(g_core_info));
	set_all_core_status(SPP_CORE_STOP);
	for (cnt = 0; cnt < RTE_MAX_LCORE; cnt++) {
		g_core_info[cnt].ref_index = 0;
		g_core_info[cnt].upd_index = 1;
	}
	memset(g_change_core, 0x00, sizeof(g_change_core));
}

/**
 * Setup port info of port on host
 *
 * TODO(yasufum) refactor, change if to iface.
 */
static int
set_nic_interface(void)
{
	int nic_cnt = 0;

	/* NIC Setting */
	g_if_info.num_nic = rte_eth_dev_count();
	if (g_if_info.num_nic > RTE_MAX_ETHPORTS) {
		g_if_info.num_nic = RTE_MAX_ETHPORTS;
	}

	for (nic_cnt = 0; nic_cnt < g_if_info.num_nic; nic_cnt++) {
		g_if_info.nic[nic_cnt].dpdk_port = nic_cnt;
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
init_manage_data(void)
{
	/* Initialize interface and core infomation */
	init_if_info();
	init_core_info();
	init_component_info();

	int ret_nic = set_nic_interface();
	if (unlikely(ret_nic != 0)) {
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
		if (g_if_info.ring[ring_cnt].if_type == UNDEF) {
			continue;
		}
		spp_ringlatencystats_get_stats(ring_cnt, &stats[ring_cnt]);
		printf(", %-18d", ring_cnt);
	}
	printf("\n");

	for (stats_cnt = 0; stats_cnt < SPP_RINGLATENCYSTATS_STATS_SLOT_COUNT; stats_cnt++) {
		printf("%3dns", stats_cnt);
		for (ring_cnt = 0; ring_cnt < RTE_MAX_ETHPORTS; ring_cnt++) {
			if (g_if_info.ring[ring_cnt].if_type == UNDEF) {
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
del_vhost_sockfile(struct spp_port_info *vhost)
{
	int cnt;

	/* Do not rmeove for if it is running in vhost-client mode. */
	if (g_startup_param.vhost_client != 0)
		return;

	for (cnt = 0; cnt < RTE_MAX_ETHPORTS; cnt++) {
		if (likely(vhost[cnt].if_type == UNDEF)) {
			/* Skip removing if it is not using vhost */
			continue;
		}

		remove(get_vhost_iface_name(cnt));
	}
}

/* Get component type of target core */
enum spp_component_type
spp_get_component_type(unsigned int lcore_id)
{
	struct core_mng_info *info = &g_core_info[lcore_id];
	return info->core[info->ref_index].type;
}

/* Get component type being updated on target core */
enum spp_component_type
spp_get_component_type_update(unsigned int lcore_id)
{
	struct core_mng_info *info = &g_core_info[lcore_id];
	return info->core[info->upd_index].type;
}

/* Get core ID of target component */
unsigned int
spp_get_component_core(int component_id)
{
	struct spp_component_info *info = &g_component_info[component_id];
	return info->lcore_id;
}

/* Get usage area of target core */
static struct core_info *
get_core_info(unsigned int lcore_id)
{
	struct core_mng_info *info = &g_core_info[lcore_id];
	return &(info->core[info->ref_index]);
}

/* Check core index change */
int
spp_check_core_index(unsigned int lcore_id)
{
	struct core_mng_info *info = &g_core_info[lcore_id];
	return info->ref_index == info->upd_index;
}

/* Main process of slave core */
static int
slave_main(void *arg __attribute__ ((unused)))
{
	int ret = 0;
	int cnt = 0;
	unsigned int lcore_id = rte_lcore_id();
	enum spp_core_status status = SPP_CORE_STOP;
	struct core_mng_info *info = &g_core_info[lcore_id];
	struct core_info *core = get_core_info(lcore_id);

	RTE_LOG(INFO, APP, "Core[%d] Start.\n", lcore_id);
	set_core_status(lcore_id, SPP_CORE_IDLE);

	while((status = spp_get_core_status(lcore_id)) != SPP_CORE_STOP_REQUEST) {
		if (status != SPP_CORE_FORWARD)
			continue;

		if (spp_check_core_index(lcore_id)) {
			/* Setting with the flush command trigger. */
			info->ref_index = (info->upd_index+1)%SPP_INFO_AREA_MAX;
			core = get_core_info(lcore_id);
		}

		for (cnt = 0; cnt < core->num; cnt++) {
			if (spp_get_component_type(lcore_id) == SPP_COMPONENT_CLASSIFIER_MAC) {
				/* Classifier loops inside the function. */
				ret = spp_classifier_mac_do(core->id[cnt]);
				break;
			} else {
				/* Forward / Merge returns at once.          */
				/* It is for processing multiple components. */
				ret = spp_forward(core->id[cnt]);
				if (unlikely(ret != 0))
					break;
			}
		}
		if (unlikely(ret != 0)) {
			RTE_LOG(ERR, APP, "Core[%d] Component Error. (id = %d)\n",
					lcore_id, core->id[cnt]);
			break;
		}
	}

	set_core_status(lcore_id, SPP_CORE_STOP);
	RTE_LOG(INFO, APP, "Core[%d] End.\n", lcore_id);
	return ret;
}

/* TODO(yasufum) refactor, change if to iface. */
/* TODO(yasufum) change test using ut_main(), or add desccription for what and why use it */
/* TODO(yasufum) change to return -1 explicity if error is occured. */
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

		/* Get lcore id of main thread to set its status after */
		g_main_lcore_id = rte_lcore_id();

		int ret_manage = init_manage_data();
		if (unlikely(ret_manage != 0)) {
			break;
		}

		int ret_classifier_mac_init = spp_classifier_mac_init();
		if (unlikely(ret_classifier_mac_init != 0)) {
			break;
		}

		spp_forward_init();

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
			rte_eal_remote_launch(slave_main, NULL, lcore_id);
		}

		/* Set the status of main thread to idle */
		g_core_info[g_main_lcore_id].status = SPP_CORE_IDLE;
		int ret_wait = check_core_status_wait(SPP_CORE_IDLE);
		if (unlikely(ret_wait != 0)) {
			break;
		}

		/* Start forwarding */
		set_all_core_status(SPP_CORE_FORWARD);
		RTE_LOG(INFO, APP, "My ID %d start handling message\n", 0);
		RTE_LOG(INFO, APP, "[Press Ctrl-C to quit ...]\n");

		/* Enter loop for accepting commands */
		int ret_do = 0;
#ifndef USE_UT_SPP_VF
		while(likely(g_core_info[g_main_lcore_id].status != SPP_CORE_STOP_REQUEST)) {
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

		if (unlikely(ret_do != 0)) {
			set_all_core_status(SPP_CORE_STOP_REQUEST);
			break;
		}

		ret = 0;
		break;
	}

	/* Finalize to exit */
	if (g_main_lcore_id == rte_lcore_id())
	{
		g_core_info[g_main_lcore_id].status = SPP_CORE_STOP;
		int ret_core_end = check_core_status_wait(SPP_CORE_STOP);
		if (unlikely(ret_core_end != 0)) {
			RTE_LOG(ERR, APP, "Core did not stop.\n");
		}

		/* Remove vhost sock file if it is not running in vhost-client mode */
		del_vhost_sockfile(g_if_info.vhost);
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
 * Check mac address used on the port for registering or removing
 *
 * TODO(yasufum) refactor, change if to iface.
 */
int
spp_check_mac_used_port(uint64_t mac_addr, enum port_type if_type, int if_no)
{
	struct spp_port_info *port_info = get_if_area(if_type, if_no);
	return (mac_addr == port_info->mac_addr);
}

/*
 * Check if port has been added.
 */
int
spp_check_added_port(enum port_type if_type, int if_no)
{
	struct spp_port_info *port = get_if_area(if_type, if_no);
	return port->if_type != UNDEF;
}

/*
 * Check if component is using port.
 */
int
spp_check_used_port(enum port_type if_type, int if_no, enum spp_port_rxtx rxtx)
{
	int cnt, port_cnt, max = 0;
	struct spp_component_info *component = NULL;
	struct spp_port_info **port_array = NULL;
	struct spp_port_info *port = get_if_area(if_type, if_no);

	if (port == NULL)
		return SPP_RET_NG;

	for (cnt = 0; cnt < RTE_MAX_LCORE; cnt++) {
		component = &g_component_info[cnt];
		if (component->type == SPP_COMPONENT_UNUSE)
			continue;

		if (rxtx == SPP_PORT_RXTX_RX) {
			max = component->num_rx_port;
			port_array = component->rx_ports;
		} else if (rxtx == SPP_PORT_RXTX_TX) {
			max = component->num_tx_port;
			port_array = component->tx_ports;
		}
		for (port_cnt = 0; port_cnt < max; port_cnt++) {
			if (unlikely(port_array[port_cnt] == port))
				return cnt;
		}
	}

	return SPP_RET_NG;
}

/*
 * Set port change to component.
 */
static void
set_component_change_port(struct spp_port_info *port, enum spp_port_rxtx rxtx)
{
	int ret = 0;
	if ((rxtx == SPP_PORT_RXTX_RX) || (rxtx == SPP_PORT_RXTX_ALL)) {
		ret = spp_check_used_port(port->if_type, port->if_no, SPP_PORT_RXTX_RX);
		if (ret >= 0)
			g_change_component[ret] = 1;
	}

	if ((rxtx == SPP_PORT_RXTX_TX) || (rxtx == SPP_PORT_RXTX_ALL)) {
		ret = spp_check_used_port(port->if_type, port->if_no, SPP_PORT_RXTX_TX);
		if (ret >= 0)
			g_change_component[ret] = 1;
	}
}

int
spp_update_classifier_table(
		enum spp_command_action action,
		enum spp_classifier_type type,
		const char *data,
		const struct spp_port_index *port)
{
	struct spp_port_info *port_info = NULL;
	int64_t ret_mac = 0;
	uint64_t mac_addr = 0;

	if (type == SPP_CLASSIFIER_TYPE_MAC) {
		RTE_LOG(DEBUG, APP, "update_classifier_table ( type = mac, data = %s, port = %d:%d )\n",
				data, port->if_type, port->if_no);

		ret_mac = spp_change_mac_str_to_int64(data);
		if (unlikely(ret_mac == -1)) {
			RTE_LOG(ERR, APP, "MAC address format error. ( mac = %s )\n", data);
			return SPP_RET_NG;
		}
		mac_addr = (uint64_t)ret_mac;

		port_info = get_if_area(port->if_type, port->if_no);
		if (unlikely(port_info == NULL)) {
			RTE_LOG(ERR, APP, "No port. ( port = %d:%d )\n",
					port->if_type, port->if_no);
			return SPP_RET_NG;
		}
		if (unlikely(port_info->if_type == UNDEF)) {
			RTE_LOG(ERR, APP, "Port not added. ( port = %d:%d )\n",
					port->if_type, port->if_no);
			return SPP_RET_NG;
		}

		if (action == SPP_CMD_ACTION_DEL) {
			/* Delete */
			if ((port_info->mac_addr != 0) &&
					unlikely(port_info->mac_addr != mac_addr)) {
				RTE_LOG(ERR, APP, "MAC address is different. ( mac = %s )\n",
						data);
				return SPP_RET_NG;
			}

			port_info->mac_addr = 0;
			memset(port_info->mac_addr_str, 0x00, SPP_MIN_STR_LEN);
		}
		else if (action == SPP_CMD_ACTION_ADD) {
			/* Setting */
			if (unlikely(port_info->mac_addr != 0)) {
				RTE_LOG(ERR, APP, "Port in used. ( port = %d:%d )\n",
						 port->if_type, port->if_no);
				return SPP_RET_NG;
			}

			port_info->mac_addr = mac_addr;
			strcpy(port_info->mac_addr_str, data);
		}
	}

	/* TODO(yasufum) add desc how it is used and why changed core is kept */
	set_component_change_port(port_info, SPP_PORT_RXTX_TX);
	return SPP_RET_OK;
}

/* Get free component */
static int
get_free_component(void)
{
	int cnt = 0;
	for (cnt = 0; cnt < RTE_MAX_LCORE; cnt++) {
		if (g_component_info[cnt].type == SPP_COMPONENT_UNUSE)
			return cnt;
	}
	return -1;
}

/* Get name matching component */
int
spp_get_component_id(const char *name)
{
	int cnt = 0;
	if (name[0] == '\0')
		return SPP_RET_NG;

	for (cnt = 0; cnt < RTE_MAX_LCORE; cnt++) {
		if (strcmp(name, g_component_info[cnt].name) == 0)
			return cnt;
	}
	return SPP_RET_NG;
}

static int
get_del_core_element(int info, int num, int *array)
{
	int cnt;
	int match = -1;
	int max = num;

	for (cnt = 0; cnt < max; cnt++) {
		if (info == array[cnt])
			match = cnt;
	}

	if (match < 0)
		return -1;

	/* Last element is excluded from movement. */
	max--;

	for (cnt = match; cnt < max; cnt++) {
		array[cnt] = array[cnt+1];
	}

	/* Last element is cleared. */
	array[cnt] = 0;
	return 0;
}

/* Component command to execute it */
int
spp_update_component(
		enum spp_command_action action,
		const char *name,
		unsigned int lcore_id,
		enum spp_component_type type)
{
	int ret = SPP_RET_NG;
	int ret_del = -1;
	int component_id = 0;
	unsigned int tmp_lcore_id = 0;
	struct spp_component_info *component = NULL;
	struct core_info *core = NULL;
	struct core_mng_info *info = NULL;

	switch (action) {
	case SPP_CMD_ACTION_START:
		info = &g_core_info[lcore_id];
		if (info->status == SPP_CORE_UNUSE) {
			RTE_LOG(ERR, APP, "Core unavailable.\n");
			return SPP_RET_NG;
		}

		component_id = spp_get_component_id(name);
		if (component_id >= 0) {
			RTE_LOG(ERR, APP, "Component name in used.\n");
			return SPP_RET_NG;
		}

		component_id = get_free_component();
		if (component_id < 0) {
			RTE_LOG(ERR, APP, "Component upper limit is over.\n");
			return SPP_RET_NG;
		}

		core = &info->core[info->upd_index];
		if ((core->type != SPP_COMPONENT_UNUSE) && (core->type != type)) {
			RTE_LOG(ERR, APP, "Component type is error.\n");
			return SPP_RET_NG;
		}

		component = &g_component_info[component_id];
		memset(component, 0x00, sizeof(struct spp_component_info));
		strcpy(component->name, name);
		component->type         = type;
		component->lcore_id     = lcore_id;
		component->component_id = component_id;

		core->type = type;
		core->id[core->num] = component_id;
		core->num++;
		ret = SPP_RET_OK;
		tmp_lcore_id = lcore_id;
		break;

	case SPP_CMD_ACTION_STOP:
		component_id = spp_get_component_id(name);
		if (component_id < 0)
			return SPP_RET_OK;

		component = &g_component_info[component_id];
		tmp_lcore_id = component->lcore_id;
		memset(component, 0x00, sizeof(struct spp_component_info));

		info = &g_core_info[tmp_lcore_id];
		core = &info->core[info->upd_index];
		ret_del = get_del_core_element(component_id,
				core->num, core->id);
		if (ret_del >= 0)
			/* If deleted, decrement number. */
			core->num--;

		if (core->num == 0)
			core->type = SPP_COMPONENT_UNUSE;

		ret = SPP_RET_OK;
		break;

	default:
		break;
	}

	g_change_core[tmp_lcore_id] = 1;
	return ret;
}

static int
check_port_element(
		struct spp_port_info *info,
		int num,
		struct spp_port_info *array[])
{
	int cnt = 0;
	int match = -1;
	for (cnt = 0; cnt < num; cnt++) {
		if (info == array[cnt])
			match = cnt;
	}
	return match;
}

static int
get_del_port_element(
		struct spp_port_info *info,
		int num,
		struct spp_port_info *array[])
{
	int cnt = 0;
	int match = -1;
	int max = num;

	match = check_port_element(info, num, array);
	if (match < 0)
		return -1;

	/* Last element is excluded from movement. */
	max--;

	for (cnt = match; cnt < max; cnt++) {
		array[cnt] = array[cnt+1];
	}

	/* Last element is cleared. */
	array[cnt] = NULL;
	return 0;
}

/* Port add or del to execute it */
int
spp_update_port(enum spp_command_action action,
		const struct spp_port_index *port,
		enum spp_port_rxtx rxtx,
		const char *name)
{
	int ret = SPP_RET_NG;
	int ret_check = -1;
	int ret_del = -1;
	int component_id = 0;
	struct spp_component_info *component = NULL;
	struct spp_port_info *port_info = NULL;
	int *num = NULL;
	struct spp_port_info **ports = NULL;

	component_id = spp_get_component_id(name);
	if (component_id < 0) {
		RTE_LOG(ERR, APP, "Unknown component by port command. (component = %s)\n",
				name);
		return SPP_RET_NG;
	}

	component = &g_component_info[component_id];
	port_info = get_if_area(port->if_type, port->if_no);
	if (rxtx == SPP_PORT_RXTX_RX) {
		num = &component->num_rx_port;
		ports = component->rx_ports;
	} else {
		num = &component->num_tx_port;
		ports = component->tx_ports;
	}

	switch (action) {
	case SPP_CMD_ACTION_ADD:
		ret_check = check_port_element(port_info, *num, ports);
		if (ret_check >= 0)
			return SPP_RET_OK;

		if (*num >= RTE_MAX_ETHPORTS) {
			RTE_LOG(ERR, APP, "Port upper limit is over.\n");
			break;
		}

		port_info->if_type = port->if_type;
		ports[*num] = port_info;
		(*num)++;

		ret = SPP_RET_OK;
		break;

	case SPP_CMD_ACTION_DEL:
		ret_del = get_del_port_element(port_info, *num, ports);
		if (ret_del == 0)
			(*num)--; /* If deleted, decrement number. */
		ret = SPP_RET_OK;
		break;
	default:
		break;
	}

	g_change_component[component_id] = 1;
	return ret;
}

/* Flush initial setting of each interface. */
static int
flush_port(void)
{
	int ret = 0;
	int cnt = 0;
	struct spp_port_info *port = NULL;

	/* Initialize added vhost. */
	for (cnt = 0; cnt < RTE_MAX_ETHPORTS; cnt++) {
		port = &g_if_info.vhost[cnt];
		if ((port->if_type != UNDEF) && (port->dpdk_port < 0)) {
			ret = add_vhost_pmd(port->if_no, g_startup_param.vhost_client);
			if (ret < 0)
				return SPP_RET_NG;
			port->dpdk_port = ret;
		}
	}

	/* Initialize added ring. */
	for (cnt = 0; cnt < RTE_MAX_ETHPORTS; cnt++) {
		port = &g_if_info.ring[cnt];
		if ((port->if_type != UNDEF) && (port->dpdk_port < 0)) {
			ret = add_ring_pmd(port->if_no);
			if (ret < 0)
				return SPP_RET_NG;
			port->dpdk_port = ret;
		}
	}
	return SPP_RET_OK;
}

/* Flush changed core. */
static void
flush_core(void)
{
	int cnt = 0;
	struct core_mng_info *info = NULL;

	/* Changed core has changed index. */
	for (cnt = 0; cnt < RTE_MAX_LCORE; cnt++) {
		if (g_change_core[cnt] != 0) {
			info = &g_core_info[cnt];
			info->upd_index = info->ref_index;
		}
	}

	/* Waiting for changed core change. */
	for (cnt = 0; cnt < RTE_MAX_LCORE; cnt++) {
		if (g_change_core[cnt] != 0) {
			info = &g_core_info[cnt];
			while(likely(info->ref_index == info->upd_index))
				rte_delay_us_block(SPP_CHANGE_UPDATE_INTERVAL);

			memcpy(&info->core[info->upd_index],
					&info->core[info->ref_index],
					sizeof(struct core_info)); 
		}
	}
}

/* Flush chagned component */
static int
flush_component(void)
{
	int ret = 0;
	int cnt = 0;
	struct spp_component_info *component_info = NULL;

	for(cnt = 0; cnt < RTE_MAX_LCORE; cnt++) {
		if (g_change_component[cnt] == 0)
			continue;

		component_info = &g_component_info[cnt];
		if (component_info->type == SPP_COMPONENT_CLASSIFIER_MAC) {
			ret = spp_classifier_mac_update(component_info);
		} else {
			ret = spp_forward_update(component_info);
		}
		if (unlikely(ret < 0)) {
			RTE_LOG(ERR, APP, "Flush error. ( component = %s, type = %d)\n",
					component_info->name, component_info->type);
			return SPP_RET_NG;
		}
	}
	return SPP_RET_OK;
}

/* Flush command to execute it */
int
spp_flush(void)
{
	int ret = -1;

	/* Initial setting of each interface. */
	ret = flush_port();
	if (ret < 0)
		return ret;

	/* Flush of core index. */
	flush_core();
	memset(g_change_core, 0x00, sizeof(g_change_core));

	/* Flush of component */
	ret = flush_component();
	if (ret < 0)
		return ret;

	/* Finally, zero-clear g_change_core */
	memset(g_change_component, 0x00, sizeof(g_change_component));
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

/**
 * Sepeparate port id of combination of iface type and number and
 * assign to given argment, if_type and if_no.
 *
 * For instance, 'ring:0' is separated to 'ring' and '0'.
 *
 * TODO(yasufum) change if to iface
 */
int
spp_get_if_info(const char *port, enum port_type *if_type, int *if_no)
{
	enum port_type type = UNDEF;
	const char *no_str = NULL;
	char *endptr = NULL;

	/* Find out which type of interface from port */
	if (strncmp(port, SPP_IFTYPE_NIC_STR ":", strlen(SPP_IFTYPE_NIC_STR)+1) == 0) {
		/* NIC */
		type = PHY;
		no_str = &port[strlen(SPP_IFTYPE_NIC_STR)+1];
	} else if (strncmp(port, SPP_IFTYPE_VHOST_STR ":", strlen(SPP_IFTYPE_VHOST_STR)+1) == 0) {
		/* VHOST */
		type = VHOST;
		no_str = &port[strlen(SPP_IFTYPE_VHOST_STR)+1];
	} else if (strncmp(port, SPP_IFTYPE_RING_STR ":", strlen(SPP_IFTYPE_RING_STR)+1) == 0) {
		/* RING */
		type = RING;
		no_str = &port[strlen(SPP_IFTYPE_RING_STR)+1];
	} else {
		/* OTHER */
		RTE_LOG(ERR, APP, "Unknown interface type. (port = %s)\n", port);
		return -1;
	}

	/* Change type of number of interface */
	int ret_no = strtol(no_str, &endptr, 0);
	if (unlikely(no_str == endptr) || unlikely(*endptr != '\0')) {
		/* No IF number */
		RTE_LOG(ERR, APP, "No interface number. (port = %s)\n", port);
		return -1;
	}

	*if_type = type;
	*if_no = ret_no;

	RTE_LOG(DEBUG, APP, "Port = %s => Type = %d No = %d\n",
			port, *if_type, *if_no);
	return 0;
}

/**
 * Generate a formatted string of conbination from interface type and
 * number and assign to given 'port'
 */
int spp_format_port_string(char *port, enum port_type if_type, int if_no)
{
	const char* if_type_str;

	switch (if_type) {
	case PHY:
		if_type_str = SPP_IFTYPE_NIC_STR;
		break;
	case RING:
		if_type_str = SPP_IFTYPE_RING_STR;
		break;
	case VHOST:
		if_type_str = SPP_IFTYPE_VHOST_STR;
		break;
	default:
		return -1;
	}

	sprintf(port, "%s:%d", if_type_str, if_no);

	return 0;
}

/**
 * Change mac address of 'aa:bb:cc:dd:ee:ff' to int64 and return it
 */
int64_t
spp_change_mac_str_to_int64(const char *mac)
{
	int64_t ret_mac = 0;
	int64_t token_val = 0;
	int token_cnt = 0;
	char tmp_mac[SPP_MIN_STR_LEN];
	char *str = tmp_mac;
	char *saveptr = NULL;
	char *endptr = NULL;

	RTE_LOG(DEBUG, APP, "MAC address change. (mac = %s)\n", mac);

	strcpy(tmp_mac, mac);
	while(1) {
		/* Split by colon(':') */
		char *ret_tok = strtok_r(str, ":", &saveptr);
		if (unlikely(ret_tok == NULL)) {
			break;
		}

		/* Check for mal-formatted address */
		if (unlikely(token_cnt >= ETHER_ADDR_LEN)) {
			RTE_LOG(ERR, APP, "MAC address format error. (mac = %s)\n",
					 mac);
			return -1;
		}

		/* Convert string to hex value */
		int ret_tol = strtol(ret_tok, &endptr, 16);
		if (unlikely(ret_tok == endptr) || unlikely(*endptr != '\0')) {
			break;
		}

		/* Append separated value to the result */
		token_val = (int64_t)ret_tol;
		ret_mac |= token_val << (token_cnt * 8);
		token_cnt++;
		str = NULL;
	}

	RTE_LOG(DEBUG, APP, "MAC address change. (mac = %s => 0x%08lx)\n",
			 mac, ret_mac);
	return ret_mac;
}

/**
 * Return the type of forwarder as a member of enum of spp_component_type
 */
enum spp_component_type
spp_change_component_type(const char *type_str)
{
	if(strncmp(type_str, CORE_TYPE_CLASSIFIER_MAC_STR,
			 strlen(CORE_TYPE_CLASSIFIER_MAC_STR)+1) == 0) {
		/* Classifier */
		return SPP_COMPONENT_CLASSIFIER_MAC;
	} else if (strncmp(type_str, CORE_TYPE_MERGE_STR,
			 strlen(CORE_TYPE_MERGE_STR)+1) == 0) {
		/* Merger */
		return SPP_COMPONENT_MERGE;
	} else if (strncmp(type_str, CORE_TYPE_FORWARD_STR,
			 strlen(CORE_TYPE_FORWARD_STR)+1) == 0) {
		/* Forwarder */
		return SPP_COMPONENT_FORWARD;
	}
	return SPP_COMPONENT_UNUSE;
}
