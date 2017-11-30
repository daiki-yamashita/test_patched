#ifndef _COMMAND_DEC_H_
#define _COMMAND_DEC_H_

//#include "spp_vf.h"

/* max number of command per request */
#define SPP_CMD_MAX_COMMANDS 32

/* maximum number of parameters per command */
#define SPP_CMD_MAX_PARAMETERS 8

/* command name string buffer size (include null char) */
#define SPP_CMD_NAME_BUFSZ  32

/* command value string buffer size (include null char) */
#define SPP_CMD_VALUE_BUFSZ 128

/* string that specify unused */
#define SPP_CMD_UNUSE "unuse"

/* decode error code */
enum spp_command_decode_error_code {
	/* not use 0, in general 0 is ok */
	SPP_CMD_DERR_BAD_FORMAT = 1,
	SPP_CMD_DERR_UNKNOWN_COMMAND,
	SPP_CMD_DERR_NO_PARAM,
	SPP_CMD_DERR_BAD_TYPE,
	SPP_CMD_DERR_BAD_VALUE,
};

/* command type
	do it same as the order of COMMAND_TYPE_STRINGS */
enum spp_command_type {
	SPP_CMDTYPE_CLASSIFIER_TABLE,
	SPP_CMDTYPE_FLUSH,
	SPP_CMDTYPE_CLIENT_ID,
	SPP_CMDTYPE_STATUS,
	SPP_CMDTYPE_EXIT,
	SPP_CMDTYPE_COMPONENT,
	SPP_CMDTYPE_PORT,
};

/* "classifier_table" command specific parameters */
struct spp_command_classifier_table {
	enum spp_command_action action;
	enum spp_classifier_type type;
	char value[SPP_CMD_VALUE_BUFSZ];
	struct spp_port_index port;
};

/* "flush" command specific parameters */
struct spp_command_flush {
	/* nothing specific */
};

/* "component" command parameters */
struct spp_command_component {
	enum spp_command_action action;
	char name[SPP_CMD_NAME_BUFSZ];
	unsigned int core;
	enum spp_component_type type;
};

/* "port" command parameters */
struct spp_command_port {
	enum spp_command_action action;
	struct spp_port_index port;
	enum spp_port_rxtx rxtx;
	char name[SPP_CMD_NAME_BUFSZ];
};

/* command parameters */
struct spp_command {
	enum spp_command_type type;

	union {
		struct spp_command_classifier_table classifier_table;
		struct spp_command_flush flush;
		struct spp_command_component component;
		struct spp_command_port port;
	} spec;
};

/* request parameters */
struct spp_command_request {
	int num_command;
	int num_valid_command;
	struct spp_command commands[SPP_CMD_MAX_COMMANDS];

	int is_requested_client_id;
	int is_requested_status;
	int is_requested_exit;
};

/* decode error information */
struct spp_command_decode_error {
	int code;
	char value_name[SPP_CMD_NAME_BUFSZ];
	char value[SPP_CMD_VALUE_BUFSZ];
};

/* decode request from no-null-terminated string */
int spp_command_decode_request(struct spp_command_request *request,
		const char *request_str, size_t request_str_len,
		struct spp_command_decode_error *error);

#endif /* _COMMAND_DEC_H_ */
