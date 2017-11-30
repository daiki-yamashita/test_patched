#include <unistd.h>
#include <string.h>

#include <rte_log.h>
#include <rte_branch_prediction.h>

#include "spp_vf.h"
#include "command_dec.h"

#define RTE_LOGTYPE_SPP_COMMAND_PROC RTE_LOGTYPE_USER1

/* classifier type string list
	do it same as the order of enum spp_classifier_type (spp_vf.h) */
static const char *CLASSIFILER_TYPE_STRINGS[] = {
	"none",
	"mac",

	/* termination */ "",
};

/* command action type string list
	do it same as the order of enum spp_command_action (spp_vf.h) */
static const char *COMMAND_ACTION_STRINGS[] = {
	"none",
	"start",
	"stop",
	"add",
	"del",

	/* termination */ "",
};

/* port rxtx string list
	do it same as the order of enum spp_port_rxtx (spp_vf.h) */
static const char *PORT_RXTX_STRINGS[] = {
	"none",
	"rx",
	"tx",

	/* termination */ "",
};

/* set decode error */
inline int
set_decode_error(struct spp_command_decode_error *error,
		const int error_code, const char *error_name)
{
	error->code = error_code;

	if (likely(error_name != NULL))
		strcpy(error->value_name, error_name);

	return error->code;
}

/* set decode error */
inline int
set_string_value_decode_error(struct spp_command_decode_error *error,
		const char* value, const char *error_name)
{
	strcpy(error->value, value);
	return set_decode_error(error, SPP_CMD_DERR_BAD_VALUE, error_name);
}

/* Split command line parameter with spaces */
static int
decode_parameter_value(char *string, int max, int *argc, char *argv[])
{
	int cnt = 0;
	const char *delim = " ";
	char *argv_tok = NULL;
	char *saveptr = NULL;

	argv_tok = strtok_r(string, delim, &saveptr);
	while(argv_tok != NULL) {
		if (cnt >= max)
			return -1;
		argv[cnt] = argv_tok;
		cnt++;
		argv_tok = strtok_r(NULL, delim, &saveptr);
	}
	*argc = cnt;

	return 0;
}

/* Get index of array */
static int
get_arrary_index(const char *match, const char *list[])
{
	int i;
	for (i = 0; list[i][0] != '\0'; i++) {
		if (strcmp(list[i], match) == 0)
			return i;
	}
	return -1;
}

/* Get unsigned int type value */
static int
get_uint_value(	unsigned int *output,
		const char *arg_val,
		unsigned int min,
		unsigned int max)
{
	unsigned int ret = 0;
	char *endptr = NULL;
	ret = strtoul(arg_val, &endptr, 0);
	if (unlikely(endptr == arg_val) || unlikely(*endptr != '\0'))
		return -1;

	if (unlikely(ret < min) || unlikely(ret > max))
		return -1;

	*output = ret;
	return 0;
}

/* decoding procedure of string */
static int
decode_str_value(char *output, const char *arg_val)
{
	if (strlen(arg_val) >= SPP_CMD_VALUE_BUFSZ)
		return -1;

	strcpy(output, arg_val);
	return 0;
}

/* decoding procedure of port */
static int
decode_port_value(void *output, const char *arg_val)
{
	int ret = 0;
	struct spp_port_index *port = output;
	ret = spp_get_if_info(arg_val, &port->if_type, &port->if_no);
	if (unlikely(ret != 0)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Bad port. val=%s\n", arg_val);
		return -1;
	}

	return 0;
}

/* decoding procedure of core */
static int
decode_core_value(void *output, const char *arg_val)
{
	int ret = 0;
	ret = get_uint_value(output, arg_val, 0, RTE_MAX_LCORE-1);
	if (unlikely(ret < 0)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Bad core id. val=%s\n", arg_val);
		return -1;
	}

	return 0;
}

