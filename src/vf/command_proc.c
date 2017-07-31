#include <unistd.h>
#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <arpa/inet.h>

#include <rte_common.h>
#include <rte_log.h>
#include <rte_branch_prediction.h>

#include <jansson.h>

#include "spp_vf.h"
#include "spp_config.h"
#include "command_proc.h"

#define RTE_LOGTYPE_SPP_COMMAND_PROC RTE_LOGTYPE_USER1


/*******************************************************************************
 *
 * operate connection with controller
 *
 ******************************************************************************/

/* receive message buffer size */
#define MESSAGE_BUFFER_BLOCK_SIZE 512

/* controller's ip address */
static char g_controller_ip[128] = "";

/* controller's port number */
static int g_controller_port = 0;

/* allocate message buffer */
inline char*
msgbuf_allocate(size_t capacity)
{
	char* buf = (char *)malloc(capacity + sizeof(size_t));
	if (unlikely(buf == NULL))
		return NULL;

	*((size_t *)buf) = capacity;

	return buf + sizeof(size_t);
}

/* free message buffer */
inline void
msgbuf_free(char* msgbuf)
{
	if (likely(msgbuf != NULL))
		free(msgbuf - sizeof(size_t));
}

/* get message buffer capacity */
inline size_t
msgbuf_get_capacity(const char *msgbuf)
{
	return *((const size_t *)(msgbuf - sizeof(size_t)));
}

/* re-allocate message buffer */
inline char*
msgbuf_reallocate(char *msgbuf, size_t required_len)
{
	size_t new_cap = msgbuf_get_capacity(msgbuf) * 2;
	char *new_msgbuf = NULL;

	while (unlikely(new_cap <= required_len))
		new_cap *= 2;

	new_msgbuf = msgbuf_allocate(new_cap);
	if (unlikely(new_msgbuf == NULL))
		return NULL;

	strcpy(new_msgbuf, msgbuf);
	msgbuf_free(msgbuf);

	return new_msgbuf;
}

/* append message to buffer */
inline char*
msgbuf_append(char *msgbuf, const char *append, size_t append_len)
{
	size_t cap = msgbuf_get_capacity(msgbuf);
	size_t len = strlen(msgbuf);
	char *new_msgbuf = msgbuf;

	if (unlikely(len + append_len >= cap)) {
		new_msgbuf = msgbuf_reallocate(msgbuf, len + append_len);
		if (unlikely(new_msgbuf == NULL))
			return NULL;
	}

	memcpy(new_msgbuf + len, append, append_len);
	*(new_msgbuf + len + append_len) = '\0';

	return new_msgbuf;
}

/* remove message from front */
inline char*
msgbuf_remove_front(char *msgbuf, size_t remove_len)
{
	size_t len = strlen(msgbuf);
	size_t new_len = len - remove_len;

	if (likely(new_len == 0)) {
		*msgbuf = '\0';
		return msgbuf;
	}

	return memmove(msgbuf, msgbuf + remove_len, new_len + 1);
}

/* connect to controller */
static int
connect_to_controller(int *sock)
{
	static struct sockaddr_in controller_addr;
	int ret = -1;
	int sock_flg = 0;

	if (likely(*sock >=0))
		return 0;

	/* create socket */
	if (*sock < 0) {
		RTE_LOG(INFO, SPP_COMMAND_PROC, "Creating socket...\n");
		*sock = socket(AF_INET, SOCK_STREAM, 0);
		if (*sock < 0) {
			RTE_LOG(ERR, SPP_COMMAND_PROC, 
					"Cannot create tcp socket. errno=%d\n", errno);
			return -1;
		}

		memset(&controller_addr, 0, sizeof(controller_addr));
		controller_addr.sin_family = AF_INET;
		controller_addr.sin_addr.s_addr = inet_addr(g_controller_ip);
		controller_addr.sin_port = htons(g_controller_port);
	}

	/* connect to */
	RTE_LOG(INFO, SPP_COMMAND_PROC, "Trying to connect ... socket=%d\n", *sock);
	ret = connect(*sock, (struct sockaddr *)&controller_addr,
			sizeof(controller_addr));
	if (ret < 0) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				"Cannot connect to controller. errno=%d\n", errno);
		return -1;
	}

	RTE_LOG(INFO, SPP_COMMAND_PROC, "Connected\n");

	/* set non-blocking */
	sock_flg = fcntl(*sock, F_GETFL, 0);
	fcntl(*sock, F_SETFL, sock_flg | O_NONBLOCK);

	return 0;
}

