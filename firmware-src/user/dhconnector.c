/*
 * dhconnector.cpp
 *
 * Copyright 2015 DeviceHive
 *
 * Author: Nikolay Khabarov
 *
 * Description: Module for connecting to remote DeviceHive server
 *
 */

#include <ets_sys.h>
#include <osapi.h>
#include <os_type.h>
#include <gpio.h>
#include <user_interface.h>
#include <espconn.h>
#include <json/jsonparse.h>
#include "dhrequest.h"
#include "dhconnector.h"
#include "dhsender.h"
#include "dhdebug.h"
#include "user_config.h"
#include "snprintf.h"

CONNECTION_STATE mConnectionState;
struct espconn mDHConnector;
dhconnector_command_json_cb mCommandCallback;
HTTP_REQUEST *mRegisterRequest;
HTTP_REQUEST *mPollRequest;
HTTP_REQUEST *mInfoRequest;
os_timer_t mRetryTimer;

LOCAL void ICACHE_FLASH_ATTR set_state(CONNECTION_STATE state);

LOCAL void ICACHE_FLASH_ATTR retry(void *arg) {
	set_state(mConnectionState);
}

LOCAL void ICACHE_FLASH_ATTR arm_repeat_timer() {
	dhdebug("Retrying in %d ms...", RETRY_CONNECTION_INTERVAL_MS);
	os_timer_disarm(&mRetryTimer);
	os_timer_setfn(&mRetryTimer, (os_timer_func_t *)retry, NULL);
	os_timer_arm(&mRetryTimer, RETRY_CONNECTION_INTERVAL_MS, 0);
}

LOCAL void ICACHE_FLASH_ATTR network_error_cb(void *arg, sint8 err) {
	dhesperrors_espconn_result("Network error occurred:", err);
	mConnectionState = CS_DISCONNECT;
	arm_repeat_timer();
}

LOCAL void ICACHE_FLASH_ATTR parse_json(struct jsonparse_state *jparser) {
	int type;
	int id;
	char command[128] = "";
	const char *params;
	int paramslen = 0;
	char timestamp[128] = "";
	while ((type = jsonparse_next(jparser)) != JSON_TYPE_ERROR) {
		if (type == JSON_TYPE_PAIR_NAME) {
			if (jsonparse_strcmp_value(jparser, "serverTimestamp") == 0) {
				jsonparse_next(jparser);
				if (jsonparse_next(jparser) != JSON_TYPE_ERROR) {
					jsonparse_copy_value(jparser, timestamp, sizeof(timestamp));
					dhdebug("Timestamp received %s", timestamp);
					dhrequest_free(mInfoRequest);
					mInfoRequest = NULL;
					mPollRequest = dhrequest_create_poll(timestamp);
				}
				break;
			} else if (jsonparse_strcmp_value(jparser, "command") == 0) {
				jsonparse_next(jparser);
				if(jsonparse_next(jparser) != JSON_TYPE_ERROR)
					jsonparse_copy_value(jparser, command, sizeof(command));
			} else if (jsonparse_strcmp_value(jparser, "id") == 0) {
				jsonparse_next(jparser);
				if(jsonparse_next(jparser) != JSON_TYPE_ERROR)
					id = jsonparse_get_value_as_int(jparser);
			} else if (jsonparse_strcmp_value(jparser, "parameters") == 0) {
				jsonparse_next(jparser);
				if(jsonparse_next(jparser) != JSON_TYPE_ERROR) {
					// there is an issue with extracting subjson with jparser->vstart or jparser_copy_value
					params = &jparser->json[jparser->pos - 1];
					if(*params == '{') {
						int end = jparser->pos;
						while(end < jparser->len && jparser->json[end] != '}') {
							end++;
						}
						paramslen = end - jparser->pos + 2;
					}
				}
			} else if (jsonparse_strcmp_value(jparser, "timestamp") == 0) {
				jsonparse_next(jparser);
				if(jsonparse_next(jparser) != JSON_TYPE_ERROR)
					jsonparse_copy_value(jparser, timestamp, sizeof(timestamp));
			}
		}
	}
	if (mConnectionState == CS_POLL) {
		if(timestamp[0]) {
			dhdebug("Timestamp received %s", timestamp);
			mPollRequest = dhrequest_update_poll(mPollRequest, timestamp);
		}
		mCommandCallback(id, command, params, paramslen);
	}
}

