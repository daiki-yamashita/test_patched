#include <unistd.h>
#include <string.h>

#include <rte_log.h>
#include <rte_branch_prediction.h>

#include <jansson.h>

#include "spp_vf.h"
#include "spp_config.h"
#include "command_dec.h"

#define RTE_LOGTYPE_SPP_COMMAND_PROC RTE_LOGTYPE_USER1

/* command type string list
	do it same as the order of enum command_type */
static const char *COMMAND_TYPE_STRINGS[] = {
#if 0 /* not supported yet */
	"add",
	"component",
#endif
	"classifier_table",
	"flush",
#if 0 /* not supported yet */
	"forward",
	"stop",
#endif
	"process",
	"status",

	/* termination */ "",
};

/* classifier type string list
	do it same as the order of enum spp_classifier_type (spp_vf.h) */
static const char *CLASSIFILER_TYPE_STRINGS[] = {
	"none",
	"mac",

	/* termination */ "",
};

/* forward declaration */
struct json_value_decode_rule;

/* definition of decode procedure function */
typedef int (*json_value_decode_proc)(
		void *, 
		const json_t *, 
		const struct json_value_decode_rule *, 
		struct spp_command_decode_error *);

/* rule definition that decode json object to c-struct */
struct json_value_decode_rule {
	char name[SPP_CMD_NAME_BUFSZ];
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

/* set decode error */
inline int
set_decode_error(struct spp_command_decode_error *error,
		int error_code,
		const struct json_value_decode_rule *error_rule)
{
	error->code = error_code;

	if (likely(error_rule != NULL))
		strcpy(error->value_name, error_rule->name);

	return error->code;
}

/* set decode error */
inline int
set_string_value_decode_error(struct spp_command_decode_error *error,
		const char* value,
		const struct json_value_decode_rule *error_rule)
{
	strcpy(error->value, value);
	return set_decode_error(error, SPP_CMD_DERR_BAD_VALUE, error_rule);
}

/* helper */
#define END_OF_DECODE_RULE {.name = ""},
#define IS_END_OF_DECODE_RULE(rule) ((rule)->name[0] == '\0')

/* definition helper that enum value decode procedure */
#define DECODE_ENUM_VALUE(proc_name, enum_type, string_table)			\
static int									\
decode_##proc_name##_value(void *output, const json_t *value_obj,		\
		const struct json_value_decode_rule *rule,			\
		struct spp_command_decode_error *error)				\
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
	return set_string_value_decode_error(error, str_val, rule);		\
}										\

/* enum value decode procedure for "command_type" */
DECODE_ENUM_VALUE(command_type, enum spp_command_type, COMMAND_TYPE_STRINGS)

/* enum value decode procedure for "classifier_type" */
DECODE_ENUM_VALUE(classifier_type, enum spp_classifier_type, CLASSIFILER_TYPE_STRINGS)

#if 0 /* not supported yet */
/* decode procedure for integer */
static int
decode_int_value(void *output, const json_t *value_obj,
		__rte_unused const struct json_value_decode_rule *rule,
		__rte_unused struct spp_command_decode_error *error)
{
	int val = json_integer_value(value_obj);
	memcpy(output, &val, sizeof(int));

	return 0;
}

/* decode procedure for string */
static int
decode_string_value(void *output, const json_t *value_obj,
		__rte_unused const struct json_value_decode_rule *rule,
		__rte_unused struct spp_command_decode_error *error)
{
	const char* str_val = json_string_value(value_obj);
	strcpy(output, str_val);

	return 0;
}
#endif

/* decode procedure for mac address string */
static int
decode_mac_addr_str_value(void *output, const json_t *value_obj,
		const struct json_value_decode_rule *rule,
		struct spp_command_decode_error *error)
{
	int ret = -1;
	const char* str_val = json_string_value(value_obj);

	/* if default specification, convert to internal dummy address */
	if (unlikely(strcmp(str_val, SPP_CONFIG_DEFAULT_CLASSIFIED_SPEC_STR) == 0))
		str_val = SPP_CONFIG_DEFAULT_CLASSIFIED_DMY_ADDR_STR;

