#include <sys/types.h>
#include <jansson.h>

#include <rte_log.h>

#include "spp_config.h"

#define CONFIG_CORE_TYPE_CLASSIFIER_MAC "classifier_mac"
#define CONFIG_CORE_TYPE_MERGE          "merge"
#define CONFIG_CORE_TYPE_FORWARD        "forward"

#define JSONPATH_CLASSIFIER_TABLE "$.classifier_table"
#define JSONPATH_PROC_TABLE "$.vfs"
#define JSONPATH_NAME       "$.name"
#define JSONPATH_TABLE      "$.table"
#define JSONPATH_MAC        "$.mac"
#define JSONPATH_PORT       "$.port"
#define JSONPATH_NUM_VHOST  "$.num_vhost"
#define JSONPATH_NUM_RING   "$.num_ring"
#define JSONPATH_FUNCTIONS  "$.functions"
#define JSONPATH_CORE_NO    "$.core"
#define JSONPATH_CORE_TYPE  "$.type"
#define JSONPATH_RX_PORT    "$.rx_port"
#define JSONPATH_TX_PORT    "$.tx_port"
#define JSONPATH_TX_TABLE   "$.tx_port_table"

/*
 * Instead of json_path_get
 */
json_t *
spp_config_get_path_obj(const json_t *json, const char *path)
{
	const json_t *obj, *array_obj;
	json_t *new_obj = NULL;
	char buf[SPP_CONFIG_PATH_LEN];
	char *str, *token, *bracket, *endptr;
	int index = 0;

	if (unlikely(path[0] != '$') || unlikely(path[1] != '.') ||
			unlikely(strlen(path) >= SPP_CONFIG_PATH_LEN))
		return NULL;

	strcpy(buf, path);
	obj = json;
	str = buf+1;
	while(str != NULL) {
		token = str+1;
		str = strpbrk(token, ".");
		if (str != NULL)
			*str = '\0';

		bracket = strpbrk(token, "[");
		if (bracket != NULL)
			*bracket = '\0';

		new_obj = json_object_get(obj, token);
		if (new_obj == NULL)
			return NULL;

		if (bracket != NULL) {
			index = strtol(bracket+1, &endptr, 0);
			if (unlikely(bracket+1 == endptr) || unlikely(*endptr != ']'))
				return NULL;

			array_obj = new_obj;
			new_obj = json_array_get(array_obj, index);
			if (new_obj == NULL)
				return NULL;
		}

		obj = new_obj;
	}

	return new_obj;
}

/*
 * Get integer data from config
 */
static int
config_get_int_value(const json_t *obj, const char *path, int *value)
{
	/* 指定パラメータのJsonオブジェクト取得 */
	json_t *tmp_obj = spp_config_get_path_obj(obj, path);
	if (unlikely(tmp_obj == NULL)) {
		/* 必須でないデータを取得する場合を考慮し、DEBUGログとする。 */
		RTE_LOG(DEBUG, APP, "No parameter. (path = %s)\n", path);
		return -1;
	}

	/* Integer type check */
	if (unlikely(!json_is_integer(tmp_obj))) {
		/* 必須でないデータを取得する場合を考慮し、DEBUGログとする。 */
		RTE_LOG(DEBUG, APP, "Not an integer. (path = %s)\n", path);
		return -1;
	}

	/* Set to OUT parameter */
	*value = json_integer_value(tmp_obj);
	RTE_LOG(DEBUG, APP, "get value = %d\n", *value);
	return 0;
}

/*
 * Get String data from config
 */
static int
config_get_str_value(const json_t *obj, const char *path, char *value)
{
	/* 指定パラメータのJsonオブジェクト取得 */
	json_t *tmp_obj = spp_config_get_path_obj(obj, path);
	if (unlikely(tmp_obj == NULL)) {
		RTE_LOG(DEBUG, APP, "No parameter. (path = %s)\n", path);
		return -1;
	}

	/* String type check */
	if (unlikely(!json_is_string(tmp_obj))) {
		RTE_LOG(DEBUG, APP, "Not a string. (path = %s)\n", path);
		return -1;
	}

	/* Set to OUT parameter */
	strcpy(value, json_string_value(tmp_obj));
	RTE_LOG(DEBUG, APP, "get value = %s\n", value);
	return 0;
}