LOCAL void ICACHE_FLASH_ATTR network_recv_cb(void *arg, char *data, unsigned short len) {
	const char *rc = dhrequest_find_http_responce_code(data, len);
	if (rc) { // HTTP
		if (*rc == '2') { // HTTP responce code 2xx - Success
			if (mConnectionState == CS_REGISTER) {
				dhdebug("Successfully register");
			} else {
				char *content = (char *) os_strstr(data, (char *) "\r\n\r\n");
				if (content) {
					int deep = 0;
					unsigned int pos = 0;
					unsigned int jsonstart = 0;
					while (pos < len) {
						if (data[pos] == '{') {
							if (deep == 0)
								jsonstart = pos;
							deep++;
						} else if (data[pos] == '}') {
							deep--;
							if (deep == 0) {
								struct jsonparse_state jparser;
								jsonparse_setup(&jparser, &data[jsonstart],
										pos - jsonstart);
								parse_json(&jparser);
							}
						}
						pos++;
					}
				}
			}
		} else {
			mConnectionState = CS_DISCONNECT;
			dhdebug("Connector HTTP response bad status %c%c%c", rc[0],rc[1],rc[2]);
		}
	} else {
		mConnectionState = CS_DISCONNECT;
		dhdebug("Connector HTTP magic number is wrong");
	}
	espconn_disconnect(&mDHConnector);
}

LOCAL void ICACHE_FLASH_ATTR network_connect_cb(void *arg) {
	HTTP_REQUEST *request;
	switch(mConnectionState) {
	case CS_GETINFO:
		request = mInfoRequest;
		dhdebug("Send info request...");
		break;
	case CS_REGISTER:
		request = mRegisterRequest;
		dhdebug("Send register request...");
		break;
	case CS_POLL:
		request = mPollRequest;
		dhdebug("Send poll request...");
		break;
	default:
		dhdebug("ASSERT: networkConnectCb wrong state %d", mConnectionState);
	}
	int res;
	if( (res = espconn_sent(&mDHConnector, request->body, request->len)) != ESPCONN_OK) {
		mConnectionState = CS_DISCONNECT;
		dhesperrors_espconn_result("network_connect_cb failed:", res);
		espconn_disconnect(&mDHConnector);
	}
}

LOCAL void ICACHE_FLASH_ATTR network_disconnect_cb(void *arg) {
	dhdebug("Connector disconnected");
	switch(mConnectionState) {
	case CS_DISCONNECT:
		arm_repeat_timer();
		break;
	case CS_GETINFO:
		set_state(CS_REGISTER);
		break;
	case CS_REGISTER:
	case CS_POLL:
		set_state(CS_POLL);
		break;
	default:
		dhdebug("ASSERT: networkDisconnectCb wrong state %d", mConnectionState);
	}
}

LOCAL void ICACHE_FLASH_ATTR dhconnector_init_connection(ip_addr_t *ip) {
	os_memcpy(mDHConnector.proto.tcp->remote_ip, &ip->addr, sizeof(ip->addr));
	espconn_regist_connectcb(&mDHConnector, network_connect_cb);
	espconn_regist_recvcb(&mDHConnector, network_recv_cb);
	espconn_regist_reconcb(&mDHConnector, network_error_cb);
	espconn_regist_disconcb(&mDHConnector, network_disconnect_cb);
}

LOCAL void ICACHE_FLASH_ATTR resolve_cb(const char *name, ip_addr_t *ip, void *arg) {
	if (ip == NULL) {
		dhdebug("Resolve %s failed. Trying again...", name);
		mConnectionState = CS_DISCONNECT;
		arm_repeat_timer();
		return;
	}
	unsigned char *bip = (unsigned char *) ip;
	dhdebug("Host %s ip: %d.%d.%d.%d, using port %d", name, bip[0], bip[1], bip[2], bip[3], mDHConnector.proto.tcp->remote_port);

	dhsender_init(ip, mDHConnector.proto.tcp->remote_port);
	dhconnector_init_connection(ip);
	if(mInfoRequest)
		set_state(CS_GETINFO);
	else
		set_state(CS_REGISTER);
}