	ret = spp_config_change_mac_str_to_int64(str_val);
	if (unlikely(ret == -1)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Bad mac address string. val=%s\n",
				str_val);
		return set_string_value_decode_error(error, str_val, rule);
	}

	strcpy(output, str_val);

	return 0;
}

/* decode procedure for spp_config_port_info */
static int
decode_port_value(void *output, const json_t *value_obj,
		const struct json_value_decode_rule *rule,
		struct spp_command_decode_error *error)
{
	int ret = -1;
	const char* str_val = json_string_value(value_obj);
	struct spp_config_port_info *port = (struct spp_config_port_info *)output;

	if (strcmp(str_val, SPP_CMD_UNUSE) == 0) {
		port->if_type = UNDEF;
		port->if_no = 0;
		return 0;
	}

	ret = spp_config_get_if_info(str_val, &port->if_type, &port->if_no);
	if (unlikely(ret != 0)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Bad port. val=%s\n", str_val);
		return set_string_value_decode_error(error, str_val, rule);
	}

	return 0;
}

/* decode json object */
static int
decode_json_object(void *output, const json_t *parent_obj,
		const struct json_value_decode_rule *rules,
		struct spp_command_decode_error *error)
{
	int ret = -1;
	int i, n;
	json_t *obj;
	json_t *value_obj;
	const struct json_value_decode_rule *rule;

	void *sub_output;