/* decoding procedure of action for component command */
static int
decode_component_action_value(void *output, const char *arg_val)
{
	int ret = 0;
	ret = get_arrary_index(arg_val, COMMAND_ACTION_STRINGS);
	if (unlikely(ret <= 0)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Unknown component action. val=%s\n", arg_val);
		return -1;
	}

	if (unlikely(ret != SPP_CMD_ACTION_START) && unlikely(ret != SPP_CMD_ACTION_STOP)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Unknown component action. val=%s\n", arg_val);
		return -1;
	}

	*(int *)output = ret;
	return 0;
}

/* decoding procedure of action for component command */
static int
decode_component_name_value(void *output, const char *arg_val)
{
	int ret = 0;
	struct spp_command_component *component = output;

	/* "stop" has no core ID parameter. */
	if (component->action == SPP_CMD_ACTION_START) {
		ret = spp_get_component_id(arg_val);
		if (unlikely(ret >= 0)) {
			RTE_LOG(ERR, SPP_COMMAND_PROC,
					"Component name in used. val=%s\n",
					arg_val);
			return -1;
		}
	}

	return decode_str_value(component->name, arg_val);
}

/* decoding procedure of core id for component command */
static int
decode_component_core_value(void *output, const char *arg_val)
{
	struct spp_command_component *component = output;

	/* "stop" has no core ID parameter. */
	if (component->action != SPP_CMD_ACTION_START)
		return 0;

	return decode_core_value(&component->core, arg_val);
}

/* decoding procedure of type for component command */
static int
decode_component_type_value(void *output, const char *arg_val)
{
	enum spp_component_type org_type, set_type;
	struct spp_command_component *component = output;

	/* "stop" has no type parameter. */
	if (component->action != SPP_CMD_ACTION_START)
		return 0;

	set_type = spp_change_component_type(arg_val);
	if (unlikely(set_type <= 0)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				"Unknown component type. val=%s\n",
				arg_val);
		return -1;
	}

	org_type = spp_get_component_type_update(component->core);
	if ((org_type != SPP_COMPONENT_UNUSE) && (org_type != set_type)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				"Component type does not match. val=%s (org=%d, new=%d)\n",
				arg_val, org_type, set_type);
		return -1;
	}

	component->type = set_type;
	return 0;
}

/* decoding procedure of action for port command */
static int
decode_port_action_value(void *output, const char *arg_val)
{
	int ret = 0;
	ret = get_arrary_index(arg_val, COMMAND_ACTION_STRINGS);
	if (unlikely(ret <= 0)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Unknown port action. val=%s\n", arg_val);
		return -1;
	}

	if (unlikely(ret != SPP_CMD_ACTION_ADD) && unlikely(ret != SPP_CMD_ACTION_DEL)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Unknown port action. val=%s\n", arg_val);
		return -1;
	}

	*(int *)output = ret;
	return 0;
}

/* decoding procedure of port for port command */
static int
decode_port_port_value(void *output, const char *arg_val)
{
	int ret = -1;
	struct spp_port_index tmp_port;
	struct spp_command_port *port = output;

	ret = decode_port_value(&tmp_port, arg_val);
	if (ret < 0)
		return -1;

	if ((port->action == SPP_CMD_ACTION_ADD) &&
			(spp_check_used_port(tmp_port.if_type, tmp_port.if_no, SPP_PORT_RXTX_RX) >= 0) &&
			(spp_check_used_port(tmp_port.if_type, tmp_port.if_no, SPP_PORT_RXTX_TX) >= 0)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Port in used. (port command) val=%s\n", arg_val);
		return -1;
	}

	port->port.if_type = tmp_port.if_type;
	port->port.if_no   = tmp_port.if_no;
	return 0;
}

