#ifndef _CLASSIFIER_MAC_H_
#define _CLASSIFIER_MAC_H_

/* forward declaration */
struct spp_component_info;
struct spp_iterate_classifier_table_params;

/**
 * classifier(mac address) initialize globals.
 */
int spp_classifier_mac_init(void);

/**
 * classifier(mac address) update component info.
 *
 * @param component_info
 *  point to struct spp_component_info.
 *
 * @ret_val 0  succeeded.
 * @ret_val -1 failed.
 */
int spp_classifier_mac_update(struct spp_component_info *component_info);

/**
 * classifier(mac address) thread function.
 *
 * @param id
 *  unique component ID.
 */
int spp_classifier_mac_do(int id);

/**
 * classifier(mac address) iterate classifier table.
 */
int spp_classifier_mac_iterate_table(
		struct spp_iterate_classifier_table_params *params);

#endif /* _CLASSIFIER_MAC_H_ */