/*
 * コンフィグ情報初期化
 */
static void
config_init_data(struct spp_config_area *config)
{
	/* 0クリア */
	memset(config, 0x00, sizeof(struct spp_config_area));
	int core_cnt, port_cnt, table_cnt;

	/* IF種別初期設定 */
	for (core_cnt = 0; core_cnt < SPP_CONFIG_CORE_MAX; core_cnt++) {
		for (port_cnt = 0; port_cnt < RTE_MAX_ETHPORTS; port_cnt++) {
			config->proc.functions[core_cnt].rx_ports[port_cnt].if_type = UNDEF;
			config->proc.functions[core_cnt].tx_ports[port_cnt].if_type = UNDEF;
		}
	}
	for (table_cnt = 0; table_cnt < SPP_CONFIG_MAC_TABLE_MAX; table_cnt++) {
		config->classifier_table.mac_tables[table_cnt].port.if_type = UNDEF;
	}

	return;
}

/*
 * IFの情報からIF種別とIF番号を取得する
 * ("ring0" -> 種別："ring"、番号：0)
 */
int
spp_config_get_if_info(const char *port, enum port_type *if_type, int *if_no)
{
	enum port_type type = UNDEF;
	const char *no_str = NULL;
	char *endptr = NULL;

	/* IF type check */
	if (strncmp(port, SPP_CONFIG_IFTYPE_NIC, strlen(SPP_CONFIG_IFTYPE_NIC)) == 0) {
		/* NIC */
		type = PHY;
		no_str = &port[strlen(SPP_CONFIG_IFTYPE_NIC)];
	} else if (strncmp(port, SPP_CONFIG_IFTYPE_VHOST, strlen(SPP_CONFIG_IFTYPE_VHOST)) == 0) {
		/* VHOST */
		type = VHOST;
		no_str = &port[strlen(SPP_CONFIG_IFTYPE_VHOST)];
	} else if (strncmp(port, SPP_CONFIG_IFTYPE_RING, strlen(SPP_CONFIG_IFTYPE_RING)) == 0) {
		/* RING */
		type = RING;
		no_str = &port[strlen(SPP_CONFIG_IFTYPE_RING)];
	} else {
		/* OTHER */
		RTE_LOG(ERR, APP, "Unknown interface type. (port = %s)\n", port);
		return -1;
	}

	/* IF番号を文字列から数値変換 */
	int ret_no = strtol(no_str, &endptr, 0);
	if (unlikely(no_str == endptr) || unlikely(*endptr != '\0')) { 
		/* No IF number */
		RTE_LOG(ERR, APP, "No interface number. (port = %s)\n", port);
		return -1;
	}

	/* Set OUT parameter */
	*if_type = type;
	*if_no = ret_no;

	RTE_LOG(DEBUG, APP, "Port = %s => Type = %d No = %d\n",
			port, *if_type, *if_no);
	return 0;
}

/*
 * MAC addressを文字列から数値へ変換
 */
int64_t
spp_config_change_mac_str_to_int64(const char *mac)
{
	int64_t ret_mac = 0;
	int64_t token_val = 0;
	int token_cnt = 0;
	char tmp_mac[SPP_CONFIG_STR_LEN];
	char *str = tmp_mac;
	char *saveptr = NULL;
	char *endptr = NULL;

	RTE_LOG(DEBUG, APP, "MAC address change. (mac = %s)\n", mac);

	strcpy(tmp_mac, mac);
	while(1) {
		/* Split by clolon(':') */
		char *ret_tok = strtok_r(str, ":", &saveptr);
		if (unlikely(ret_tok == NULL)) {
			break;
		}

		/* Convert string to hex value */
		int ret_tol = strtol(ret_tok, &endptr, 16);
		if (unlikely(ret_tok == endptr) || unlikely(*endptr != '\0')) {
			break;
		}

		/* 各数値をまとめる */
		token_val = (int64_t)ret_tol;
		ret_mac |= token_val << (token_cnt * 8);
		token_cnt++;
		str = NULL;
	}

	/* 区切り文字が5個以外 */
	if (unlikely(token_cnt != ETHER_ADDR_LEN)) {
		RTE_LOG(ERR, APP, "MAC address format error. (mac = %s)\n",
				 mac);
		return -1;
	}

	RTE_LOG(DEBUG, APP, "MAC address change. (mac = %s => 0x%08lx)\n",
			 mac, ret_mac);
	return ret_mac;
}

