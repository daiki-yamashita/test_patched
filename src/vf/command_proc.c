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


#define RTE_LOGTYPE_SPP_COMMAND_PROC RTE_LOGTYPE_USER1


////////////////////////////////////////////////////////////////////////////////


#define MESSAGE_BUFFER_BLOCK_SIZE 512

static char g_controller_ip[128] = "";
static int g_controller_port = 0;

/*  */
inline char*
msgbuf_allocate(size_t capacity)
{
	char* buf = (char *)malloc(capacity + sizeof(size_t));
	if (unlikely(buf == NULL))
		return NULL;

	*((size_t *)buf) = capacity;

	return buf + sizeof(size_t);
}

/*  */
inline void
msgbuf_free(char* msgbuf)
{
	if (likely(msgbuf != NULL))
		free(msgbuf - sizeof(size_t));
}

/*  */
inline size_t
msgbuf_get_capacity(const char *msgbuf)
{
	return *((const size_t *)(msgbuf - sizeof(size_t)));
}

/*  */
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

/*  */
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

/*  */
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

/*  */
static int
connect_to_controller(int *sock)
{
	static struct sockaddr_in controller_addr;
	int ret = -1;
	int sock_flg = 0;

	if (likely(*sock >=0))
		return 0;

	if (*sock < 0) {
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

	ret = connect(*sock, (struct sockaddr *)&controller_addr,
			sizeof(controller_addr));
	if (ret < 0) {
		RTE_LOG(ERR, SPP_COMMAND_PROC,
				"Cannot connect to controller. errno=%d\n", errno);
		return -1;
	}

	sock_flg = fcntl(*sock, F_GETFL, 0);
	fcntl(*sock, F_SETFL, sock_flg | O_NONBLOCK);

	return 0;
}

/*  */
static int
receive_request(int *sock, char **msgbuf)
{
	int ret = -1;
	int n_rx = 0;
	char *new_msgbuf = NULL;

	char rx_buf[MESSAGE_BUFFER_BLOCK_SIZE];
	size_t rx_buf_sz = MESSAGE_BUFFER_BLOCK_SIZE;

	ret = recv(*sock, rx_buf, rx_buf_sz, 0);
	if (ret <= 0) {
		if (ret == 0) {

		} else {
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				RTE_LOG(ERR, SPP_COMMAND_PROC,
						"Cannot receive from socket. errno=%d\n", errno);
			}
			return 0;
		}
	}

	n_rx = ret;

	new_msgbuf = msgbuf_append(*msgbuf, rx_buf, n_rx);
	if (unlikely(new_msgbuf == NULL)) {
		return -1;
	}

	*msgbuf = new_msgbuf;

	return n_rx;
}

/*  */
static int
send_response(void)
{
	return 0;
}

////////////////////////////////////////////////////////////////////////////////

#define CMD_MAX_COMMANDS 32

#define CMD_NAME_BUFSZ 32

#define CMD_CLASSIFIER_TABLE_VALUE_BUFSZ 62

#define component_type spp_core_type

/* command type */
enum command_type {
	CMDTYPE_ADD,
	CMDTYPE_COMPONENT,
	CMDTYPE_CLASSIFIER_TABLE,
	CMDTYPE_FLUSH,
	CMDTYPE_FORWARD,
	CMDTYPE_STOP,

	CMDTYPE_PROCESS,
};

/* command type string list */
static const char *COMMAND_TYPE_STRINGS[] = {
	"add",
	"component",
	"classifier_table",
	"flush",
	"forward",
	"stop",

	/* termination */ "",
};

/* classifier type */
enum classifier_type {
	CLASSIFIERTYPE_MAC,	
};

/* classifier type string list */
static const char *CLASSIFILER_TYPE_STRINGS[] = {
	"mac",

	/* termination */ "",
};

/* "add" command parameters */
struct add_command {
	struct spp_config_port_info ports[RTE_MAX_ETHPORTS];
};

/* "component" command specific parameters */
struct component_command {
	enum component_type type;
	unsigned int core_id;
	struct spp_config_port_info rx_ports[RTE_MAX_ETHPORTS];
	struct spp_config_port_info tx_ports[RTE_MAX_ETHPORTS];
};