/* decoding procedure of rxtx type for port command */
static int
decode_port_rxtx_value(void *output, const char *arg_val)
{
	int ret = 0;
	struct spp_command_port *port = output;

	ret = get_arrary_index(arg_val, PORT_RXTX_STRINGS);
	if (unlikely(ret <= 0)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Unknown port rxtx. val=%s\n", arg_val);
		return -1;
	}

	if ((port->action == SPP_CMD_ACTION_ADD) &&
			(spp_check_used_port(port->port.if_type, port->port.if_no, ret) >= 0)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Port in used. (port command) val=%s\n", arg_val);
		return -1;
	}

	port->rxtx = ret;
	return 0;
}

/* decoding procedure of component name for port command */
static int
decode_port_name_value(void *output, const char *arg_val)
{
	int ret = 0;

	ret = spp_get_component_id(arg_val);
	if (unlikely(ret < 0)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Unknown component name. val=%s\n",
				arg_val);
		return -1;
	}

	return decode_str_value(output, arg_val);
}

/* decoding procedure of mac address string */
static int
decode_mac_addr_str_value(void *output, const char *arg_val)
{
	int64_t ret = 0;
	const char *str_val = arg_val;

	/* if default specification, convert to internal dummy address */
	if (unlikely(strcmp(str_val, SPP_DEFAULT_CLASSIFIED_SPEC_STR) == 0))
		str_val = SPP_DEFAULT_CLASSIFIED_DMY_ADDR_STR;

	ret = spp_change_mac_str_to_int64(str_val);
	if (unlikely(ret < 0)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Bad mac address string. val=%s\n",
				str_val);
		return -1;
	}

	strcpy((char *)output, str_val);
	return 0;
}

/* decoding procedure of action for classifier_table command */
static int
decode_classifier_action_value(void *output, const char *arg_val)
{
	int ret = 0;
	ret = get_arrary_index(arg_val, COMMAND_ACTION_STRINGS);
	if (unlikely(ret <= 0)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Unknown port action. val=%s\n", arg_val);
		return -1;
	}

	if (unlikely(ret != SPP_CMD_ACTION_ADD) && unlikely(ret != SPP_CMD_ACTION_DEL)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Unknown port action. val=%s\n", arg_val);
		return -1;
	}

	*(int *)output = ret;
	return 0;
}

/* decoding procedure of type for classifier_table command */
static int
decode_classifier_type_value(void *output, const char *arg_val)
{
	int ret = 0;
	ret = get_arrary_index(arg_val, CLASSIFILER_TYPE_STRINGS);
	if (unlikely(ret <= 0)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Unknown classifier type. val=%s\n", arg_val);
		return -1;
	}

	*(int *)output = ret;
	return 0;
}

/* decoding procedure of value for classifier_table command */
static int
decode_classifier_value_value(void *output, const char *arg_val)
{
	int ret = -1;
	struct spp_command_classifier_table *classifier_table = output;
	switch(classifier_table->type) {
		case SPP_CLASSIFIER_TYPE_MAC:
			ret = decode_mac_addr_str_value(classifier_table->value, arg_val);
			break;
		default:
			break;
	}
	return ret;
}

/* decoding procedure of port for classifier_table command */
static int
decode_classifier_port_value(void *output, const char *arg_val)
{
	int ret = 0;
	struct spp_command_classifier_table *classifier_table = output;
	struct spp_port_index tmp_port;
	int64_t mac_addr = 0;

	ret = decode_port_value(&tmp_port, arg_val);
	if (ret < 0)
		return -1;

	if (spp_check_added_port(tmp_port.if_type, tmp_port.if_no) == 0) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Port not added. val=%s\n", arg_val);
		return -1;
	}

	if (unlikely(classifier_table->action == SPP_CMD_ACTION_ADD)) {
		if (!spp_check_mac_used_port(0, tmp_port.if_type, tmp_port.if_no)) {
			RTE_LOG(ERR, SPP_COMMAND_PROC,
					"Port in used. (classifier_table command) val=%s\n",
					arg_val);
			return -1;
		}
	} else if (unlikely(classifier_table->action == SPP_CMD_ACTION_DEL)) {
		mac_addr = spp_change_mac_str_to_int64(classifier_table->value);
		if (mac_addr < 0)
			return -1;

		if (!spp_check_mac_used_port((uint64_t)mac_addr, tmp_port.if_type, tmp_port.if_no)) {
			RTE_LOG(ERR, SPP_COMMAND_PROC,
					"Port in used. (classifier_table command) val=%s\n",
					arg_val);
			return -1;
		}
	}

	classifier_table->port.if_type = tmp_port.if_type;
	classifier_table->port.if_no   = tmp_port.if_no;
	return 0;
}

