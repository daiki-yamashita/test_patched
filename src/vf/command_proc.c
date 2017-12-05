#include <unistd.h>
#include <string.h>

#include <rte_log.h>
#include <rte_branch_prediction.h>

#include <jansson.h>

#include "spp_vf.h"
#include "string_buffer.h"
#include "command_conn.h"
#include "command_dec.h"
#include "command_proc.h"

#define RTE_LOGTYPE_SPP_COMMAND_PROC RTE_LOGTYPE_USER1

/* request message initial size */
#define CMD_REQ_BUF_INIT_SIZE 2048

/* command execution result code */
enum command_result_code {
	CRES_SUCCESS = 0,
	CRES_FAILURE,
	CRES_INVALID,
};

/* command execution result information */
struct command_result {
	int code;
};

/* execute one command */
static int
execute_command(const struct spp_command *command)
{
	int ret = 0;

	switch (command->type) {
	case SPP_CMDTYPE_CLASSIFIER_TABLE:
		RTE_LOG(INFO, SPP_COMMAND_PROC, "Execute classifier_table command.\n");
		ret = spp_update_classifier_table(
				command->spec.classifier_table.action,
				command->spec.classifier_table.type,
				command->spec.classifier_table.value,
				&command->spec.classifier_table.port);
		break;

	case SPP_CMDTYPE_FLUSH:
		RTE_LOG(INFO, SPP_COMMAND_PROC, "Execute flush command.\n");
		ret = spp_flush();
		break;

	case SPP_CMDTYPE_COMPONENT:
		RTE_LOG(INFO, SPP_COMMAND_PROC, "Execute component command.\n");
		ret = spp_update_component(
				command->spec.component.action,
				command->spec.component.name,
				command->spec.component.core,
				command->spec.component.type);
		break;

	case SPP_CMDTYPE_PORT:
		RTE_LOG(INFO, SPP_COMMAND_PROC, "Execute port command. (act = %d)\n",
				command->spec.port.action);
		ret = spp_update_port(
				command->spec.port.action,
				&command->spec.port.port,
				command->spec.port.rxtx,
				command->spec.port.name);
		break;

	default:
		RTE_LOG(INFO, SPP_COMMAND_PROC, "Execute other command. type=%d\n", command->type);
		/* nothing to do here */
		break;
	}

	return ret;
}

/* make decode error message for response */
static const char *
make_decode_error_message(const struct spp_command_decode_error *decode_error, char *message)
{
	switch (decode_error->code) {
	case SPP_CMD_DERR_BAD_FORMAT:
		sprintf(message, "bad message format");
		break;

	case SPP_CMD_DERR_UNKNOWN_COMMAND:
		sprintf(message, "unknown command(%s)", decode_error->value);
		break;

	case SPP_CMD_DERR_NO_PARAM:
		sprintf(message, "not enough parameter(%s)", decode_error->value_name);
		break;

	case SPP_CMD_DERR_BAD_TYPE:
		sprintf(message, "bad value type(%s)", decode_error->value_name);
		break;

	case SPP_CMD_DERR_BAD_VALUE:
		sprintf(message, "bad value(%s)", decode_error->value_name);
		break;

	default:
		sprintf(message, "error occur");
		break;
	}

	return message;
}

/* create error result object form decode error information */
inline json_t *
create_result_object(const char* res_str)
{
	return json_pack("{ss}", "result", res_str);
}

/* create error result object form decode error information */
inline json_t *
create_error_result_object(const char* err_msg)
{
	return json_pack("{sss{ss}}", "result", "error", "error_details", 
			"message", err_msg);
}

