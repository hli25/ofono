/*
 *
 *  oFono - Open Source Telephony - RIL Modem Support
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2013 Canonical, Ltd. All rights reserved.
 *  Copyright (C) 2015 Ratchanan Srirattanamet.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
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

#define _GNU_SOURCE
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/sim.h>

#include "ofono.h"

#include "simutil.h"
#include "util.h"

#include "gril.h"
#include "grilutil.h"
#include "parcel.h"
#include "ril_constants.h"
#include "rilmodem.h"

#include "grilreply.h"
#include "grilrequest.h"
#include "grilunsol.h"

#include <drivers/infineonmodem/infineon_constants.h>

/* Number of passwords in EPINC response */
#define MTK_EPINC_NUM_PASSWD 4

/* Commands defined for TS 27.007 +CRSM */
#define CMD_READ_BINARY   176 /* 0xB0   */
#define CMD_READ_RECORD   178 /* 0xB2   */
#define CMD_GET_RESPONSE  192 /* 0xC0   */
#define CMD_UPDATE_BINARY 214 /* 0xD6   */
#define CMD_UPDATE_RECORD 220 /* 0xDC   */

/*
 * Based on ../drivers/atmodem/sim.c.
 *
 * TODO:
 * 1. Defines constants for hex literals
 * 2. Document P1-P3 usage (+CSRM)
 */

/*
 * TODO: CDMA/IMS
 *
 * This code currently only grabs the AID/application ID from
 * the gsm_umts application on the SIM card.  This code will
 * need to be modified for CDMA support, and possibly IMS-based
 * applications.  In this case, app_id should be changed to an
 * array or HashTable of app_status structures.
 *
 * The same applies to the app_type.
 */

static void ril_pin_change_state(struct ofono_sim *sim,
				enum ofono_sim_password_type passwd_type,
				int enable, const char *passwd,
				ofono_sim_lock_unlock_cb_t cb, void *data);

struct sim_data {
	GRil *ril;
	enum ofono_ril_vendor vendor;
	gchar *aid_str;
	guint app_type;
	enum ofono_sim_password_type passwd_type;
	int retries[OFONO_SIM_PASSWORD_INVALID];
	enum ofono_sim_password_type passwd_state;
	struct ofono_modem *modem;
	ofono_sim_state_event_cb_t ril_state_watch;
	ofono_bool_t unlock_pending;
};

struct change_state_cbd {
	struct ofono_sim *sim;
	enum ofono_sim_password_type passwd_type;
	int enable;
	const char *passwd;
	ofono_sim_lock_unlock_cb_t cb;
	void *data;
};

static void send_get_sim_status(struct ofono_sim *sim);

static gboolean parse_sim_io(GRil *ril, struct ril_msg *message,
				int *sw1, int *sw2, char **hex_response)
{
	struct parcel rilp;

	/*
	 * Minimum length of SIM_IO_Response is 12:
	 * sw1 (int32)
	 * sw2 (int32)
	 * simResponse (string)
	 */
	if (message->buf_len < 12) {
		ofono_error("Invalid SIM IO reply: size too small (< 12): %lu",
				message->buf_len);
		return FALSE;
	}

	g_ril_init_parcel(message, &rilp);
	*sw1 = parcel_r_int32(&rilp);
	*sw2 = parcel_r_int32(&rilp);

	*hex_response = parcel_r_string(&rilp);

	g_ril_append_print_buf(ril, "(sw1=0x%.2X,sw2=0x%.2X,%s)",
				*sw1, *sw2, *hex_response);
	g_ril_print_response(ril, message);

	if (rilp.malformed) {
		g_free(*hex_response);
		return FALSE;
	}

	return TRUE;
}

static void ril_file_info_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_file_info_cb_t cb = cbd->cb;
	struct sim_data *sd = cbd->user;
	int sw1, sw2;
	char *hex_response;
	unsigned char *response = NULL;
	long len;
	gboolean ok = FALSE;
	int flen = 0, rlen = 0, str = 0;
	guchar access[3] = { 0x00, 0x00, 0x00 };
	guchar file_status;

	/* Error, and no data */
	if (message->error != RIL_E_SUCCESS && message->buf_len == 0) {
		ofono_error("%s: Reply failure: %s", __func__,
				ril_error_to_string(message->error));
		goto error;
	}

	/*
	 * The reply can have event data even when message->error is not zero
	 * in mako.
	 *
	 */

	if (parse_sim_io(sd->ril, message, &sw1, &sw2, &hex_response) == FALSE)
		goto error;

	if (hex_response != NULL) {
		response = decode_hex(hex_response, -1, &len, -1);
		g_free(hex_response);
		hex_response = NULL;

		if (response == NULL)
			goto error;
	}

	/*
	 * SIM app file not found || USIM app file not found
	 * See 3gpp TS 51.011, 9.4.4, and ETSI TS 102 221, 10.2.1.5.3
	 * This can happen with result SUCCESS (maguro) or GENERIC_FAILURE
	 * (mako)
	 */
	if ((sw1 == 0x94 && sw2 == 0x04) || (sw1 == 0x6A && sw2 == 0x82)) {
		DBG("File not found. Error %s",
			ril_error_to_string(message->error));
		goto error;
	}

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s: Reply failure: %s, %02x, %02x", __func__,
				ril_error_to_string(message->error), sw1, sw2);
		goto error;
	}

	if ((sw1 != 0x90 && sw1 != 0x91 && sw1 != 0x92 && sw1 != 0x9f) ||
			(sw1 == 0x90 && sw2 != 0x00)) {
		struct ofono_error error;

		ofono_error("Error reply, invalid values: sw1: %02x sw2: %02x",
				sw1, sw2);

		g_free(response);
		response = NULL;

		memset(&error, 0, sizeof(error));
		error.type = OFONO_ERROR_TYPE_SIM;
		error.error = (sw1 << 8) | sw2;

		cb(&error, -1, -1, -1, NULL, EF_STATUS_INVALIDATED, cbd->data);
		return;
	}

	if (len < 0)
		goto error;

	if (response[0] == 0x62) {
		ok = sim_parse_3g_get_response(response, len,
						&flen, &rlen, &str,
						access, NULL);
		file_status = EF_STATUS_VALID;
	} else
		ok = sim_parse_2g_get_response(response, len,
						&flen, &rlen, &str,
						access, &file_status);

	g_free(response);

	if (!ok)
		goto error;

	CALLBACK_WITH_SUCCESS(cb, flen, str, rlen,
					access, file_status, cbd->data);
	return;