#define DECODE_PARAMETER_LIST_EMPTY { NULL, 0, NULL }

/* parameter list for decoding */
struct decode_parameter_list {
	const char *name;
	size_t offset;
	int (*func)(void *output, const char *arg_val);
};

/* parameter list for each command */
static struct decode_parameter_list parameter_list[][SPP_CMD_MAX_PARAMETERS] = {
	{                                /* classifier_table */
		{
			.name = "action",
			.offset = offsetof(struct spp_command, spec.classifier_table.action),
			.func = decode_classifier_action_value
		},
		{
			.name = "type",
			.offset = offsetof(struct spp_command, spec.classifier_table.type),
			.func = decode_classifier_type_value
		},
		{
			.name = "value",
			.offset = offsetof(struct spp_command, spec.classifier_table),
			.func = decode_classifier_value_value
		},
		{
			.name = "port",
			.offset = offsetof(struct spp_command, spec.classifier_table),
			.func = decode_classifier_port_value
		},
		DECODE_PARAMETER_LIST_EMPTY,
	},
	{ DECODE_PARAMETER_LIST_EMPTY }, /* flush            */
	{ DECODE_PARAMETER_LIST_EMPTY }, /* _get_client_id   */
	{ DECODE_PARAMETER_LIST_EMPTY }, /* status           */
	{ DECODE_PARAMETER_LIST_EMPTY }, /* exit             */
	{                                /* component        */
		{
			.name = "action",
			.offset = offsetof(struct spp_command, spec.component.action),
			.func = decode_component_action_value
		},
		{
			.name = "component name",
			.offset = offsetof(struct spp_command, spec.component),
			.func = decode_component_name_value
		},
		{
			.name = "core",
			.offset = offsetof(struct spp_command, spec.component),
			.func = decode_component_core_value
		},
		{
			.name = "component type",
			.offset = offsetof(struct spp_command, spec.component),
			.func = decode_component_type_value
		},
		DECODE_PARAMETER_LIST_EMPTY,
	},
	{                                /* port             */
		{
			.name = "action",
			.offset = offsetof(struct spp_command, spec.port.action),
			.func = decode_port_action_value
		},
		{
			.name = "port",
			.offset = offsetof(struct spp_command, spec.port),
			.func = decode_port_port_value
		},
		{
			.name = "port rxtx",
			.offset = offsetof(struct spp_command, spec.port),
			.func = decode_port_rxtx_value
		},
		{
			.name = "component name",
			.offset = offsetof(struct spp_command, spec.port.name),
			.func = decode_port_name_value
		},
		DECODE_PARAMETER_LIST_EMPTY,
	},
	{ DECODE_PARAMETER_LIST_EMPTY }, /* termination      */
};

/* check by list for each command line parameter */
static int
decode_comand_parameter_in_list(struct spp_command_request *request,
				int argc, char *argv[],
				struct spp_command_decode_error *error)
{
	int ret = 0;
	int ci = request->commands[0].type;
	int pi = 0;
	static struct decode_parameter_list *list = NULL;
	for(pi = 1; pi < argc; pi++) {
		list = &parameter_list[ci][pi-1];
		ret = (*list->func)((void *)((char*)&request->commands[0]+list->offset), argv[pi]);
		if (unlikely(ret < 0)) {
			RTE_LOG(ERR, SPP_COMMAND_PROC,
					"Bad value. command=%s, name=%s, index=%d, value=%s\n",
					argv[0], list->name, pi, argv[pi]);
			return set_string_value_decode_error(error, argv[pi], list->name);
		}
	}
	return 0;
}

