#ifndef __SPP_FORWARD_H__
#define __SPP_FORWARD_H__

/* Clear info */
void spp_forward_init(void);

/* Clear info for one element. */
void spp_forward_init_info(int id);

/* Update forward info */
int spp_forward_update(struct spp_component_info *component);

/*
 * Merge/Forward
 */
int spp_forward(int id);

/* Merge/Forward iterate component information */
int spp_forward_core_info_iterate(unsigned int lcore_id, int id,
		struct spp_iterate_core_params *params);

#endif /* __SPP_FORWARD_H__ */