/*
 * Classifier table読み込み
 */
static int
config_load_classifier_table(const json_t *obj,
		struct spp_config_classifier_table *classifier_table)
{
	/* classifier_table用オブジェクト取得 */
	json_t *classifier_obj = spp_config_get_path_obj(obj, JSONPATH_CLASSIFIER_TABLE);
	if (unlikely(classifier_obj == NULL)) {
		RTE_LOG(INFO, APP, "No classifier table.\n");
		return 0;
	}

	/* name取得 */
	int ret_name = config_get_str_value(classifier_obj, JSONPATH_NAME,
			classifier_table->name);
	if (unlikely(ret_name != 0)) {
		RTE_LOG(ERR, APP, "Classifier table name get failed.\n");
		return -1;
	}

	/* table用オブジェクト取得 */
	json_t *array_obj = spp_config_get_path_obj(classifier_obj, JSONPATH_TABLE);
	if (unlikely(!array_obj)) {
		RTE_LOG(ERR, APP, "Json object get failed. (path = %s)\n",
				JSONPATH_TABLE);
		return -1;
	}

	/* table用オブジェクトが配列かチェック */
	if (unlikely(!json_is_array(array_obj))) {
		RTE_LOG(ERR, APP, "Not an array. (path = %s)\n",
				JSONPATH_TABLE);
		return -1;
	}

	/* table用オブジェクトの要素数取得 */
	int array_num = json_array_size(array_obj);
	if (unlikely(array_num <= 0) ||
			unlikely(array_num > SPP_CONFIG_MAC_TABLE_MAX)) {
		RTE_LOG(ERR, APP, "Table size out of range. (path = %s, size = %d)\n",
				JSONPATH_TABLE, array_num);
		return -1;
	}
	classifier_table->num_table = array_num;

	/* テーブルの各要素毎にデータ取得 */
	struct spp_config_mac_table_element *tmp_table = NULL;
	char if_str[SPP_CONFIG_STR_LEN];
	int table_cnt = 0;
	for (table_cnt = 0; table_cnt < array_num; table_cnt++) {
		tmp_table = &classifier_table->mac_tables[table_cnt];

		/* 要素取得 */
		json_t *elements_obj = json_array_get(array_obj, table_cnt);
		if (unlikely(elements_obj == NULL)) {
			RTE_LOG(ERR, APP,
				"Element get failed. (No = %d, path = %s)\n",
				table_cnt, JSONPATH_TABLE);
			return -1;
		}

		/* MACアドレス(文字列)取得 */
		int ret_mac = config_get_str_value(elements_obj, JSONPATH_MAC,
				tmp_table->mac_addr_str);
		if (unlikely(ret_mac != 0)) {
			RTE_LOG(ERR, APP,
				"MAC address get failed. (No = %d, path = %s)\n",
				table_cnt, JSONPATH_MAC);
			return -1;
		}

		/* デフォルト転送先指定であれば内部流通用ダミーアドレスに変換 */
		if (unlikely(strcmp(tmp_table->mac_addr_str,
				SPP_CONFIG_DEFAULT_CLASSIFIED_SPEC_STR) == 0))
			strcpy(tmp_table->mac_addr_str,
					SPP_CONFIG_DEFAULT_CLASSIFIED_DMY_ADDR_STR);

		/* MACアドレス数値変換 */
		int64_t ret_mac64 = spp_config_change_mac_str_to_int64(
				tmp_table->mac_addr_str);
		if (unlikely(ret_mac64 == -1)) {
			RTE_LOG(ERR, APP,
				"MAC address change failed. (No = %d, mac = %s)\n",
				table_cnt, tmp_table->mac_addr_str);
			return -1;
		}
		tmp_table->mac_addr = ret_mac64;

		/* IF情報取得 */
		int ret_if_str = config_get_str_value(elements_obj,
				JSONPATH_PORT, if_str);
		if (unlikely(ret_if_str != 0)) {
			RTE_LOG(ERR, APP,
				"Interface get failed. (No = %d, path = %s)\n",
				table_cnt, JSONPATH_PORT);
			return -1;
		}

		/* IF種別とIF番号に分割 */
		int ret_if = spp_config_get_if_info(if_str, &tmp_table->port.if_type,
				&tmp_table->port.if_no);
		if (unlikely(ret_if != 0)) {
			RTE_LOG(ERR, APP,
				"Interface change failed. (No = %d, IF = %s)\n",
				table_cnt, if_str);
			return -1;
		}
	}

