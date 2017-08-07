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
 * @retval 0  succeeded.
 * @retval -1 failed.
 */
int
spp_command_proc_init(const char *controller_ip, int controller_port);

/**
 * process command from controller.
 *
 * @retval 0  succeeded.
 * @retval -1 failed.
 */
int
spp_command_proc_do(void);

#endif /* _COMMAND_PROC_H_ */
