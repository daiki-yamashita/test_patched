#include <unistd.h>
#include <string.h>

#include <rte_log.h>
#include <rte_branch_prediction.h>

#include "spp_vf.h"
#include "spp_config.h"
#include "command_dec.h"

#define RTE_LOGTYPE_SPP_COMMAND_PROC RTE_LOGTYPE_USER1

/* classifier type string list
	do it same as the order of enum spp_classifier_type (spp_vf.h) */
static const char *CLASSIFILER_TYPE_STRINGS[] = {
	"none",
	"mac",

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

/* Split command line arguments with spaces */
static void
decode_argument_value(char *string, int *argc, char *argv[])
{
	int cnt = 0;
	const char *delim = " ";
	char *argv_tok = NULL;

	argv_tok = strtok(string, delim);
	while(argv_tok != NULL) {
		argv[cnt] = argv_tok;
		cnt++;
		argv_tok = strtok(NULL, delim);
	}
	*argc = cnt;
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

/* decoding procedure of port */
static int
decode_port_value(void *output, const char *arg_val)
{
	int ret = 0;
	struct spp_config_port_info *port = output;
	ret = spp_config_get_if_info(arg_val, &port->if_type, &port->if_no);
	if (unlikely(ret != 0)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Bad port. val=%s\n", arg_val);
		return -1;
	}

	return 0;
}

/* decoding procedure of mac address string */
static int
decode_mac_addr_str_value(void *output, const char *arg_val)
{
	int64_t ret = 0;
	const char *str_val = arg_val;

	/* if default specification, convert to internal dummy address */
	if (unlikely(strcmp(str_val, SPP_CONFIG_DEFAULT_CLASSIFIED_SPEC_STR) == 0))
		str_val = SPP_CONFIG_DEFAULT_CLASSIFIED_DMY_ADDR_STR;

	ret = spp_config_change_mac_str_to_int64(str_val);
	if (unlikely(ret < 0)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Bad mac address string. val=%s\n",
				str_val);
		return -1;
	}

	strcpy((char *)output, str_val);
	return 0;
}

/* decoding procedure of classifier type */
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

/* decode procedure for classifier value */
static int
decode_classifiert_value_value(void *output, const char *arg_val)
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

/* decode procedure for classifier port */
static int
decode_classifier_port_value(void *output, const char *arg_val)
{
	struct spp_config_port_info *port = output;

        if (strcmp(arg_val, SPP_CMD_UNUSE) == 0) {
                port->if_type = UNDEF;
                port->if_no = 0;
                return 0;
        }

	return decode_port_value(port, arg_val);
}

/* parameter list for decoding */
struct decode_parameter_list {
        const char *name;
        size_t offset;
        int (*func)(void *output, const char *arg_val);
};

/* parameter list for each command */
static struct decode_parameter_list parameter_list[][SPP_CMD_MAX_PARAMETERS] = {
	{                      /* classifier_table */
		{
			.name = "type",
			.offset = offsetof(struct spp_command, spec.classifier_table.type),
			.func = decode_classifier_type_value
		},
		{
			.name = "value",
			.offset = offsetof(struct spp_command, spec.classifier_table),
			.func = decode_classifiert_value_value
		},
		{
			.name = "port",
			.offset = offsetof(struct spp_command, spec.classifier_table.port),
			.func = decode_classifier_port_value
		},
		{ NULL, 0, NULL },
	},
	{ { NULL, 0, NULL } }, /* flush            */
	{ { NULL, 0, NULL } }, /* _get_client_id   */
	{ { NULL, 0, NULL } }, /* status           */
	{ { NULL, 0, NULL } }, /* termination      */
};

/* check by list for each command line argument */
static int
check_comand_argment_in_list(struct spp_command_request *request,
				int argc, char *argv[],
				struct spp_command_decode_error *error)
{
	int ret = 0;
	int ci = request->commands[0].type;
	int pi = 0;
	static struct decode_parameter_list *list = NULL;
	for(pi = 1; pi < argc; pi++) {
		list = &parameter_list[ci][pi-1];
RTE_LOG(ERR, SPP_COMMAND_PROC, "TEST: command=%s, name=%s, index=%d, value=%s\n", argv[0], list->name, pi, argv[pi]);
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
	{ "classifier_table", 4, 4, check_comand_argment_in_list }, /* classifier_table */
	{ "flush",            1, 1, NULL                         }, /* flush            */
	{ "_get_client_id",   1, 1, NULL                         }, /* _get_client_id   */
	{ "status",           1, 1, NULL                         }, /* status           */
	{ "",                 0, 0, NULL                         }  /* termination      */
};

/* Decode command line arguments */
static int
decode_command_argment(struct spp_command_request *request,
			const char *request_str,
			struct spp_command_decode_error *error)
{
	struct decode_command_list *list = NULL;
	int i = 0;
	int argc = 0;
	char *argv[SPP_CMD_MAX_PARAMETERS];
	char tmp_str[SPP_CMD_MAX_PARAMETERS*SPP_CMD_VALUE_BUFSZ];
	memset(argv, 0x00, sizeof(argv));
	memset(tmp_str, 0x00, sizeof(tmp_str));

	strcpy(tmp_str, request_str);
	decode_argument_value(tmp_str, &argc, argv);
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

RTE_LOG(ERR, SPP_COMMAND_PROC, "ERROR:request_str=%s\n", request_str);
	/* decode request */
	request->num_command = 1;
	ret = decode_command_argment(request, request_str, error);
	if (unlikely(ret != 0)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Cannot decode command request. "
				"ret=%d, request_str=%.*s\n", 
				ret, (int)request_str_len, request_str);
		return ret;
	}
	request->num_valid_command = 1;
RTE_LOG(ERR, SPP_COMMAND_PROC, "ERROR:command type=%d\n", request->commands[0].type);

	/* check getter command */
	for (i = 0; i < request->num_valid_command; ++i) {
		switch (request->commands[i].type) {
		case SPP_CMDTYPE_CLIENT_ID:
			request->is_requested_client_id = 1;
			break;
		case SPP_CMDTYPE_STATUS:
			request->is_requested_status = 1;
			break;
		default:
			/* nothing to do */
			break;
		}
	}

	return ret;
}