	return 0;
}

/*
 * 処理種別を文字列から数値変換
 */
static enum spp_core_type
config_change_core_type(const char *core_type)
{
	if(strncmp(core_type, CONFIG_CORE_TYPE_CLASSIFIER_MAC,
			 strlen(CONFIG_CORE_TYPE_CLASSIFIER_MAC)+1) == 0) {
		/* Classifier */
		return SPP_CONFIG_CLASSIFIER_MAC;
	} else if (strncmp(core_type, CONFIG_CORE_TYPE_MERGE,
			 strlen(CONFIG_CORE_TYPE_MERGE)+1) == 0) {
		/* Merge */
		return SPP_CONFIG_MERGE;
	} else if (strncmp(core_type, CONFIG_CORE_TYPE_FORWARD,
			 strlen(CONFIG_CORE_TYPE_FORWARD)+1) == 0) {
		/* Forward */
		return SPP_CONFIG_FORWARD;
	}
	return SPP_CONFIG_UNUSE;
}

/*
 * 受信ポート取得
 */
static int
config_set_rx_port(enum spp_core_type type, json_t *obj,
		struct spp_config_functions *functions)
{
	struct spp_config_port_info *tmp_rx_port = NULL;
	char if_str[SPP_CONFIG_STR_LEN];
	if (type == SPP_CONFIG_MERGE) {
		/* Merge */
		/* 受信ポート用オブジェクト取得 */
		json_t *array_obj = spp_config_get_path_obj(obj, JSONPATH_RX_PORT);
		if (unlikely(!array_obj)) {
			RTE_LOG(ERR, APP, "Json object get failed. (path = %s, route = merge)\n",
				JSONPATH_RX_PORT);
			return -1;
		}

		/* 受信ポート用オブジェクトが配列かチェック */
		if (unlikely(!json_is_array(array_obj))) {
			RTE_LOG(ERR, APP, "Not an array. (path = %s, route = merge)\n",
				JSONPATH_TABLE);
			return -1;
		}

		/* 受信ポート用オブジェクトの要素数取得 */
		int port_num = json_array_size(array_obj);
		if (unlikely(port_num <= 0) ||
				unlikely(port_num > RTE_MAX_ETHPORTS)) {
			RTE_LOG(ERR, APP, "RX port out of range. (path = %s, port = %d, route = merge)\n",
					JSONPATH_RX_PORT, port_num);
			return -1;
		}
		functions->num_rx_port = port_num;

		/* 要素毎にデータ取得 */
		int array_cnt = 0;
		for (array_cnt = 0; array_cnt < port_num; array_cnt++) {
			tmp_rx_port = &functions->rx_ports[array_cnt];

			/* 要素取得 */
			json_t *elements_obj = json_array_get(array_obj, array_cnt);
			if (unlikely(elements_obj == NULL)) {
				RTE_LOG(ERR, APP,
					"Element get failed. (No = %d, path = %s, route = merge)\n",
					array_cnt, JSONPATH_RX_PORT);
				return -1;
			}

			/* String type check */
			if (unlikely(!json_is_string(elements_obj))) {
				RTE_LOG(ERR, APP, "Not a string. (path = %s, No = %d, route = merge)\n",
						JSONPATH_RX_PORT, array_cnt);
				return -1;
			}
			strcpy(if_str, json_string_value(elements_obj));

			/* IF種別とIF番号に分割 */
			int ret_if = spp_config_get_if_info(if_str, &tmp_rx_port->if_type,
					&tmp_rx_port->if_no);
			if (unlikely(ret_if != 0)) {
				RTE_LOG(ERR, APP,
					"Interface change failed. (No = %d, port = %s, route = merge)\n",
					array_cnt, if_str);
				return -1;
			}
		}
	} else {
		/* Classifier/Forward */
		tmp_rx_port = &functions->rx_ports[0];
		functions->num_rx_port = 1;

		/* 受信ポート取得 */
		int ret_rx_port = config_get_str_value(obj, JSONPATH_RX_PORT, if_str);
		if (unlikely(ret_rx_port != 0)) {
			RTE_LOG(ERR, APP, "RX port get failed.\n");
			return -1;
		}

		/* IF種別とIF番号に分割 */
		int ret_if = spp_config_get_if_info(if_str, &tmp_rx_port->if_type,
				&tmp_rx_port->if_no);
		if (unlikely(ret_if != 0)) {
			RTE_LOG(ERR, APP,
				"Interface change failed. (port = %s)\n", if_str);
			return -1;
		}
	}