/* receive message */
static int
receive_message(int *sock, char **msgbuf)
{
	int ret = -1;
	int n_rx = 0;
	char *new_msgbuf = NULL;

	char rx_buf[MESSAGE_BUFFER_BLOCK_SIZE];
	size_t rx_buf_sz = MESSAGE_BUFFER_BLOCK_SIZE;

	ret = recv(*sock, rx_buf, rx_buf_sz, 0);
	RTE_LOG(DEBUG, SPP_COMMAND_PROC, "Receive message. count=%d\n", ret);
	if (ret <= 0) {
		if (ret == 0) {
			RTE_LOG(INFO, SPP_COMMAND_PROC,
					"Controller has performed an shutdown.");
		} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
			/* no receive message */
			return 0;
		} else {
			RTE_LOG(ERR, SPP_COMMAND_PROC,
					"Receive failure. errno=%d\n", errno);
		}

		RTE_LOG(INFO, SPP_COMMAND_PROC, "Assume Server closed connection\n");
		close(*sock);
		*sock = -1;
		return -1;
	}

	n_rx = ret;

	new_msgbuf = msgbuf_append(*msgbuf, rx_buf, n_rx);
	if (unlikely(new_msgbuf == NULL)) {
		return -1;
	}

	*msgbuf = new_msgbuf;

	return n_rx;
}

/* send message */
static int
send_message(int *sock, const char* message, size_t message_len)
{
	int ret = -1;

	ret = send(*sock, message, message_len, 0);
	if (unlikely(ret == -1)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Send failure. ret=%d\n", ret);
		return -1;
	}

	return 0;
}


/*******************************************************************************
 *
 * process command request(json string)
 *
 ******************************************************************************/

#define CMD_MAX_COMMANDS 32

#define CMD_NAME_BUFSZ 32

#define CMD_CLASSIFIER_TABLE_VALUE_BUFSZ 62

#define CMD_UNUSE "unuse"

#define component_type spp_core_type

/* decode error code */
enum decode_error_code {
	DERR_BAD_FORMAT = 1,
	DERR_UNKNOWN_COMMAND,
	DERR_NO_PARAM,
	DERR_BAD_TYPE,
	DERR_BAD_VALUE,
};

/* command type
	do it same as the order of COMMAND_TYPE_STRINGS */
enum command_type {
	CMDTYPE_ADD,
	CMDTYPE_COMPONENT,
	CMDTYPE_CLASSIFIER_TABLE,
	CMDTYPE_FLUSH,
	CMDTYPE_FORWARD,
	CMDTYPE_STOP,

	CMDTYPE_PROCESS,
};

/* command type string list
	do it same as the order of enum command_type */
static const char *COMMAND_TYPE_STRINGS[] = {
	"add",
	"component",
	"classifier_table",
	"flush",
	"forward",
	"stop",

	/* termination */ "",
};

/* classifier type string list
	do it same as the order of enum spp_classifier_type (spp_vf.h) */
static const char *CLASSIFILER_TYPE_STRINGS[] = {
	"none",
	"mac",

	/* termination */ "",
};

#if 0 /* not supported */
/* "add" command parameters */
struct add_command {
	int num_port;
	struct spp_config_port_info ports[RTE_MAX_ETHPORTS];
};

/* "component" command specific parameters */
struct component_command {
	enum component_type type;
	unsigned int core_id;
	int num_rx_port;
	int num_tx_port;
	struct spp_config_port_info rx_ports[RTE_MAX_ETHPORTS];
	struct spp_config_port_info tx_ports[RTE_MAX_ETHPORTS];
};
#endif

/* "classifier_table" command specific parameters */
struct classifier_table_command {
	enum spp_classifier_type type;
	char value[CMD_CLASSIFIER_TABLE_VALUE_BUFSZ];
	struct spp_config_port_info port;
};

/* "flush" command specific parameters */
struct flush_command {
	/* nothing specific */
};

/* command parameters */
struct command {
	enum command_type type;

	union {
#if 0 /* not supported */
		struct add_command add;
		struct component_command component;
#endif
		struct classifier_table_command classifier_table;
		struct flush_command flush;
	} spec;
};

/* request parameters */
struct request {
	int num_command;
	int num_valid_command;
	struct command commands[CMD_MAX_COMMANDS];
};

/* forward declaration */
struct json_value_decode_rule;

/* definition of decode procedure function */
typedef int (*json_value_decode_proc)(void *, const json_t *, const struct json_value_decode_rule *);

/* rule definition that decode json object to c-struct */
struct json_value_decode_rule {
	char name[CMD_NAME_BUFSZ];
	json_type json_type;
	size_t offset;
	json_value_decode_proc decode_proc;