error:
	g_free(response);
	CALLBACK_WITH_FAILURE(cb, -1, -1, -1, NULL,
				EF_STATUS_INVALIDATED, cbd->data);
}

#define ROOTMF ((char[]) {'\x3F', '\x00'})
#define ROOTMF_SZ sizeof(ROOTMF)

static char *get_path(int vendor, guint app_type, const int fileid,
			const unsigned char *path, unsigned int path_len)
{
	unsigned char db_path[6] = { 0x00 };
	unsigned char *comm_path = db_path;
	int len = 0;

	if (path_len > 0 && path_len < 7) {
		memcpy(db_path, path, path_len);
		len = path_len;
		goto done;
	}

	switch (app_type) {
	case RIL_APPTYPE_USIM:
		len = sim_ef_db_get_path_3g(fileid, db_path);
		break;
	case RIL_APPTYPE_SIM:
		len = sim_ef_db_get_path_2g(fileid, db_path);
		break;
	default:
		ofono_error("Unsupported app_type: 0%x", app_type);
		return NULL;
	}

done:
	/*
	 * db_path contains the ID of the MF, but MediaTek modems return an
	 * error if we do not remove it. Other devices work the other way
	 * around: they need the MF in the path. In fact MTK behaviour seem to
	 * be the right one: to have the MF in the file is forbidden following
	 * ETSI TS 102 221, section 8.4.2 (we are accessing the card in mode
	 * "select by path from MF", see 3gpp 27.007, +CRSM).
	 */
	if (vendor == OFONO_RIL_VENDOR_MTK && len >= (int) ROOTMF_SZ &&
			memcmp(db_path, ROOTMF, ROOTMF_SZ) == 0) {
		comm_path = db_path + ROOTMF_SZ;
		len -= ROOTMF_SZ;
	}

	if (len == 0)
		return NULL;

	return encode_hex(comm_path, len, 0);
}

static void ril_sim_read_info(struct ofono_sim *sim, int fileid,
				const unsigned char *path,
				unsigned int path_len,
				ofono_sim_file_info_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data, sd);
	struct parcel rilp;
	char *hex_path;

	DBG("file %04x", fileid);

	hex_path = get_path(g_ril_vendor(sd->ril),
					sd->app_type, fileid, path, path_len);
	if (hex_path == NULL) {
		ofono_error("Couldn't build SIM read info request - NULL path");
		goto error;
	}

	parcel_init(&rilp);

	parcel_w_int32(&rilp, CMD_GET_RESPONSE);
	parcel_w_int32(&rilp, fileid);
	parcel_w_string(&rilp, hex_path);
	parcel_w_int32(&rilp, 0);           /* P1 */
	parcel_w_int32(&rilp, 0);           /* P2 */

	/*
	 * TODO: review parameters values used by Android.
	 * The values of P1-P3 in this code were based on
	 * values used by the atmodem driver impl.
	 *
	 * NOTE:
	 * GET_RESPONSE_EF_SIZE_BYTES == 15; !255
	 */
	parcel_w_int32(&rilp, 15);         /* P3 - max length */
	parcel_w_string(&rilp, NULL);       /* data; only req'd for writes */
	parcel_w_string(&rilp, NULL);       /* pin2; only req'd for writes */
	parcel_w_string(&rilp, sd->aid_str); /* AID (Application ID) */

	/*
	 * sessionId, specific to latest MTK modems (harmless for older ones).
	 * It looks like this field selects one or another SIM application, but
	 * we use only one at a time so using zero here seems safe.
	 */
	if (g_ril_vendor(sd->ril) == OFONO_RIL_VENDOR_MTK)
		parcel_w_int32(&rilp, 0);

	g_ril_append_print_buf(sd->ril, "(cmd=0x%.2X,efid=0x%.4X,path=%s,"
					"0,0,15,(null),pin2=(null),aid=%s)",
					CMD_GET_RESPONSE, fileid, hex_path,
					sd->aid_str);
	g_free(hex_path);

	if (g_ril_send(sd->ril, RIL_REQUEST_SIM_IO, &rilp,
				ril_file_info_cb, cbd, g_free) > 0)
		return;

error:
	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, -1, -1, -1, NULL,
				EF_STATUS_INVALIDATED, data);
}

static void ril_file_io_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_read_cb_t cb = cbd->cb;
	struct sim_data *sd = cbd->user;
	struct ofono_error error;
	int sw1, sw2;
	char *hex_response;
	unsigned char *response = NULL;
	long len;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("RILD reply failure: %s",
				ril_error_to_string(message->error));
		goto error;
	}

	if (parse_sim_io(sd->ril, message, &sw1, &sw2, &hex_response) == FALSE)
		goto error;

	if (hex_response == NULL)
		goto error;

	response = decode_hex(hex_response, -1, &len, -1);
	g_free(hex_response);
	hex_response = NULL;

	if (response == NULL || len == 0) {
		ofono_error("Null SIM IO response from RILD");
		goto error;
	}

	cb(&error, response, len, cbd->data);
	g_free(response);
	return;

error:
	g_free(response);
	CALLBACK_WITH_FAILURE(cb, NULL, 0, cbd->data);
}