/* append decode result array object to specified object */
static int
append_response_decode_results_object(json_t *parent_obj,
		const struct spp_command_request *request,
		const struct spp_command_decode_error *decode_error)
{
	int ret = -1;
	int i;
	json_t *results_obj;
	char err_msg[128];

	results_obj = json_array();
	if (unlikely(results_obj == NULL))
		return -1;

	if (unlikely(decode_error->code == SPP_CMD_DERR_BAD_FORMAT)) {
		/* create & append bad message format result */
		ret = json_array_append_new(results_obj,
				create_error_result_object(
				make_decode_error_message(decode_error, err_msg)));
		if (unlikely(ret != 0)) {
			json_decref(results_obj);
			return -1;
		}
	} else {
		/* create & append results */
		for (i = 0; i < request->num_command; ++i) {
			ret = json_array_append_new(results_obj, create_result_object("invalid"));
			if (unlikely(ret != 0)) {
				json_decref(results_obj);
				return -1;
			}
		}

		/* create & rewrite error result */
		if (unlikely(request->num_command != request->num_valid_command)) {
			ret = json_array_set_new(results_obj,
					request->num_valid_command,
					create_error_result_object(
					make_decode_error_message(decode_error, err_msg)));
			if (unlikely(ret != 0)) {
				json_decref(results_obj);
				return -1;
			}
		}
	}

	/* set results object in parent object */
	ret = json_object_set_new(parent_obj, "results", results_obj);
	if (unlikely(ret != 0))
		return -1;

	return 0;
}

/* append command execution result array object to specified object */
static int
append_response_command_results_object(json_t *parent_obj,
		const struct spp_command_request *request,
		const struct command_result *results)
{
	int ret = -1;
	int i;
	json_t *results_obj, *res_obj;

	results_obj = json_array();
	if (unlikely(results_obj == NULL))
		return -1;

	/* create & append results */
	for (i = 0; i < request->num_command; ++i) {
		switch (results[i].code) {
		case CRES_SUCCESS:
			res_obj = create_result_object("success");
			break;
		case CRES_FAILURE:
			res_obj = create_error_result_object("error occur");
			break;
		case CRES_INVALID: /* FALLTHROUGH */
		default:
			res_obj = create_result_object("invalid");
			break;
		}

		ret = json_array_append_new(results_obj, res_obj);
		if (unlikely(ret != 0)) {
			json_decref(results_obj);
			return -1;
		}
	}

	/* set results object in parent object */
	ret = json_object_set_new(parent_obj, "results", results_obj);
	if (unlikely(ret != 0))
		return -1;

	return 0;
}

/* append client id value to specified json object */
static int
append_response_client_id_value(json_t *parent_obj)
{
	int ret = -1;
	json_t *proc_obj;

	proc_obj = json_integer(spp_get_client_id());
	if (unlikely(proc_obj == NULL))
		return -1;

	ret = json_object_set_new(parent_obj, "client_id", proc_obj);
	if (unlikely(ret != 0))
		return -1;

	return 0;
}

/* append client-id value */
static int
append_client_id_value(json_t *parent_obj)
{
	int ret = -1;
	ret = json_object_set_new(parent_obj, "client-id",
			json_integer(spp_get_client_id()));
	if (unlikely(ret != 0))
		return -1;

	return 0;
}

/* append interface array */
static int
append_interface_array(json_t *parent_obj, const char *name,
		const enum port_type type)
{
	int ret = -1;
	int i = 0;
	json_t *array_obj;
	array_obj = json_array();
	if (unlikely(array_obj == NULL))
		return -1;

	for (i = 0; i < RTE_MAX_ETHPORTS; i++) {
		if (!spp_check_flush_port(type, i))
			continue;

		ret = json_array_append_new(array_obj, json_integer(i));
		if (unlikely(ret != 0)) {
			json_decref(array_obj);
			return -1;
		}
	}

	ret = json_object_set_new(parent_obj, name, array_obj);
	if (unlikely(ret != 0)) {
		json_decref(array_obj);
		return -1;
	}

	return 0;
}