	struct {
		json_type json_type;
		size_t element_sz;
		size_t offset_num;
		size_t offset_num_valid;
	} array;
};

/* get output address for decoded json value */
#define DR_GET_OUTPUT(output_base, rule__) ((char *)output_base + rule__->offset)

/* helper */
#define END_OF_DECODE_RULE {.name = ""},
#define IS_END_OF_DECODE_RULE(rule) ((rule)->name[0] == '\0')

/* definition helper that enum value decode procedure */
#define DECODE_ENUM_VALUE(proc_name, enum_type, string_table)			\
static int									\
decode_##proc_name##_value(void *output, const json_t *value_obj,		\
		__rte_unused const struct json_value_decode_rule *rule)		\
{										\
	int i;									\
	enum_type type;								\
	const char *str_val = json_string_value(value_obj);			\
										\
	for (i = 0; string_table[i][0] != '\0'; ++i) {				\
		if (unlikely(strcmp(str_val, string_table[i]) == 0)) {		\
			type = i;						\
			memcpy(output, &type, sizeof(enum_type));		\
			return 0;						\
		}								\
	}									\
										\
	return -1;								\
}										\

/* enum value decode procedure for "command_type" */
DECODE_ENUM_VALUE(command_type, enum command_type, COMMAND_TYPE_STRINGS)

/* enum value decode procedure for "classifier_type" */
DECODE_ENUM_VALUE(classifier_type, enum spp_classifier_type, CLASSIFILER_TYPE_STRINGS)

#if 0 /* not supported */
/* decode procedure for integer */
static int
decode_int_value(void *output, const json_t *value_obj,
		__rte_unused const struct json_value_decode_rule *rule)
{
	int val = json_integer_value(value_obj);
	memcpy(output, &val, sizeof(int));

	return 0;
}

/* decode procedure for string */
static int
decode_string_value(void *output, const json_t *value_obj,
		__rte_unused const struct json_value_decode_rule *rule)
{
	const char* str_val = json_string_value(value_obj);
	strcpy(output, str_val);

	return 0;
}
#endif

/* decode procedure for mac address string */
static int
decode_mac_addr_str_value(void *output, const json_t *value_obj,
		__rte_unused const struct json_value_decode_rule *rule)
{
	int ret = -1;
	const char* str_val = json_string_value(value_obj);

	ret = spp_config_change_mac_str_to_int64(str_val);
	if (unlikely(ret == -1)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Bad mac address string. val=%s\n",
				str_val);
		return DERR_BAD_VALUE;
	}

	strcpy(output, str_val);

	return 0;
}

/* decode procedure for spp_config_port_info */
static int
decode_port_value(void *output, const json_t *value_obj,
		__rte_unused const struct json_value_decode_rule *rule)
{
	int ret = -1;
	const char* str_val = json_string_value(value_obj);
	struct spp_config_port_info *port = (struct spp_config_port_info *)output;

	if (strcmp(str_val, CMD_UNUSE) == 0) {
		port->if_type = UNDEF;
		port->if_no = 0;
		return 0;
	}

	ret = spp_config_get_if_info(str_val, &port->if_type, &port->if_no);
	if (unlikely(ret != 0)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Bad port. val=%s\n", str_val);
		return DERR_BAD_VALUE;
	}

	return 0;
}