static void ril_file_write_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_write_cb_t cb = cbd->cb;
	struct sim_data *sd = cbd->user;
	int sw1, sw2;
	char *hex_response;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s: RILD reply failure: %s",
				__func__, ril_error_to_string(message->error));
		goto error;
	}

	if (parse_sim_io(sd->ril, message, &sw1, &sw2, &hex_response) == FALSE)
		goto error;

	g_free(hex_response);

	if ((sw1 != 0x90 && sw1 != 0x91 && sw1 != 0x92 && sw1 != 0x9f) ||
			(sw1 == 0x90 && sw2 != 0x00)) {
		struct ofono_error error;

		ofono_error("%s: error sw1 %02x sw2 %02x", __func__, sw1, sw2);

		error.type = OFONO_ERROR_TYPE_SIM;
		error.error = (sw1 << 8) | sw2;

		cb(&error, cbd->data);
		return;
	}

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);
}

static void ril_sim_read_binary(struct ofono_sim *sim, int fileid,
				int start, int length,
				const unsigned char *path,
				unsigned int path_len,
				ofono_sim_read_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data, sd);
	char *hex_path;
	struct parcel rilp;

	DBG("file %04x", fileid);

	hex_path = get_path(g_ril_vendor(sd->ril),
					sd->app_type, fileid, path, path_len);
	if (hex_path == NULL) {
		ofono_error("Couldn't build SIM read info request - NULL path");
		goto error;
	}

	parcel_init(&rilp);
	parcel_w_int32(&rilp, CMD_READ_BINARY);
	parcel_w_int32(&rilp, fileid);
	parcel_w_string(&rilp, hex_path);
	parcel_w_int32(&rilp, start >> 8);   /* P1 */
	parcel_w_int32(&rilp, start & 0xff); /* P2 */
	parcel_w_int32(&rilp, length);         /* P3 */
	parcel_w_string(&rilp, NULL);          /* data; only req'd for writes */
	parcel_w_string(&rilp, NULL);          /* pin2; only req'd for writes */
	parcel_w_string(&rilp, sd->aid_str);

	/* sessionId, specific to latest MTK modems (harmless for older ones) */
	if (g_ril_vendor(sd->ril) == OFONO_RIL_VENDOR_MTK)
		parcel_w_int32(&rilp, 0);

	g_ril_append_print_buf(sd->ril, "(cmd=0x%.2X,efid=0x%.4X,path=%s,"
					"%d,%d,%d,(null),pin2=(null),aid=%s)",
					CMD_READ_BINARY, fileid, hex_path,
					start >> 8, start & 0xff,
					length, sd->aid_str);
	g_free(hex_path);

	if (g_ril_send(sd->ril, RIL_REQUEST_SIM_IO, &rilp,
				ril_file_io_cb, cbd, g_free) > 0)
		return;

error:
	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, NULL, 0, data);
}

static void ril_sim_read_record(struct ofono_sim *sim, int fileid,
				int record, int length,
				const unsigned char *path,
				unsigned int path_len,
				ofono_sim_read_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data, sd);
	char *hex_path;
	struct parcel rilp;

	DBG("file %04x", fileid);

	hex_path = get_path(g_ril_vendor(sd->ril),
					sd->app_type, fileid, path, path_len);
	if (hex_path == NULL) {
		ofono_error("Couldn't build SIM read info request - NULL path");
		goto error;
	}

	parcel_init(&rilp);
	parcel_w_int32(&rilp, CMD_READ_RECORD);
	parcel_w_int32(&rilp, fileid);
	parcel_w_string(&rilp, hex_path);
	parcel_w_int32(&rilp, record);      /* P1 */
	parcel_w_int32(&rilp, 4);           /* P2 */
	parcel_w_int32(&rilp, length);      /* P3 */
	parcel_w_string(&rilp, NULL);       /* data; only req'd for writes */
	parcel_w_string(&rilp, NULL);       /* pin2; only req'd for writes */
	parcel_w_string(&rilp, sd->aid_str); /* AID (Application ID) */

	/* sessionId, specific to latest MTK modems (harmless for older ones) */
	if (g_ril_vendor(sd->ril) == OFONO_RIL_VENDOR_MTK)
		parcel_w_int32(&rilp, 0);

	g_ril_append_print_buf(sd->ril, "(cmd=0x%.2X,efid=0x%.4X,path=%s,"
					"%d,%d,%d,(null),pin2=(null),aid=%s)",
					CMD_READ_RECORD, fileid, hex_path,
					record, 4, length, sd->aid_str);
	g_free(hex_path);

	if (g_ril_send(sd->ril, RIL_REQUEST_SIM_IO, &rilp,
				ril_file_io_cb, cbd, g_free) > 0)
		return;

error:
	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, NULL, 0, data);
}

static void ril_sim_update_binary(struct ofono_sim *sim, int fileid,
					int start, int length,
					const unsigned char *value,
					const unsigned char *path,
					unsigned int path_len,
					ofono_sim_write_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data, sd);
	char *hex_path;
	struct parcel rilp;
	char *hex_data;
	int p1, p2;

	DBG("file 0x%04x", fileid);

	hex_path = get_path(g_ril_vendor(sd->ril),
					sd->app_type, fileid, path, path_len);
	if (hex_path == NULL) {
		ofono_error("Couldn't build SIM read info request - NULL path");
		goto error;
	}

	p1 = start >> 8;
	p2 = start & 0xff;
	hex_data = encode_hex(value, length, 0);

	parcel_init(&rilp);
	parcel_w_int32(&rilp, CMD_UPDATE_BINARY);
	parcel_w_int32(&rilp, fileid);
	parcel_w_string(&rilp, hex_path);
	parcel_w_int32(&rilp, p1);		/* P1 */
	parcel_w_int32(&rilp, p2);		/* P2 */
	parcel_w_int32(&rilp, length);		/* P3 (Lc) */
	parcel_w_string(&rilp, hex_data);	/* data */
	parcel_w_string(&rilp, NULL);		/* pin2; only for FDN/BDN */
	parcel_w_string(&rilp, sd->aid_str);	/* AID (Application ID) */

	/* sessionId, specific to latest MTK modems (harmless for older ones) */
	if (g_ril_vendor(sd->ril) == OFONO_RIL_VENDOR_MTK)
		parcel_w_int32(&rilp, 0);

	g_ril_append_print_buf(sd->ril, "(cmd=0x%02X,efid=0x%04X,path=%s,"
					"%d,%d,%d,%s,pin2=(null),aid=%s),",
					CMD_UPDATE_BINARY, fileid, hex_path,
					p1, p2, length, hex_data, sd->aid_str);
	g_free(hex_path);
	g_free(hex_data);

	if (g_ril_send(sd->ril, RIL_REQUEST_SIM_IO, &rilp,
				ril_file_write_cb, cbd, g_free) > 0)
		return;

