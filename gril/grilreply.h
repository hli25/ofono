/*
 *
 *  RIL library with GLib integration
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012-2014  Canonical Ltd.
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

#ifndef __GRILREPLY_H
#define __GRILREPLY_H

#include <ofono/types.h>
#include <ofono/sim.h>

#include "gril.h"

#ifdef __cplusplus
extern "C" {
#endif

struct reply_reg_state {
	int status;
	int lac;
	int ci;
	int tech;
};

struct reply_data_reg_state {
	struct reply_reg_state reg_state;
	unsigned int max_cids;
};

struct reply_oem_hook {
	int length;
	void *data;
};

struct reply_reg_state *g_ril_reply_parse_voice_reg_state(GRil *gril,
						const struct ril_msg *message);
struct reply_data_reg_state *g_ril_reply_parse_data_reg_state(GRil *gril,
						const struct ril_msg *message);

GSList *g_ril_reply_parse_get_calls(GRil *gril, const struct ril_msg *message);

int *g_ril_reply_parse_retries(GRil *gril, const struct ril_msg *message,
				enum ofono_sim_password_type passwd_type);

void g_ril_reply_free_oem_hook(struct reply_oem_hook *oem_hook);

struct reply_oem_hook *g_ril_reply_oem_hook_raw(GRil *gril,
						const struct ril_msg *message);

struct parcel_str_array *g_ril_reply_oem_hook_strings(GRil *gril,
						const struct ril_msg *message);

#ifdef __cplusplus
}
#endif

#endif /* __GRILREPLY_H */