/* append interface value */
static int
append_interface_value(json_t *parent_obj)
{
	int ret = -1;
	ret = append_interface_array(parent_obj, "phy", PHY);
	if (unlikely(ret != 0))
		return -1;

	ret = append_interface_array(parent_obj, "vhost", VHOST);
	if (unlikely(ret != 0))
		return -1;

	ret = append_interface_array(parent_obj, "ring", RING);
	if (unlikely(ret != 0))
		return -1;

	return 0;
}

/* append port array */
static int
apeend_port_array(json_t *parent_obj, const char *name,
		const int num, const struct spp_port_index *ports)
{
	int ret = -1;
	int i = 0;
	char port_str[64];
	json_t *array_obj;
	array_obj = json_array();
	if (unlikely(array_obj == NULL))
		return -1;

	for (i = 0; i < num; i++) {
		spp_format_port_string(port_str, ports[i].if_type,
				ports[i].if_no);
		ret = json_array_append_new(array_obj, json_string(port_str));
		if (unlikely(ret != 0)) {
			json_decref(array_obj);
			return -1;
		}
	}

	ret = json_object_set_new(parent_obj, name, array_obj);
	if (unlikely(ret != 0)) {
		json_decref(array_obj);
		return -1;
	}

	return 0;
}

/* append core element value */
static int
append_core_element_value(
		void *opaque, const unsigned int lcore_id,
		const char *name, const char *type,
		const int num_rx, const struct spp_port_index *rx_ports,
		const int num_tx, const struct spp_port_index *tx_ports)
{
	int ret = -1;
	json_t *parent_obj = (json_t *)opaque;
	json_t *tab_obj;

	tab_obj = json_object();
	if (unlikely(tab_obj == NULL))
		return -1;

	ret = json_object_set_new(tab_obj, "core", json_integer(lcore_id));
	if (unlikely(ret != 0)) {
		json_decref(tab_obj);
		return -1;
	}

	ret = json_object_set_new(tab_obj, "name", json_string(name));
	if (unlikely(ret != 0)) {
		json_decref(tab_obj);
		return -1;
	}

	ret = json_object_set_new(tab_obj, "type", json_string(type));
	if (unlikely(ret != 0)) {
		json_decref(tab_obj);
		return -1;
	}

	ret = apeend_port_array(tab_obj, "rx_port", num_rx, rx_ports);
	if (unlikely(ret != 0)) {
		json_decref(tab_obj);
		return -1;
	}

	ret = apeend_port_array(tab_obj, "tx_port", num_tx, tx_ports);
	if (unlikely(ret != 0)) {
		json_decref(tab_obj);
		return -1;
	}

	ret = json_array_append_new(parent_obj, tab_obj);
	if (unlikely(ret != 0)) {
		json_decref(tab_obj);
		return -1;
	}

	return 0;
}

/* append core value */
static int
append_response_core_value(json_t *parent_obj)
{
	int ret = -1;
	json_t *tab_obj;
	struct spp_iterate_core_params itr_params;

	tab_obj = json_array();
	if (unlikely(tab_obj == NULL))
		return -1;

	itr_params.opaque = tab_obj;
	itr_params.element_proc = append_core_element_value;

	ret = spp_iterate_core_info(&itr_params);
	if (unlikely(ret != 0)) {
		json_decref(tab_obj);
		return -1;
	}

	ret = json_object_set_new(parent_obj, "core", tab_obj);
	if (unlikely(ret != 0)) {
		json_decref(tab_obj);
		return -1;
	}

	return 0;
}

/* append classifier element value */
static int
append_classifier_element_value(
		void *opaque,
		__rte_unused enum spp_classifier_type type,
		const char *data,
		const struct spp_port_index *port)
{
	json_t *parent_obj = (json_t *)opaque;

	char port_str[64];
	spp_format_port_string(port_str, port->if_type, port->if_no);

	json_array_append_new(parent_obj, json_pack(
			"{ssssss}",
			"type", "mac",
			"value", data,
			"port", port_str));

	return 0;
}