/* "classifier_table" command specific parameters */
struct classifier_table_command {
	enum classifier_type type;
	char value[CMD_CLASSIFIER_TABLE_VALUE_BUFSZ];
	struct spp_config_port_info port;
};

#if 0
/* "flush" command specific parameters */
struct flush_command {
	/* nothing specific */
};
#endif

/* command parameters */
struct command {
	enum command_type type;

	union {
		struct add_command add;
		struct component_command component;
		struct classifier_table_command classifier_table;
	} spec;
};

/* request parameters */
struct request {
	int num_command;
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
#if 0
	union {
		size_t buf_sz;
	} spec;

	json_type sub_json_type;
	const struct json_value_decode_rule *sub_rules;
#endif
	size_t sub_offset;
	size_t sub_offset_num;
};

/* get output address for decoded json value */
#define DR_GET_OUTPUT(output_base, rule__) ((char *)output_base + rule__->offset)

/* helper */
#define END_OF_DECODE_RULE {.name = ""},
#define IS_END_OF_DECODE_RULE(rule) ((rule)->name[0] == '\0')

/* definition helper that enum value decode procedure */
#define DECODE_ENUM_VALUE(proc_name, enum_type, string_table)			\
int										\
decode_##proc_name##_value(void *output, const json_t *value_obj,		\
		const struct json_value_decode_rule *rule)			\
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
DECODE_ENUM_VALUE(classifier_type, enum classifier_type, CLASSIFILER_TYPE_STRINGS)

/* decode procedure for integer */
static int
decode_int_value(void *output, const json_t *value_obj,
		const struct json_value_decode_rule *rule)
{
	int val = json_integer_value(value_obj);
	memcpy(output, &val, sizeof(int));

	return 0;
}

/* decode procedure for string */
static int
decode_string_value(void *output, const json_t *value_obj,
		const struct json_value_decode_rule *rule)
{
	const char* str_val = json_string_value(value_obj);
#if 0
	size_t len = strlen(str_val);
	if (unlikely(len >= rule->spec.buf_sz)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Invalid json value. "
				"name=%s\n", rule->name);
		return -1;
	}
#endif
	strcpy(output, str_val);

	return 0;
}

/* decode procedure for spp_config_port_info */
static int
decode_port_value(void *output, const json_t *value_obj,
		const struct json_value_decode_rule *rule)
{
	int ret = -1;
	struct spp_config_port_info *port = (struct spp_config_port_info *)output;

	const char* str_val = json_string_value(value_obj);

	ret = spp_config_get_if_info(str_val, &port->if_type, &port->if_no);
	if (unlikely(ret != 0))
		return -1;

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

		value_obj = json_object_get(parent_obj, rule->name);
		if (unlikely(value_obj == NULL) || 
				unlikely(json_typeof(value_obj) != rule->json_type)) {
			RTE_LOG(ERR, SPP_COMMAND_PROC, "Invalid json value. "
					"name=%s\n", rule->name);
			return -1;
		}

		switch (rule->json_type) {
		case JSON_ARRAY:
			json_array_foreach(value_obj, n, obj) {
				sub_output = DR_GET_OUTPUT(output, rule) + (rule->sub_offset * i);
				ret = (*rule->decode_proc)(sub_output, obj, rule);
			}
			*(int *)((char *)output + rule->sub_offset_num) = n;
			break;
		default:
			sub_output = DR_GET_OUTPUT(output, rule);
			ret = (*rule->decode_proc)(sub_output, value_obj, rule);
			break;
		}
	}

	return 0;
}

/*  */
const struct json_value_decode_rule DECODERULE_COMMAND_BASE[] = {
	{
		.name = "command",
		.json_type = JSON_STRING,
		.offset = offsetof(struct command, type),
		.decode_proc = decode_command_type_value,
	},
	END_OF_DECODE_RULE
};

