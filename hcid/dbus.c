/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2004-2006  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <syslog.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

#include <dbus/dbus.h>

#include "glib-ectomy.h"

#include "hcid.h"
#include "dbus.h"
#include "textfile.h"

#ifndef DBUS_NAME_FLAG_PROHIBIT_REPLACEMENT
#define DBUS_NAME_FLAG_PROHIBIT_REPLACEMENT	0x00
#endif

static DBusConnection *connection;
static int default_dev = -1;

#define TIMEOUT				(30 * 1000)		/* 30 seconds */
#define DBUS_RECONNECT_TIMER		(5 * 1000 * 1000)	/* 5 sec */
#define MAX_PATH_LENGTH			64
#define MAX_CONN_NUMBER			10

#define PINAGENT_SERVICE_NAME BASE_INTERFACE ".PinAgent"
#define PINAGENT_INTERFACE PINAGENT_SERVICE_NAME
#define PIN_REQUEST "PinRequest"
#define PINAGENT_PATH BASE_PATH "/PinAgent"

struct pin_request {
	int dev;
	bdaddr_t bda;
};

typedef DBusMessage* (service_handler_func_t) (DBusMessage *, void *);

struct service_data {
	const char		*name;
	service_handler_func_t	*handler_func;
	const char		*signature;
};

struct hci_dbus_data {
	uint16_t dev_id;
	uint16_t path_id;
	uint32_t path_data;
};

typedef int register_function_t(DBusConnection *conn, uint16_t id);
typedef int unregister_function_t(DBusConnection *conn, uint16_t id);

/*
 * Utility functions
 */
static char *get_device_name(const bdaddr_t *local, const bdaddr_t *peer)
{
	char filename[PATH_MAX + 1], addr[18];

	ba2str(local, addr);
	snprintf(filename, PATH_MAX, "%s/%s/names", STORAGEDIR, addr);

	ba2str(peer, addr);
	return textfile_get(filename, addr);
}

static int8_t dev_append_signal_args(DBusMessage *signal, int first, va_list var_args)
{
	void *value;
	DBusMessageIter iter;
	int type;
	int8_t retval = 0;

	type = first;

	dbus_message_iter_init_append (signal, &iter);

	while (type != DBUS_TYPE_INVALID)
	{
		value = va_arg (var_args, void*);

		if (!dbus_message_iter_append_basic (&iter, type, value)) {
			syslog(LOG_INFO, "Append property argument error! type:%d", type);
			retval = -1;
			goto failed;
		}
		type = va_arg (var_args, int);
	}
failed:
	return retval;
}

static DBusMessage *dev_signal_factory(const int devid, const char *prop_name, const int first, ...)
{
	DBusMessage *signal = NULL;
	char path[MAX_PATH_LENGTH];
	va_list var_args;

	snprintf(path, sizeof(path)-1, "%s/hci%d", DEVICE_PATH, devid);

	signal = dbus_message_new_signal(path, DEVICE_INTERFACE,
					 prop_name);
	if (signal == NULL) {
		syslog(LOG_ERR, "Can't allocate D-BUS inquiry complete message");
		return NULL;
	}

	va_start(var_args, first);
	if (dev_append_signal_args(signal, first, var_args) < 0) {
		dbus_message_unref(signal);
		return NULL;
	}

	va_end(var_args);
	return signal;
}

/*
 * D-Bus error messages functions and declarations.
 * This section should be moved to a common file 
 * in the future
 *
 */

typedef struct  {
	uint32_t code;
	const char *str;
} bluez_error_t;

typedef struct {
	char *str;
	unsigned int val;
} hci_map;

static hci_map dev_flags_map[] = {
	{ "INIT",	HCI_INIT	},
	{ "RUNNING",	HCI_RUNNING	},
	{ "RAW",	HCI_RAW		},
	{ "PSCAN",	HCI_PSCAN	},
	{ "ISCAN",	HCI_ISCAN	},
	{ "INQUIRY",	HCI_INQUIRY	},
	{ "AUTH",	HCI_AUTH	},
	{ "ENCRYPT",	HCI_ENCRYPT	},
	{ "SECMGR",	HCI_SECMGR	},
	{ NULL }
};

static const bluez_error_t dbus_error_array[] = {
	{ BLUEZ_EDBUS_UNKNOWN_METHOD,	"Method not found"		},
	{ BLUEZ_EDBUS_WRONG_SIGNATURE,	"Wrong method signature"	},
	{ BLUEZ_EDBUS_WRONG_PARAM,	"Invalid parameters"		},
	{ BLUEZ_EDBUS_RECORD_NOT_FOUND,	"No record found"		},
	{ BLUEZ_EDBUS_NO_MEM,		"No memory"			},
	{ BLUEZ_EDBUS_CONN_NOT_FOUND,	"Connection not found"		},
	{ BLUEZ_EDBUS_UNKNOWN_PATH,	"Unknown D-BUS path"		},
	{ BLUEZ_EDBUS_NOT_IMPLEMENTED,	"Method not implemented"	},
	{ 0, NULL }
};

static const bluez_error_t hci_error_array[] = {
	{ HCI_UNKNOWN_COMMAND,			"Unknown HCI Command"						},
	{ HCI_NO_CONNECTION,			"Unknown Connection Identifier"					},
	{ HCI_HARDWARE_FAILURE,			"Hardware Failure"						},
	{ HCI_PAGE_TIMEOUT,			"Page Timeout"							},
	{ HCI_AUTHENTICATION_FAILURE,		"Authentication Failure"					},
	{ HCI_PIN_OR_KEY_MISSING,		"PIN Missing"							},
	{ HCI_MEMORY_FULL,			"Memory Capacity Exceeded"					},
	{ HCI_CONNECTION_TIMEOUT,		"Connection Timeout"						},
	{ HCI_MAX_NUMBER_OF_CONNECTIONS,	"Connection Limit Exceeded"					},
	{ HCI_MAX_NUMBER_OF_SCO_CONNECTIONS,	"Synchronous Connection Limit To A Device Exceeded"		},
	{ HCI_ACL_CONNECTION_EXISTS,		"ACL Connection Already Exists"					},
	{ HCI_COMMAND_DISALLOWED,		"Command Disallowed"						},
	{ HCI_REJECTED_LIMITED_RESOURCES,	"Connection Rejected due to Limited Resources"			},
	{ HCI_REJECTED_SECURITY,		"Connection Rejected Due To Security Reasons"			},
	{ HCI_REJECTED_PERSONAL,		"Connection Rejected due to Unacceptable BD_ADDR"		},
	{ HCI_HOST_TIMEOUT,			"Connection Accept Timeout Exceeded"				},
	{ HCI_UNSUPPORTED_FEATURE,		"Unsupported Feature or Parameter Value"			},
	{ HCI_INVALID_PARAMETERS,		"Invalid HCI Command Parameters"				},
	{ HCI_OE_USER_ENDED_CONNECTION,		"Remote User Terminated Connection"				},
	{ HCI_OE_LOW_RESOURCES,			"Remote Device Terminated Connection due to Low Resources"	},
	{ HCI_OE_POWER_OFF,			"Remote Device Terminated Connection due to Power Off"		},
	{ HCI_CONNECTION_TERMINATED,		"Connection Terminated By Local Host"				},
	{ HCI_REPEATED_ATTEMPTS,		"Repeated Attempts"						},
	{ HCI_PAIRING_NOT_ALLOWED,		"Pairing Not Allowed"						},
	{ HCI_UNKNOWN_LMP_PDU,			"Unknown LMP PDU"						},
	{ HCI_UNSUPPORTED_REMOTE_FEATURE,	"Unsupported Remote Feature"					},
	{ HCI_SCO_OFFSET_REJECTED,		"SCO Offset Rejected"						},
	{ HCI_SCO_INTERVAL_REJECTED,		"SCO Interval Rejected"						},
	{ HCI_AIR_MODE_REJECTED,		"SCO Air Mode Rejected"						},
	{ HCI_INVALID_LMP_PARAMETERS,		"Invalid LMP Parameters"					},
	{ HCI_UNSPECIFIED_ERROR,		"Unspecified Error"						},
	{ HCI_UNSUPPORTED_LMP_PARAMETER_VALUE,	"Unsupported LMP Parameter Value"				},
	{ HCI_ROLE_CHANGE_NOT_ALLOWED,		"Role Change Not Allowed"					},
	{ HCI_LMP_RESPONSE_TIMEOUT,		"LMP Response Timeout"						},
	{ HCI_LMP_ERROR_TRANSACTION_COLLISION,	"LMP Error Transaction Collision"				},
	{ HCI_LMP_PDU_NOT_ALLOWED,		"LMP PDU Not Allowed"						},
	{ HCI_ENCRYPTION_MODE_NOT_ACCEPTED,	"Encryption Mode Not Acceptable"				},
	{ HCI_UNIT_LINK_KEY_USED,		"Link Key Can Not be Changed"					},
	{ HCI_QOS_NOT_SUPPORTED,		"Requested QoS Not Supported"					},
	{ HCI_INSTANT_PASSED,			"Instant Passed"						},
	{ HCI_PAIRING_NOT_SUPPORTED,		"Pairing With Unit Key Not Supported"				},
	{ HCI_TRANSACTION_COLLISION,		"Different Transaction Collision"				},
	{ HCI_QOS_UNACCEPTABLE_PARAMETER,	"QoS Unacceptable Parameter"					},
	{ HCI_QOS_REJECTED,			"QoS Rejected"							},
	{ HCI_CLASSIFICATION_NOT_SUPPORTED,	"Channel Classification Not Supported"				},
	{ HCI_INSUFFICIENT_SECURITY,		"Insufficient Security"						},
	{ HCI_PARAMETER_OUT_OF_RANGE,		"Parameter Out Of Mandatory Range"				},
	{ HCI_ROLE_SWITCH_PENDING,		"Role Switch Pending"						},
	{ HCI_SLOT_VIOLATION,			"Reserved Slot Violation"					},
	{ HCI_ROLE_SWITCH_FAILED,		"Role Switch Failed"						},
	{ 0, NULL },
};