/* append classifier_table value */
static int 
append_response_classifier_value(json_t *parent_obj)
{
	int ret = -1;
	json_t *tab_obj;
	struct spp_iterate_classifier_table_params itr_params;

	/* create classifier_table array */
	tab_obj = json_array();
	if (unlikely(tab_obj == NULL))
		return -1;

	itr_params.opaque = tab_obj;
	itr_params.element_proc = append_classifier_element_value;

	ret = spp_iterate_classifier_table(&itr_params);
	if (unlikely(ret != 0)) {
		json_decref(tab_obj);
		return -1;
	}

	ret = json_object_set_new(parent_obj, "classifier_table", tab_obj);
	if (unlikely(ret != 0)) {
		json_decref(tab_obj);
		return -1;
	}

	return 0;
}

/* append info value(status response) to specified json object */
static int 
append_response_info_value(json_t *parent_obj)
{
	int ret = -1;
	json_t *info_obj;

	/* set classifier_table object in info object */
	info_obj = json_object();
	if (unlikely(info_obj == NULL))
		return -1;

	ret = append_client_id_value(info_obj);
	if (unlikely(ret != 0)) {
		json_decref(info_obj);
		return -1;
	}

	ret = append_interface_value(info_obj);
	if (unlikely(ret != 0)) {
		json_decref(info_obj);
		return -1;
	}

	ret = append_response_core_value(info_obj);
	if (unlikely(ret != 0)) {
		json_decref(info_obj);
		return -1;
	}

	ret = append_response_classifier_value(info_obj);
	if (unlikely(ret != 0)) {
		json_decref(info_obj);
		return -1;
	}

	/* set info object in parent object */
	ret = json_object_set_new(parent_obj, "info", info_obj);
	if (unlikely(ret != 0)) {
		json_decref(info_obj);
		return -1;
	}

	return 0;
}

/* send response for decode error */
static void
send_decode_error_response(int *sock, const struct spp_command_request *request,
		const struct spp_command_decode_error *decode_error)
{
	int ret = -1;
	char *msg;
	json_t *top_obj;

	top_obj = json_object();
	if (unlikely(top_obj == NULL)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Failed to make decode error response.");
		return;
	}

	/* create & append result array */
	ret = append_response_decode_results_object(top_obj, request, decode_error);
	if (unlikely(ret != 0)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Failed to make decode error response.");
		json_decref(top_obj);
		return;
	}

	/* serialize */
	msg = json_dumps(top_obj, JSON_INDENT(2) | JSON_PRESERVE_ORDER);
	json_decref(top_obj);

	RTE_LOG(DEBUG, SPP_COMMAND_PROC, "Make command response (decode error). "
			"response_str=\n%s\n", msg);

	/* send response to requester */
	ret = spp_send_message(sock, msg, strlen(msg));
	if (unlikely(ret != 0)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Failed to send decode error response.");
		/* not return */
	}

	free(msg);
}

/* send response for command execution result */
static void
send_command_result_response(int *sock, const struct spp_command_request *request,
		const struct command_result *command_results)
{
	int ret = -1;
	char *msg;
	json_t *top_obj;

	top_obj = json_object();
	if (unlikely(top_obj == NULL)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Failed to make command result response.");
		return;
	}

	/* create & append result array */
	ret = append_response_command_results_object(top_obj, request, command_results);
	if (unlikely(ret != 0)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Failed to make command result response.");
		json_decref(top_obj);
		return;
	}

	/* append client id information value */
	if (request->is_requested_client_id) {
		ret = append_response_client_id_value(top_obj);
		if (unlikely(ret != 0)) {
			RTE_LOG(ERR, SPP_COMMAND_PROC, "Failed to make command result response.");
			json_decref(top_obj);
			return;
		}
	}

	/* append info value */
	if (request->is_requested_status) {
		ret = append_response_info_value(top_obj);
		if (unlikely(ret != 0)) {
			RTE_LOG(ERR, SPP_COMMAND_PROC, "Failed to make command result response.");
			json_decref(top_obj);
			return;
		}
	}

	/* serialize */
	msg = json_dumps(top_obj, JSON_INDENT(2) | JSON_PRESERVE_ORDER);
	json_decref(top_obj);

	RTE_LOG(DEBUG, SPP_COMMAND_PROC, "Make command response (command result). "
			"response_str=\n%s\n", msg);

	/* send response to requester */
	ret = spp_send_message(sock, msg, strlen(msg));
	if (unlikely(ret != 0)) {
		RTE_LOG(ERR, SPP_COMMAND_PROC, "Failed to send command result response.");
		/* not return */
	}

	free(msg);
}

