# SPP_VF

SPP_VF is a SR-IOV like network functionality using DPDK for NFV.

## Overview

The application distributes incoming packets depends on MAC address
similar to SR-IOV functionality.
Network configuration is defined in JSON config file which is imported
while launching the application.
The configuration is able to change after initialization by sending
commnad from spp controller.

SPP_VF is a multi-thread application.
It consists of manager thread and forwarder threads.
There are three types of forwarder for 1:1, 1:N and N:1.

  * forward: 1:1
  * classifier_mac: 1:N (Destination is determined by MAC address)
  * merge: N:1

This is an example of network configration, in which one classifier_mac,
one merger and four forwarders are runnig in spp_vf process for two
destinations of vhost interface.
Incoming packets from rx on host1 are sent to each of vhosts on guest
by looking MAC address in the packet..

![spp_vf_overview](spp_vf_overview.svg)

## Build the Application

See [setup guide](setup_guide.md).

## Running the Application

See [how to use](how_to_use.md).

## Sample Usage

See [sample usage](sample_usage.md).

## Explanation

The following sections provide some explanation of the code.

### Configuration

The config file is imported after rte_eal_init() and initialization
of the application.
spp_config_load_file() is defined in spp_config.c and default file
path SPP_CONFIG_FILE_PATH is defined in its header file..

  ```c
  /* set default config file path */
  strcpy(config_file_path, SPP_CONFIG_FILE_PATH);

  unsigned int main_lcore_id = 0xffffffff;
  while(1) {
  	/* Parse dpdk parameters */
  	int ret_parse = parse_dpdk_args(argc, argv);
  	if (unlikely(ret_parse != 0)) {
  		break;
  	}

  	/* DPDK initialize */
  	int ret_dpdk = rte_eal_init(argc, argv);
  	if (unlikely(ret_dpdk < 0)) {
  		break;
  	}

  	/* Skip dpdk parameters */
  	argc -= ret_dpdk;
  	argv += ret_dpdk;

  	/* Set log level  */
  	rte_log_set_global_level(RTE_LOG_LEVEL);

  	/* Parse application parameters */
  	ret_parse = parse_app_args(argc, argv);
  	if (unlikely(ret_parse != 0)) {
  		break;
  	}

  	RTE_LOG(INFO, APP, "Load config file(%s)\n", config_file_path);

  	/* Load config */
  	int ret_config = spp_config_load_file(config_file_path, 0, &g_config);
  	if (unlikely(ret_config != 0)) {
  		break;
  	}

  	/* Get core id. */
  	main_lcore_id = rte_lcore_id();
  ```

spp_config_load_file() uses [jansson]() for parsing JSON.
json_load_file() is a function  of jansson to parse raw JSON
file and return json_t object as a result.
In spp_config_load_file(), configuration of classifier table and
resource assignment of threads are loaded into config of spp.

After importing config, each of threads are launched.

  ```c
	/* Start  thread */
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
  ```

### Forwarding


### Packet Cloning