static const char *bluez_dbus_error_to_str(const uint32_t ecode) 
{
	const bluez_error_t *ptr;
	uint32_t raw_code = 0;

	if (ecode & BLUEZ_ESYSTEM_OFFSET) {
		/* System error */
		raw_code = (~BLUEZ_ESYSTEM_OFFSET) & ecode;
		syslog(LOG_INFO, "%s - msg:%s", __PRETTY_FUNCTION__, strerror(raw_code));
		return strerror(raw_code);
	} else if (ecode & BLUEZ_EDBUS_OFFSET) {
		/* D-Bus error */
		for (ptr = dbus_error_array; ptr->code; ptr++) {
			if (ptr->code == ecode) {
				syslog(LOG_INFO, "%s - msg:%s", __PRETTY_FUNCTION__, ptr->str);
				return ptr->str;
			}
		}
	} else {
		/* BLUEZ_EBT_OFFSET - Bluetooth HCI errors */
		for (ptr = hci_error_array; ptr->code; ptr++) {
			if (ptr->code == ecode) {
				syslog(LOG_INFO, "%s - msg:%s", __PRETTY_FUNCTION__, ptr->str);
				return ptr->str;
			}
		}
	}

	return NULL;
}

static DBusMessage *bluez_new_failure_msg(DBusMessage *msg, const uint32_t ecode)
{
	DBusMessageIter iter;
	DBusMessage *reply;
	const char *error_msg;

	error_msg = bluez_dbus_error_to_str(ecode);
	if (!error_msg)
		return NULL;

	reply = dbus_message_new_error(msg, ERROR_INTERFACE, error_msg);

	dbus_message_iter_init_append(reply, &iter);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT32 ,&ecode);

	return reply;
}


/*
 * Virtual table that handle the object path hierarchy
 */
static DBusHandlerResult msg_func_device(DBusConnection *conn, DBusMessage *msg, void *data);
static DBusHandlerResult msg_func_manager(DBusConnection *conn, DBusMessage *msg, void *data);

static const DBusObjectPathVTable obj_dev_vtable = {
	.message_function	= &msg_func_device,
	.unregister_function	= NULL
};

static const DBusObjectPathVTable obj_mgr_vtable = {
	.message_function	= &msg_func_manager,
	.unregister_function	= NULL
};

/*
 * Services provided under the path DEVICE_PATH
 */

static DBusMessage* handle_dev_get_address_req(DBusMessage *msg, void *data);
static DBusMessage* handle_dev_get_alias_req(DBusMessage *msg, void *data);
static DBusMessage* handle_dev_get_company_req(DBusMessage *msg, void *data);
static DBusMessage* handle_dev_get_discoverable_to_req(DBusMessage *msg, void *data);
static DBusMessage* handle_dev_get_features_req(DBusMessage *msg, void *data);
static DBusMessage* handle_dev_get_manufacturer_req(DBusMessage *msg, void *data);
static DBusMessage* handle_dev_get_mode_req(DBusMessage *msg, void *data);
static DBusMessage* handle_dev_get_name_req(DBusMessage *msg, void *data);

static DBusMessage* handle_dev_get_revision_req(DBusMessage *msg, void *data);
static DBusMessage* handle_dev_get_version_req(DBusMessage *msg, void *data);

static DBusMessage* handle_dev_is_connectable_req(DBusMessage *msg, void *data);
static DBusMessage* handle_dev_is_discoverable_req(DBusMessage *msg, void *data);

static DBusMessage* handle_dev_set_alias_req(DBusMessage *msg, void *data);
static DBusMessage* handle_dev_set_class_req(DBusMessage *msg, void *data);
static DBusMessage* handle_dev_set_discoverable_to_req(DBusMessage *msg, void *data);
static DBusMessage* handle_dev_set_mode_req(DBusMessage *msg, void *data);
static DBusMessage* handle_dev_set_name_req(DBusMessage *msg, void *data);

static DBusMessage* handle_dev_discover_req(DBusMessage *msg, void *data);
static DBusMessage* handle_dev_discover_cache_req(DBusMessage *msg, void *data);
static DBusMessage* handle_dev_discover_cancel_req(DBusMessage *msg, void *data);
static DBusMessage* handle_dev_discover_service_req(DBusMessage *msg, void *data);

static DBusMessage* handle_dev_last_seen_req(DBusMessage *msg, void *data);
static DBusMessage* handle_dev_last_used_req(DBusMessage *msg, void *data);

static DBusMessage* handle_dev_remote_alias_req(DBusMessage *msg, void *data);
static DBusMessage* handle_dev_remote_name_req(DBusMessage *msg, void *data);
static DBusMessage* handle_dev_remote_version_req(DBusMessage *msg, void *data);

static DBusMessage* handle_dev_create_bonding_req(DBusMessage *msg, void *data);
static DBusMessage* handle_dev_list_bondings_req(DBusMessage *msg, void *data);
static DBusMessage* handle_dev_has_bonding_req(DBusMessage *msg, void *data);
static DBusMessage* handle_dev_remove_bonding_req(DBusMessage *msg, void *data);

static DBusMessage* handle_dev_pin_code_length_req(DBusMessage *msg, void *data);
static DBusMessage* handle_dev_encryption_key_size_req(DBusMessage *msg, void *data);


static const struct service_data dev_services[] = {
	{ DEV_GET_ADDRESS,		handle_dev_get_address_req,		DEV_GET_ADDRESS_SIGNATURE		},
	{ DEV_GET_ALIAS,		handle_dev_get_alias_req,		DEV_GET_ALIAS_SIGNATURE			},
	{ DEV_GET_COMPANY,		handle_dev_get_company_req,		DEV_GET_COMPANY_SIGNATURE		},
	{ DEV_GET_DISCOVERABLE_TO,	handle_dev_get_discoverable_to_req,	DEV_GET_DISCOVERABLE_TO_SIGNATURE	},
	{ DEV_GET_FEATURES,		handle_dev_get_features_req,		DEV_GET_FEATURES_SIGNATURE		},
	{ DEV_GET_MANUFACTURER,		handle_dev_get_manufacturer_req,	DEV_GET_MANUFACTURER_SIGNATURE		},
	{ DEV_GET_MODE,			handle_dev_get_mode_req,		DEV_GET_MODE_SIGNATURE			},
	{ DEV_GET_NAME,                 handle_dev_get_name_req,                DEV_GET_NAME_SIGNATURE                  },
	{ DEV_GET_REVISION,		handle_dev_get_revision_req,		DEV_GET_REVISION_SIGNATURE		},
	{ DEV_GET_VERSION,		handle_dev_get_version_req,		DEV_GET_VERSION_SIGNATURE		},

	{ DEV_IS_CONNECTABLE,		handle_dev_is_connectable_req,		DEV_IS_CONNECTABLE_SIGNATURE		},
	{ DEV_IS_DISCOVERABLE,		handle_dev_is_discoverable_req,		DEV_IS_DISCOVERABLE_SIGNATURE		},

	{ DEV_SET_ALIAS,		handle_dev_set_alias_req,		DEV_SET_ALIAS_SIGNATURE			},
	{ DEV_SET_CLASS,		handle_dev_set_class_req,		DEV_SET_CLASS_SIGNATURE			},
	{ DEV_SET_DISCOVERABLE_TO,	handle_dev_set_discoverable_to_req,	DEV_SET_DISCOVERABLE_TO_SIGNATURE	},
	{ DEV_SET_MODE,			handle_dev_set_mode_req,		DEV_SET_MODE_SIGNATURE			},
	{ DEV_SET_NAME,			handle_dev_set_name_req,		DEV_SET_NAME_SIGNATURE			},

	{ DEV_DISCOVER,			handle_dev_discover_req,		DEV_DISCOVER_SIGNATURE			},
	{ DEV_DISCOVER_CACHE,		handle_dev_discover_cache_req,		DEV_DISCOVER_CACHE_SIGNATURE		},
	{ DEV_DISCOVER_CANCEL,		handle_dev_discover_cancel_req,		DEV_DISCOVER_CANCEL_SIGNATURE		},
	{ DEV_DISCOVER_SERVICE,		handle_dev_discover_service_req,	DEV_DISCOVER_SERVICE_SIGNATURE		},

	{ DEV_LAST_SEEN,		handle_dev_last_seen_req,		DEV_LAST_SEEN_SIGNATURE			},
	{ DEV_LAST_USED,		handle_dev_last_used_req,		DEV_LAST_USED_SIGNATURE			},

	{ DEV_REMOTE_ALIAS,		handle_dev_remote_alias_req,		DEV_REMOTE_ALIAS_SIGNATURE		},
	{ DEV_REMOTE_NAME,		handle_dev_remote_name_req,		DEV_REMOTE_NAME_SIGNATURE		},
	{ DEV_REMOTE_VERSION,		handle_dev_remote_version_req,		DEV_REMOTE_VERSION_SIGNATURE		},

	{ DEV_CREATE_BONDING,		handle_dev_create_bonding_req,		DEV_CREATE_BONDING_SIGNATURE		},
	{ DEV_LIST_BONDINGS,		handle_dev_list_bondings_req,		DEV_LIST_BONDINGS_SIGNATURE		},
	{ DEV_HAS_BONDING_NAME,		handle_dev_has_bonding_req,		DEV_HAS_BONDING_SIGNATURE		},
	{ DEV_REMOVE_BONDING,		handle_dev_remove_bonding_req,		DEV_REMOVE_BONDING_SIGNATURE		},

	{ DEV_PIN_CODE_LENGTH,		handle_dev_pin_code_length_req,		DEV_PIN_CODE_LENGTH_SIGNATURE		},
	{ DEV_ENCRYPTION_KEY_SIZE,	handle_dev_encryption_key_size_req,	DEV_ENCRYPTION_KEY_SIZE_SIGNATURE	},

	{ NULL, NULL, NULL}
};

/*
 * Services provided under the path MANAGER_PATH
 */
static DBusMessage* handle_mgr_device_list_req(DBusMessage *msg, void *data);
static DBusMessage* handle_mgr_default_device_req(DBusMessage *msg, void *data);

static const struct service_data mgr_services[] = {
	{ MGR_DEVICE_LIST,	handle_mgr_device_list_req,		MGR_DEVICE_LIST_SIGNATURE	},
	{ MGR_DEFAULT_DEVICE,	handle_mgr_default_device_req,		MGR_DEFAULT_DEVICE_SIGNATURE	},
	{ NULL, NULL, NULL }
};