error:
	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, data);
}

static void update_record(struct ofono_sim *sim, int fileid,
				int access_mode,
				int record, int length,
				const unsigned char *value,
				const unsigned char *path,
				unsigned int path_len,
				ofono_sim_write_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data, sd);
	char *hex_path;
	struct parcel rilp;
	char *hex_data;

	DBG("file 0x%04x", fileid);

	hex_path = get_path(g_ril_vendor(sd->ril),
					sd->app_type, fileid, path, path_len);
	if (hex_path == NULL) {
		ofono_error("Couldn't build SIM read info request - NULL path");
		goto error;
	}

	hex_data = encode_hex(value, length, 0);

	parcel_init(&rilp);
	parcel_w_int32(&rilp, CMD_UPDATE_RECORD);
	parcel_w_int32(&rilp, fileid);
	parcel_w_string(&rilp, hex_path);
	parcel_w_int32(&rilp, record);	/* P1 */
	parcel_w_int32(&rilp, access_mode);	/* P2 (access mode) */
	parcel_w_int32(&rilp, length);	/* P3 (Lc) */
	parcel_w_string(&rilp, hex_data);	/* data */
	parcel_w_string(&rilp, NULL);		/* pin2; only for FDN/BDN */
	parcel_w_string(&rilp, sd->aid_str);	/* AID (Application ID) */

	/* sessionId, specific to latest MTK modems (harmless for older ones) */
	if (g_ril_vendor(sd->ril) == OFONO_RIL_VENDOR_MTK)
		parcel_w_int32(&rilp, 0);

	g_ril_append_print_buf(sd->ril, "(cmd=0x%02X,efid=0x%04X,path=%s,"
					"%d,%d,%d,%s,pin2=(null),aid=%s)",
					CMD_UPDATE_RECORD, fileid, hex_path,
					record, access_mode, length, hex_data,
					sd->aid_str);
	g_free(hex_path);
	g_free(hex_data);

	if (g_ril_send(sd->ril, RIL_REQUEST_SIM_IO, &rilp,
				ril_file_write_cb, cbd, g_free) > 0)
		return;
error:
	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, data);
}

static void ril_sim_update_record(struct ofono_sim *sim, int fileid,
					int record, int length,
					const unsigned char *value,
					const unsigned char *path,
					unsigned int path_len,
					ofono_sim_write_cb_t cb, void *data)
{
	update_record(sim, fileid, 4, record, length, value,
			path, path_len, cb, data);
}

static void ril_sim_update_cyclic(struct ofono_sim *sim, int fileid,
					int length, const unsigned char *value,
					const unsigned char *path,
					unsigned int path_len,
					ofono_sim_write_cb_t cb, void *data)
{
	/* Only mode valid for cyclic files is PREVIOUS */
	update_record(sim, fileid, 3, 0, length, value,
			path, path_len, cb, data);
}

static void ril_imsi_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_imsi_cb_t cb = cbd->cb;
	struct sim_data *sd = cbd->user;
	struct parcel rilp;
	gchar *imsi;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("Reply failure: %s",
				ril_error_to_string(message->error));
		goto error;
	}

	g_ril_init_parcel(message, &rilp);
	imsi = parcel_r_string(&rilp);

	g_ril_append_print_buf(sd->ril, "{%s}", imsi ? imsi : "NULL");
	g_ril_print_response(sd->ril, message);

	if (imsi == NULL)
		goto error;

	CALLBACK_WITH_SUCCESS(cb, imsi, cbd->data);
	g_free(imsi);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
}

static void ril_read_imsi(struct ofono_sim *sim, ofono_sim_imsi_cb_t cb,
				void *data)
{
	static const int GET_IMSI_NUM_PARAMS = 1;
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data, sd);
	struct parcel rilp;

	parcel_init(&rilp);
	parcel_w_int32(&rilp, GET_IMSI_NUM_PARAMS);
	parcel_w_string(&rilp, sd->aid_str);

	g_ril_append_print_buf(sd->ril, "(%d,%s)",
					GET_IMSI_NUM_PARAMS, sd->aid_str);

	if (g_ril_send(sd->ril, RIL_REQUEST_GET_IMSI, &rilp,
			ril_imsi_cb, cbd, g_free) == 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, NULL, data);
	}
}