/*  */
const struct json_value_decode_rule DECODERULE_ADD_COMMAND[] = {
	{
		.name = "ports",
		.json_type = JSON_ARRAY,
		.offset = offsetof(struct add_command, ports),
		.decode_proc = decode_port_value,
//		.sub_json_type = JSON_STRING,
		.sub_offset = sizeof(struct spp_config_port_info),
	},
	END_OF_DECODE_RULE
};

/*  */
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
		.decode_proc = decode_string_value,
	},{
		.name = "port",
		.json_type = JSON_STRING,
		.offset = offsetof(struct classifier_table_command, port),
		.decode_proc = decode_port_value,
	},
	END_OF_DECODE_RULE
};

/*  */
static int
decode_command_object(void* output, const json_t *parent_obj,
		const struct json_value_decode_rule *rule)
{
	int ret = -1;
	struct command *command = (struct command *)output;
	const struct json_value_decode_rule *spec_rules = NULL;

	/* decode command base */
	ret = decode_json_object(command, parent_obj, DECODERULE_COMMAND_BASE);
	if (unlikely(ret != 0)) {
		// TODO:コマンドがデコード失敗
		return -1;
	}

	/* decode command specific */
	switch (command->type) {
		case CMDTYPE_CLASSIFIER_TABLE:
			spec_rules = DECODERULE_CLASSIFIER_TABLE_COMMAND;
			break;

		case CMDTYPE_FLUSH:
			break;

		default:
			break;
	}

	if (likely(spec_rules != NULL)) {
		ret = decode_json_object(&command->spec, parent_obj, spec_rules);
		if (unlikely(ret != 0)) {
			// TODO:コマンドがデコード失敗
			return -1;
		}
	}

	return 0;
}

/*  */
const struct json_value_decode_rule DECODERULE_REQUEST[] = {
	{
		.name = "commands",
		.json_type = JSON_ARRAY,
		.offset = offsetof(struct request, commands),
		.decode_proc = decode_command_object,
//		.sub_json_type = JSON_OBJECT,
		.sub_offset = sizeof(struct command),
		.sub_offset_num = offsetof(struct request, num_command),
	},
	END_OF_DECODE_RULE
};

/*  */
static int
decode_request(struct request *request, json_t **top_obj, const char *request_str, size_t request_str_len)
{
	int ret = -1;
	int i;
	json_error_t json_error;

	/* parse json string */
	*top_obj = json_loadb(request_str, request_str_len, 0, &json_error);
	if (unlikely(*top_obj == NULL)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Cannot parse command request. "
				"error=%s\n", 
				json_error.text);
		return -1;
	}

	/* decode request object */
	ret = decode_json_object(request, *top_obj, DECODERULE_REQUEST);
	if (unlikely(ret != 0)) {
		// TODO:エラー
		return -1;
	}

	return 0;
}

/*  */
static int
execute_command(const struct command *command)
{
	int ret = -1;

	switch (command->type) {
	case CMDTYPE_CLASSIFIER_TABLE:
		ret = spp_update_classifier_table(
				SPP_CLASSIFIER_TYPE_MAC,
//				command->spec.classifier_table.type,
				command->spec.classifier_table.value,
				&command->spec.classifier_table.port);
		break;

	case CMDTYPE_FLUSH:
		ret = spp_flush();
		break;

	case CMDTYPE_PROCESS:
		break;

	default:
		ret = 0;
		break;
	}

	return 0;
}

/*  */
static int
process_request(const char *request_str, size_t request_str_len)
{
	int ret = -1;
	int i;

	struct request request;
	json_t *top_obj;

	memset(&request, 0, sizeof(struct request));

	ret = decode_request(&request, &top_obj, request_str, request_str_len);
	if (unlikely(ret != 0))
		return -1;

	for (i = 0; i < request.num_command ; ++i) {
		ret = execute_command(request.commands + i);
	}
}


////////////////////////////////////////////////////////////////////////////////


/*  */
int
spp_command_proc_init(const char *controller_ip, int controller_port)
{
	strcpy(g_controller_ip, controller_ip);
	g_controller_port = controller_port;

	return 0;
}

/*  */
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

	ret = receive_request(&sock, &msgbuf);
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
