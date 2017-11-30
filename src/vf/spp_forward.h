#ifndef __SPP_FORWARD_H__
#define __SPP_FORWARD_H__


void spp_forward_init(void);

void spp_forward_init_info(int id);

int spp_forward_update(struct spp_component_info *component);

/*
 * Merge/Forward
 */
int spp_forward(int id);

#endif /* __SPP_FORWARD_H__ */
