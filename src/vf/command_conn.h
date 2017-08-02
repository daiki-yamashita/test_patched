#ifndef _COMMAND_CONN_H_
#define _COMMAND_CONN_H_

/**
 * intialize command connection.
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
int spp_command_conn_init(const char *controller_ip, int controller_port);

/**
 * connect to controller.
 *
 * @note bocking.
 *
 * @param sock
 *  socket that connect to controller.
 *
 * @ret_val 0 succeeded.
 * @ret_val -1 failed.
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
 * @ret_val 0 succeeded.
 * @ret_val -1 failed.
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
 * @ret_val 0 succeeded.
 * @ret_val -1 failed.
 */
int spp_send_message(int *sock, const char* message, size_t message_len);

#endif /* _COMMAND_CONN_H_ */
