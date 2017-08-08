#ifndef _COMMAND_CONN_H_
#define _COMMAND_CONN_H_

/** result code - temporary error. please retry */
#define SPP_CONNERR_TEMPORARY -1
/** result code - fatal error occurred. should teminate process. */
#define SPP_CONNERR_FATAL     -2

/**
 * intialize command connection.
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
int spp_command_conn_init(const char *controller_ip, int controller_port);

/**
 * connect to controller.
 *
 * @note bocking.
 *
 * @param sock
 *  socket that connect to controller.
 *
 * @retval 0                     succeeded.
 * @retval SPP_CONNERR_TEMPORARY temporary error. please retry.
 */
int spp_connect_to_controller(int *sock);

/**
 * receive message.
 *
 * @note non-blocking.
 *
 * @param sock
 *  socket that read data.
 *
 * @retval 0 <                   succeeded. number of bytes received.
 * @retval 0                     no receive message.
 * @retval SPP_CONNERR_TEMPORARY temporary error. please reconnect.
 * @retval SPP_CONNERR_FATAL     fatal error occurred. should teminate process.
 */
int spp_receive_message(int *sock, char **msgbuf);

/**
 * send message.
 *
 * @note non-blocking.
 *
 * @param sock
 *  socket that write data.
 *
 * @param message
 *  send data.
 *
 * @param message_len
 *  send data length.
 *
 * @retval 0                     succeeded.
 * @retval SPP_CONNERR_TEMPORARY temporary error. please reconnect.
 */
int spp_send_message(int *sock, const char* message, size_t message_len);

#endif /* _COMMAND_CONN_H_ */