/* process command request from no-null-terminated string */
static int
process_request(int *sock, const char *request_str, size_t request_str_len)
{
	int ret = -1;
	int i;

	struct spp_command_request request;
	struct spp_command_decode_error decode_error;
	struct command_result command_results[SPP_CMD_MAX_COMMANDS];

	memset(&request, 0, sizeof(struct spp_command_request));
	memset(&decode_error, 0, sizeof(struct spp_command_decode_error));
	memset(command_results, 0, sizeof(command_results));

	RTE_LOG(DEBUG, SPP_COMMAND_PROC, "Start command request processing. "
			"request_str=\n%.*s\n", (int)request_str_len, request_str);

	/* decode request message */
	ret = spp_command_decode_request(
			&request, request_str, request_str_len, &decode_error);
	if (unlikely(ret != 0)) {
		/* send error response */
		send_decode_error_response(sock, &request, &decode_error);
		RTE_LOG(DEBUG, SPP_COMMAND_PROC, "End command request processing.\n");
		return 0;
	}

	RTE_LOG(DEBUG, SPP_COMMAND_PROC, "Command request is valid. "
			"num_command=%d, num_valid_command=%d\n",
			request.num_command, request.num_valid_command);

	/* execute commands */
	for (i = 0; i < request.num_command ; ++i) {
		ret = execute_command(request.commands + i);
		if (unlikely(ret != 0)) {
			command_results[i].code = CRES_FAILURE;

			/* not execute remaining commands */
			for (++i; i < request.num_command ; ++i)
				command_results[i].code = CRES_INVALID;
			break;
		}

		command_results[i].code = CRES_SUCCESS;
	}

	if (request.is_requested_exit) {
		/* Terminated by process exit command.                       */
		/* Other route is normal end because it responds to command. */
		RTE_LOG(INFO, SPP_COMMAND_PROC, "No response with process exit command.");
		return -1;
	}

	/* send response */
	send_command_result_response(sock, &request, command_results);

	RTE_LOG(DEBUG, SPP_COMMAND_PROC, "End command request processing.\n");

	return 0;
}

/* initialize command processor. */
int
spp_command_proc_init(const char *controller_ip, int controller_port)
{
	return spp_command_conn_init(controller_ip, controller_port);
}

/* process command from controller. */
int
spp_command_proc_do(void)
{
	int ret = -1;
	int msg_ret = -1;

	static int sock = -1;
	static char *msgbuf = NULL;

	if (unlikely(msgbuf == NULL)) {
		msgbuf = spp_strbuf_allocate(CMD_REQ_BUF_INIT_SIZE);
		if (unlikely(msgbuf == NULL)) {
			RTE_LOG(ERR, SPP_COMMAND_PROC,
					"Cannot allocate memory for receive data(init).\n");
			return -1;
		}
	}

	ret = spp_connect_to_controller(&sock);
	if (unlikely(ret != 0))
		return 0;

	msg_ret = spp_receive_message(&sock, &msgbuf);
	if (unlikely(msg_ret <= 0)) {
		if (likely(msg_ret == 0))
			return 0;
		else if (unlikely(msg_ret == SPP_CONNERR_TEMPORARY))
			return 0;
		else
			return -1;
	}

	ret = process_request(&sock, msgbuf, msg_ret);
	spp_strbuf_remove_front(msgbuf, msg_ret);

	return ret;
}