/* command list for decoding */
struct decode_command_list {
	const char *name;
	int   param_min;
	int   param_max;
	int (*func)(struct spp_command_request *request, int argc, char *argv[],
			struct spp_command_decode_error *error);
};

/* command list */
static struct decode_command_list command_list[] = {
	{ "classifier_table", 5, 5, decode_comand_parameter_in_list }, /* classifier_table */
	{ "flush",            1, 1, NULL                            }, /* flush            */
	{ "_get_client_id",   1, 1, NULL                            }, /* _get_client_id   */
	{ "status",           1, 1, NULL                            }, /* status           */
	{ "exit",             1, 1, NULL                            }, /* exit             */
	{ "component",        3, 5, decode_comand_parameter_in_list }, /* port             */
	{ "port",             5, 5, decode_comand_parameter_in_list }, /* port             */
	{ "",                 0, 0, NULL                            }  /* termination      */
};

/* Decode command line parameters */
static int
decode_command_in_list(struct spp_command_request *request,
			const char *request_str,
			struct spp_command_decode_error *error)
{
	int ret = 0;
	struct decode_command_list *list = NULL;
	int i = 0;
	int argc = 0;
	char *argv[SPP_CMD_MAX_PARAMETERS];
	char tmp_str[SPP_CMD_MAX_PARAMETERS*SPP_CMD_VALUE_BUFSZ];
	memset(argv, 0x00, sizeof(argv));
	memset(tmp_str, 0x00, sizeof(tmp_str));

	strcpy(tmp_str, request_str);
	ret = decode_parameter_value(tmp_str, SPP_CMD_MAX_PARAMETERS, &argc, argv);
	if (ret < 0) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Parameter number over limit."
				"request_str=%s\n", request_str);
		return set_decode_error(error, SPP_CMD_DERR_BAD_FORMAT, NULL);
	}
	RTE_LOG(DEBUG, SPP_COMMAND_PROC, "Decode array. num=%d\n", argc);

	for (i = 0; command_list[i].name[0] != '\0'; i++) {
		list = &command_list[i];
		if (strcmp(argv[0], list->name) != 0) {
			continue;
		}

		if (unlikely(argc < list->param_min) || unlikely(list->param_max < argc)) {
			RTE_LOG(ERR, SPP_COMMAND_PROC, "Parameter number out of range."
					"request_str=%s\n", request_str);
			return set_decode_error(error, SPP_CMD_DERR_BAD_FORMAT, NULL);
		}

		request->commands[0].type = i;
		if (list->func != NULL)
			return (*list->func)(request, argc, argv, error);

		return 0;
	}

	RTE_LOG(ERR, SPP_COMMAND_PROC, "Unknown command. command=%s, request_str=%s\n",
			argv[0], request_str);
	return set_string_value_decode_error(error, argv[0], "command");
}

/* decode request from no-null-terminated string */
int
spp_command_decode_request(struct spp_command_request *request, const char *request_str,
		size_t request_str_len, struct spp_command_decode_error *error)
{
	int ret = -1;
	int i;

	/* decode request */
	request->num_command = 1;
	ret = decode_command_in_list(request, request_str, error);
	if (unlikely(ret != 0)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Cannot decode command request. "
				"ret=%d, request_str=%.*s\n", 
				ret, (int)request_str_len, request_str);
		return ret;
	}
	request->num_valid_command = 1;

	/* check getter command */
	for (i = 0; i < request->num_valid_command; ++i) {
		switch (request->commands[i].type) {
		case SPP_CMDTYPE_CLIENT_ID:
			request->is_requested_client_id = 1;
			break;
		case SPP_CMDTYPE_STATUS:
			request->is_requested_status = 1;
			break;
		case SPP_CMDTYPE_EXIT:
			request->is_requested_exit = 1;
			break;
		default:
			/* nothing to do */
			break;
		}
	}

	return ret;
}