/*
 * HCI D-Bus services
 */
static DBusHandlerResult hci_dbus_signal_filter(DBusConnection *conn, DBusMessage *msg, void *data);

static void reply_handler_function(DBusPendingCall *call, void *user_data)
{
	struct pin_request *req = (struct pin_request *) user_data;
	pin_code_reply_cp pr;
	DBusMessage *message;
	DBusMessageIter iter;
	int arg_type;
	int msg_type;
	size_t len;
	char *pin;
	const char *error_msg;

	message = dbus_pending_call_steal_reply(call);

	if (!message)
		goto done;

	msg_type = dbus_message_get_type(message);
	dbus_message_iter_init(message, &iter);

	if (msg_type == DBUS_MESSAGE_TYPE_ERROR) {
		dbus_message_iter_get_basic(&iter, &error_msg);

		/* handling WRONG_ARGS_ERROR, DBUS_ERROR_NO_REPLY, DBUS_ERROR_SERVICE_UNKNOWN */
		syslog(LOG_ERR, "%s: %s", dbus_message_get_error_name(message), error_msg);
		hci_send_cmd(req->dev, OGF_LINK_CTL,
					OCF_PIN_CODE_NEG_REPLY, 6, &req->bda);

		goto done;
	}

	/* check signature */
	arg_type = dbus_message_iter_get_arg_type(&iter);
	if (arg_type != DBUS_TYPE_STRING) {
		syslog(LOG_ERR, "Wrong reply signature: expected PIN");
		hci_send_cmd(req->dev, OGF_LINK_CTL,
					OCF_PIN_CODE_NEG_REPLY, 6, &req->bda);
	} else {
		dbus_message_iter_get_basic(&iter, &pin);
		len = strlen(pin);

		memset(&pr, 0, sizeof(pr));
		bacpy(&pr.bdaddr, &req->bda);
		memcpy(pr.pin_code, pin, len);
		pr.pin_len = len;
		hci_send_cmd(req->dev, OGF_LINK_CTL,
			OCF_PIN_CODE_REPLY, PIN_CODE_REPLY_CP_SIZE, &pr);
	}

done:
	if (message)
		dbus_message_unref(message);

	dbus_pending_call_unref(call);
}

static void free_pin_req(void *req)
{
	free(req);
}

static gboolean register_dbus_path(const char *path, uint16_t path_id, uint16_t dev_id,
				const DBusObjectPathVTable *pvtable, gboolean fallback)
{
	gboolean ret = FALSE;
	struct hci_dbus_data *data = NULL;

	syslog(LOG_INFO, "[%s,%d] path:%s, fallback:%d", __PRETTY_FUNCTION__, __LINE__, path, fallback);

	data = malloc(sizeof(struct hci_dbus_data));
	if (data == NULL) {
		syslog(LOG_ERR, "Failed to alloc memory to DBUS path register data (%s)", path);
		goto failed;
	}

	data->path_id = path_id;
	data->dev_id = dev_id;

	if (fallback) {
		if (!dbus_connection_register_fallback(connection, path, pvtable, data)) {
			syslog(LOG_ERR, "DBUS failed to register %s fallback", path);
			goto failed;
		}
	} else {
		if (!dbus_connection_register_object_path(connection, path, pvtable, data)) {
			syslog(LOG_ERR, "DBUS failed to register %s object", path);
			goto failed;
		}
	}

	ret = TRUE;

failed:
	if (!ret && data)
		free(data);

	return ret;
}

static gboolean unregister_dbus_path(const char *path)
{
	void *data;

	syslog(LOG_INFO, "[%s,%d] path:%s", __PRETTY_FUNCTION__, __LINE__, path);

	if (dbus_connection_get_object_path_data(connection, path, &data) && data)
		free(data);

	if (!dbus_connection_unregister_object_path (connection, path)) {
		syslog(LOG_ERR, "DBUS failed to unregister %s object", path);
		return FALSE;
	}

	return TRUE;
}

/*****************************************************************
 *
 *  Section reserved to HCI commands confirmation handling and low
 *  level events(eg: device attached/dettached.
 *
 *****************************************************************/

gboolean hcid_dbus_register_device(uint16_t id) 
{
	char path[MAX_PATH_LENGTH];
	char *pptr = path;
	gboolean ret;
	DBusMessage *message = NULL;
	int dd = -1;
	read_scan_enable_rp rp;
	struct hci_request rq;
	struct hci_dbus_data* pdata;

	snprintf(path, sizeof(path), "%s/hci%d", DEVICE_PATH, id);
	ret = register_dbus_path(path, DEVICE_PATH_ID, id, &obj_dev_vtable, FALSE);

	dd = hci_open_dev(id);
	if (dd < 0) {
		syslog(LOG_ERR, "HCI device open failed: hci%d", id);
		rp.enable = SCAN_PAGE | SCAN_INQUIRY;
	} else {
		memset(&rq, 0, sizeof(rq));
		rq.ogf    = OGF_HOST_CTL;
		rq.ocf    = OCF_READ_SCAN_ENABLE;
		rq.rparam = &rp;
		rq.rlen   = READ_SCAN_ENABLE_RP_SIZE;
	
		if (hci_send_req(dd, &rq, 500) < 0) {
			syslog(LOG_ERR, "Sending read scan enable command failed: %s (%d)",
								strerror(errno), errno);
			rp.enable = SCAN_PAGE | SCAN_INQUIRY;
		} else if (rp.status) {
			syslog(LOG_ERR, "Getting scan enable failed with status 0x%02x",
										rp.status);
			rp.enable = SCAN_PAGE | SCAN_INQUIRY;
		}
	}

	if (!dbus_connection_get_object_path_data(connection, path, (void*) &pdata))
		syslog(LOG_ERR, "Getting path data failed!");
	else
		pdata->path_data = rp.enable; /* Keep the current scan status */

	message = dbus_message_new_signal(MANAGER_PATH, MANAGER_INTERFACE,
							BLUEZ_MGR_DEV_ADDED);
	if (message == NULL) {
		syslog(LOG_ERR, "Can't allocate D-BUS remote name message");
		goto failed;
	}

	/*FIXME: append a friendly name instead of device path */
	dbus_message_append_args(message,
					DBUS_TYPE_STRING, &pptr,
					DBUS_TYPE_INVALID);

	if (!dbus_connection_send(connection, message, NULL)) {
		syslog(LOG_ERR, "Can't send D-BUS added device message");
		goto failed;
	}

	dbus_connection_flush(connection);

failed:
	if (message)
		dbus_message_unref(message);

	if (ret && default_dev < 0)
		default_dev = id;

	if (dd >= 0)
		close(dd);

	return ret;
}

gboolean hcid_dbus_unregister_device(uint16_t id)
{
	gboolean ret;
	DBusMessage *message = NULL;
	char path[MAX_PATH_LENGTH];
	char *pptr = path;

	snprintf(path, sizeof(path), "%s/hci%d", DEVICE_PATH, id);

	message = dbus_message_new_signal(MANAGER_PATH, MANAGER_INTERFACE,
							BLUEZ_MGR_DEV_REMOVED);
	if (message == NULL) {
		syslog(LOG_ERR, "Can't allocate D-BUS remote name message");
		goto failed;
	}

	/*FIXME: append a friendly name instead of device path */
	dbus_message_append_args(message,
					DBUS_TYPE_STRING, &pptr,
					DBUS_TYPE_INVALID);

	if (!dbus_connection_send(connection, message, NULL)) {
		syslog(LOG_ERR, "Can't send D-BUS added device message");
		goto failed;
	}

	dbus_connection_flush(connection);

failed:
	if (message)
		dbus_message_unref(message);

	ret = unregister_dbus_path(path);

	if (ret && default_dev == id)
		default_dev = hci_get_route(NULL);

	return ret;
}

void hcid_dbus_request_pin(int dev, struct hci_conn_info *ci)
{
	DBusMessage *message = NULL;
	DBusPendingCall *pending = NULL;
	struct pin_request *req;
	uint8_t *addr = (uint8_t *) &ci->bdaddr;
	dbus_bool_t out = ci->out;

	if (!connection) {
		if (!hcid_dbus_init())
			goto failed;
	}

	message = dbus_message_new_method_call(PINAGENT_SERVICE_NAME, PINAGENT_PATH,
						PINAGENT_INTERFACE, PIN_REQUEST);
	if (message == NULL) {
		syslog(LOG_ERR, "Couldn't allocate D-BUS message");
		goto failed;
	}

	req = malloc(sizeof(*req));
	req->dev = dev;
	bacpy(&req->bda, &ci->bdaddr);

	dbus_message_append_args(message, DBUS_TYPE_BOOLEAN, &out,
			DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE,
			&addr, sizeof(bdaddr_t), DBUS_TYPE_INVALID);

	if (dbus_connection_send_with_reply(connection, message,
						&pending, TIMEOUT) == FALSE) {
		syslog(LOG_ERR, "D-BUS send failed");
		goto failed;
	}

	dbus_pending_call_set_notify(pending, reply_handler_function,
							req, free_pin_req);

	dbus_connection_flush(connection);

	dbus_message_unref(message);

	return;

failed:
	if (message)
		dbus_message_unref(message);

	hci_send_cmd(dev, OGF_LINK_CTL, OCF_PIN_CODE_NEG_REPLY, 6, &ci->bdaddr);
}

void hcid_dbus_bonding_created_complete(bdaddr_t *local, bdaddr_t *peer, const uint8_t status)
{
	DBusMessage *message = NULL;
	char *local_addr, *peer_addr;
	bdaddr_t tmp;
	char path[MAX_PATH_LENGTH];
	int id;

	baswap(&tmp, local); local_addr = batostr(&tmp);
	baswap(&tmp, peer); peer_addr = batostr(&tmp);

	id = hci_devid(local_addr);
	if (id < 0) {
		syslog(LOG_ERR, "No matching device id for %s", local_addr);
		goto failed;
	}

	snprintf(path, sizeof(path), "%s/hci%d", DEVICE_PATH, id);

	message = dbus_message_new_signal(path, DEVICE_INTERFACE,
						DEV_SIG_BONDING_CREATED);
	if (message == NULL) {
		syslog(LOG_ERR, "Can't allocate D-BUS remote name message");
		goto failed;
	}

	/*FIXME: create the signal based on status value - BondingCreated or BondingFailed*/
	dbus_message_append_args(message,
					DBUS_TYPE_STRING, &peer_addr,
					DBUS_TYPE_BYTE, &status,
					DBUS_TYPE_INVALID);

	if (dbus_connection_send(connection, message, NULL) == FALSE) {
		syslog(LOG_ERR, "Can't send D-BUS remote name message");
		goto failed;
	}

	dbus_connection_flush(connection);

failed:
	if (message)
		dbus_message_unref(message);

	bt_free(local_addr);
	bt_free(peer_addr);
}

