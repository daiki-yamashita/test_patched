#ifndef _COMMAND_PROC_H_
#define _COMMAND_PROC_H_

/**
 * intialize command processor.
 *
 * @param controller_ip
 *  controller listen ip address.
 *
 * @param controller_port
 *  controller listen port number.
 *
 * @ret_val 0 succeeded.
 * @ret_val -1 failed.
 */
int
spp_command_proc_init(const char *controller_ip, int controller_port);

/**
 * process command from controller.
 */
void
spp_command_proc_do(void);

#endif /* _COMMAND_PROC_H_ */
