# SPP_VF

SPP_VF is a SR-IOV like network functionality using DPDK for NFV.

## Overview

The application distributes incoming packets referring virtual MAC
address similar to SR-IOV functionality.
Network configuration is defined in JSON config file which is imported
while launching the application.
The configuration is able to change after initialization by sending
commnad from spp controller.

SPP_VF is a multi-thread application.
It consists of manager thread and forwarder threads.
There are three types of forwarder for 1:1, 1:N and N:1 as following.

  * forward: 1:1
  * classifier_mac: 1:N (Destination is determined by MAC address)
  * merge: N:1

This is an example of network configration, in which one classifier_mac,
one merger and four forwarders are runnig in SPP_VF process for two
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

### Forwarding

SPP_VF supports three types of forwarding, for 1:1, 1:N and N:1.
After importing config, each of forwarding threads are launched
from`rte_eal_remote_launch()`.

  ```c
  /* spp_vf.c */

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

`spp_classifier_mac_do()` is a forwarding function of 1:N defined in
`classifier_mac.c`.
Configuration of destination is managed as a table structured info.
`classifier_mac_info` and `classifier_mac_mng_info` struct are for
the purpose.

TODO(yasufum) add desc for table structure and it's doubled for
redundancy.

  ```c
  /* classifier_mac.c */

  /* classifier information */
  struct classifier_mac_info {
  	struct rte_hash *classifier_table;
  	int num_active_classified;
  	int active_classifieds[RTE_MAX_ETHPORTS];
  	int default_classified;
  };

  /* classifier management information */
  struct classifier_mac_mng_info {
  	struct classifier_mac_info info[NUM_CLASSIFIER_MAC_INFO];
  	volatile int ref_index;
  	volatile int upd_index;
  	struct classified_data classified_data[RTE_MAX_ETHPORTS];
  };
  ```

In `spp_classifier_mac_do()`, it receives packets from rx port and send them
to destinations with `classify_packet()`.
`classifier_info` is an argument of `classify_packet()` and is used to decide
the destinations.

  ```c
  /* classifier_mac.c */

	while(likely(core_info->status == SPP_CORE_IDLE) ||
			likely(core_info->status == SPP_CORE_FORWARD)) {

		while(likely(core_info->status == SPP_CORE_FORWARD)) {
			/* change index of update side */
			change_update_index(classifier_mng_info, lcore_id);

			/* decide classifier infomation of the current cycle */
			classifier_info = classifier_mng_info->info +
					classifier_mng_info->ref_index;

			/* drain tx packets, if buffer is not filled for interval */
			cur_tsc = rte_rdtsc();
			if (unlikely(cur_tsc - prev_tsc > drain_tsc)) {
				for (i = 0; i < n_classified_data; i++) {
					if (unlikely(classified_data[i].num_pkt != 0)) {
						RTE_LOG(DEBUG, SPP_CLASSIFIER_MAC,
                        					"transimit packets (drain). "
								"index=%d, "
								"num_pkt=%hu, "
								"interval=%lu\n",
								i,
								classified_data[i].num_pkt,
								cur_tsc - prev_tsc);
						transmit_packet(&classified_data[i]);
					}
				}
				prev_tsc = cur_tsc;
			}

			/* retrieve packets */
			n_rx = rte_eth_rx_burst(core_info->rx_ports[0].dpdk_port, 0,
					rx_pkts, MAX_PKT_BURST);
			if (unlikely(n_rx == 0))
				continue;

#ifdef SPP_RINGLATENCYSTATS_ENABLE
			if (core_info->rx_ports[0].if_type == RING)
				spp_ringlatencystats_calculate_latency(
						core_info->rx_ports[0].if_no, rx_pkts, n_rx);
#endif

			/* classify and transmit (filled) */
			classify_packet(rx_pkts, n_rx, classifier_info, classified_data);
		}
	}
  ```

On the other hand, `spp_forward` is for 1:1 or N:1 (called as merge)
forwarding defined in `spp_forward.c`.
Source and destination ports are decided from `core_info`
and given to `set_use_interface()` in which first argment is
destination info and second one is source.

  ```c
  /* spp_forward.c */

	/* RX/TX Info setting */
	rxtx_num = core_info->num_rx_port;
	for (if_cnt = 0; if_cnt < rxtx_num; if_cnt++) {
		set_use_interface(&patch[if_cnt].rx,
				&core_info->rx_ports[if_cnt]);
		if (core_info->type == SPP_CONFIG_FORWARD) {
			/* FORWARD */
			set_use_interface(&patch[if_cnt].tx,
					&core_info->tx_ports[if_cnt]);
		} else {
			/* MERGE */
			set_use_interface(&patch[if_cnt].tx,
					&core_info->tx_ports[0]);
		}
	}
  ```

  After ports are decided, forwarding is executed.

  ```c
  /* spp_forward.c */

	int cnt, nb_rx, nb_tx, buf;
	struct spp_core_port_info *rx;
	struct spp_core_port_info *tx;
	struct rte_mbuf *bufs[MAX_PKT_BURST];
	while (likely(core_info->status == SPP_CORE_IDLE) ||
			likely(core_info->status == SPP_CORE_FORWARD)) {
		while (likely(core_info->status == SPP_CORE_FORWARD)) {
			for (cnt = 0; cnt < rxtx_num; cnt++) {
				rx = &patch[cnt].rx;
				tx = &patch[cnt].tx;

				/* Packet receive */
				nb_rx = rte_eth_rx_burst(rx->dpdk_port, 0, bufs, MAX_PKT_BURST);
				if (unlikely(nb_rx == 0)) {
					continue;
				}

#ifdef SPP_RINGLATENCYSTATS_ENABLE
				if (rx->if_type == RING) {
					/* Receive port is RING */
					spp_ringlatencystats_calculate_latency(rx->if_no,
							bufs, nb_rx);
				}
				if (tx->if_type == RING) {
					/* Send port is RING */
					spp_ringlatencystats_add_time_stamp(tx->if_no,
							bufs, nb_rx);
				}
#endif /* SPP_RINGLATENCYSTATS_ENABLE */

				/* Send packet */
				nb_tx = rte_eth_tx_burst(tx->dpdk_port, 0, bufs, nb_rx);

				/* Free any unsent packets. */
				if (unlikely(nb_tx < nb_rx)) {
					for (buf = nb_tx; buf < nb_rx; buf++) {
						rte_pktmbuf_free(bufs[buf]);
					}
				}
			}
		}
	}
  ```

### L2 Multicast Support

Multicast for resolving ARP requests is also supported in SPP_VF.
It is implemented as `handle_l2multicast_packet()` and called from
`classify_packet()` for incoming multicast packets.

  ```c
  /* classify_packet() in classifier_mac.c */

  /* L2 multicast(include broadcast) ? */
  if (unlikely(is_multicast_ether_addr(&eth->d_addr))) {
  	RTE_LOG(DEBUG, SPP_CLASSIFIER_MAC,
  			"multicast mac address.\n");
  	handle_l2multicast_packet(rx_pkts[i],
  			classifier_info, classified_data);
  	continue;
  }
  ```

For distributing multicast packet, it is cloned with
`rte_mbuf_refcnt_update()`.

  ```c
  /* classifier_mac.c */

  /* handle L2 multicast(include broadcast) packet */
  static inline void
  handle_l2multicast_packet(struct rte_mbuf *pkt,
  		struct classifier_mac_info *classifier_info,
  		struct classified_data *classified_data)
  {
  	int i;

  	if (unlikely(classifier_info->num_active_classified == 0)) {
  		RTE_LOG(ERR, SPP_CLASSIFIER_MAC, "No mac address.(l2multicast packet)\n");
  		rte_pktmbuf_free(pkt);
  		return;
  	}

  	rte_mbuf_refcnt_update(pkt, classifier_info->num_active_classified);

  	for (i= 0; i < classifier_info->num_active_classified; i++) {
  		push_packet(pkt, classified_data +
  				(long)classifier_info->active_classifieds[i]);
  	}
  }
  ```