void hcid_dbus_discover_start(bdaddr_t *local)
{
	DBusMessage *message = NULL;
	char path[MAX_PATH_LENGTH];
	char *local_addr;
	bdaddr_t tmp;
	int id;

	baswap(&tmp, local); local_addr = batostr(&tmp);

	id = hci_devid(local_addr);
	if (id < 0) {
		syslog(LOG_ERR, "No matching device id for %s", local_addr);
		goto failed;
	}

	snprintf(path, sizeof(path), "%s/hci%d", DEVICE_PATH, id);

	message = dbus_message_new_signal(path, DEVICE_INTERFACE,
						DEV_SIG_DISCOVER_START);
	if (message == NULL) {
		syslog(LOG_ERR, "Can't allocate D-BUS inquiry start message");
		goto failed;
	}

	if (dbus_connection_send(connection, message, NULL) == FALSE) {
		syslog(LOG_ERR, "Can't send D-BUS inquiry start message");
		goto failed;
	}

	dbus_connection_flush(connection);

failed:
	dbus_message_unref(message);
	bt_free(local_addr);
}

void hcid_dbus_discover_complete(bdaddr_t *local)
{
	DBusMessage *message = NULL;
	char path[MAX_PATH_LENGTH];
	char *local_addr;
	bdaddr_t tmp;
	int id;

	baswap(&tmp, local); local_addr = batostr(&tmp);

	id = hci_devid(local_addr);
	if (id < 0) {
		syslog(LOG_ERR, "No matching device id for %s", local_addr);
		goto failed;
	}

	snprintf(path, sizeof(path), "%s/hci%d", DEVICE_PATH, id);

	message = dbus_message_new_signal(path, DEVICE_INTERFACE,
						DEV_SIG_DISCOVER_COMPLETE);
	if (message == NULL) {
		syslog(LOG_ERR, "Can't allocate D-BUS inquiry complete message");
		goto failed;
	}

	if (dbus_connection_send(connection, message, NULL) == FALSE) {
		syslog(LOG_ERR, "Can't send D-BUS inquiry complete message");
		goto failed;
	}

	dbus_connection_flush(connection);

failed:
	dbus_message_unref(message);
	bt_free(local_addr);
}

void hcid_dbus_discover_result(bdaddr_t *local, bdaddr_t *peer, uint32_t class, int8_t rssi)
{
	DBusMessage *message = NULL;
	char path[MAX_PATH_LENGTH];
	char *local_addr, *peer_addr;
	dbus_uint32_t tmp_class = class;
	dbus_int32_t tmp_rssi = rssi;
	bdaddr_t tmp;
	int id;

	baswap(&tmp, local); local_addr = batostr(&tmp);
	baswap(&tmp, peer); peer_addr = batostr(&tmp);

	id = hci_devid(local_addr);
	if (id < 0) {
		syslog(LOG_ERR, "No matching device id for %s", local_addr);
		goto failed;
	}

	snprintf(path, sizeof(path), "%s/hci%d", DEVICE_PATH, id);

	message = dbus_message_new_signal(path, DEVICE_INTERFACE,
						DEV_SIG_DISCOVER_RESULT);
	if (message == NULL) {
		syslog(LOG_ERR, "Can't allocate D-BUS inquiry result message");
		goto failed;
	}

	dbus_message_append_args(message,
					DBUS_TYPE_STRING, &peer_addr,
					DBUS_TYPE_UINT32, &tmp_class,
					DBUS_TYPE_INT32, &tmp_rssi,
					DBUS_TYPE_INVALID);

	if (dbus_connection_send(connection, message, NULL) == FALSE) {
		syslog(LOG_ERR, "Can't send D-BUS inquiry result message");
		goto failed;
	}

	dbus_connection_flush(connection);

failed:
	dbus_message_unref(message);

	bt_free(local_addr);
	bt_free(peer_addr);
}

void hcid_dbus_remote_name(bdaddr_t *local, bdaddr_t *peer, char *name)
{
	DBusMessage *message = NULL;
	char path[MAX_PATH_LENGTH];
	char *local_addr, *peer_addr;
	bdaddr_t tmp;
	int id;

	baswap(&tmp, local); local_addr = batostr(&tmp);
	baswap(&tmp, peer); peer_addr = batostr(&tmp);

	id = hci_devid(local_addr);
	if (id < 0) {
		syslog(LOG_ERR, "No matching device id for %s", local_addr);
		goto failed;
	}

	snprintf(path, sizeof(path), "%s/hci%d", DEVICE_PATH, id);

	message = dbus_message_new_signal(path, DEVICE_INTERFACE,
						DEV_REMOTE_NAME);
	if (message == NULL) {
		syslog(LOG_ERR, "Can't allocate D-BUS remote name message");
		goto failed;
	}

	dbus_message_append_args(message,
					DBUS_TYPE_STRING, &peer_addr,
					DBUS_TYPE_STRING, &name,
					DBUS_TYPE_INVALID);

	if (dbus_connection_send(connection, message, NULL) == FALSE) {
		syslog(LOG_ERR, "Can't send D-BUS remote name message");
		goto failed;
	}

	dbus_connection_flush(connection);

failed:
	if (message)
		dbus_message_unref(message);

	bt_free(local_addr);
	bt_free(peer_addr);
}

void hcid_dbus_remote_name_failed(bdaddr_t *local, bdaddr_t *peer, uint8_t status)
{
	DBusMessage *message = NULL;
	char path[MAX_PATH_LENGTH];
	char *local_addr, *peer_addr;
	bdaddr_t tmp;
	int id;

	baswap(&tmp, local); local_addr = batostr(&tmp);
	baswap(&tmp, peer); peer_addr = batostr(&tmp);

	id = hci_devid(local_addr);
	if (id < 0) {
		syslog(LOG_ERR, "No matching device id for %s", local_addr);
		goto failed;
	}

	snprintf(path, sizeof(path), "%s/hci%d", DEVICE_PATH, id);

	message = dbus_message_new_signal(path, DEVICE_INTERFACE,
						DEV_SIG_REMOTE_NAME_FAILED);
	if (message == NULL) {
		syslog(LOG_ERR, "Can't allocate D-BUS remote name message");
		goto failed;
	}

	dbus_message_append_args(message,
					DBUS_TYPE_STRING, &peer_addr,
					DBUS_TYPE_BYTE, &status,
					DBUS_TYPE_INVALID);

	if (dbus_connection_send(connection, message, NULL) == FALSE) {
		syslog(LOG_ERR, "Can't send D-BUS remote name message");
		goto failed;
	}

	dbus_connection_flush(connection);

failed:
	if (message)
		dbus_message_unref(message);

	bt_free(local_addr);
	bt_free(peer_addr);
}

void hcid_dbus_conn_complete(bdaddr_t *local, bdaddr_t *peer)
{
}

void hcid_dbus_disconn_complete(bdaddr_t *local, bdaddr_t *peer, uint8_t reason)
{
}


/*****************************************************************
 *
 *  Section reserved to D-Bus watch functions
 *
 *****************************************************************/
gboolean watch_func(GIOChannel *chan, GIOCondition cond, gpointer data)
{
	DBusWatch *watch = (DBusWatch *) data;
	int flags = 0;

	if (cond & G_IO_IN)  flags |= DBUS_WATCH_READABLE;
	if (cond & G_IO_OUT) flags |= DBUS_WATCH_WRITABLE;
	if (cond & G_IO_HUP) flags |= DBUS_WATCH_HANGUP;
	if (cond & G_IO_ERR) flags |= DBUS_WATCH_ERROR;

	dbus_watch_handle(watch, flags);

	dbus_connection_ref(connection);

	/* Dispatch messages */
	while (dbus_connection_dispatch(connection) == DBUS_DISPATCH_DATA_REMAINS);

	dbus_connection_unref(connection);

	return TRUE;
}

dbus_bool_t add_watch(DBusWatch *watch, void *data)
{
	GIOCondition cond = G_IO_HUP | G_IO_ERR;
	GIOChannel *io;
	guint *id;
	int fd, flags;

	if (!dbus_watch_get_enabled(watch))
		return TRUE;

	id = malloc(sizeof(guint));
	if (id == NULL)
		return FALSE;

	fd = dbus_watch_get_fd(watch);
	io = g_io_channel_unix_new(fd);
	flags = dbus_watch_get_flags(watch);

	if (flags & DBUS_WATCH_READABLE) cond |= G_IO_IN;
	if (flags & DBUS_WATCH_WRITABLE) cond |= G_IO_OUT;

	*id = g_io_add_watch(io, cond, watch_func, watch);

	dbus_watch_set_data(watch, id, NULL);

	return TRUE;
}

static void remove_watch(DBusWatch *watch, void *data)
{
	guint *id = dbus_watch_get_data(watch);

	dbus_watch_set_data(watch, NULL, NULL);

	if (id) {
		g_io_remove_watch(*id);
		free(id);
	}
}

static void watch_toggled(DBusWatch *watch, void *data)
{
	/* Because we just exit on OOM, enable/disable is
	 * no different from add/remove */
	if (dbus_watch_get_enabled(watch))
		add_watch(watch, data);
	else
		remove_watch(watch, data);
}