LOCAL void ICACHE_FLASH_ATTR start_resolve_dh_server() {
	static ip_addr_t ip;
	char host[os_strlen(dhrequest_current_server()) + 1];
	char *fr = os_strchr(dhrequest_current_server(), ':');
	if(fr) {
		fr++;
		if(*fr != '/')
			fr = 0;
	}
	if (fr) {
		while (*fr == '/')
			fr++;
		int i = 0;
		while (*fr != '/' && *fr != ':' && *fr != 0)
			host[i++] = *fr++;
		// read port if present
		int port = 0;
		if(*fr == ':') {
			unsigned char d;
			fr++;
			while ( (d = *fr - 0x30) < 10) {
				fr++;
				port = port*10 + d;
				if(port > 0xFFFF)
					break;
			}
		}
		if(port && port < 0xFFFF)
			mDHConnector.proto.tcp->remote_port = port;
		else if (os_strncmp(dhrequest_current_server(), "https", 5) == 0)
			mDHConnector.proto.tcp->remote_port = 443; // HTTPS default port
		else
			mDHConnector.proto.tcp->remote_port = 80; //HTTP default port
		host[i] = 0;
		dhdebug("Resolving %s", host);
		err_t r = espconn_gethostbyname(&mDHConnector, host, &ip, resolve_cb);
		if(r == ESPCONN_OK) {
			resolve_cb(host, &ip, &mDHConnector);
		} else if(r != ESPCONN_INPROGRESS) {
			dhesperrors_espconn_result("Resolving failed:", r);
			arm_repeat_timer();
		}
	} else {
		dhdebug("Can not find scheme in server url");
	}
}

LOCAL void ICACHE_FLASH_ATTR set_state(CONNECTION_STATE state) {
	mConnectionState = state;
	switch(state) {
	case CS_DISCONNECT:
		start_resolve_dh_server();
		break;
	case CS_GETINFO:
	case CS_REGISTER:
	case CS_POLL:
	{
		const sint8 cr = espconn_connect(&mDHConnector);
		if(cr == ESPCONN_ISCONN)
			return;
		if(cr != ESPCONN_OK) {
			dhesperrors_espconn_result("Connector espconn_connect failed:", cr);
			arm_repeat_timer();
		}
		break;
	}
	default:
		dhdebug("ASSERT: set_state wrong state %d", mConnectionState);
	}
}

LOCAL void wifi_state_cb(System_Event_t *event) {
	if(event->event == EVENT_STAMODE_GOT_IP) {
		if(event->event_info.got_ip.ip.addr != 0) {
			const unsigned char * const bip = (unsigned char *)&event->event_info.got_ip.ip;
			dhdebug("WiFi connected, ip: %d.%d.%d.%d", bip[0], bip[1], bip[2], bip[3]);
			set_state(CS_DISCONNECT);
		} else {
			dhdebug("ERROR: WiFi reports STAMODE_GOT_IP, but no actual ip found");
		}
	} else if(event->event == EVENT_STAMODE_DISCONNECTED) {
		os_timer_disarm(&mRetryTimer);
		dhesperrors_disconnect_reason("WiFi disconnected", event->event_info.disconnected.reason);
	} else {
		dhesperrors_wifi_state("WiFi event", event->event);
	}
}

void ICACHE_FLASH_ATTR dhconnector_init(dhconnector_command_json_cb cb) {
	dhrequest_load_settings();
	mCommandCallback = cb;
	mRegisterRequest = dhrequest_create_register();
	mPollRequest = NULL;
	mInfoRequest = dhrequest_create_info();
	mConnectionState = CS_DISCONNECT;

	wifi_set_opmode(STATION_MODE);
	wifi_station_set_auto_connect(1);
	wifi_station_set_reconnect_policy(true);
	struct station_config stationConfig;
	wifi_station_get_config(&stationConfig);
	os_memset(stationConfig.ssid, 0, sizeof(stationConfig.ssid));
	os_memset(stationConfig.password, 0, sizeof(stationConfig.password));
	snprintf(stationConfig.ssid, sizeof(stationConfig.ssid), "%s", dhsettings_get_wifi_ssid());
	snprintf(stationConfig.password, sizeof(stationConfig.password), "%s", dhsettings_get_wifi_password());
	wifi_station_set_config(&stationConfig);

	static esp_tcp tcp;
	os_memset(&tcp, 0, sizeof(tcp));
	os_memset(&mDHConnector, 0, sizeof(mDHConnector));
	mDHConnector.type = ESPCONN_TCP;
	mDHConnector.state = ESPCONN_NONE;
	mDHConnector.proto.tcp = &tcp;
	mDHConnector.proto.tcp->local_port = espconn_port();

	wifi_set_event_handler_cb(wifi_state_cb);
}

CONNECTION_STATE dhconnector_get_state() {
	return mConnectionState;
}
