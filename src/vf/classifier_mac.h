#ifndef _CLASSIFIER_MAC_H_
#define _CLASSIFIER_MAC_H_

/* forward declaration */
struct spp_core_info;

/**
 * classifier(mac address) update component info.
 *
 * @param core_info
 *  point to struct spp_core_info.
 *
 * @ret_val 0  succeeded.
 * @ret_val -1 failed.
 */
int spp_classifier_mac_update(struct spp_core_info *core_info);

/**
 * classifier(mac address) thread function.
 *
 * @param arg
 *  pointer to struct spp_core_info.
 */
int spp_classifier_mac_do(void *arg);

#endif /* _CLASSIFIER_MAC_H_ */