gboolean hcid_dbus_init(void)
{
	DBusError error;

	dbus_error_init(&error);

	connection = dbus_bus_get(DBUS_BUS_SYSTEM, &error);

	if (dbus_error_is_set(&error)) {
		syslog(LOG_ERR, "Can't open system message bus connection: %s",
								error.message);
		dbus_error_free(&error);
		return FALSE;
	}

	dbus_connection_set_exit_on_disconnect(connection, FALSE);

	dbus_bus_request_name(connection, BASE_INTERFACE,
				DBUS_NAME_FLAG_PROHIBIT_REPLACEMENT, &error);

	if (dbus_error_is_set(&error)) {
		syslog(LOG_ERR, "Can't get system message bus name: %s",
								error.message);
		dbus_error_free(&error);
		return FALSE;
	}

	if (!register_dbus_path(DEVICE_PATH, DEVICE_ROOT_ID, INVALID_DEV_ID,
				&obj_dev_vtable, TRUE))
		return FALSE;

	if (!register_dbus_path(MANAGER_PATH, MANAGER_ROOT_ID, INVALID_DEV_ID,
				&obj_mgr_vtable, FALSE))
		return FALSE;

	if (!dbus_connection_add_filter(connection, hci_dbus_signal_filter, NULL, NULL)) {
		syslog(LOG_ERR, "Can't add new HCI filter");
		return FALSE;
	}

	dbus_connection_set_watch_functions(connection,
			add_watch, remove_watch, watch_toggled, NULL, NULL);

	return TRUE;
}

void hcid_dbus_exit(void)
{
	char **children = NULL;

	if (!connection)
		return;

	/* Unregister all paths in Device path hierarchy */
	if (!dbus_connection_list_registered(connection, DEVICE_PATH, &children))
		goto done;

	for (; *children; children++) {
		char dev_path[MAX_PATH_LENGTH];

		snprintf(dev_path, sizeof(dev_path), "%s/%s", DEVICE_PATH, *children);

		unregister_dbus_path(dev_path);
	}

	if (*children)
		dbus_free_string_array(children);

done:
	unregister_dbus_path(DEVICE_PATH);
	unregister_dbus_path(MANAGER_PATH);

	dbus_connection_close(connection);
}

/*****************************************************************
 *
 *  Section reserved to re-connection timer
 *
 *****************************************************************/
static void reconnect_timer_handler(int signum)
{
	struct hci_dev_list_req *dl = NULL;
	struct hci_dev_req *dr;
	int sk;
	int i;

	if (hcid_dbus_init() == FALSE)
		return;

	/* stop the timer */
	sigaction(SIGALRM, NULL, NULL);
	setitimer(ITIMER_REAL, NULL, NULL);

	/* register the device based paths */

	/* Create and bind HCI socket */
	sk = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
	if (sk < 0) {
		syslog(LOG_ERR, "Can't open HCI socket: %s (%d)",
							strerror(errno), errno);
		return;
	}

	dl = malloc(HCI_MAX_DEV * sizeof(*dr) + sizeof(*dl));
	if (!dl) {
		syslog(LOG_ERR, "Can't allocate memory");
		goto failed;
	}

	dl->dev_num = HCI_MAX_DEV;
	dr = dl->dev_req;

	if (ioctl(sk, HCIGETDEVLIST, (void *) dl) < 0) {
		syslog(LOG_INFO, "Can't get device list: %s (%d)",
							strerror(errno), errno);
		goto failed;
	}

	/* reset the default device */
	default_dev = -1;

	for (i = 0; i < dl->dev_num; i++, dr++)
		hcid_dbus_register_device(dr->dev_id);

failed:
	if (sk >= 0)
		close(sk);

	if (dl)
		free(dl);

}

static void reconnect_timer_start(void)
{
	struct sigaction sa;
	struct itimerval timer;

	memset (&sa, 0, sizeof (sa));
	sa.sa_handler = &reconnect_timer_handler;
	sigaction(SIGALRM, &sa, NULL);

	/* expire after X  msec... */
	timer.it_value.tv_sec = 0;
	timer.it_value.tv_usec = DBUS_RECONNECT_TIMER;

	/* ... and every x msec after that. */
	timer.it_interval.tv_sec = 0;
	timer.it_interval.tv_usec = DBUS_RECONNECT_TIMER;

	setitimer(ITIMER_REAL, &timer, NULL);
}

/*****************************************************************
 *
 *  Section reserved to D-Bus signal/messages handling function
 *
 *****************************************************************/
static DBusHandlerResult hci_dbus_signal_filter (DBusConnection *conn, DBusMessage *msg, void *data)
{
	DBusHandlerResult ret = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	const char *iface;
	const char *method;

	if (!msg || !conn)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (dbus_message_get_type (msg) != DBUS_MESSAGE_TYPE_SIGNAL)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	iface = dbus_message_get_interface(msg);
	method = dbus_message_get_member(msg);

	if ((strcmp(iface, DBUS_INTERFACE_LOCAL) == 0) &&
			(strcmp(method, "Disconnected") == 0)) {
		syslog(LOG_ERR, "Got disconnected from the system message bus");
		dbus_connection_dispatch(conn);
		dbus_connection_close(conn);
		dbus_connection_unref(conn);
		reconnect_timer_start();
		ret = DBUS_HANDLER_RESULT_HANDLED;
	} else if (strcmp(iface, DBUS_INTERFACE_DBUS) == 0) {
		if (strcmp(method, "NameOwnerChanged") == 0)
			ret = DBUS_HANDLER_RESULT_HANDLED;
		else if (strcmp(method, "NameAcquired") == 0)
			ret = DBUS_HANDLER_RESULT_HANDLED;
	}

	return ret;
}

static DBusHandlerResult msg_func_device(DBusConnection *conn, DBusMessage *msg, void *data)
{
	const struct service_data *handlers = dev_services;
	DBusMessage *reply = NULL;
	struct hci_dbus_data *dbus_data = data;
	const char *method;
	const char *signature;
	uint32_t error = BLUEZ_EDBUS_UNKNOWN_METHOD;
	DBusHandlerResult ret = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	method = dbus_message_get_member(msg);
	signature = dbus_message_get_signature(msg);

	syslog(LOG_INFO, "[%s,%d] path:%s, method:%s", __PRETTY_FUNCTION__, __LINE__, dbus_message_get_path(msg), method);
	       
	if (dbus_data->path_id == DEVICE_ROOT_ID) {
		/* Device is down(path unregistered) or the path is wrong */
		ret = DBUS_HANDLER_RESULT_HANDLED;
		error = BLUEZ_EDBUS_UNKNOWN_PATH;
		goto failed;
	}

	/* It's a device path id */
	for (; handlers->name != NULL; handlers++) {
		if (strcmp(handlers->name, method))
			continue;

		ret = DBUS_HANDLER_RESULT_HANDLED;

		if (!strcmp(handlers->signature, signature)) {
			reply = handlers->handler_func(msg, data);
			error = 0;
			break;
		} else {
			/* Set the error, but continue looping incase there is
			 * another method with the same name but a different
			 * signature */
			error = BLUEZ_EDBUS_WRONG_SIGNATURE;
			continue;
		}
	}

failed:
	if (error)
		reply = bluez_new_failure_msg(msg, error);

	if (reply) {
		if (!dbus_connection_send (conn, reply, NULL))
			syslog(LOG_ERR, "Can't send reply message!");
		dbus_message_unref(reply);
	}

	return ret;
}

static DBusHandlerResult msg_func_manager(DBusConnection *conn, DBusMessage *msg, void *data)
{
	const struct service_data *handlers;
	DBusMessage *reply = NULL;
	const char *iface;
	const char *method;
	const char *signature;
	uint32_t error = BLUEZ_EDBUS_UNKNOWN_METHOD;
	DBusHandlerResult ret = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	iface = dbus_message_get_interface(msg);
	method = dbus_message_get_member(msg);
	signature = dbus_message_get_signature(msg);

	syslog(LOG_INFO, "[%s,%d] path:%s, method:%s", __PRETTY_FUNCTION__, __LINE__, dbus_message_get_path(msg), method);

	if (strcmp(iface, MANAGER_INTERFACE) != 0)
		return ret;

	for (handlers = mgr_services; handlers->name != NULL; handlers++) {
		if (strcmp(handlers->name, method))
			continue;

		if (strcmp(handlers->signature, signature) != 0)
			error = BLUEZ_EDBUS_WRONG_SIGNATURE;
		else {
			reply = handlers->handler_func(msg, data);
			error = 0;
		}

		ret = DBUS_HANDLER_RESULT_HANDLED;
	}

	if (error)
		reply = bluez_new_failure_msg(msg, error);

	if (reply) {
		if (!dbus_connection_send (conn, reply, NULL))
			syslog(LOG_ERR, "Can't send reply message!");
		dbus_message_unref(reply);
	}

	return ret;
}

/*****************************************************************
 *
 *  Section reserved to device D-Bus services implementation
 *
 *****************************************************************/