static void sim_status_cb(struct ril_msg *message, gpointer user_data)
{
	struct ofono_sim *sim = user_data;
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct parcel rilp;
	int card_state;
	int universal_pin_state;
	int gsm_umts_app_index;
	int cdma_app_index;
	int ims_app_index;
	int num_apps;
	int i;
	int app_state;
	int perso_substate;

	g_ril_init_parcel(message, &rilp);

	card_state = parcel_r_int32(&rilp);

	/*
	 * NOTE:
	 *
	 * The global pin_status is used for multi-application
	 * UICC cards.  For example, there are SIM cards that
	 * can be used in both GSM and CDMA phones.  Instead
	 * of managed PINs for both applications, a global PIN
	 * is set instead.  It's not clear at this point if
	 * such SIM cards are supported by ofono or RILD.
	 */
	universal_pin_state = parcel_r_int32(&rilp);
	gsm_umts_app_index = parcel_r_int32(&rilp);
	cdma_app_index = parcel_r_int32(&rilp);
	ims_app_index = parcel_r_int32(&rilp);
	num_apps = parcel_r_int32(&rilp);

	if (rilp.malformed)
		return;

	if (gsm_umts_app_index >= num_apps)
		return;

	DBG("[%d,%04d]< %s", g_ril_get_slot(sd->ril),
				message->serial_no,
				"RIL_REQUEST_GET_SIM_STATUS");

	DBG("card_state=%d,universal_pin_state=%d,"
			"gsm_umts_index=%d,cdma_index=%d,ims_index=%d,"
			"num_apps=%d",
			card_state, universal_pin_state,
			gsm_umts_app_index, cdma_app_index, ims_app_index,
			num_apps);

	switch (card_state) {
	case RIL_CARDSTATE_PRESENT:
		break;
	case RIL_CARDSTATE_ABSENT:
		ofono_sim_inserted_notify(sim, FALSE);
		return;
	default:
		ofono_error("%s: bad SIM state (%u)", __func__, card_state);
		return;
	}

	ofono_sim_inserted_notify(sim, TRUE);

	for (i = 0; i != gsm_umts_app_index; i++) {
		parcel_r_int32(&rilp);		/* AppType */
		parcel_r_int32(&rilp);		/* AppState */
		parcel_r_int32(&rilp);		/* PersoSubstate */
		parcel_skip_string(&rilp);	/* AID */
		parcel_skip_string(&rilp);	/* App Label */
		parcel_r_int32(&rilp);		/* PIN1 Replaced */
		parcel_r_int32(&rilp);		/* PIN1 PinState */
		parcel_r_int32(&rilp);		/* PIN2 PinState */

		if (rilp.malformed)
			return;
	}

	/*
	 * We cache the current password state. Ideally this should be done
	 * by issuing a GET_SIM_STATUS request from ril_query_passwd_state,
	 * which is called by the core after sending a password, but
	 * unfortunately the response to GET_SIM_STATUS is not reliable in mako
	 * when sent just after sending the password. Some time is needed
	 * before the modem refreshes its internal state, and when it does it
	 * sends a SIM_STATUS_CHANGED event. In that moment we retrieve the
	 * status and this function is executed. We call
	 * __ofono_sim_recheck_pin as it is the only way to indicate the core
	 * to call query_passwd_state again. An option that can be explored in
	 * the future is wait before invoking core callback for send_passwd
	 * until we know the real password state.
	 */
	sd->app_type = parcel_r_int32(&rilp);	/* AppType */
	app_state = parcel_r_int32(&rilp);	/* AppState */
	perso_substate = parcel_r_int32(&rilp);	/* PersoSubstate */

	switch (app_state) {
	case RIL_APPSTATE_PIN:
		sd->passwd_state = OFONO_SIM_PASSWORD_SIM_PIN;
		break;
	case RIL_APPSTATE_PUK:
		sd->passwd_state = OFONO_SIM_PASSWORD_SIM_PUK;
		break;
	case RIL_APPSTATE_SUBSCRIPTION_PERSO:
		switch (perso_substate) {
		case RIL_PERSOSUBSTATE_SIM_NETWORK:
			sd->passwd_state = OFONO_SIM_PASSWORD_PHNET_PIN;
			break;
		case RIL_PERSOSUBSTATE_SIM_NETWORK_SUBSET:
			sd->passwd_state = OFONO_SIM_PASSWORD_PHNETSUB_PIN;
			break;
		case RIL_PERSOSUBSTATE_SIM_CORPORATE:
			sd->passwd_state = OFONO_SIM_PASSWORD_PHCORP_PIN;
			break;
		case RIL_PERSOSUBSTATE_SIM_SERVICE_PROVIDER:
			sd->passwd_state = OFONO_SIM_PASSWORD_PHSP_PIN;
			break;
		case RIL_PERSOSUBSTATE_SIM_SIM:
			sd->passwd_state = OFONO_SIM_PASSWORD_PHSIM_PIN;
			break;
		case RIL_PERSOSUBSTATE_SIM_NETWORK_PUK:
			sd->passwd_state = OFONO_SIM_PASSWORD_PHNET_PUK;
			break;
		case RIL_PERSOSUBSTATE_SIM_NETWORK_SUBSET_PUK:
			sd->passwd_state = OFONO_SIM_PASSWORD_PHNETSUB_PUK;
			break;
		case RIL_PERSOSUBSTATE_SIM_CORPORATE_PUK:
			sd->passwd_state = OFONO_SIM_PASSWORD_PHCORP_PUK;
			break;
		case RIL_PERSOSUBSTATE_SIM_SERVICE_PROVIDER_PUK:
			sd->passwd_state = OFONO_SIM_PASSWORD_PHSP_PUK;
			break;
		case RIL_PERSOSUBSTATE_SIM_SIM_PUK:
			sd->passwd_state = OFONO_SIM_PASSWORD_PHFSIM_PUK;
			break;
		default:
			sd->passwd_state = OFONO_SIM_PASSWORD_NONE;
			break;
		};
		break;
	case RIL_APPSTATE_READY:
		sd->passwd_state = OFONO_SIM_PASSWORD_NONE;
		break;
	case RIL_APPSTATE_UNKNOWN:
	case RIL_APPSTATE_DETECTED:
	default:
		sd->passwd_state = OFONO_SIM_PASSWORD_INVALID;
		break;
	}

	g_free(sd->aid_str);
	sd->aid_str = parcel_r_string(&rilp);	/* AID */

	DBG("app_type: %d, passwd_state: %d, aid_str (AID): %s",
		sd->app_type, sd->passwd_state, sd->aid_str);

	/*
	 * Note: There doesn't seem to be any other way to force the core SIM
	 * code to recheck the PIN. This call causes the core to call this
	 * atom's query_passwd() function.
	 */
	__ofono_sim_recheck_pin(sim);
}