	for (i = 0; unlikely(! IS_END_OF_DECODE_RULE(&rules[i])); ++ i) {
		rule = rules + i;

		RTE_LOG(DEBUG, SPP_COMMAND_PROC, "Get one object. name=%s\n",
				rule->name);

		value_obj = json_object_get(parent_obj, rule->name);
		if (unlikely(value_obj == NULL)) {
			RTE_LOG(ERR, SPP_COMMAND_PROC, "No parameter. "
					"name=%s\n", rule->name);
			return set_decode_error(error, SPP_CMD_DERR_NO_PARAM, rule);
		} else if (unlikely(json_typeof(value_obj) != rule->json_type)) {
			RTE_LOG(ERR, SPP_COMMAND_PROC, "Bad value type. "
					"name=%s\n", rule->name);
			return set_decode_error(error, SPP_CMD_DERR_BAD_TYPE, rule);
		}

		switch (rule->json_type) {
		case JSON_ARRAY:
			RTE_LOG(DEBUG, SPP_COMMAND_PROC, "Decode array. num=%lu\n",
					json_array_size(value_obj));

			*(int *)((char *)output + rule->array.offset_num) = 
					(int)json_array_size(value_obj);

			json_array_foreach(value_obj, n, obj) {
				RTE_LOG(DEBUG, SPP_COMMAND_PROC, "Decode array element. "
						"index=%d\n", n);
				
				if (unlikely(json_typeof(obj) != rule->array.json_type)) {
					RTE_LOG(ERR, SPP_COMMAND_PROC, "Bad value type. "
							"name=%s, index=%d\n", rule->name, n);
					return set_decode_error(error, SPP_CMD_DERR_BAD_TYPE, rule);
				}

				sub_output = DR_GET_OUTPUT(output, rule) + 
						(rule->array.element_sz * n);
				ret = (*rule->decode_proc)(sub_output, obj, rule, error);
				if (unlikely(ret != 0)) {
					RTE_LOG(ERR, SPP_COMMAND_PROC, "Bad value. "
							"name=%s, index=%d\n", rule->name, n);
					/* decode error is set in decode function */
					return ret;
				}

				*(int *)((char *)output +
						rule->array.offset_num_valid) = n + 1;
			}
			break;
		default:
			sub_output = DR_GET_OUTPUT(output, rule);
			ret = (*rule->decode_proc)(sub_output, value_obj, rule, error);
			if (unlikely(ret != 0)) {
				RTE_LOG(ERR, SPP_COMMAND_PROC, "Bad value. "
						"name=%s\n", rule->name);
				/* decode error is set in decode function */
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
		.offset = offsetof(struct spp_command, type),
		.decode_proc = decode_command_type_value,
	},
	END_OF_DECODE_RULE
};

#if 0 /* not supported yet */
/* decode rule for add-command-spec */
const struct json_value_decode_rule DECODERULE_ADD_COMMAND[] = {
	{
		.name = "ports",
		.json_type = JSON_ARRAY,
		.offset = offsetof(struct spp_command_add, ports),
		.decode_proc = decode_port_value,

		.array.element_sz = sizeof(struct spp_config_port_info),
		.array.json_type = JSON_STRING,
		.array.offset_num = offsetof(struct spp_command_add, num_port),
	},
	END_OF_DECODE_RULE
};
#endif

/* decode rule for classifier-table-command-spec */
const struct json_value_decode_rule DECODERULE_CLASSIFIER_TABLE_COMMAND[] = {
	{
		.name = "type",
		.json_type = JSON_STRING,
		.offset = offsetof(struct spp_command_classifier_table, type),
		.decode_proc = decode_classifier_type_value,
	},{
		.name = "value",
		.json_type = JSON_STRING,
		.offset = offsetof(struct spp_command_classifier_table, value),
		.decode_proc = decode_mac_addr_str_value,
	},{
		.name = "port",
		.json_type = JSON_STRING,
		.offset = offsetof(struct spp_command_classifier_table, port),
		.decode_proc = decode_port_value,
	},
	END_OF_DECODE_RULE
};

/* decode procedure for command */
static int
decode_command_object(void* output, const json_t *parent_obj,
		__rte_unused const struct json_value_decode_rule *rule,
		struct spp_command_decode_error *error)
{
	int ret = -1;
	struct spp_command *command = (struct spp_command *)output;
	const struct json_value_decode_rule *spec_rules = NULL;

	/* decode command-base */
	ret = decode_json_object(command, parent_obj, DECODERULE_COMMAND_BASE, error);
	if (unlikely(ret != 0)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Bad command. ret=%d\n", ret);
		/* decode error is set in decode_json_object function */
		return ret;
	}

	/* decode command-specific */
	switch (command->type) {
		case SPP_CMDTYPE_CLASSIFIER_TABLE:
			spec_rules = DECODERULE_CLASSIFIER_TABLE_COMMAND;
			break;

		default:
			/* nothing specific */
			/* (unknown command is already checked) */
			break;
	}

	if (likely(spec_rules != NULL)) {
		ret = decode_json_object(&command->spec, parent_obj, spec_rules, error);
		if (unlikely(ret != 0)) {
			RTE_LOG(ERR, SPP_COMMAND_PROC, "Bad command. ret=%d\n", ret);
			/* decode error is set in decode_json_object function */
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
		.offset = offsetof(struct spp_command_request, commands),
		.decode_proc = decode_command_object,

		.array.element_sz = sizeof(struct spp_command),
		.array.json_type = JSON_OBJECT,
		.array.offset_num = offsetof(struct spp_command_request, num_command),
		.array.offset_num_valid = offsetof(struct spp_command_request, num_valid_command),
	},
	END_OF_DECODE_RULE
};

/* decode request from no-null-terminated string */
int
spp_command_decode_request(struct spp_command_request *request, const char *request_str,
		size_t request_str_len, struct spp_command_decode_error *error)
{
	int ret = -1;
	int i;
	json_t *top_obj;
	json_error_t json_error;

	/* parse json string */
	top_obj = json_loadb(request_str, request_str_len, 0, &json_error);
	if (unlikely(top_obj == NULL)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Cannot parse command request. "
				"error=%s, request_str=%.*s\n", 
				json_error.text, (int)request_str_len, request_str);
		return set_decode_error(error, SPP_CMD_DERR_BAD_FORMAT, NULL);
	}

	/* decode request object */
	ret = decode_json_object(request, top_obj, DECODERULE_REQUEST, error);
	if (unlikely(ret != 0)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Cannot decode command request. "
				"ret=%d, request_str=%.*s\n", 
				ret, (int)request_str_len, request_str);
		/* decode error is set in decode_json_object function */
	}

	/* free json object */
	json_decref(top_obj);

	/* check getter command */
	for (i = 0; i < request->num_valid_command; ++i) {
		switch (request->commands[i].type) {
		case SPP_CMDTYPE_PROCESS:
			request->is_requested_process = 1;
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