static DBusMessage* handle_dev_get_address_req(DBusMessage *msg, void *data)
{
	struct hci_dbus_data *dbus_data = data;
	DBusMessage *reply;
	char str[18], *str_ptr = str;

	get_device_address(dbus_data->dev_id, str, sizeof(str));

	reply = dbus_message_new_method_return(msg);

	dbus_message_append_args(reply, DBUS_TYPE_STRING, &str_ptr,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage* handle_dev_get_version_req(DBusMessage *msg, void *data)
{
	struct hci_dbus_data *dbus_data = data;
	DBusMessage *reply;
	char str[20], *str_ptr = str;

	get_device_version(dbus_data->dev_id, str, sizeof(str));

	reply = dbus_message_new_method_return(msg);

	dbus_message_append_args(reply, DBUS_TYPE_STRING, &str_ptr,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage* handle_dev_get_revision_req(DBusMessage *msg, void *data)
{
	struct hci_dbus_data *dbus_data = data;
	DBusMessage *reply;
	char str[20], *str_ptr = str;

	get_device_revision(dbus_data->dev_id, str, sizeof(str));

	reply = dbus_message_new_method_return(msg);

	dbus_message_append_args(reply, DBUS_TYPE_STRING, &str_ptr,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage* handle_dev_get_manufacturer_req(DBusMessage *msg, void *data)
{
	struct hci_dbus_data *dbus_data = data;
	DBusMessage *reply;
	char str[64], *str_ptr = str;

	get_device_manufacturer(dbus_data->dev_id, str, sizeof(str));

	reply = dbus_message_new_method_return(msg);

	dbus_message_append_args(reply, DBUS_TYPE_STRING, &str_ptr,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage* handle_dev_get_company_req(DBusMessage *msg, void *data)
{
	struct hci_dbus_data *dbus_data = data;
	DBusMessage *reply;
	char str[64], *str_ptr = str;

	get_device_company(dbus_data->dev_id, str, sizeof(str));

	reply = dbus_message_new_method_return(msg);

	dbus_message_append_args(reply, DBUS_TYPE_STRING, &str_ptr,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage* handle_dev_get_features_req(DBusMessage *msg, void *data)
{
	/*FIXME: */
	return bluez_new_failure_msg(msg, BLUEZ_EDBUS_NOT_IMPLEMENTED);
}

static DBusMessage* handle_dev_get_alias_req(DBusMessage *msg, void *data)
{
	/*FIXME: */
	return bluez_new_failure_msg(msg, BLUEZ_EDBUS_NOT_IMPLEMENTED);
}

static DBusMessage* handle_dev_get_discoverable_to_req(DBusMessage *msg, void *data)
{
	/*FIXME: */
	return bluez_new_failure_msg(msg, BLUEZ_EDBUS_NOT_IMPLEMENTED);
}

static DBusMessage* handle_dev_get_mode_req(DBusMessage *msg, void *data)
{
	const struct hci_dbus_data *dbus_data = data;
	DBusMessage *reply = NULL;
	const uint8_t hci_mode = dbus_data->path_data;
	uint8_t scan_mode;

	switch (hci_mode) {
	case SCAN_DISABLED:
		scan_mode = MODE_OFF;
		break;
	case SCAN_PAGE:
		scan_mode = MODE_CONNECTABLE;
		break;
	case (SCAN_PAGE | SCAN_INQUIRY):
		scan_mode = MODE_DISCOVERABLE;
		break;
	case SCAN_INQUIRY:
	/* inquiry scan mode is not handled, return 0xff */
	default:
		/* reserved */
		scan_mode = 0xff;
	}

	reply = dbus_message_new_method_return(msg);

	dbus_message_append_args(reply,
					DBUS_TYPE_BYTE, &scan_mode,
					DBUS_TYPE_INVALID);

	return reply;
}

static DBusMessage* handle_dev_get_name_req(DBusMessage *msg, void *data)
{
	struct hci_dbus_data *dbus_data = data;
	DBusMessage *reply = NULL;
	int dd = -1;
	read_local_name_rp rp;
	struct hci_request rq;
	const char *pname = (char*) rp.name;
	char name[249];

	dd = hci_open_dev(dbus_data->dev_id);
	if (dd < 0) {
		syslog(LOG_ERR, "HCI device open failed: hci%d", dbus_data->dev_id);
		reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_ENODEV);
		goto failed;
	}

	memset(&rq, 0, sizeof(rq));
	rq.ogf    = OGF_HOST_CTL;
	rq.ocf    = OCF_READ_LOCAL_NAME;
	rq.rparam = &rp;
	rq.rlen   = READ_LOCAL_NAME_RP_SIZE;

	if (hci_send_req(dd, &rq, 100) < 0) {
		syslog(LOG_ERR, "Sending getting name command failed: %s (%d)",
							strerror(errno), errno);
		reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_OFFSET + errno);
		goto failed;
	}

	if (rp.status) {
		syslog(LOG_ERR, "Getting name failed with status 0x%02x", rp.status);
		reply = bluez_new_failure_msg(msg, BLUEZ_EBT_OFFSET + rp.status);
		goto failed;
	}

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL) {
		syslog(LOG_ERR, "Out of memory while calling dbus_message_new_method_return");
		goto failed;
	}

	strncpy(name,pname,sizeof(name)-1);
	name[248]='\0';
	pname = name;

	dbus_message_append_args(reply,
				DBUS_TYPE_STRING, &pname,
				DBUS_TYPE_INVALID);

failed:
	if (dd >= 0)
		close(dd);

	return reply;
}

static DBusMessage* handle_dev_is_connectable_req(DBusMessage *msg, void *data)
{
	/*FIXME: */
	return bluez_new_failure_msg(msg, BLUEZ_EDBUS_NOT_IMPLEMENTED);
}

static DBusMessage* handle_dev_is_discoverable_req(DBusMessage *msg, void *data)
{
	/*FIXME: */
	return bluez_new_failure_msg(msg, BLUEZ_EDBUS_NOT_IMPLEMENTED);
}

static DBusMessage* handle_dev_set_alias_req(DBusMessage *msg, void *data)
{
	/*FIXME: */
	return bluez_new_failure_msg(msg, BLUEZ_EDBUS_NOT_IMPLEMENTED);
}

static DBusMessage* handle_dev_set_class_req(DBusMessage *msg, void *data)
{
	/*FIXME: */
	return bluez_new_failure_msg(msg, BLUEZ_EDBUS_NOT_IMPLEMENTED);
}

static DBusMessage* handle_dev_set_discoverable_to_req(DBusMessage *msg, void *data)
{
	/*FIXME: */
	return bluez_new_failure_msg(msg, BLUEZ_EDBUS_NOT_IMPLEMENTED);
}

static DBusMessage* handle_dev_set_mode_req(DBusMessage *msg, void *data)
{
	const struct hci_dbus_data *dbus_data = data;
	DBusMessage *reply = NULL;
	struct hci_request rq;
	int dd = -1;
	const uint8_t scan_mode;
	uint8_t hci_mode;
	uint8_t status = 0;
	const uint8_t current_mode = dbus_data->path_data;

	dbus_message_get_args(msg, NULL,
					DBUS_TYPE_BYTE, &scan_mode,
					DBUS_TYPE_INVALID);

	switch (scan_mode) {
	case MODE_OFF:
		hci_mode = SCAN_DISABLED;
		break;
	case MODE_CONNECTABLE:
		hci_mode = SCAN_PAGE;
		break;
	case MODE_DISCOVERABLE:
		hci_mode = (SCAN_PAGE | SCAN_INQUIRY);
		break;
	default:
		/* invalid mode */
		reply = bluez_new_failure_msg(msg, BLUEZ_EDBUS_WRONG_PARAM);
		goto failed;
	}

	dd = hci_open_dev(dbus_data->dev_id);
	if (dd < 0) {
		syslog(LOG_ERR, "HCI device open failed: hci%d", dbus_data->dev_id);
		reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_ENODEV);
		goto failed;
	}

	/* Check if the new requested mode is different from the current */
	if (current_mode != hci_mode) {
		memset(&rq, 0, sizeof(rq));
		rq.ogf    = OGF_HOST_CTL;
		rq.ocf    = OCF_WRITE_SCAN_ENABLE;
		rq.cparam = &hci_mode;
		rq.clen   = sizeof(hci_mode);
		rq.rparam = &status;
		rq.rlen   = sizeof(status);

		if (hci_send_req(dd, &rq, 100) < 0) {
			syslog(LOG_ERR, "Sending write scan enable command failed: %s (%d)",
							strerror(errno), errno);
			reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_OFFSET | errno);
			goto failed;
		}
		if (status) {
			syslog(LOG_ERR, "Setting scan enable failed with status 0x%02x", status);
			reply = bluez_new_failure_msg(msg, BLUEZ_EBT_OFFSET | status);
			goto failed;
		}

	}

	reply = dbus_message_new_method_return(msg);

failed:
	if (dd >= 0)
		close(dd);

	return reply;
}

static DBusMessage* handle_dev_set_name_req(DBusMessage *msg, void *data)
{
	struct hci_dbus_data *dbus_data = data;
	DBusMessageIter iter;
	DBusMessage *reply = NULL;
	char *str_name;
	int dd = -1;
	uint8_t status;
	change_local_name_cp cp;
	struct hci_request rq;

	dbus_message_iter_init(msg, &iter);
	dbus_message_iter_get_basic(&iter, &str_name);

	if (strlen(str_name) == 0) {
		syslog(LOG_ERR, "HCI change name failed - Invalid Name!");
		reply = bluez_new_failure_msg(msg, BLUEZ_EDBUS_WRONG_PARAM);
		goto failed;
	}

	dd = hci_open_dev(dbus_data->dev_id);
	if (dd < 0) {
		syslog(LOG_ERR, "HCI device open failed: hci%d", dbus_data->dev_id);
		reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_ENODEV);
		goto failed;
	}

	memset(&rq, 0, sizeof(rq));
	strncpy((char *) cp.name, str_name, sizeof(cp.name));
	rq.ogf    = OGF_HOST_CTL;
	rq.ocf    = OCF_CHANGE_LOCAL_NAME;
	rq.cparam = &cp;
	rq.clen   = CHANGE_LOCAL_NAME_CP_SIZE;
	rq.rparam = &status;
	rq.rlen   = sizeof(status);

	if (hci_send_req(dd, &rq, 100) < 0) {
		syslog(LOG_ERR, "Sending change name command failed: %s (%d)",
							strerror(errno), errno);
		reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_OFFSET + errno);
		goto failed;
	}

	if (status) {
		syslog(LOG_ERR, "Setting name failed with status 0x%02x", status);
		reply = bluez_new_failure_msg(msg, BLUEZ_EBT_OFFSET + status);
		goto failed;
	}

	reply = dbus_message_new_method_return(msg);

failed:
	if (dd >= 0)
		close(dd);

	return reply;

}

static DBusMessage* handle_dev_discover_req(DBusMessage *msg, void *data)
{
	DBusMessage *reply = NULL;
	inquiry_cp cp;
	evt_cmd_status rp;
	struct hci_request rq;
	struct hci_dbus_data *dbus_data = data;
	int dd = -1;
	uint8_t length = 8, num_rsp = 0;
	uint32_t lap = 0x9e8b33;

	dd = hci_open_dev(dbus_data->dev_id);
	if (dd < 0) {
		syslog(LOG_ERR, "Unable to open device %d: %s (%d)",
					dbus_data->dev_id, strerror(errno), errno);
		reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_OFFSET + errno);
		goto failed;
	}

	memset(&cp, 0, sizeof(cp));
	cp.lap[0]  = lap & 0xff;
	cp.lap[1]  = (lap >> 8) & 0xff;
	cp.lap[2]  = (lap >> 16) & 0xff;
	cp.length  = length;
	cp.num_rsp = num_rsp;

	memset(&rq, 0, sizeof(rq));
	rq.ogf    = OGF_LINK_CTL;
	rq.ocf    = OCF_INQUIRY;
	rq.cparam = &cp;
	rq.clen   = INQUIRY_CP_SIZE;
	rq.rparam = &rp;
	rq.rlen   = EVT_CMD_STATUS_SIZE;
	rq.event  = EVT_CMD_STATUS;

	if (hci_send_req(dd, &rq, 100) < 0) {
		syslog(LOG_ERR, "Unable to start inquiry: %s (%d)",
							strerror(errno), errno);
		reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_OFFSET + errno);
		goto failed;
	}

	reply = dbus_message_new_method_return(msg);

failed:
	if (dd >= 0)
		hci_close_dev(dd);

	return reply;

}

static DBusMessage* handle_dev_discover_cache_req(DBusMessage *msg, void *data)
{
	/*FIXME: */
	return bluez_new_failure_msg(msg, BLUEZ_EDBUS_NOT_IMPLEMENTED);
}

static DBusMessage* handle_dev_discover_cancel_req(DBusMessage *msg, void *data)
{
	DBusMessage *reply = NULL;
	struct hci_request rq;
	struct hci_dbus_data *dbus_data = data;
	uint8_t status;
	int dd = -1;

	dd = hci_open_dev(dbus_data->dev_id);
	if (dd < 0) {
		syslog(LOG_ERR, "Unable to open device %d: %s (%d)",
					dbus_data->dev_id, strerror(errno), errno);
		reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_OFFSET + errno);
		goto failed;
	}

	memset(&rq, 0, sizeof(rq));
	rq.ogf    = OGF_LINK_CTL;
	rq.ocf    = OCF_INQUIRY_CANCEL;
	rq.rparam = &status;
	rq.rlen   = sizeof(status);

	if (hci_send_req(dd, &rq, 100) < 0) {
		syslog(LOG_ERR, "Sending cancel inquiry failed: %s (%d)",
							strerror(errno), errno);
		reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_OFFSET + errno);
		goto failed;
	}

	if (status) {
		syslog(LOG_ERR, "Cancel inquiry failed with status 0x%02x", status);
		reply = bluez_new_failure_msg(msg, BLUEZ_EBT_OFFSET + status);
		goto failed;
	}

	reply = dbus_message_new_method_return(msg);