static void send_get_sim_status(struct ofono_sim *sim)
{
	struct sim_data *sd = ofono_sim_get_data(sim);

	g_ril_send(sd->ril, RIL_REQUEST_GET_SIM_STATUS, NULL,
			sim_status_cb, sim, NULL);
}

static void ril_sim_status_changed(struct ril_msg *message, gpointer user_data)
{
	struct ofono_sim *sim = (struct ofono_sim *) user_data;
	struct sim_data *sd = ofono_sim_get_data(sim);

	DBG("");

	g_ril_print_unsol_no_args(sd->ril, message);

	send_get_sim_status(sim);
}

static void inf_pin_retries_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_pin_retries_cb_t cb = cbd->cb;
	struct sim_data *sd = cbd->user;
	struct reply_oem_hook *reply = NULL;
	int32_t *ret_data;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("Reply failure: %s",
				ril_error_to_string(message->error));
		goto error;
	}

	reply = g_ril_reply_oem_hook_raw(sd->ril, message);
	if (reply == NULL) {
		ofono_error("%s: parse error", __func__);
		goto error;
	}

	if (reply->length < 5 * (int) sizeof(int32_t)) {
		ofono_error("%s: reply too small", __func__);
		goto error;
	}

	/* First integer is INF_RIL_REQUEST_OEM_GET_REMAIN_SIM_PIN_ATTEMPTS */
	ret_data = reply->data;
	sd->retries[OFONO_SIM_PASSWORD_SIM_PIN] = *(++ret_data);
	sd->retries[OFONO_SIM_PASSWORD_SIM_PIN2] = *(++ret_data);
	sd->retries[OFONO_SIM_PASSWORD_SIM_PUK] = *(++ret_data);
	sd->retries[OFONO_SIM_PASSWORD_SIM_PUK2] = *(++ret_data);

	g_ril_reply_free_oem_hook(reply);
	CALLBACK_WITH_SUCCESS(cb, sd->retries, cbd->data);

	return;

error:
	g_ril_reply_free_oem_hook(reply);
	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
}

static void mtk_pin_retries_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_pin_retries_cb_t cb = cbd->cb;
	struct sim_data *sd = cbd->user;
	struct parcel_str_array *str_arr = NULL;
	int pin[MTK_EPINC_NUM_PASSWD];
	int num_pin;

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("Reply failure: %s",
				ril_error_to_string(message->error));
		goto error;
	}

	str_arr = g_ril_reply_oem_hook_strings(sd->ril, message);
	if (str_arr == NULL || str_arr->num_str < 1) {
		ofono_error("%s: parse error", __func__);
		goto error;
	}

	num_pin = sscanf(str_arr->str[0], "+EPINC:%d,%d,%d,%d",
					&pin[0], &pin[1], &pin[2], &pin[3]);

	if (num_pin != MTK_EPINC_NUM_PASSWD) {
		ofono_error("%s: failed parsing %s", __func__, str_arr->str[0]);
		goto error;
	}

	sd->retries[OFONO_SIM_PASSWORD_SIM_PIN] = pin[0];
	sd->retries[OFONO_SIM_PASSWORD_SIM_PIN2] = pin[1];
	sd->retries[OFONO_SIM_PASSWORD_SIM_PUK] = pin[2];
	sd->retries[OFONO_SIM_PASSWORD_SIM_PUK2] = pin[3];

	parcel_free_str_array(str_arr);
	CALLBACK_WITH_SUCCESS(cb, sd->retries, cbd->data);
	return;

error:
	parcel_free_str_array(str_arr);
	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
}

static void ril_query_pin_retries(struct ofono_sim *sim,
					ofono_sim_pin_retries_cb_t cb,
					void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);

	DBG("");

	if (sd->vendor == OFONO_RIL_VENDOR_INFINEON) {
		struct cb_data *cbd = cb_data_new(cb, data, sd);
		struct parcel rilp;
		int32_t oem_req =
			INF_RIL_REQUEST_OEM_GET_REMAIN_SIM_PIN_ATTEMPTS;

		g_ril_request_oem_hook_raw(sd->ril, &oem_req,
						sizeof(oem_req), &rilp);

		/* Send request to RIL */
		if (g_ril_send(sd->ril, RIL_REQUEST_OEM_HOOK_RAW, &rilp,
				inf_pin_retries_cb, cbd, g_free) == 0) {
			g_free(cbd);
			CALLBACK_WITH_FAILURE(cb, NULL, data);
		}
	} else if (sd->vendor == OFONO_RIL_VENDOR_MTK) {
		struct cb_data *cbd = cb_data_new(cb, data, sd);
		struct parcel rilp;
		const char *at_epinc[] = { "AT+EPINC", "+EPINC:" };

		g_ril_request_oem_hook_strings(sd->ril, at_epinc,
						G_N_ELEMENTS(at_epinc), &rilp);

		if (g_ril_send(sd->ril, RIL_REQUEST_OEM_HOOK_STRINGS, &rilp,
				mtk_pin_retries_cb, cbd, g_free) == 0) {
			g_free(cbd);
			CALLBACK_WITH_FAILURE(cb, NULL, data);
		}
	} else {
		CALLBACK_WITH_SUCCESS(cb, sd->retries, data);
	}
}

static void ril_query_passwd_state(struct ofono_sim *sim,
					ofono_sim_passwd_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	DBG("passwd_state %u", sd->passwd_state);

	if (sd->passwd_state == OFONO_SIM_PASSWORD_INVALID)
		CALLBACK_WITH_FAILURE(cb, -1, data);
	else
		CALLBACK_WITH_SUCCESS(cb, sd->passwd_state, data);
}

