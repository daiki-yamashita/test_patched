#ifndef _COMMAND_DEC_H_
#define _COMMAND_DEC_H_

/* max number of command per request */
#define SPP_CMD_MAX_COMMANDS 32

/* command name string buffer size (include null char) */
#define SPP_CMD_NAME_BUFSZ  32

/* command value string buffer size (include null char) */
#define SPP_CMD_VALUE_BUFSZ 128

/* string that specify unused */
#define SPP_CMD_UNUSE "unuse"

/* component type */
#define spp_component_type spp_core_type

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
#if 0 /* not supported yet yet */
	SPP_CMDTYPE_ADD,
	SPP_CMDTYPE_COMPONENT,
#endif
	SPP_CMDTYPE_CLASSIFIER_TABLE,
	SPP_CMDTYPE_FLUSH,
#if 0 /* not supported yet */
	SPP_CMDTYPE_FORWARD,
	SPP_CMDTYPE_STOP,
#endif
	SPP_CMDTYPE_PROCESS,
	SPP_CMDTYPE_STATUS,
};

#if 0 /* not supported yet */
/* "add" command parameters */
struct spp_command_add {
	int num_port;
	struct spp_config_port_info ports[RTE_MAX_ETHPORTS];
};

/* "component" command specific parameters */
struct spp_command_component {
	enum spp_component_type type;
	unsigned int core_id;
	int num_rx_port;
	int num_tx_port;
	struct spp_config_port_info rx_ports[RTE_MAX_ETHPORTS];
	struct spp_config_port_info tx_ports[RTE_MAX_ETHPORTS];
};
#endif

/* "classifier_table" command specific parameters */
struct spp_command_classifier_table {
	enum spp_classifier_type type;
	char value[SPP_CMD_VALUE_BUFSZ];
	struct spp_config_port_info port;
};

/* "flush" command specific parameters */
struct spp_command_flush {
	/* nothing specific */
};

/* command parameters */
struct spp_command {
	enum spp_command_type type;

	union {
#if 0 /* not supported yet */
		struct spp_command_add add;
		struct spp_command_component component;
#endif
		struct spp_command_classifier_table classifier_table;
		struct spp_command_flush flush;
	} spec;
};

/* request parameters */
struct spp_command_request {
	int num_command;
	int num_valid_command;
	struct spp_command commands[SPP_CMD_MAX_COMMANDS];

	int is_requested_process;
	int is_requested_status;
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