/* decode json object */
static int
decode_json_object(void *output, const json_t *parent_obj,
		const struct json_value_decode_rule *rules)
{
	int ret = -1;
	int i, n;
	json_t *obj;
	json_t *value_obj;
	const struct json_value_decode_rule *rule;

	void *sub_output;

	for (i = 0; unlikely(! IS_END_OF_DECODE_RULE(&rules[i])); ++ i) {
		rule = rules + 1;

		RTE_LOG(DEBUG, SPP_COMMAND_PROC, "get one object. name=%s\n",
				rule->name);

		value_obj = json_object_get(parent_obj, rule->name);
		if (unlikely(value_obj == NULL)) {
			RTE_LOG(ERR, SPP_COMMAND_PROC, "No parameter. "
					"name=%s\n", rule->name);
			return DERR_NO_PARAM;
		} else if (unlikely(json_typeof(value_obj) != rule->json_type)) {
			RTE_LOG(ERR, SPP_COMMAND_PROC, "Bad value type. "
					"name=%s\n", rule->name);
			return DERR_BAD_TYPE;
		}

		switch (rule->json_type) {
		case JSON_ARRAY:
			RTE_LOG(DEBUG, SPP_COMMAND_PROC, "Decode array. num=%lu\n",
					json_array_size(value_obj));

			*(int *)((char *)output + rule->array.offset_num) = 
					(int)json_array_size(value_obj);

			json_array_foreach(value_obj, n, obj) {
				RTE_LOG(DEBUG, SPP_COMMAND_PROC, "Decode array element. "
						"index=%d\n", i);
				
				if (unlikely(json_typeof(obj) != rule->array.json_type)) {
					RTE_LOG(ERR, SPP_COMMAND_PROC, "Bad value type. "
							"name=%s, index=%d\n", rule->name, i);
					return DERR_BAD_TYPE;
				}

				sub_output = DR_GET_OUTPUT(output, rule) + 
						(rule->array.element_sz * i);
				ret = (*rule->decode_proc)(sub_output, obj, rule);
				if (unlikely(ret != 0)) {
					RTE_LOG(ERR, SPP_COMMAND_PROC, "Bad value. "
							"name=%s, index=%d\n", rule->name, i);
					return ret;
				}
			}
			break;
		default:
			sub_output = DR_GET_OUTPUT(output, rule);
			ret = (*rule->decode_proc)(sub_output, value_obj, rule);
			if (unlikely(ret != 0)) {
				RTE_LOG(ERR, SPP_COMMAND_PROC, "Bad value. "
						"name=%s\n", rule->name);
				return ret;
			}
			break;
		}
	}

	return 0;
}

/* decode rule for command-base */
const struct json_value_decode_rule DECODERULE_COMMAND_BASE[] = {
	{
		.name = "command",
		.json_type = JSON_STRING,
		.offset = offsetof(struct command, type),
		.decode_proc = decode_command_type_value,
	},
	END_OF_DECODE_RULE
};

#if 0 /* not supported */
/* decode rule for add-command-spec */
const struct json_value_decode_rule DECODERULE_ADD_COMMAND[] = {
	{
		.name = "ports",
		.json_type = JSON_ARRAY,
		.offset = offsetof(struct add_command, ports),
		.decode_proc = decode_port_value,

		.array.element_sz = sizeof(struct spp_config_port_info),
		.array.json_type = JSON_STRING,
		.array.offset_num = offsetof(struct add_command, num_port),
	},
	END_OF_DECODE_RULE
};
#endif

/* decode rule for classifier-table-command-spec */
const struct json_value_decode_rule DECODERULE_CLASSIFIER_TABLE_COMMAND[] = {
	{
		.name = "type",
		.json_type = JSON_STRING,
		.offset = offsetof(struct classifier_table_command, type),
		.decode_proc = decode_classifier_type_value,
	},{
		.name = "value",
		.json_type = JSON_STRING,
		.offset = offsetof(struct classifier_table_command, value),
		.decode_proc = decode_mac_addr_str_value,
	},{
		.name = "port",
		.json_type = JSON_STRING,
		.offset = offsetof(struct classifier_table_command, port),
		.decode_proc = decode_port_value,
	},
	END_OF_DECODE_RULE
};

/* decode procedure for command */
static int
decode_command_object(void* output, const json_t *parent_obj,
		__rte_unused const struct json_value_decode_rule *rule)
{
	int ret = -1;
	struct command *command = (struct command *)output;
	const struct json_value_decode_rule *spec_rules = NULL;

	/* decode command-base */
	ret = decode_json_object(command, parent_obj, DECODERULE_COMMAND_BASE);
	if (unlikely(ret != 0)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Bad command. ret=%d\n", ret);
		return ret;
	}

	/* decode command-specific */
	switch (command->type) {
		case CMDTYPE_CLASSIFIER_TABLE:
			spec_rules = DECODERULE_CLASSIFIER_TABLE_COMMAND;
			break;

		case CMDTYPE_FLUSH:
			/* nothing specific */
			break;

		default:
			/* unknown command */
			RTE_LOG(ERR, SPP_COMMAND_PROC, "Unknown command. type=%d\n",
					command->type);
			return DERR_UNKNOWN_COMMAND;
	}

	if (likely(spec_rules != NULL)) {
		ret = decode_json_object(&command->spec, parent_obj, spec_rules);
		if (unlikely(ret != 0)) {
			RTE_LOG(ERR, SPP_COMMAND_PROC, "Bad command. ret=%d\n", ret);
			return ret;
		}
	}

	return 0;
}