static void ril_pin_change_state_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sim_lock_unlock_cb_t cb = cbd->cb;
	struct ofono_sim *sim = cbd->user;
	struct sim_data *sd = ofono_sim_get_data(sim);
	int *retries;
	/*
	 * There is no reason to ask SIM status until
	 * unsolicited sim status change indication
	 * Looks like state does not change before that.
	 */

	DBG("Enter password: type %d, result %d",
		sd->passwd_type, message->error);

	retries = g_ril_reply_parse_retries(sd->ril, message, sd->passwd_type);
	if (retries != NULL) {
		memcpy(sd->retries, retries, sizeof(sd->retries));
		g_free(retries);
	}

	/* TODO: re-factor to not use macro for FAILURE;
	   doesn't return error! */
	if (message->error == RIL_E_SUCCESS) {
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	} else {
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		/*
		 * Refresh passwd_state (not needed if the unlock is
		 * successful, as an event will refresh the state in that case)
		 */
		send_get_sim_status(sim);
	}
}

static void ril_pin_send(struct ofono_sim *sim, const char *passwd,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	/*
	 * TODO: This function is supposed to enter the pending password, which
	 * might be also PIN2. So we must check the pending PIN in the future.
	 */

	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data, sim);
	struct parcel rilp;

	sd->passwd_type = OFONO_SIM_PASSWORD_SIM_PIN;

	parcel_init(&rilp);

	parcel_w_int32(&rilp, 2);
	parcel_w_string(&rilp, passwd);
	parcel_w_string(&rilp, sd->aid_str);

	g_ril_append_print_buf(sd->ril, "(%s,aid=%s)", passwd, sd->aid_str);

	if (g_ril_send(sd->ril, RIL_REQUEST_ENTER_SIM_PIN, &rilp,
			ril_pin_change_state_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, data);
}

static void enter_pin_done(const struct ofono_error *error, void *data)
{
	struct change_state_cbd *csd = data;
	struct sim_data *sd = ofono_sim_get_data(csd->sim);

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_error("%s: wrong password", __func__);
		sd->unlock_pending = FALSE;
		CALLBACK_WITH_FAILURE(csd->cb, csd->data);
	} else {
		ril_pin_change_state(csd->sim, csd->passwd_type, csd->enable,
					csd->passwd, csd->cb, csd->data);
	}

	g_free(csd);
}

static const char *const clck_cpwd_fac[] = {
	[OFONO_SIM_PASSWORD_SIM_PIN] = "SC",
	[OFONO_SIM_PASSWORD_SIM_PIN2] = "P2",
	[OFONO_SIM_PASSWORD_PHSIM_PIN] = "PS",
	[OFONO_SIM_PASSWORD_PHFSIM_PIN] = "PF",
	[OFONO_SIM_PASSWORD_PHNET_PIN] = "PN",
	[OFONO_SIM_PASSWORD_PHNETSUB_PIN] = "PU",
	[OFONO_SIM_PASSWORD_PHSP_PIN] = "PP",
	[OFONO_SIM_PASSWORD_PHCORP_PIN] = "PC",
};

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