failed:
	if (dd >= 0)
		hci_close_dev(dd);

	return reply;
}


static DBusMessage* handle_dev_discover_service_req(DBusMessage *msg, void *data)
{
	/*FIXME: */
	return bluez_new_failure_msg(msg, BLUEZ_EDBUS_NOT_IMPLEMENTED);
}

static DBusMessage* handle_dev_last_seen_req(DBusMessage *msg, void *data)
{
	/*FIXME: */
	return bluez_new_failure_msg(msg, BLUEZ_EDBUS_NOT_IMPLEMENTED);
}

static DBusMessage* handle_dev_last_used_req(DBusMessage *msg, void *data)
{
	/*FIXME: */
	return bluez_new_failure_msg(msg, BLUEZ_EDBUS_NOT_IMPLEMENTED);
}

static DBusMessage* handle_dev_remote_alias_req(DBusMessage *msg, void *data)
{
	/*FIXME: */
	return bluez_new_failure_msg(msg, BLUEZ_EDBUS_NOT_IMPLEMENTED);
}

static DBusMessage* handle_dev_remote_name_req(DBusMessage *msg, void *data)
{
	DBusMessage *reply = NULL;
	DBusMessage *signal = NULL;
	struct hci_dbus_data *dbus_data = data;
	const char *str_bdaddr;
	char *name;
	char path[MAX_PATH_LENGTH];
	bdaddr_t bdaddr;
	struct hci_dev_info di;
	struct hci_request rq;
	remote_name_req_cp cp;
	evt_cmd_status rp;
	int dd = -1;

	dbus_message_get_args(msg, NULL,
					DBUS_TYPE_STRING, &str_bdaddr,
					DBUS_TYPE_INVALID);

	str2ba(str_bdaddr, &bdaddr);
	if (hci_devinfo(dbus_data->dev_id, &di) < 0) {
		syslog(LOG_ERR, "Can't get device info");
		reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_ENODEV);
		goto failed;
	}

	/* Try retrieve from local cache */
	name = get_device_name(&di.bdaddr, &bdaddr);
	if (name) {

		reply = dbus_message_new_method_return(msg);

		snprintf(path, sizeof(path), "%s/hci%d", DEVICE_PATH, dbus_data->dev_id);

		signal = dbus_message_new_signal(path, DEVICE_INTERFACE,
							DEV_SIG_REMOTE_NAME);

		dbus_message_append_args(signal,
						DBUS_TYPE_STRING, &str_bdaddr,
						DBUS_TYPE_STRING, &name,
						DBUS_TYPE_INVALID);

		if (dbus_connection_send(connection, signal, NULL) == FALSE) {
			syslog(LOG_ERR, "Can't send D-BUS remote name signal message");
			goto failed;
		}

		dbus_message_unref(signal);
		free(name);

	} else {

		/* Send HCI command */
		dd = hci_open_dev(dbus_data->dev_id);
		if (dd < 0) {
			syslog(LOG_ERR, "Unable to open device %d: %s (%d)",
						dbus_data->dev_id, strerror(errno), errno);
			reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_OFFSET | errno);
			goto failed;
		}

		memset(&cp, 0, sizeof(cp));
		cp.bdaddr = bdaddr;
		cp.pscan_rep_mode = 0x02;

		memset(&rq, 0, sizeof(rq));
		rq.ogf    = OGF_LINK_CTL;
		rq.ocf    = OCF_REMOTE_NAME_REQ;
		rq.cparam = &cp;
		rq.clen   = REMOTE_NAME_REQ_CP_SIZE;
		rq.rparam = &rp;
		rq.rlen   = EVT_CMD_STATUS_SIZE;
		rq.event  = EVT_CMD_STATUS;

		if (hci_send_req(dd, &rq, 100) < 0) {
			syslog(LOG_ERR, "Unable to send remote name request: %s (%d)",
						strerror(errno), errno);
			reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_OFFSET | errno);
			goto failed;
		}

		if (rp.status) {
			syslog(LOG_ERR, "Remote name request failed");
			reply = bluez_new_failure_msg(msg, BLUEZ_EBT_OFFSET | rp.status);
			goto failed;
		}
	}

	reply = dbus_message_new_method_return(msg);
failed:
	if (dd >= 0)
		hci_close_dev(dd);

	return reply;
}

static DBusMessage* handle_dev_remote_version_req(DBusMessage *msg, void *data)
{
	/*FIXME: */
	return bluez_new_failure_msg(msg, BLUEZ_EDBUS_NOT_IMPLEMENTED);
}

static DBusMessage* handle_dev_create_bonding_req(DBusMessage *msg, void *data)
{
	struct hci_request rq;
	auth_requested_cp cp;
	evt_cmd_status rp;
	DBusMessage *reply = NULL;
	char *str_bdaddr = NULL;
	struct hci_dbus_data *dbus_data = data;
	struct hci_conn_info_req *cr = NULL;
	bdaddr_t bdaddr;
	int dev_id = -1;
	int dd = -1;

	dbus_message_get_args(msg, NULL,
					DBUS_TYPE_STRING, &str_bdaddr,
					DBUS_TYPE_INVALID);

	str2ba(str_bdaddr, &bdaddr);

	dev_id = hci_for_each_dev(HCI_UP, find_conn, (long) &bdaddr);

	if (dev_id < 0) {
		reply = bluez_new_failure_msg(msg, BLUEZ_EDBUS_CONN_NOT_FOUND);
		goto failed;
	}

	if (dbus_data->dev_id != dev_id) {
		reply = bluez_new_failure_msg(msg, BLUEZ_EDBUS_CONN_NOT_FOUND);
		goto failed;
	}

	dd = hci_open_dev(dev_id);
	if (dd < 0) {
		reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_ENODEV);
		goto failed;
	}

	cr = malloc(sizeof(*cr) + sizeof(struct hci_conn_info));
	if (!cr) {
		reply = bluez_new_failure_msg(msg, BLUEZ_EDBUS_NO_MEM);
		goto failed;
	}

	bacpy(&cr->bdaddr, &bdaddr);
	cr->type = ACL_LINK;

	if (ioctl(dd, HCIGETCONNINFO, (unsigned long) cr) < 0) {
		reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_OFFSET + errno);
		goto failed;
	}

	memset(&cp, 0, sizeof(cp));
	cp.handle = cr->conn_info->handle;

	memset(&rq, 0, sizeof(rq));
	rq.ogf    = OGF_LINK_CTL;
	rq.ocf    = OCF_AUTH_REQUESTED;
	rq.cparam = &cp;
	rq.clen   = AUTH_REQUESTED_CP_SIZE;
	rq.rparam = &rp;
	rq.rlen   = EVT_CMD_STATUS_SIZE;
	rq.event  = EVT_CMD_STATUS;

	if (hci_send_req(dd, &rq, 100) < 0) {
		syslog(LOG_ERR, "Unable to send authentication request: %s (%d)",
							strerror(errno), errno);
		reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_OFFSET + errno);
		goto failed;
	}

	reply = dbus_message_new_method_return(msg);

failed:
	if (dd >= 0)
		close(dd);

	if (cr)
		free(cr);

	return reply;
}

static DBusMessage* handle_dev_list_bondings_req(DBusMessage *msg, void *data)
{
	/*FIXME: */
	return bluez_new_failure_msg(msg, BLUEZ_EDBUS_NOT_IMPLEMENTED);
}



static DBusMessage* handle_dev_has_bonding_req(DBusMessage *msg, void *data)
{
	/*FIXME: */
	return bluez_new_failure_msg(msg, BLUEZ_EDBUS_NOT_IMPLEMENTED);
}

static DBusMessage* handle_dev_remove_bonding_req(DBusMessage *msg, void *data)
{
	/*FIXME: */
	return bluez_new_failure_msg(msg, BLUEZ_EDBUS_NOT_IMPLEMENTED);
}