	return 0;
}

/*
 * 送信先ポート情報取得
 */
static int
config_set_tx_port(enum spp_core_type type, json_t *obj,
		struct spp_config_functions *functions,
		struct spp_config_classifier_table *classifier_table)
{
	int cnt = 0;
	struct spp_config_port_info *tmp_tx_port = NULL;
	char if_str[SPP_CONFIG_STR_LEN];
	if ((type == SPP_CONFIG_MERGE) || (type == SPP_CONFIG_FORWARD)) {
		/* Merge or Forward */
		tmp_tx_port = &functions->tx_ports[0];
		functions->num_tx_port = 1;

		/* 送信ポート取得 */
		int ret_tx_port = config_get_str_value(obj,
				JSONPATH_TX_PORT, if_str);
		if (unlikely(ret_tx_port != 0)) {
			RTE_LOG(ERR, APP, "TX port get failed.\n");
			return -1;
		}

		/* IF種別とIF番号に分割 */
		int ret_if = spp_config_get_if_info(if_str, &tmp_tx_port->if_type,
				&tmp_tx_port->if_no);
		if (unlikely(ret_if != 0)) {
			RTE_LOG(ERR, APP,
				"Interface change failed. (port = %s)\n",
				if_str);
			return -1;
		}
	} else {
		/* Classifier */
		json_t *table_obj = spp_config_get_path_obj(obj, JSONPATH_TX_TABLE);
		if (unlikely(table_obj != NULL)) {
			/* Classifier Tableから取得 */
			functions->num_tx_port = classifier_table->num_table;
			struct spp_config_mac_table_element *tmp_mac_table = NULL;
			for (cnt = 0; cnt < classifier_table->num_table; cnt++) {
				tmp_tx_port = &functions->tx_ports[cnt];
				tmp_mac_table = &classifier_table->mac_tables[cnt];

				/* MAC振り分けテーブルより設定 */
				tmp_tx_port->if_type = tmp_mac_table->port.if_type;
				tmp_tx_port->if_no   = tmp_mac_table->port.if_no;
			}
			
		}
		else
		{
			/* tx_portパラメータより取得 */
			/* 送信ポート用オブジェクト取得 */
			json_t *array_obj = spp_config_get_path_obj(obj, JSONPATH_TX_PORT);
			if (unlikely(array_obj == NULL)) {
				RTE_LOG(ERR, APP, "Json object get failed. (path = %s, route = classifier)\n",
					JSONPATH_TX_PORT);
				return -1;
			}

			/* 送信ポート用オブジェクトが配列かチェック */
			if (unlikely(!json_is_array(array_obj))) {
				RTE_LOG(ERR, APP, "Not an array. (path = %s, route = classifier)\n",
					JSONPATH_TX_PORT);
				return -1;
			}

			/* 受信ポート用オブジェクトの要素数取得 */
			int port_num = json_array_size(array_obj);
			if (unlikely(port_num <= 0) ||
					unlikely(port_num > RTE_MAX_ETHPORTS)) {
				RTE_LOG(ERR, APP, "TX port out of range. (path = %s, port = %d, route = classifier)\n",
						JSONPATH_TX_PORT, port_num);
				return -1;
			}
			functions->num_tx_port = port_num;

			/* 要素毎にデータ取得 */
			int array_cnt = 0;
			for (array_cnt = 0; array_cnt < port_num; array_cnt++) {
				tmp_tx_port = &functions->tx_ports[array_cnt];

				/* 要素取得 */
				json_t *elements_obj = json_array_get(array_obj, array_cnt);
				if (unlikely(elements_obj == NULL)) {
					RTE_LOG(ERR, APP,
						"Element get failed. (No = %d, path = %s, route = classifier)\n",
						array_cnt, JSONPATH_TX_PORT);
					return -1;
				}

				/* String type check */
				if (unlikely(!json_is_string(elements_obj))) {
					RTE_LOG(ERR, APP, "Not a string. (path = %s, No = %d, route = classifier)\n",
							JSONPATH_TX_PORT, array_cnt);
					return -1;
				}
				strcpy(if_str, json_string_value(elements_obj));

				/* IF種別とIF番号に分割 */
				int ret_if = spp_config_get_if_info(if_str, &tmp_tx_port->if_type,
						&tmp_tx_port->if_no);
				if (unlikely(ret_if != 0)) {
					RTE_LOG(ERR, APP,
						"Interface change failed. (No = %d, port = %s, route = classifier)\n",
						array_cnt, if_str);
					return -1;
				}
			}
		}
	}