static void ril_pin_change_state(struct ofono_sim *sim,
				enum ofono_sim_password_type passwd_type,
				int enable, const char *passwd,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd;
	struct parcel rilp;

	/*
	 * If we want to unlock a password that has not been entered yet,
	 * we enter it before trying to unlock. We need sd->unlock_pending as
	 * the password still has not yet been refreshed when this function is
	 * called from enter_pin_done().
	 */
	if (ofono_sim_get_password_type(sim) == passwd_type
			&& enable == FALSE && sd->unlock_pending == FALSE) {
		struct change_state_cbd *csd = g_malloc0(sizeof(*csd));
		csd->sim = sim;
		csd->passwd_type = passwd_type;
		csd->enable = enable;
		csd->passwd = passwd;
		csd->cb = cb;
		csd->data = data;
		sd->unlock_pending = TRUE;

		ril_pin_send(sim, passwd, enter_pin_done, csd);

		return;
	}

	sd->unlock_pending = FALSE;

	if (passwd_type >= ARRAY_SIZE(clck_cpwd_fac) ||
			clck_cpwd_fac[passwd_type] == NULL)
		goto error;

	cbd = cb_data_new(cb, data, sim);

	parcel_init(&rilp);
	parcel_w_int32(&rilp, 5);
	parcel_w_string(&rilp, clck_cpwd_fac[passwd_type]);
	parcel_w_string(&rilp, enable ? "1" : "0");
	parcel_w_string(&rilp, passwd);
	/* TODO: make this a constant... */
	parcel_w_string(&rilp, "0");		/* class */
	parcel_w_string(&rilp, sd->aid_str);

	g_ril_append_print_buf(sd->ril, "(%s,%d,%s,0,aid=%s)",
				clck_cpwd_fac[passwd_type], enable, passwd,
				sd->aid_str);

	if (g_ril_send(sd->ril, RIL_REQUEST_SET_FACILITY_LOCK, &rilp,
				ril_pin_change_state_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);
error:
	CALLBACK_WITH_FAILURE(cb, data);
}

static void ril_pin_send_puk(struct ofono_sim *sim,
				const char *puk, const char *passwd,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data, sim);
	struct parcel rilp;

	sd->passwd_type = OFONO_SIM_PASSWORD_SIM_PUK;

	parcel_init(&rilp);

	parcel_w_int32(&rilp, 3);
	parcel_w_string(&rilp, puk);
	parcel_w_string(&rilp, passwd);
	parcel_w_string(&rilp, sd->aid_str);

	g_ril_append_print_buf(sd->ril, "(puk=%s,pin=%s,aid=%s)",
				puk, passwd, sd->aid_str);

	if (g_ril_send(sd->ril, RIL_REQUEST_ENTER_SIM_PUK, &rilp,
			ril_pin_change_state_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, data);
}

static void ril_change_passwd(struct ofono_sim *sim,
				enum ofono_sim_password_type passwd_type,
				const char *old_passwd, const char *new_passwd,
				ofono_sim_lock_unlock_cb_t cb, void *data)
{
	struct sim_data *sd = ofono_sim_get_data(sim);
	struct cb_data *cbd = cb_data_new(cb, data, sim);
	struct parcel rilp;
	int request;

	switch (passwd_type) {
	case OFONO_SIM_PASSWORD_SIM_PIN:
		request = RIL_REQUEST_CHANGE_SIM_PIN;
		break;
	case OFONO_SIM_PASSWORD_SIM_PIN2:
		request = RIL_REQUEST_CHANGE_SIM_PIN2;
		break;
	default:
		goto error;
	};

	parcel_init(&rilp);

	parcel_w_int32(&rilp, 3);
	parcel_w_string(&rilp, old_passwd);
	parcel_w_string(&rilp, new_passwd);
	parcel_w_string(&rilp, sd->aid_str);

	g_ril_append_print_buf(sd->ril, "(old=%s,new=%s,aid=%s)",
				old_passwd, new_passwd, sd->aid_str);

	if (g_ril_send(sd->ril, request, &rilp, ril_pin_change_state_cb,
			cbd, g_free) > 0)
		return;

error:
	g_free(cbd);
	CALLBACK_WITH_FAILURE(cb, data);
}

static gboolean listen_and_get_sim_status(gpointer user)
{
	struct ofono_sim *sim = user;
	struct sim_data *sd = ofono_sim_get_data(sim);

	send_get_sim_status(sim);

	g_ril_register(sd->ril, RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED,
			(GRilNotifyFunc) ril_sim_status_changed, sim);

	/* TODO: should we also register for RIL_UNSOL_SIM_REFRESH? */
	return FALSE;
}

static gboolean ril_sim_register(gpointer user)
{
	struct ofono_sim *sim = user;
	struct sim_data *sd = ofono_sim_get_data(sim);

	DBG("");

	ofono_sim_register(sim);

	if (sd->ril_state_watch != NULL &&
			!ofono_sim_add_state_watch(sim, sd->ril_state_watch,
							sd->modem, NULL))
		ofono_error("Error registering ril sim watch");

	/*
	 * We use g_idle_add here to make sure that the presence of the SIM
	 * interface is signalled before signalling anything else from the said
	 * interface, as ofono_sim_register also uses g_idle_add.
	 */
	g_idle_add(listen_and_get_sim_status, sim);

	return FALSE;
}

static int ril_sim_probe(struct ofono_sim *sim, unsigned int vendor,
				void *data)
{
	struct ril_sim_data *ril_data = data;
	GRil *ril = ril_data->gril;
	struct sim_data *sd;
	int i;

	sd = g_new0(struct sim_data, 1);
	sd->ril = g_ril_clone(ril);
	sd->vendor = vendor;
	sd->aid_str = NULL;
	sd->app_type = RIL_APPTYPE_UNKNOWN;
	sd->passwd_state = OFONO_SIM_PASSWORD_NONE;
	sd->passwd_type = OFONO_SIM_PASSWORD_NONE;
	sd->modem = ril_data->modem;
	sd->ril_state_watch = ril_data->ril_state_watch;

	for (i = 0; i < OFONO_SIM_PASSWORD_INVALID; i++)
		sd->retries[i] = -1;

	ofono_sim_set_data(sim, sd);

	/*
	 * TODO: analyze if capability check is needed
	 * and/or timer should be adjusted.
	 *
	 * ofono_sim_register() needs to be called after the
	 * driver has been set in ofono_sim_create(), which
	 * calls this function.	 Most other drivers make some
	 * kind of capabilities query to the modem, and then
	 * call register in the callback; we use an idle event
	 * instead.
	 */
	g_idle_add(ril_sim_register, sim);

	return 0;
}

static void ril_sim_remove(struct ofono_sim *sim)
{
	struct sim_data *sd = ofono_sim_get_data(sim);

	ofono_sim_set_data(sim, NULL);

	g_ril_unref(sd->ril);
	g_free(sd->aid_str);
	g_free(sd);
}

static struct ofono_sim_driver driver = {
	.name			= RILMODEM,
	.probe			= ril_sim_probe,
	.remove			= ril_sim_remove,
	.read_file_info		= ril_sim_read_info,
	.read_file_transparent	= ril_sim_read_binary,
	.read_file_linear	= ril_sim_read_record,
	.read_file_cyclic	= ril_sim_read_record,
	.write_file_transparent	= ril_sim_update_binary,
	.write_file_linear	= ril_sim_update_record,
	.write_file_cyclic	= ril_sim_update_cyclic,
	.read_imsi		= ril_read_imsi,
	.query_passwd_state	= ril_query_passwd_state,
	.send_passwd		= ril_pin_send,
	.query_pin_retries	= ril_query_pin_retries,
	.reset_passwd		= ril_pin_send_puk,
	.change_passwd		= ril_change_passwd,
	.lock			= ril_pin_change_state,
/*
 * TODO: Implmenting PIN/PUK support requires defining
 * the following driver methods.
 *
 * In the meanwhile, as long as the SIM card is present,
 * and unlocked, the core SIM code will check for the
 * presence of query_passwd_state, and if null, then the
 * function sim_initialize_after_pin() is called.
 *
 *	.query_locked		= ril_pin_query_enabled,
 */
};

void ril_sim_init(void)
{
	DBG("");
	ofono_sim_driver_register(&driver);
}

void ril_sim_exit(void)
{
	ofono_sim_driver_unregister(&driver);
}