/* decode rule for command request */
const struct json_value_decode_rule DECODERULE_REQUEST[] = {
	{
		.name = "commands",
		.json_type = JSON_ARRAY,
		.offset = offsetof(struct request, commands),
		.decode_proc = decode_command_object,

		.array.element_sz = sizeof(struct command),
		.array.json_type = JSON_OBJECT,
		.array.offset_num = offsetof(struct request, num_command),
		.array.offset_num_valid = offsetof(struct request, num_valid_command),
	},
	END_OF_DECODE_RULE
};

/* decode request from no-null-terminated string */
static int
decode_request(struct request *request, const char *request_str, size_t request_str_len)
{
	int ret = -1;
	json_t *top_obj;
	json_error_t json_error;

	/* parse json string */
	top_obj = json_loadb(request_str, request_str_len, 0, &json_error);
	if (unlikely(top_obj == NULL)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Cannot parse command request. "
				"error=%s, request_str=%.*s\n", 
				json_error.text, (int)request_str_len, request_str);
		return DERR_BAD_FORMAT;
	}

	/* decode request object */
	ret = decode_json_object(request, top_obj, DECODERULE_REQUEST);
	if (unlikely(ret != 0)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Cannot decode command request. "
				"ret=%d, request_str=%.*s\n", 
				ret, (int)request_str_len, request_str);
		return ret;
	}

	return 0;
}

/* execute one command */
static int
execute_command(const struct command *command)
{
	int ret = -1;

	switch (command->type) {
	case CMDTYPE_CLASSIFIER_TABLE:
		RTE_LOG(DEBUG, SPP_COMMAND_PROC, "Execute classifier_table command.");
		ret = spp_update_classifier_table(
				command->spec.classifier_table.type,
				command->spec.classifier_table.value,
				&command->spec.classifier_table.port);
		break;

	case CMDTYPE_FLUSH:
		RTE_LOG(DEBUG, SPP_COMMAND_PROC, "Execute flush command.");
		ret = spp_flush();
		break;

	case CMDTYPE_PROCESS:
		RTE_LOG(DEBUG, SPP_COMMAND_PROC, "Execute process command.");
		ret = spp_get_process_id();
		break;

	default:
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Unknown command. type=%d\n", command->type);
		ret = 0;
		break;
	}

	return ret;
}

/* process command request from no-null-terminated string */
static int
process_request(const char *request_str, size_t request_str_len)
{
	int ret = -1;
	int i;

	struct request request;
	memset(&request, 0, sizeof(struct request));

	RTE_LOG(DEBUG, SPP_COMMAND_PROC, "Start command request processing. "
			"request_str=%.*s\n", (int)request_str_len, request_str);

	ret = decode_request(&request, request_str, request_str_len);
	if (unlikely(ret != 0)) {
		RTE_LOG(DEBUG, SPP_COMMAND_PROC, "Failed to process command request. "
		"ret=%d\n", ret);
		return ret;
	}

	RTE_LOG(DEBUG, SPP_COMMAND_PROC, "Decoded command request. "
			"num_command=%d, num_valid_command=%d\n",
			request.num_command, request.num_valid_command);

	for (i = 0; i < request.num_command ; ++i) {
		ret = execute_command(request.commands + i);
	}

	RTE_LOG(DEBUG, SPP_COMMAND_PROC, "Succeeded to process command request.\n");

	return 0;
}

/* initialize command processor. */
int
spp_command_proc_init(const char *controller_ip, int controller_port)
{
	strcpy(g_controller_ip, controller_ip);
	g_controller_port = controller_port;

	return 0;
}

/* process command from controller. */
void
spp_command_proc_do(void)
{
	int ret = -1;
	int i;

	static int sock = -1;
	static char *msgbuf = NULL;
	static size_t msg_len = 0;

	static size_t rb_cnt = 0;
	static size_t lb_cnt = 0;

	if (unlikely(msgbuf == NULL))
		msgbuf = msgbuf_allocate(MESSAGE_BUFFER_BLOCK_SIZE);

	ret = connect_to_controller(&sock);
	if (unlikely(ret != 0))
		return;

	ret = receive_message(&sock, &msgbuf);
	if (likely(ret == 0)) {
		return;
	}

	for (i = 0; i < ret; ++i) {
		switch (*(msgbuf + msg_len + i)) {
		case '{':
			++lb_cnt;
			break;
		case '}':
			++rb_cnt;
			break;
		}

		if (likely(lb_cnt != 0) && unlikely(rb_cnt == lb_cnt)) {
			msg_len += (i + 1);
			ret = process_request(msgbuf, msg_len);

			msgbuf_remove_front(msgbuf, msg_len);
			msg_len = 0;
			rb_cnt = 0;
			lb_cnt = 0;
		}
	}
}