static DBusMessage* handle_dev_pin_code_length_req(DBusMessage *msg, void *data)
{
	/*FIXME: */
	return bluez_new_failure_msg(msg, BLUEZ_EDBUS_NOT_IMPLEMENTED);
}

static DBusMessage* handle_dev_encryption_key_size_req(DBusMessage *msg, void *data)
{
	/*FIXME: */
	return bluez_new_failure_msg(msg, BLUEZ_EDBUS_NOT_IMPLEMENTED);
}


/*****************************************************************
 *  
 *  Section reserved to device HCI callbacks
 *  
 *****************************************************************/
void hcid_dbus_setname_complete(bdaddr_t *local)
{
	DBusMessage *signal = NULL;
	char *local_addr;
	bdaddr_t tmp;
	int id;
	int dd = -1;
	read_local_name_rp rp;
	struct hci_request rq;
	const char *pname = (char*) rp.name;
	char name[249];

	baswap(&tmp, local); local_addr = batostr(&tmp);

	id = hci_devid(local_addr);
	if (id < 0) {
		syslog(LOG_ERR, "No matching device id for %s", local_addr);
		goto failed;
	}

	dd = hci_open_dev(id);
	if (dd < 0) {
		syslog(LOG_ERR, "HCI device open failed: hci%d", id);
		memset(&rq, 0, sizeof(rq));
	} else {
		memset(&rq, 0, sizeof(rq));
		rq.ogf    = OGF_HOST_CTL;
		rq.ocf    = OCF_READ_LOCAL_NAME;
		rq.rparam = &rp;
		rq.rlen   = READ_LOCAL_NAME_RP_SIZE;

		if (hci_send_req(dd, &rq, 100) < 0) {
			syslog(LOG_ERR,
				"Sending getting name command failed: %s (%d)",
				strerror(errno), errno);
			rp.name[0] = '\0';
		}

		if (rp.status) {
			syslog(LOG_ERR,
				"Getting name failed with status 0x%02x",
				rp.status);
			rp.name[0] = '\0';
		}
	}

	strncpy(name, pname, sizeof(name) - 1);
	name[248] = '\0';
	pname = name;

	signal = dev_signal_factory(id, DEV_SIG_NAME_CHANGED, DBUS_TYPE_STRING, &pname, DBUS_TYPE_INVALID);
	if (dbus_connection_send(connection, signal, NULL) == FALSE) {
		syslog(LOG_ERR, "Can't send D-BUS %s signal", DEV_SIG_NAME_CHANGED);
		goto failed;
	}

	dbus_connection_flush(connection);

failed:
	if (signal)
		dbus_message_unref(signal);

	if (dd >= 0)
		close(dd);

	bt_free(local_addr);
}


void hcid_dbus_setscan_enable_complete(bdaddr_t *local)
{
	DBusMessage *message = NULL;
	struct hci_dbus_data *pdata = NULL;
	char *local_addr;
	char path[MAX_PATH_LENGTH];
	bdaddr_t tmp;
	read_scan_enable_rp rp;
	struct hci_request rq;
	int id;
	int dd = -1;
	uint8_t scan_mode;

	baswap(&tmp, local); local_addr = batostr(&tmp);
	id = hci_devid(local_addr);
	if (id < 0) {
		syslog(LOG_ERR, "No matching device id for %s", local_addr);
		goto failed;
	}

	snprintf(path, sizeof(path), "%s/hci%d", DEVICE_PATH, id);

	dd = hci_open_dev(id);
	if (dd < 0) {
		syslog(LOG_ERR, "HCI device open failed: hci%d", id);
		goto failed;
	}

	memset(&rq, 0, sizeof(rq));
	rq.ogf    = OGF_HOST_CTL;
	rq.ocf    = OCF_READ_SCAN_ENABLE;
	rq.rparam = &rp;
	rq.rlen   = READ_SCAN_ENABLE_RP_SIZE;

	if (hci_send_req(dd, &rq, 100) < 0) {
		syslog(LOG_ERR, "Sending read scan enable command failed: %s (%d)",
							strerror(errno), errno);
		goto failed;
	}

	if (rp.status) {
		syslog(LOG_ERR,
			"Getting scan enable failed with status 0x%02x",
			rp.status);
		goto failed;
	}

	if (!dbus_connection_get_object_path_data(connection, path, (void*) &pdata)) {
		syslog(LOG_ERR, "Getting path data failed!");
		goto failed;
	}

	/* update the current scan mode value */
	pdata->path_data = rp.enable;

	switch (rp.enable) {
	case SCAN_DISABLED:
		scan_mode = MODE_OFF;
		break;
	case SCAN_PAGE:
		scan_mode = MODE_CONNECTABLE;
		break;
	case (SCAN_PAGE | SCAN_INQUIRY):
		scan_mode = MODE_DISCOVERABLE;
		break;
	case SCAN_INQUIRY:
		/* ignore, this event should not be sent*/
	default:
		/* ignore, reserved */
		goto failed;
	}

	message = dbus_message_new_signal(path, DEVICE_INTERFACE,
						DEV_SIG_MODE_CHANGED);
	if (message == NULL) {
		syslog(LOG_ERR, "Can't allocate D-BUS inquiry complete message");
		goto failed;
	}

	dbus_message_append_args(message,
					DBUS_TYPE_BYTE, &scan_mode,
					DBUS_TYPE_INVALID);

	if (dbus_connection_send(connection, message, NULL) == FALSE) {
		syslog(LOG_ERR, "Can't send D-BUS ModeChanged(%x) signal", rp.enable);
		goto failed;
	}


	dbus_connection_flush(connection);

failed:

	if (message)
		dbus_message_unref(message);

	if (dd >= 0)
		close(dd);

	bt_free(local_addr);
}
 
/*****************************************************************
 *  
 *  Section reserved to Manager D-Bus services implementation
 *  
 *****************************************************************/
static DBusMessage* handle_mgr_device_list_req(DBusMessage *msg, void *data)
{
	DBusMessageIter iter;
	DBusMessageIter array_iter;
	DBusMessage *reply = NULL;
	struct hci_dev_list_req *dl = NULL;
	struct hci_dev_req *dr      = NULL;
	int sk = -1;
	int i;
	const char array_sig[] = MGR_REPLY_DEVICE_LIST_STRUCT_SIGNATURE;

	/* Create and bind HCI socket */
	sk = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
	if (sk < 0) {
		syslog(LOG_ERR, "Can't open HCI socket: %s (%d)", strerror(errno), errno);
		reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_OFFSET + errno);
		goto failed;
	}

	dl = malloc(HCI_MAX_DEV * sizeof(*dr) + sizeof(*dl));
	if (!dl) {
		syslog(LOG_ERR, "Can't allocate memory");
		reply = bluez_new_failure_msg(msg, BLUEZ_EDBUS_NO_MEM);
		goto failed;
	}

	dl->dev_num = HCI_MAX_DEV;
	dr = dl->dev_req;

	if (ioctl(sk, HCIGETDEVLIST, dl) < 0) {
		reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_OFFSET + errno);
		goto failed;
	}

	/* active bluetooth adapter found */
	reply = dbus_message_new_method_return(msg);
	if (reply == NULL) {
		syslog(LOG_ERR, "Out of memory while calling dbus_message_new_method_return");
		goto failed;
	}

	dbus_message_iter_init_append(reply, &iter);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, array_sig, &array_iter);
	dr = dl->dev_req;

	for (i = 0; i < dl->dev_num; i++, dr++) {
		char apath[MAX_PATH_LENGTH];
		char aaddr[18];
		char *paddr = aaddr;
		char *ppath = apath;
		char *ptype;
		const char *flag;
		DBusMessageIter flag_array_iter, struct_iter;
		struct hci_dev_info di;
		hci_map *mp;

		mp = dev_flags_map;
		memset(&di, 0 , sizeof(struct hci_dev_info));
		di.dev_id = dr->dev_id;

		if (ioctl(sk, HCIGETDEVINFO, &di) < 0)
			continue;

		snprintf(apath, sizeof(apath), "%s/%s", DEVICE_PATH, di.name);

		ba2str(&di.bdaddr, aaddr);
		ptype = hci_dtypetostr(di.type);

		dbus_message_iter_open_container(&array_iter,
				DBUS_TYPE_STRUCT, NULL, &struct_iter);

		dbus_message_iter_append_basic(&struct_iter, DBUS_TYPE_STRING, &ppath);
		dbus_message_iter_append_basic(&struct_iter, DBUS_TYPE_STRING, &paddr);
		dbus_message_iter_append_basic(&struct_iter, DBUS_TYPE_STRING, &ptype);

		if (hci_test_bit(HCI_UP, &dr->dev_opt))
			flag = "UP";
		else
			flag = "DOWN";

		dbus_message_iter_append_basic(&struct_iter, DBUS_TYPE_STRING, &flag);

		dbus_message_iter_open_container(&struct_iter,
					DBUS_TYPE_ARRAY, DBUS_TYPE_STRING_AS_STRING, &flag_array_iter);

		while (mp->str) {
			if (hci_test_bit(mp->val, &dr->dev_opt))
				dbus_message_iter_append_basic(&flag_array_iter, DBUS_TYPE_STRING, &mp->str);
			mp++;
		}
		dbus_message_iter_close_container(&struct_iter, &flag_array_iter);
		dbus_message_iter_close_container(&array_iter, &struct_iter);
	}

	dbus_message_iter_close_container(&iter, &array_iter);

failed:
	if (sk >= 0)
		close(sk);

	if (dl)
		free(dl);

	return reply;
}

static DBusMessage* handle_mgr_default_device_req(DBusMessage *msg, void *data)
{
	char path[MAX_PATH_LENGTH];
	char *pptr = path;
	DBusMessage *reply = NULL;

	if (default_dev < 0) {
		reply = bluez_new_failure_msg(msg, BLUEZ_ESYSTEM_ENODEV);
		goto failed;
	}

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL) {
		syslog(LOG_ERR, "Out of memory while calling dbus_message_new_method_return");
		goto failed;
	}

	snprintf(path, sizeof(path), "%s/hci%d", DEVICE_PATH, default_dev);
	dbus_message_append_args(reply,
					DBUS_TYPE_STRING, &pptr,
					DBUS_TYPE_INVALID);

failed:
	return reply;
}