	return 0;
}

/*
 * プロセス情報取得
 */
static int
config_load_proc_info(const json_t *obj, int node_id, struct spp_config_area *config)
{
	struct spp_config_proc_info *proc = &config->proc;
	struct spp_config_classifier_table *classifier_table = &config->classifier_table;

	/* proc_table用オブジェクト取得 */
	json_t *proc_table_obj = spp_config_get_path_obj(obj, JSONPATH_PROC_TABLE);
	if (unlikely(proc_table_obj == NULL)) {
		RTE_LOG(ERR, APP, "Json object get failed. (path = %s)\n",
				JSONPATH_PROC_TABLE);
		return -1;
	}

	/* table用オブジェクトが配列かチェック */
	if (unlikely(!json_is_array(proc_table_obj))) {
		RTE_LOG(ERR, APP, "Not an array. (path = %s)\n",
				JSONPATH_TABLE);
		return -1;
	}

	/* table用オブジェクトの要素数取得 */
	int proc_table_num = json_array_size(proc_table_obj);
	if (unlikely(proc_table_num < node_id)) {
		RTE_LOG(ERR, APP, "No process data. (Size = %d, Node = %d)\n",
			proc_table_num, node_id);
		return -1;
	}

	/* 要素取得 */
	json_t *proc_obj = json_array_get(proc_table_obj, node_id);
	if (unlikely(proc_obj == NULL)) {
		RTE_LOG(ERR, APP, "Process data get failed. (Node = %d)\n",
				node_id);
		return -1;
	}

	/* name取得 */
	int ret_name = config_get_str_value(proc_obj, JSONPATH_NAME, proc->name);
	if (unlikely(ret_name != 0)) {
		RTE_LOG(ERR, APP, "Process name get failed.\n");
		return -1;
	}

	/* VHOST数取得 */
	int ret_vhost = config_get_int_value(proc_obj, JSONPATH_NUM_VHOST,
			&proc->num_vhost);
	if (unlikely(ret_vhost != 0)) {
		RTE_LOG(ERR, APP, "VHOST number get failed.\n");
		return -1;
	}

	/* RING数取得 */
	int ret_ring = config_get_int_value(proc_obj, JSONPATH_NUM_RING,
			&proc->num_ring);
	if (unlikely(ret_ring != 0)) {
		RTE_LOG(ERR, APP, "RING number get failed.\n");
		return -1;
	}

	/* functions用オブジェクト取得 */
	json_t *array_obj = spp_config_get_path_obj(proc_obj, JSONPATH_FUNCTIONS);
	if (unlikely(!array_obj)) {
		RTE_LOG(ERR, APP, "Json object get failed. (path = %s)\n",
				JSONPATH_FUNCTIONS);
		return -1;
	}

	/* functions用オブジェクトが配列かチェック */
	if (unlikely(!json_is_array(array_obj))) {
		RTE_LOG(ERR, APP, "Not an array. (path = %s)\n",
				JSONPATH_FUNCTIONS);
		return -1;
	}

	/* functions用オブジェクトの要素数取得 */
	int array_num = json_array_size(array_obj);
	if (unlikely(array_num <= 0) ||
			unlikely(array_num > SPP_CONFIG_CORE_MAX)) {
		RTE_LOG(ERR, APP, "Functions size out of range. (path = %s, size = %d)\n",
				JSONPATH_TABLE, array_num);
		return -1;
	}
	proc->num_func = array_num;

	/* 要素毎にデータ取得 */
	struct spp_config_functions *tmp_functions = NULL;
	char core_type_str[SPP_CONFIG_STR_LEN];
	int array_cnt = 0;
	for (array_cnt = 0; array_cnt < array_num; array_cnt++) {
		tmp_functions = &proc->functions[array_cnt];

		/* 要素取得 */
		json_t *elements_obj = json_array_get(array_obj, array_cnt);
		if (unlikely(elements_obj == NULL)) {
			RTE_LOG(ERR, APP,
				"Element get failed. (No = %d, path = %s)\n",
				array_cnt, JSONPATH_FUNCTIONS);
			return -1;
		}

		/* CORE番号取得 */
		int ret_core = config_get_int_value(elements_obj, JSONPATH_CORE_NO,
				&tmp_functions->core_no);
		if (unlikely(ret_core != 0)) {
			RTE_LOG(ERR, APP, "Core number get failed. (No = %d)\n",
					array_cnt);
			return -1;
		}

		/* 処理種別取得 */
		int ret_core_type = config_get_str_value(elements_obj,
				 JSONPATH_CORE_TYPE, core_type_str);
		if (unlikely(ret_core_type != 0)) {
			RTE_LOG(ERR, APP, "Core type get failed. (No = %d)\n",
					array_cnt);
			return -1;
		}

		/* 処理種別を数値に変換 */
		enum spp_core_type core_type = config_change_core_type(core_type_str);
		if (unlikely(core_type == SPP_CONFIG_UNUSE)) {
			RTE_LOG(ERR, APP,
				"Unknown core type. (No = %d, type = %s)\n",
				array_cnt, core_type_str);
			return -1;
		}
		tmp_functions->type = core_type;

		/* 受信ポート取得 */
		int ret_rx_port = config_set_rx_port(core_type, elements_obj,
				tmp_functions);
		if (unlikely(ret_rx_port != 0)) {
			RTE_LOG(ERR, APP, "RX port set failure. (No = %d)\n",
					array_cnt);
			return -1;
		}

		/* 送信ポート取得 */
		int ret_tx_port = config_set_tx_port(core_type, elements_obj,
				tmp_functions, classifier_table);
		if (unlikely(ret_tx_port != 0)) {
			RTE_LOG(ERR, APP, "TX port set failure. (No = %d)\n",
					array_cnt);
			return -1;
		}
	}

	return 0;
}

/*
 * Load config file
 * OK : 0
 * NG : -1
 */
int
spp_config_load_file(const char* config_file_path, int node_id, struct spp_config_area *config)
{
	/* Config initialize */
	config_init_data(config);
	
	/* Config load */
	json_error_t json_error;
	json_t *conf_obj = json_load_file(config_file_path, 0, &json_error);
	if (unlikely(conf_obj == NULL)) {
		/* Load error */
		RTE_LOG(ERR, APP, "Config load failed. (path = %s, text = %s)\n",
				 config_file_path, json_error.text);
		return -1;
	}

	/* classifier table */
	int ret_classifier = config_load_classifier_table(conf_obj,
			&config->classifier_table);
	if (unlikely(ret_classifier != 0)) {
		RTE_LOG(ERR, APP, "Classifier table load failed.\n");
		json_decref(conf_obj);
		return -1;
	}

	/* proc info */
	int ret_proc = config_load_proc_info(conf_obj, node_id, config);
	if (unlikely(ret_proc != 0)) {
		RTE_LOG(ERR, APP, "Process table load failed.\n");
		json_decref(conf_obj);
		return -1;
	}

	/* Config object release */
	json_decref(conf_obj);

	return 0;
}
