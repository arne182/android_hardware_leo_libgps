/******************************************************************************
 * RPC Client of GPS HAL (hardware abstraction layer) for HD2/Leo
 * 
 * leo-gps-rpc.c
 * 
 * Copyright (C) 2009-2010 The XDAndroid Project
 * Copyright (C) 2010      dan1j3l @ xda-developers
 * Copyright (C) 2011      tytung  @ xda-developers
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * 
 ******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <librpc/rpc/rpc.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <librpc/rpc/rpc_router_ioctl.h>
#include <pthread.h>
#include <cutils/log.h>
#include <gps.h>

#define  LOG_TAG  "gps_leo_rpc"

#define  ENABLE_NMEA 1

#define  DUMP_DATA  0
#define  GPS_DEBUG  0
#define  DISABLE_CLEANUP   1 // fully shutting down the GPS is temporarily disabled

#if GPS_DEBUG
#  define  D(...)   LOGD(__VA_ARGS__)
#else
#  define  D(...)   ((void)0)
#endif
typedef uint32_t pdsm_client_type_e;
typedef int pdsm_client_id_type;

typedef uint32_t pdsm_pa_e_type;

typedef uint32_t pdsm_pa_nmea_port_e_type;
typedef uint32_t pdsm_pa_nmea_reporting_e_type;

typedef uint32_t pdsm_pd_session_e_type;
typedef uint32_t pdsm_pd_session_operation_e_type;

typedef uint32_t pdsm_server_option_e_type;
typedef uint32_t pdsm_server_address_e_type;

typedef uint32_t pdsm_sess_jgps_type_e_type;

typedef uint32_t pdsm_pd_cmd_cb_f_type;
typedef uint32_t pdsm_pa_cmd_cb_f_type;
typedef uint32_t pdsm_xtra_cmd_cb_f_type;

typedef uint32_t pdsm_xtra_dc_status_e_type;

typedef uint32_t pdsm_pa_event_f_type;
typedef uint32_t pdsm_client_event_reg_e_type;
typedef uint32_t pdsm_pa_event_type;
typedef uint32_t pdsm_pa_cmd_err_f_type;

typedef uint32_t pdsm_pd_event_f_type;
typedef uint32_t pdsm_pd_event_type;
typedef uint32_t pdsm_pd_cmd_err_f_type;
typedef uint32_t pdsm_pd_end_session_e_type;

typedef uint32_t pdsm_xtra_event_f_type;
typedef uint32_t pdsm_xtra_event_type;
typedef uint32_t pdsm_xtra_cmd_err_f_type;

typedef uint32_t pdsm_ext_status_event_f_type;
typedef uint32_t pdsm_ext_status_event_type;
typedef uint32_t pdsm_ext_status_cmd_err_f_type;

typedef uint32_t pdsm_gps_lock_e_type;
typedef uint32_t pdsm_position_mode_type;
typedef uint32_t pdsm_pa_mt_lr_support_e_type;
typedef uint32_t pdsm_pa_mo_method_e_type;
typedef uint8_t pdsm_pa_nmea_type;
typedef uint32_t pdsm_pa_sbas_status_e_type;

typedef uint32_t pdsm_atl_type;
typedef uint32_t pdsm_atl_open_f_type;
typedef uint32_t pdsm_atl_close_f_type;
typedef uint32_t pdsm_atl_dns_lookup_f_type;

typedef uint32_t pdsm_lcs_event_f_type;
typedef uint32_t pdsm_lcs_event_type;
typedef uint32_t pdsm_lcs_cmd_err_f_type;


typedef struct registered_server_struct {
    /* MUST BE AT OFFSET ZERO!  The client code assumes this when it overwrites
     * the XDR for server entries which represent a callback client.  Those
     * server entries do not have their own XDRs.
     */
    XDR *xdr;
    /* Because the xdr is NULL for callback clients (as opposed to true
     * servers), we keep track of the program number and version number in this
     * structure as well.
     */
    rpcprog_t x_prog; /* program number */
    rpcvers_t x_vers; /* program version */

    int active;
    struct registered_server_struct *next;
    SVCXPRT *xprt;
    __dispatch_fn_t dispatch;
} registered_server;

struct SVCXPRT {
    fd_set fdset;
    int max_fd;
    pthread_attr_t thread_attr;
    pthread_t  svc_thread;
    pthread_mutexattr_t lock_attr;
    pthread_mutex_t lock;
    registered_server *servers;
    volatile int num_servers;
};

#define SEND_VAL(x) do { \
    val=x;\
    XDR_SEND_UINT32(clnt, &val);\
} while(0);

static uint32_t client_IDs[16];//highest known value is 0xb
static uint32_t no_fix=1;
#if ENABLE_NMEA
static uint32_t use_nmea=1;
#else
static uint32_t use_nmea=0;
#endif
static struct CLIENT *_clnt;
static struct CLIENT *_clnt_atl;
static struct timeval timeout;
static SVCXPRT *_svc;

typedef struct xtra_conf_auto_params_struct
{
    int auto_enable;
	uint16_t interval;
} xtra_conf_auto_params;

static uint8_t CHECKED[4] = {0};
static uint8_t XTRA_AUTO_DOWNLOAD_ENABLED = 0;
static uint8_t XTRA_DOWNLOAD_INTERVAL = 24;  // hours
static uint8_t CLEANUP_ENABLED = 1;
static uint8_t SESSION_TIMEOUT = 2;  // seconds
static uint8_t MEASUREMENT_PRECISION = 10;  // meters

struct params {
    uint32_t *data;
    int length;
};

typedef struct pdsm_client_init_args_struct
{
    pdsm_client_type_e pdsm_client_type_e;
} pdsm_client_init_args;

typedef struct pdsm_client_act_args_struct
{
	pdsm_client_id_type pdsm_client_id_type;
} pdsm_client_act_args;

typedef struct pdsm_client_deact_args_struct
{
	pdsm_client_id_type pdsm_client_id_type;
} pdsm_client_deact_args;

typedef struct pdsm_client_release_args_struct
{
	pdsm_client_id_type pdsm_client_id_type;
} pdsm_client_release_args;

typedef struct pdsm_client_pa_reg_args_struct
{
	pdsm_client_id_type pdsm_client_id_type;
	uint32_t pdsm_client_pa_reg_args_client_data_ptr;
	pdsm_pa_event_f_type pdsm_pa_event_f_type;
	pdsm_client_event_reg_e_type pdsm_client_event_reg_e_type;
	pdsm_pa_event_type pdsm_pa_event_type;
	pdsm_pa_cmd_err_f_type pdsm_pa_cmd_err_f_type;
} pdsm_client_pa_reg_args;

typedef struct pdsm_client_pd_reg_args_struct
{
	pdsm_client_id_type pdsm_client_id_type;
	uint32_t pdsm_client_pd_reg_args_client_data_ptr;
	pdsm_pd_event_f_type pdsm_pd_event_f_type;
	pdsm_client_event_reg_e_type pdsm_client_event_reg_e_type;
	pdsm_pd_event_type pdsm_pd_event_type;
	pdsm_pd_cmd_err_f_type pdsm_pd_cmd_err_f_type;
} pdsm_client_pd_reg_args;

typedef struct pdsm_client_xtra_reg_args_struct
{
	pdsm_client_id_type pdsm_client_id_type;
	uint32_t pdsm_client_xtra_reg_args_client_data_ptr;
	pdsm_xtra_event_f_type pdsm_xtra_event_f_type;
	pdsm_client_event_reg_e_type pdsm_client_event_reg_e_type;
	pdsm_xtra_event_type pdsm_xtra_event_type;
	pdsm_xtra_cmd_err_f_type pdsm_xtra_cmd_err_f_type;
} pdsm_client_xtra_reg_args;

typedef struct pdsm_client_lcs_reg_args_struct
{
	pdsm_client_id_type pdsm_client_id_type;
	uint32_t pdsm_client_lcs_reg_args_client_data_ptr;
	pdsm_lcs_event_f_type pdsm_lcs_event_f_type;
	pdsm_client_event_reg_e_type pdsm_client_event_reg_e_type;
	pdsm_lcs_event_type pdsm_lcs_event_type;
	pdsm_lcs_cmd_err_f_type pdsm_lcs_cmd_err_f_type;
} pdsm_client_lcs_reg_args;

typedef struct pdsm_client_ext_status_reg_args_struct
{
	pdsm_client_id_type pdsm_client_id_type;
	uint32_t pdsm_client_ext_status_reg_args_client_data_ptr;
	pdsm_ext_status_event_f_type pdsm_ext_status_event_f_type;
	pdsm_client_event_reg_e_type pdsm_client_event_reg_e_type;
	pdsm_ext_status_event_type pdsm_ext_status_event_type;
	pdsm_ext_status_cmd_err_f_type pdsm_ext_status_cmd_err_f_type;
} pdsm_client_ext_status_reg_args;

typedef struct pdsm_alt_l2_proxy_reg_args_struct
{
	pdsm_atl_type pdsm_atl_type;
	pdsm_atl_open_f_type pdsm_atl_open_f_type;
	pdsm_atl_close_f_type pdsm_atl_close_f_type;
} pdsm_alt_l2_proxy_reg_args;

typedef struct pdsm_atl_dns_proxy_reg_args_struct
{
	pdsm_atl_type pdsm_atl_type;
	pdsm_atl_dns_lookup_f_type pdsm_atl_dns_lookup_f_type;
} pdsm_atl_dns_proxy_reg_args;

typedef struct pdsm_pa_info_type_struct
{
	pdsm_pa_e_type pa_set;
	void *pa_ptr;
} pdsm_pa_info_type;

typedef struct pdsm_pd_sec_update_rate_s_type_struct
{
	uint8_t val0;
} pdsm_pd_sec_update_rate_s_type;

typedef struct pdsm_pa_nmea_config_s_type_struct
{
	pdsm_pa_nmea_port_e_type pdsm_pa_nmea_port_e_type;
	pdsm_pa_nmea_reporting_e_type pdsm_pa_nmea_reporting_e_type;
} pdsm_pa_nmea_config_s_type;

typedef struct pdsm_delete_parms_type_struct
{
	uint32_t val0;
	uint32_t val1;
	uint32_t val2;
	uint32_t val3;
	uint32_t val4;
	uint32_t val5;
	uint32_t val6;
	uint32_t val7;
} pdsm_delete_parms_type;

typedef struct pdsm_set_parameters_args_struct
{
	pdsm_pa_cmd_cb_f_type pdsm_pa_cmd_cb_f_type;
	uint32_t pdsm_set_parameters_args_client_data_ptr;
	pdsm_pa_e_type pdsm_pa_e_type;
	pdsm_pa_info_type *pdsm_pa_info_type;
	pdsm_client_id_type pdsm_client_id_type;
} pdsm_set_parameters_args;

typedef struct pdsm_pd_qos_struct
{
	uint32_t accuracy;
	uint8_t performance;
} pdsm_pd_qos_type;

typedef struct pdsm_fix_rate_s_type_struct
{
	uint32_t num_fixes;
	uint32_t time_between_fixes;
} pdsm_fix_rate_s_type;

typedef struct pdsm_server_ipv4_address_type_struct
{
	uint32_t val0;
	uint32_t val1;
} pdsm_server_ipv4_address_type;

typedef struct pdsm_server_ipv6_address_type_struct
{
	uint32_t val1;
	//missing array needs work
} pdsm_server_ipv6_address_type;

typedef struct pdsm_server_url_address_type_struct
{
	uint8_t val0;
	char *byte_array;
} pdsm_server_url_address_type;

typedef struct pdsm_server_address_u_type_struct
{
	pdsm_server_address_e_type pdsm_server_address_e_type;
	void *address_struct;
} pdsm_server_address_u_type;

typedef struct pdsm_server_address_s_type_struct
{
	pdsm_server_address_e_type pdsm_server_address_e_type;
	pdsm_server_address_u_type *pdsm_server_address_u_type;
} pdsm_server_address_s_type;

typedef struct pdsm_pd_server_info_s_type_struct
{
	pdsm_server_option_e_type pdsm_server_option_e_type;
	pdsm_server_address_s_type *pdsm_server_address_s_type;
} pdsm_pd_server_info_s_type;

typedef struct pdsm_pd_sec_data_s_type_struct
{
	uint8_t val0;
	uint8_t val1;
	unsigned char *byte_array;
} pdsm_pd_sec_data_s_type;

typedef struct pdsm_pd_auth_s_type_struct
{
	pdsm_pd_sec_data_s_type *pdsm_pd_sec_data_s_type;
} pdsm_pd_auth_s_type;

typedef struct pdsm_srch_jgps_ppm_info_s_type_struct
{
	uint8_t val0;
	uint8_t val1;
} pdsm_srch_jgps_ppm_info_s_type;

typedef struct pdsm_srch_jgps_prm_info_s_type_struct
{
	uint8_t val0;
	uint8_t val1;
} pdsm_srch_jgps_prm_info_s_type;

typedef struct pdsm_pd_meas_mode_info_s_type_struct
{
	pdsm_sess_jgps_type_e_type pdsm_sess_jgps_type_e_type;
	pdsm_srch_jgps_ppm_info_s_type *pdsm_srch_jgps_ppm_info_s_type;
	pdsm_srch_jgps_prm_info_s_type *pdsm_srch_jgps_prm_info_s_type;
} pdsm_pd_meas_mode_info_s_type;

typedef struct pdsm_pd_option_s_type_struct
{
	pdsm_pd_session_e_type pdsm_pd_session_e_type;
	pdsm_pd_session_operation_e_type pdsm_pd_session_operation_e_type;
	pdsm_fix_rate_s_type *pdsm_fix_rate_s_type;
	pdsm_pd_server_info_s_type *pdsm_pd_server_info_s_type;
	uint32_t unknown;
	pdsm_pd_auth_s_type *pdsm_pd_auth_s_type;
	pdsm_pd_meas_mode_info_s_type *pdsm_pd_meas_mode_info_s_type;
} pdsm_pd_option_s_type;

typedef struct pdsm_get_position_args_struct
{
	pdsm_pd_cmd_cb_f_type pdsm_pd_cmd_cb_f_type;
	uint32_t pdsm_get_position_args_client_data_ptr;
	pdsm_pd_option_s_type *pdsm_pd_option_s_type;
	pdsm_pd_qos_type *pdsm_pd_qos_type;
	pdsm_client_id_type pdsm_client_id_type;
} pdsm_get_position_args;

typedef struct pdsm_end_session_args_struct
{
	pdsm_pd_cmd_cb_f_type pdsm_pd_cmd_cb_f_type;
	pdsm_pd_end_session_e_type pdsm_pd_end_session_e_type;
	uint32_t pdsm_end_session_args_client_data_ptr;
	pdsm_client_id_type pdsm_client_id_type;
} pdsm_end_session_args;

typedef struct pdsm_xtra_set_auto_download_params_args_struct
{
	pdsm_xtra_cmd_cb_f_type pdsm_xtra_cmd_cb_f_type;
	pdsm_client_id_type pdsm_client_id_type;
	uint32_t pdsm_xtra_set_auto_download_params_args_client_data_ptr;
	uint8_t enabled;
	uint16_t interval;
}pdsm_xtra_set_auto_download_params_args;

/*typedef struct pdsm_xtra_time_info {
    uint32_t uncertainty;
    uint64_t time_utc;
    bool_t ref_to_utc_time;
    bool_t force_flag;
} pdsm_xtra_time_info_type;*/

typedef struct pdsm_xtra_time_info_type_struct 
{
    uint32_t TimeUncMsec;
	uint64_t TimeMsec;
	uint8_t b_RefToUtcTime;
	uint8_t b_ForceFlag;
} pdsm_xtra_time_info_type;

typedef struct pdsm_xtra_inject_time_info_args_struct {
    pdsm_xtra_cmd_cb_f_type pdsm_xtra_cmd_cb_f_type;
	pdsm_client_id_type pdsm_client_id_type;
	uint32_t pdsm_xtra_inject_time_info_args_client_data_ptr;
	pdsm_xtra_time_info_type *pdsm_xtra_time_info_type;
} pdsm_xtra_inject_time_info_args;

typedef struct pdsm_xtra_set_data_args_struct
{
	pdsm_xtra_cmd_cb_f_type pdsm_xtra_cmd_cb_f_type;
	pdsm_client_id_type pdsm_client_id_type;
	uint32_t pdsm_xtra_set_data_args_client_data_ptr;
	unsigned char *xtra_data_ptr;
	uint32_t xtra_data_len;
	uint8_t part_number;
	uint8_t total_parts;
	pdsm_xtra_dc_status_e_type xtra_dc_status;
} pdsm_xtra_set_data_args;

typedef struct pdsm_xtra_query_data_validity_args_struct
{
	pdsm_xtra_cmd_cb_f_type pdsm_xtra_cmd_cb_f_type;
	pdsm_client_id_type pdsm_client_id_type;
	uint32_t pdsm_xtra_query_data_validity_args_client_data_ptr;
} pdsm_xtra_query_data_validity_args;

typedef struct pdsm_xtra_client_initiate_download_request_args_struct
{
	pdsm_xtra_cmd_cb_f_type pdsm_xtra_cmd_cb_f_type;
	pdsm_client_id_type pdsm_client_id_type;
	uint32_t pdsm_xtra_client_initiate_download_request_args_client_data_ptr;
} pdsm_xtra_client_initiate_download_request_args;

typedef struct pdsm_get_parameters_args_struct 
{
	pdsm_pa_cmd_cb_f_type pdsm_pa_cmd_cb_f_type;
	uint32_t pdsm_get_parameters_args_client_data_ptr;
	pdsm_pa_e_type pdsm_pa_e_type;
	pdsm_client_id_type pdsm_client_id_type;
} pdsm_get_parameters_args;

/*struct xtra_time_params {
    uint32_t *data;
    pdsm_xtra_time_info_type *time_info_ptr;
};*/

struct xtra_data_params {
    uint32_t *data;
    unsigned char *xtra_data_ptr;
    uint32_t part_len;
    uint8_t part;
    uint8_t total_parts;
};

struct xtra_validity_params {
    //Used in two functions pdsm_xtra_query_data_validity and pdsm_xtra_client_initiate_download_request
    uint32_t *data;
};

struct xtra_auto_params {
    uint32_t *data;
    uint8_t boolean;
    uint16_t interval;
};

static bool_t xdr_args(XDR *clnt, struct params *par) {
    int i;
    uint32_t val=0;
    for(i=0;par->length>i;++i)
        SEND_VAL(par->data[i]);
    return 1;
}

static bool_t xdr_result_int(XDR *clnt, uint32_t *result) {
    XDR_RECV_UINT32(clnt, result);
    return 1;
}

static bool_t xdr_rpc_pdsm_xtra_set_data_args(XDR *xdrs, pdsm_xtra_set_data_args *pdsm_xtra_set_data_args) 
{
    if (!xdr_u_long(xdrs, &pdsm_xtra_set_data_args->pdsm_xtra_cmd_cb_f_type))
		return 0;
	if (!xdr_int(xdrs, &pdsm_xtra_set_data_args->pdsm_client_id_type))
		return 0;
	if (!xdr_u_long(xdrs, &pdsm_xtra_set_data_args->pdsm_xtra_set_data_args_client_data_ptr))
		return 0;
	if (!xdr_u_long(xdrs, &pdsm_xtra_set_data_args->xtra_data_len))
		return 0;
	if (!xdr_bytes(xdrs, &pdsm_xtra_set_data_args->xtra_data_ptr, &pdsm_xtra_set_data_args->xtra_data_len, pdsm_xtra_set_data_args->xtra_data_len))
		return 0;
	if (!xdr_u_char(xdrs, &pdsm_xtra_set_data_args->part_number))
		return 0;
	if (!xdr_u_char(xdrs, &pdsm_xtra_set_data_args->total_parts))
		return 0;
	if (!xdr_u_long(xdrs, &pdsm_xtra_set_data_args->xtra_dc_status))
		return 0;
	
	return 1;
}

static bool_t xdr_rpc_pdsm_get_parameters_args(XDR *xdrs, pdsm_get_parameters_args *pdsm_get_parameters_args) 
{
    if (!xdr_u_long(xdrs, &pdsm_get_parameters_args->pdsm_pa_cmd_cb_f_type))
		return 0;
	if (!xdr_u_long(xdrs, &pdsm_get_parameters_args->pdsm_get_parameters_args_client_data_ptr))
		return 0;
	if (!xdr_u_long(xdrs, &pdsm_get_parameters_args->pdsm_pa_e_type))
		return 0;
	if (!xdr_int(xdrs, &pdsm_get_parameters_args->pdsm_client_id_type))
		return 0;
	
	return 1;
}

static bool_t xdr_rpc_pdsm_server_url_address_type(XDR *xdrs, pdsm_server_url_address_type *pdsm_server_url_address_type)
{
    if(!xdr_u_char(xdrs, &pdsm_server_url_address_type->val0))
		return 0;
	if(!xdr_opaque(xdrs, pdsm_server_url_address_type->byte_array, pdsm_server_url_address_type->val0)) //Not sure if val0 is the size
		return 0;
	
	return 1;
}

static bool_t xdr_rpc_pdsm_server_ipv4_address_type(XDR *xdrs, pdsm_server_ipv4_address_type *pdsm_server_ipv4_address_type)
{
    if (!xdr_u_long(xdrs, &pdsm_server_ipv4_address_type->val0))
		return 0;
	if (!xdr_u_long(xdrs, &pdsm_server_ipv4_address_type->val1))
		return 0;
	
	return 1;
}

static bool_t xdr_rpc_pdsm_pd_server_address_u_type(XDR *xdrs, pdsm_server_address_u_type *pdsm_server_address_u_type)
{
	if(!xdr_u_long(xdrs, &pdsm_server_address_u_type->pdsm_server_address_e_type))
		return 0;
	if(pdsm_server_address_u_type->pdsm_server_address_e_type == 0)
	{
		if(!xdr_rpc_pdsm_server_ipv4_address_type(xdrs, pdsm_server_address_u_type->address_struct))
			return 0;   
	}
	else if (pdsm_server_address_u_type->pdsm_server_address_e_type == 1)
	{
		if(!xdr_rpc_pdsm_server_url_address_type(xdrs, pdsm_server_address_u_type->address_struct))
			return 0;
	}
	else if(pdsm_server_address_u_type->pdsm_server_address_e_type == 2)
	{
		//ipv6
	}
	return 1;
}
static bool_t xdr_rpc_pdsm_pd_server_address_s_type(XDR *xdrs, pdsm_server_address_s_type *pdsm_server_address_s_type)
{
    if(!xdr_u_long(xdrs, &pdsm_server_address_s_type->pdsm_server_address_e_type))
		return 0;
	if(!xdr_rpc_pdsm_pd_server_address_u_type(xdrs, pdsm_server_address_s_type->pdsm_server_address_u_type))
		return 0;
	
	return 1;
}

static bool_t xdr_rpc_pdsm_pd_server_info_s_type(XDR *xdrs, pdsm_pd_server_info_s_type *pdsm_pd_server_info_s_type)
{
	if(!xdr_u_long(xdrs, &pdsm_pd_server_info_s_type->pdsm_server_option_e_type))
		return 0;
	if(!xdr_rpc_pdsm_pd_server_address_s_type(xdrs, pdsm_pd_server_info_s_type->pdsm_server_address_s_type))
		return 0;
	
	return 1;
}

static bool_t xdr_rpc_pdsm_pd_sec_update_rate_s_type(XDR *xdrs, pdsm_pd_sec_update_rate_s_type *pdsm_pd_sec_update_rate_s_type)
{
	if (!xdr_u_char(xdrs, pdsm_pd_sec_update_rate_s_type->val0))
		return 0;
	
	return 1;
}

static bool_t xdr_rpc_pdsm_pa_sbas_status_e_type(XDR *xdrs, pdsm_pa_sbas_status_e_type *pdsm_pa_sbas_status_e_type)
{
	if (!xdr_u_long(xdrs, pdsm_pa_sbas_status_e_type))
		return 0;
	
	return 1;
}

static bool_t xdr_rpc_pdsm_pa_nmea_config_s_type(XDR *xdrs, pdsm_pa_nmea_config_s_type *pdsm_pa_nmea_config_s_type)
{
	if (!xdr_u_long(xdrs, &pdsm_pa_nmea_config_s_type->pdsm_pa_nmea_port_e_type))
		return 0;
	if (!xdr_u_long(xdrs, &pdsm_pa_nmea_config_s_type->pdsm_pa_nmea_reporting_e_type))
		return 0;
	
	return 1;
}

static bool_t xdr_rpc_pdsm_pa_nmea_type(XDR *xdrs, pdsm_pa_nmea_type *pdsm_pa_nmea_type)
{
	if (!xdr_u_char(xdrs, pdsm_pa_nmea_type))
		return 0;
	
	return 1;
}

static bool_t xdr_rpc_pdsm_pa_mo_method_e_type(XDR *xdrs, pdsm_pa_mo_method_e_type *pdsm_pa_mo_method_e_type)
{
	if (!xdr_u_long(xdrs, pdsm_pa_mo_method_e_type))
		return 0;
	
	return 1;
}

static bool_t xdr_rpc_pdsm_pa_mt_lr_support_e_type(XDR *xdrs, pdsm_pa_mt_lr_support_e_type *pdsm_pa_mt_lr_support_e_type)
{
	if (!xdr_u_long(xdrs, pdsm_pa_mt_lr_support_e_type))
		return 0;
	
	return 1;
}

static bool_t xdr_rpc_pdsm_position_mode_type(XDR *xdrs, pdsm_position_mode_type *pdsm_position_mode_type)
{
	if (!xdr_u_long(xdrs, pdsm_position_mode_type))
		return 0;
	
	return 1;
}

static bool_t xdr_rpc_pdsm_gps_lock_e_type(XDR *xdrs, pdsm_gps_lock_e_type *pdsm_gps_lock_e_type)
{
	if (!xdr_u_long(xdrs, pdsm_gps_lock_e_type))
		return 0;
	
	return 1;
}

static bool_t xdr_rpc_pdsm_delete_params_type(XDR *xdrs, pdsm_delete_parms_type *pdsm_delete_parms_type)
{
	if (!xdr_u_long(xdrs, &pdsm_delete_parms_type->val0))
		return 0;
	if (!xdr_u_long(xdrs, &pdsm_delete_parms_type->val1))
		return 0;
	if (!xdr_u_long(xdrs, &pdsm_delete_parms_type->val2))
		return 0;
	if (!xdr_u_long(xdrs, &pdsm_delete_parms_type->val3))
		return 0;
	if (!xdr_u_long(xdrs, &pdsm_delete_parms_type->val4))
		return 0;
	if (!xdr_u_long(xdrs, &pdsm_delete_parms_type->val5))
		return 0;
	if (!xdr_u_long(xdrs, &pdsm_delete_parms_type->val6))
		return 0;
	if (!xdr_u_long(xdrs, &pdsm_delete_parms_type->val7))
		return 0;
	
	return 1;
}

static bool_t xdr_rpc_pdsm_pa_info(XDR *xdrs, pdsm_pa_info_type *pdsm_pa_info_type)
{
	if (!xdr_u_long(xdrs, &pdsm_pa_info_type->pa_set))
		return 0;
	switch(pdsm_pa_info_type->pa_set)
	{
		case 1:
			//xdr_rpc_pdsm_pm_jgpsone_app_s_type needs implementing
			return 0;
			break;
		case 2:
			if(!xdr_rpc_pdsm_gps_lock_e_type(xdrs, pdsm_pa_info_type->pa_ptr))
				return 0;
			break;
		case 3:
			if(!xdr_u_char(xdrs, pdsm_pa_info_type->pa_ptr))
				return 0;
			break;
		case 4:
			if(!xdr_rpc_pdsm_delete_params_type(xdrs, pdsm_pa_info_type->pa_ptr))
				return 0;
			break;
		case 5:
			if(!xdr_rpc_pdsm_position_mode_type(xdrs, pdsm_pa_info_type->pa_ptr))
				return 0;
			break;
		case 6:
			if(!xdr_rpc_pdsm_pa_mt_lr_support_e_type(xdrs, pdsm_pa_info_type->pa_ptr))
				return 0;
			break;
		case 7:
			if(!xdr_rpc_pdsm_pa_mo_method_e_type(xdrs, pdsm_pa_info_type->pa_ptr))
				return 0;
			break;
		case 8:
			if(!xdr_rpc_pdsm_pa_nmea_type(xdrs, pdsm_pa_info_type->pa_ptr))
				return 0;
			break;
		case 9:
			if(!xdr_rpc_pdsm_pd_server_address_s_type(xdrs, pdsm_pa_info_type->pa_ptr))
				return 0;
			break;
		case 10:
			if(!xdr_rpc_pdsm_pd_server_address_s_type(xdrs, pdsm_pa_info_type->pa_ptr))
				return 0;
			break;
		case 11:
			if(!xdr_rpc_pdsm_pd_server_address_s_type(xdrs, pdsm_pa_info_type->pa_ptr))
				return 0;
			break;
		case 12:
			//xdr_rpc_pdsm_pd_ssd_s_type needs implementing
			return 0;
			break;
		case 13:
			if(!xdr_rpc_pdsm_pd_sec_update_rate_s_type(xdrs, pdsm_pa_info_type->pa_ptr))
				return 0;
			break;
		case 14:
			if(!xdr_u_char(xdrs, pdsm_pa_info_type->pa_ptr))
				return 0;
			break;
		case 15:
			if(!xdr_rpc_pdsm_pa_nmea_config_s_type(xdrs, pdsm_pa_info_type->pa_ptr))
				return 0;
			break;
		case 16:
			//xdr_rpc_pdsm_efs_data_s_type needs implementing
			return 0;
			break;
		case 17:
			if(!xdr_u_char(xdrs, pdsm_pa_info_type->pa_ptr))
				return 0;
			break;
		case 18:
			if(!xdr_rpc_pdsm_pa_sbas_status_e_type(xdrs, pdsm_pa_info_type->pa_ptr))
				return 0;
			break;
		case 19:
			if(!xdr_u_char(xdrs, pdsm_pa_info_type->pa_ptr))
				return 0;
			break;
		case 20:
			if(!xdr_u_char(xdrs, pdsm_pa_info_type->pa_ptr))
				return 0;
			break;
		case 21:
			if(!xdr_u_char(xdrs, pdsm_pa_info_type->pa_ptr))
				return 0;
			break;
		case 22:
			if(!xdr_u_char(xdrs, pdsm_pa_info_type->pa_ptr))
				return 0;
			break;
		default:
			return 0;
	}
	
	return 1;
}
static bool_t xdr_rpc_pdsm_set_parameters_args(XDR *xdrs, pdsm_set_parameters_args *pdsm_set_parameters_args) 
{    
	if (!xdr_u_long(xdrs, &pdsm_set_parameters_args->pdsm_pa_cmd_cb_f_type))
		return 0;
	if (!xdr_u_long(xdrs, &pdsm_set_parameters_args->pdsm_set_parameters_args_client_data_ptr))
		return 0;
	if (!xdr_u_long(xdrs, &pdsm_set_parameters_args->pdsm_pa_e_type))
		return 0;
	if (!xdr_pointer(xdrs, &pdsm_set_parameters_args->pdsm_pa_info_type, sizeof(pdsm_pa_info_type), xdr_rpc_pdsm_pa_info))
		return 0;
	if (!xdr_int(xdrs, &pdsm_set_parameters_args->pdsm_client_id_type))
		return 0;
	
	return 1;
}

static bool_t xdr_rpc_pdsm_pd_qos_type(XDR *xdrs, pdsm_pd_qos_type *pdsm_pd_qos_type) 
{
	if (!xdr_u_long(xdrs, &pdsm_pd_qos_type->accuracy))
		return 0;
	if (!xdr_u_char(xdrs, &pdsm_pd_qos_type->performance))
		return 0;
	
	return 1;
}

static bool_t xdr_rpc_pdsm_pd_fix_rate_s_type(XDR *xdrs, pdsm_fix_rate_s_type *pdsm_fix_rate_s_type)
{
	if(!xdr_u_long(xdrs, &pdsm_fix_rate_s_type->num_fixes))
		return 0;
	if(!xdr_u_long(xdrs, &pdsm_fix_rate_s_type->time_between_fixes))
		return 0;
	
	return 1;
}

static bool_t xdr_rpc_pdsm_pd_sec_data_s_type(XDR *xdrs, pdsm_pd_sec_data_s_type *pdsm_pd_sec_data_s_type)
{
	if(!xdr_u_long(xdrs, &pdsm_pd_sec_data_s_type->val0))
		return 0;
	if(!xdr_u_long(xdrs, &pdsm_pd_sec_data_s_type->val1))
		return 0;
	if(!xdr_opaque(xdrs, pdsm_pd_sec_data_s_type->byte_array, 20))
		return 0;
	
	return 1;
}

static bool_t xdr_rpc_pdsm_pd_auth_s_type(XDR *xdrs, pdsm_pd_auth_s_type *pdsm_pd_auth_s_type)
{
	if(!xdr_rpc_pdsm_pd_sec_data_s_type(xdrs, pdsm_pd_auth_s_type->pdsm_pd_sec_data_s_type))
		return 0;
	
	return 1;
}

static bool_t xdr_rpc_pdsm_srch_jgps_ppm_info_s_type(XDR *xdrs, pdsm_srch_jgps_ppm_info_s_type *pdsm_srch_jgps_ppm_info_s_type)
{
	if(!xdr_u_char(xdrs, &pdsm_srch_jgps_ppm_info_s_type->val0))
		return 0;
	if(!xdr_u_char(xdrs, &pdsm_srch_jgps_ppm_info_s_type->val1))
		return 0;
	
	return 1;
}

static bool_t xdr_rpc_pdsm_srch_jgps_prm_info_s_type(XDR *xdrs, pdsm_srch_jgps_prm_info_s_type *pdsm_srch_jgps_prm_info_s_type)
{
	if(!xdr_u_char(xdrs, &pdsm_srch_jgps_prm_info_s_type->val0))
		return 0;
	if(!xdr_u_char(xdrs, &pdsm_srch_jgps_prm_info_s_type->val1))
		return 0;
	
	return 1;
}

static bool_t xdr_rpc_pdsm_pd_meas_mode_info_s_type( XDR *xdrs, pdsm_pd_meas_mode_info_s_type *pdsm_pd_meas_mode_info_s_type)
{
	if(!xdr_u_long(xdrs, &pdsm_pd_meas_mode_info_s_type->pdsm_sess_jgps_type_e_type))
		return 0;
	if(!xdr_rpc_pdsm_srch_jgps_ppm_info_s_type(xdrs, pdsm_pd_meas_mode_info_s_type->pdsm_srch_jgps_ppm_info_s_type))
		return 0;
	if(!xdr_rpc_pdsm_srch_jgps_prm_info_s_type(xdrs, pdsm_pd_meas_mode_info_s_type->pdsm_srch_jgps_prm_info_s_type))
		return 0;
	
	return 1;
}

static bool_t xdr_rpc_pdsm_pd_option_s_type(XDR *xdrs, pdsm_pd_option_s_type *pdsm_pd_option_s_type) 
{	
	if(!xdr_u_long(xdrs, &pdsm_pd_option_s_type->pdsm_pd_session_e_type))
		return 0;
	if(!xdr_u_long(xdrs, &pdsm_pd_option_s_type->pdsm_pd_session_operation_e_type))
		return 0;
	if(!xdr_rpc_pdsm_pd_fix_rate_s_type(xdrs, pdsm_pd_option_s_type->pdsm_fix_rate_s_type))
		return 0;
	if(!xdr_rpc_pdsm_pd_server_info_s_type(xdrs, pdsm_pd_option_s_type->pdsm_pd_server_info_s_type))
		return 0;
	if(!xdr_u_long(xdrs, &pdsm_pd_option_s_type->unknown))
		return 0;
	if(!xdr_rpc_pdsm_pd_auth_s_type(xdrs, pdsm_pd_option_s_type->pdsm_pd_auth_s_type))
		return 0;
	if(!xdr_rpc_pdsm_pd_meas_mode_info_s_type(xdrs, pdsm_pd_option_s_type->pdsm_pd_meas_mode_info_s_type))
		return 0;
	
	return 1;
}

static bool_t xdr_rpc_pdsm_get_position_args(XDR *xdrs, pdsm_get_position_args *pdsm_get_position_args) 
{
	if (!xdr_u_long(xdrs, &pdsm_get_position_args->pdsm_pd_cmd_cb_f_type))
		return 0;
	if (!xdr_u_long(xdrs, &pdsm_get_position_args->pdsm_get_position_args_client_data_ptr))
		return 0;
	if (!xdr_pointer(xdrs, &pdsm_get_position_args->pdsm_pd_option_s_type, sizeof(pdsm_pd_option_s_type), (xdrproc_t)xdr_rpc_pdsm_pd_option_s_type))
		return 0;
	if (!xdr_pointer(xdrs, &pdsm_get_position_args->pdsm_pd_qos_type, sizeof(pdsm_pd_qos_type), (xdrproc_t)xdr_rpc_pdsm_pd_qos_type))
		return 0;
	if (!xdr_int(xdrs, &pdsm_get_position_args->pdsm_client_id_type))
		return 0;
	
	return 1;
}

static bool_t xdr_rpc_pdsm_end_session_args(XDR *xdrs, pdsm_end_session_args *pdsm_end_session_args) 
{
	if (!xdr_u_long(xdrs, &pdsm_end_session_args->pdsm_pd_cmd_cb_f_type))
		return 0;
	if (!xdr_u_long(xdrs, &pdsm_end_session_args->pdsm_pd_end_session_e_type))
		return 0;
	if (!xdr_u_long(xdrs, &pdsm_end_session_args->pdsm_end_session_args_client_data_ptr))
		return 0;
	if (!xdr_int(xdrs, &pdsm_end_session_args->pdsm_client_id_type))
		return 0;
	
	return 1;
}

static bool_t xdr_rpc_pdsm_xtra_set_auto_download_params_args(XDR *xdrs, pdsm_xtra_set_auto_download_params_args *pdsm_xtra_set_auto_download_params_args) 
{
	if (!xdr_u_long(xdrs, &pdsm_xtra_set_auto_download_params_args->pdsm_xtra_cmd_cb_f_type))
		return 0;
	if (!xdr_int(xdrs, &pdsm_xtra_set_auto_download_params_args->pdsm_client_id_type))
		return 0;
	if (!xdr_u_long(xdrs, &pdsm_xtra_set_auto_download_params_args->pdsm_xtra_set_auto_download_params_args_client_data_ptr))
		return 0;
	if (!xdr_u_char(xdrs, &pdsm_xtra_set_auto_download_params_args->enabled))
		return 0;
	if (!xdr_u_short(xdrs, &pdsm_xtra_set_auto_download_params_args->interval))
		return 0;

	return 1;
}

bool_t xdr_rpc_pdsm_xtra_time_info(XDR *xdrs, pdsm_xtra_time_info_type *time_info_ptr) 
{	
	if (!xdr_u_quad_t(xdrs, &time_info_ptr->TimeMsec))
		return 0;
	if (!xdr_u_long(xdrs, &time_info_ptr->TimeUncMsec))
		return 0;
	if (!xdr_u_char(xdrs, &time_info_ptr->b_RefToUtcTime))
		return 0;
	if (!xdr_u_char(xdrs, &time_info_ptr->b_ForceFlag))
		return 0;

	return 1;
}

static bool_t xdr_rpc_pdsm_xtra_inject_time_info_args(XDR *xdrs, pdsm_xtra_inject_time_info_args *pdsm_xtra_inject_time_info_args) 
{
	if (!xdr_u_long(xdrs, &pdsm_xtra_inject_time_info_args->pdsm_xtra_cmd_cb_f_type))
		return 0;
	if (!xdr_int(xdrs, &pdsm_xtra_inject_time_info_args->pdsm_client_id_type))
		return 0;
	if (!xdr_u_long(xdrs, &pdsm_xtra_inject_time_info_args->pdsm_xtra_inject_time_info_args_client_data_ptr))
		return 0;
	if (!xdr_pointer(xdrs, &pdsm_xtra_inject_time_info_args->pdsm_xtra_time_info_type, sizeof(pdsm_xtra_time_info_type), (xdrproc_t) xdr_rpc_pdsm_xtra_time_info))
		return 0;

	return 1;
}

static bool_t xdr_rpc_pdsm_client_init_args(XDR *xdrs, pdsm_client_init_args *pdsm_client_init_args)
{
	if(!xdr_u_long(xdrs, &pdsm_client_init_args->pdsm_client_type_e))
		return 0;
	
	return 1;
}

static bool_t xdr_rpc_pdsm_client_act_args(XDR *xdrs, pdsm_client_act_args *pdsm_client_act_args)
{
	if(!xdr_u_long(xdrs, &pdsm_client_act_args->pdsm_client_id_type))
		return 0;
	
	return 1;
}

static bool_t xdr_rpc_pdsm_client_deact_args(XDR *xdrs, pdsm_client_deact_args *pdsm_client_deact_args)
{
	if(!xdr_u_long(xdrs, &pdsm_client_deact_args->pdsm_client_id_type))
		return 0;
	
	return 1;
}

static bool_t xdr_rpc_pdsm_client_release_args(XDR *xdrs, pdsm_client_release_args *pdsm_client_release_args)
{
	if(!xdr_u_long(xdrs, &pdsm_client_release_args->pdsm_client_id_type))
		return 0;
	
	return 1;
}

static bool_t xdr_rpc_pdsm_client_pa_reg_args(XDR *xdrs, pdsm_client_pa_reg_args *pdsm_client_pa_reg_args)
{
	if(!xdr_int(xdrs, &pdsm_client_pa_reg_args->pdsm_client_id_type))
		return 0;
	if(!xdr_u_long(xdrs, &pdsm_client_pa_reg_args->pdsm_client_pa_reg_args_client_data_ptr))
		return 0;
	if(!xdr_u_long(xdrs, &pdsm_client_pa_reg_args->pdsm_pa_event_f_type))
		return 0;
	if(!xdr_u_long(xdrs, &pdsm_client_pa_reg_args->pdsm_client_event_reg_e_type))
		return 0;
	if(!xdr_u_long(xdrs, &pdsm_client_pa_reg_args->pdsm_pa_event_type))
		return 0;
	if(!xdr_u_long(xdrs, &pdsm_client_pa_reg_args->pdsm_pa_cmd_err_f_type))
		return 0;
	
	return 1;
}

static bool_t xdr_rpc_pdsm_client_pd_reg_args(XDR *xdrs, pdsm_client_pd_reg_args *pdsm_client_pd_reg_args)
{
	if(!xdr_int(xdrs, &pdsm_client_pd_reg_args->pdsm_client_id_type))
		return 0;
	if(!xdr_u_long(xdrs, &pdsm_client_pd_reg_args->pdsm_client_pd_reg_args_client_data_ptr))
		return 0;
	if(!xdr_u_long(xdrs, &pdsm_client_pd_reg_args->pdsm_pd_event_f_type))
		return 0;
	if(!xdr_u_long(xdrs, &pdsm_client_pd_reg_args->pdsm_client_event_reg_e_type))
		return 0;
	if(!xdr_u_long(xdrs, &pdsm_client_pd_reg_args->pdsm_pd_event_type))
		return 0;
	if(!xdr_u_long(xdrs, &pdsm_client_pd_reg_args->pdsm_pd_cmd_err_f_type))
		return 0;
	
	return 1;
}

static bool_t xdr_rpc_pdsm_client_xtra_reg_args(XDR *xdrs, pdsm_client_xtra_reg_args *pdsm_client_xtra_reg_args)
{
	if(!xdr_int(xdrs, &pdsm_client_xtra_reg_args->pdsm_client_id_type))
		return 0;
	if(!xdr_u_long(xdrs, &pdsm_client_xtra_reg_args->pdsm_client_xtra_reg_args_client_data_ptr))
		return 0;
	if(!xdr_u_long(xdrs, &pdsm_client_xtra_reg_args->pdsm_xtra_event_f_type))
		return 0;
	if(!xdr_u_long(xdrs, &pdsm_client_xtra_reg_args->pdsm_client_event_reg_e_type))
		return 0;
	if(!xdr_u_long(xdrs, &pdsm_client_xtra_reg_args->pdsm_xtra_event_type))
		return 0;
	if(!xdr_u_long(xdrs, &pdsm_client_xtra_reg_args->pdsm_xtra_cmd_err_f_type))
		return 0;
	
	return 1;
}

static bool_t xdr_rpc_pdsm_client_lcs_reg_args(XDR *xdrs, pdsm_client_lcs_reg_args *pdsm_client_lcs_reg_args)
{
	if(!xdr_int(xdrs, &pdsm_client_lcs_reg_args->pdsm_client_id_type))
		return 0;
	if(!xdr_u_long(xdrs, &pdsm_client_lcs_reg_args->pdsm_client_lcs_reg_args_client_data_ptr))
		return 0;
	if(!xdr_u_long(xdrs, &pdsm_client_lcs_reg_args->pdsm_lcs_event_f_type))
		return 0;
	if(!xdr_u_long(xdrs, &pdsm_client_lcs_reg_args->pdsm_client_event_reg_e_type))
		return 0;
	if(!xdr_u_long(xdrs, &pdsm_client_lcs_reg_args->pdsm_lcs_event_type))
		return 0;
	if(!xdr_u_long(xdrs, &pdsm_client_lcs_reg_args->pdsm_lcs_cmd_err_f_type))
		return 0;
	
	return 1;
}

static bool_t xdr_rpc_pdsm_client_ext_status_reg_args(XDR *xdrs, pdsm_client_ext_status_reg_args *pdsm_client_ext_status_reg_args)
{
	if(!xdr_int(xdrs, &pdsm_client_ext_status_reg_args->pdsm_client_id_type))
		return 0;
	if(!xdr_u_long(xdrs, &pdsm_client_ext_status_reg_args->pdsm_client_ext_status_reg_args_client_data_ptr))
		return 0;
	if(!xdr_u_long(xdrs, &pdsm_client_ext_status_reg_args->pdsm_ext_status_event_f_type))
		return 0;
	if(!xdr_u_long(xdrs, &pdsm_client_ext_status_reg_args->pdsm_client_event_reg_e_type))
		return 0;
	if(!xdr_u_long(xdrs, &pdsm_client_ext_status_reg_args->pdsm_ext_status_event_type))
		return 0;
	if(!xdr_u_long(xdrs, &pdsm_client_ext_status_reg_args->pdsm_ext_status_cmd_err_f_type))
		return 0;
	
	return 1;
}

static bool_t xdr_rpc_pdsm_atl_l2_proxy_reg_args(XDR *xdrs, pdsm_alt_l2_proxy_reg_args *pdsm_alt_l2_proxy_reg_args)
{
	if(!xdr_u_long(xdrs, &pdsm_alt_l2_proxy_reg_args->pdsm_atl_type))
		return 0;
	if(!xdr_u_long(xdrs, &pdsm_alt_l2_proxy_reg_args->pdsm_atl_open_f_type))
		return 0;
	if(!xdr_u_long(xdrs, &pdsm_alt_l2_proxy_reg_args->pdsm_atl_close_f_type))
		return 0;
	
	return 1;
}

static bool_t xdr_rpc_pdsm_atl_dns_proxy_reg_args(XDR *xdrs, pdsm_atl_dns_proxy_reg_args *pdsm_atl_dns_proxy_reg_args)
{
	if(!xdr_u_long(xdrs, &pdsm_atl_dns_proxy_reg_args->pdsm_atl_type))
		return 0;
	if(!xdr_u_long(xdrs, &pdsm_atl_dns_proxy_reg_args->pdsm_atl_dns_lookup_f_type))
		return 0;
	
	return 1;
}

static bool_t xdr_rpc_pdsm_xtra_client_initiate_download_request_args(XDR *xdrs, pdsm_xtra_client_initiate_download_request_args *pdsm_xtra_client_initiate_download_request_args)
{
	if (!xdr_u_long(xdrs, &pdsm_xtra_client_initiate_download_request_args->pdsm_xtra_cmd_cb_f_type))
		return 0;
	if (!xdr_int(xdrs, &pdsm_xtra_client_initiate_download_request_args->pdsm_client_id_type))
		return 0;
	if (!xdr_u_long(xdrs, &pdsm_xtra_client_initiate_download_request_args->pdsm_xtra_client_initiate_download_request_args_client_data_ptr))
		return 0;
	
	return 1;
}

static int pdsm_client_init(int client) 
{
	uint32_t res;
	pdsm_client_init_args pdsm_client_init_args;
	pdsm_client_init_args.pdsm_client_type_e = client;
	if(clnt_call(_clnt, 0x2, xdr_rpc_pdsm_client_init_args, &pdsm_client_init_args, xdr_result_int, &res, timeout)) {
		D("pdsm_client_init(%d) failed\n", client);
		exit(-1);
	}
	D("pdsm_client_init(%d)=%u\n", client, res);
	client_IDs[client]=res;
	return 0;
}

static int pdsm_client_release(int client) 
{
	uint32_t res;
	pdsm_client_release_args pdsm_client_release_args;
	pdsm_client_release_args.pdsm_client_id_type = client_IDs[client];
	if(clnt_call(_clnt, 0x3, xdr_rpc_pdsm_client_release_args, &pdsm_client_release_args, xdr_result_int, &res, timeout)) {
		D("pdsm_client_release(%d) failed\n", client_IDs[client]);
		exit(-1);
	}
	D("pdsm_client_release(%d)=%u\n", client_IDs[client], res);
	client_IDs[client]=res;
	return 0;
}

int pdsm_atl_l2_proxy_reg(uint32_t val0, uint32_t val1, uint32_t val2) 
{
	uint32_t res;
	pdsm_alt_l2_proxy_reg_args pdsm_alt_l2_proxy_reg_args;
	pdsm_alt_l2_proxy_reg_args.pdsm_atl_type = val0;
	pdsm_alt_l2_proxy_reg_args.pdsm_atl_open_f_type = val1;
	pdsm_alt_l2_proxy_reg_args.pdsm_atl_close_f_type = val2;
	if(clnt_call(_clnt_atl, 0x3, xdr_rpc_pdsm_atl_l2_proxy_reg_args, &pdsm_alt_l2_proxy_reg_args, xdr_result_int, &res, timeout)) {
		D("pdsm_atl_l2_proxy_reg(%u, %u, %u) failed\n", val0, val1, val2);
		exit(-1);
	}
	D("pdsm_atl_l2_proxy_reg(%u, %u, %u)=%u\n", val0, val1, val2, res);
	return res;
}

int pdsm_atl_dns_proxy_reg(uint32_t val0, uint32_t val1) 
{
	uint32_t res;
	pdsm_atl_dns_proxy_reg_args pdsm_atl_dns_proxy_reg_args;
	pdsm_atl_dns_proxy_reg_args.pdsm_atl_type = val0;
	pdsm_atl_dns_proxy_reg_args.pdsm_atl_dns_lookup_f_type = val1;
	if(clnt_call(_clnt_atl, 0x6, xdr_rpc_pdsm_atl_dns_proxy_reg_args, &pdsm_atl_dns_proxy_reg_args, xdr_result_int, &res, timeout)) {
		D("pdsm_atl_dns_proxy_reg(%u, %u) failed\n", val0, val1);
		exit(-1);
	}
	D("pdsm_atl_dns_proxy(%u, %u)=%u\n", val0, val1, res);
	return res;
}

int pdsm_client_pd_reg(int client, uint32_t val0, uint32_t val1, uint32_t val2, uint32_t val3, uint32_t val4) 
{
	uint32_t res;
	pdsm_client_pd_reg_args pdsm_client_pd_reg_args;
	pdsm_client_pd_reg_args.pdsm_client_id_type = client_IDs[client];
	pdsm_client_pd_reg_args.pdsm_client_pd_reg_args_client_data_ptr = val0;
	pdsm_client_pd_reg_args.pdsm_pd_event_f_type = val1;
	pdsm_client_pd_reg_args.pdsm_client_event_reg_e_type = val2;
	pdsm_client_pd_reg_args.pdsm_pd_event_type = val3;
	pdsm_client_pd_reg_args.pdsm_pd_cmd_err_f_type = val4;
	if(clnt_call(_clnt, 0x4, xdr_rpc_pdsm_client_pd_reg_args, &pdsm_client_pd_reg_args, xdr_result_int, &res, timeout)) {
		D("pdsm_client_pd_reg(%d, %u, %u, %u, %u, %u) failed\n", client, val0, val1, val2, val3, val4);
		exit(-1);
	}
	D("pdsm_client_pd_reg(%u, %u, %u, %u, %u, %u)=%u\n", client, val0, val1, val2, val3, val4, res);
	return res;
}

int pdsm_client_pa_reg(int client, uint32_t val0, uint32_t val1, uint32_t val2, uint32_t val3, uint32_t val4) 
{
	uint32_t res;
	pdsm_client_pa_reg_args pdsm_client_pa_reg_args;
	pdsm_client_pa_reg_args.pdsm_client_id_type = client_IDs[client];
	pdsm_client_pa_reg_args.pdsm_client_pa_reg_args_client_data_ptr = val0;
	pdsm_client_pa_reg_args.pdsm_pa_event_f_type = val1;
	pdsm_client_pa_reg_args.pdsm_client_event_reg_e_type = val2;
	pdsm_client_pa_reg_args.pdsm_pa_event_type = val3;
	pdsm_client_pa_reg_args.pdsm_pa_cmd_err_f_type = val4;
	if(clnt_call(_clnt, 0x5, xdr_rpc_pdsm_client_pa_reg_args, &pdsm_client_pa_reg_args, xdr_result_int, &res, timeout)) {
		D("pdsm_client_pa_reg(%d, %u, %u, %u, %u, %u) failed\n", client, val0, val1, val2, val3, val4);
		exit(-1);
	}
	D("pdsm_client_pa_reg(%d, %u, %u, %u, %u, %u)=%u\n", client, val0, val1, val2, val3, val4, res);
	return res;
}

int pdsm_client_lcs_reg(int client, uint32_t val0, uint32_t val1, uint32_t val2, uint32_t val3, uint32_t val4) 
{
	uint32_t res;
	pdsm_client_lcs_reg_args pdsm_client_lcs_reg_args;
	pdsm_client_lcs_reg_args.pdsm_client_id_type = client_IDs[client];
	pdsm_client_lcs_reg_args.pdsm_client_lcs_reg_args_client_data_ptr = val0;
	pdsm_client_lcs_reg_args.pdsm_lcs_event_f_type = val1;
	pdsm_client_lcs_reg_args.pdsm_client_event_reg_e_type = val2;
	pdsm_client_lcs_reg_args.pdsm_lcs_event_type = val3;
	pdsm_client_lcs_reg_args.pdsm_lcs_cmd_err_f_type = val4;
	if(clnt_call(_clnt, 0x6, xdr_rpc_pdsm_client_lcs_reg_args, &pdsm_client_lcs_reg_args, xdr_result_int, &res, timeout)) {
		D("pdsm_client_lcs_reg(%d, %u, %u, %u, %u, %u) failed\n", client, val0, val1, val2, val3, val4);
		exit(-1);
	}
	D("pdsm_client_lcs_reg(%d, %u, %u, %u, %u, %u)=%u\n", client, val0, val1, val2, val3, val4, res);
	return res;
}

int pdsm_client_ext_status_reg(int client, uint32_t val0, uint32_t val1, uint32_t val2, uint32_t val3, uint32_t val4) 
{
	uint32_t res;
	pdsm_client_ext_status_reg_args pdsm_client_ext_status_reg_args;
	pdsm_client_ext_status_reg_args.pdsm_client_id_type = client_IDs[client];
	pdsm_client_ext_status_reg_args.pdsm_client_ext_status_reg_args_client_data_ptr = val0;
	pdsm_client_ext_status_reg_args.pdsm_ext_status_event_f_type = val1;
	pdsm_client_ext_status_reg_args.pdsm_client_event_reg_e_type = val2;
	pdsm_client_ext_status_reg_args.pdsm_ext_status_event_type = val3;
	pdsm_client_ext_status_reg_args.pdsm_ext_status_cmd_err_f_type = val4;
	if(clnt_call(_clnt, 0x8, xdr_rpc_pdsm_client_ext_status_reg_args, &pdsm_client_ext_status_reg_args, xdr_result_int, &res, timeout)) {
		D("pdsm_client_ext_status_reg(%d, %u, %u, %u, %u, %u) failed\n", client, val0, val1, val2, val3, val4);
		exit(-1);
	}
	D("pdsm_client_ext_status_reg(%d, %u, %u, %u, %u, %u)=%u\n", client, val0, val1, val2, val3, val4, res);
	return res;
}

int pdsm_client_xtra_reg(int client, uint32_t val0, uint32_t val1, uint32_t val2, uint32_t val3, uint32_t val4) 
{
	uint32_t res;
	pdsm_client_xtra_reg_args pdsm_client_xtra_reg_args;
	pdsm_client_xtra_reg_args.pdsm_client_id_type = client_IDs[client];
	pdsm_client_xtra_reg_args.pdsm_client_xtra_reg_args_client_data_ptr = val0;
	pdsm_client_xtra_reg_args.pdsm_xtra_event_f_type = val1;
	pdsm_client_xtra_reg_args.pdsm_client_event_reg_e_type = val2;
	pdsm_client_xtra_reg_args.pdsm_xtra_event_type = val3;
	pdsm_client_xtra_reg_args.pdsm_xtra_cmd_err_f_type = val4;
	if(clnt_call(_clnt, 0x7, xdr_rpc_pdsm_client_xtra_reg_args, &pdsm_client_xtra_reg_args, xdr_result_int, &res, timeout)) {
		D("pdsm_client_xtra_reg(%d, %u, %u, %u, %u, %u) failed\n", client, val0, val1, val2, val3, val4 );
		exit(-1);
	}
	D("pdsm_client_xtra_reg(%d, %u, %u, %u, %u, %u)=%u\n", client, val0, val1, val2, val3, val4, res);
	return res;
}

int pdsm_client_act(int client) 
{
	uint32_t res;
	pdsm_client_act_args pdsm_client_act_args;
	pdsm_client_act_args.pdsm_client_id_type = client_IDs[client];
	if(clnt_call(_clnt, 0x9, xdr_rpc_pdsm_client_act_args, &pdsm_client_act_args, xdr_result_int, &res, timeout)) {
		D("pdsm_client_act(%d) failed\n", client);
		exit(-1);
	}
	D("pdsm_client_act(%d)=%u\n", client, res);
	return res;
}

int pdsm_client_deact(int client) 
{
	uint32_t res;
	pdsm_client_deact_args pdsm_client_deact_args;
	pdsm_client_deact_args.pdsm_client_id_type = client_IDs[client];
	if(clnt_call(_clnt, 0x9, xdr_rpc_pdsm_client_deact_args, &pdsm_client_deact_args, xdr_result_int, &res, timeout)) {
		D("pdsm_client_deact(%d) failed\n", client);
		exit(-1);
	}
	D("pdsm_client_deact(%d)=%u\n", client, res);
	return res;
}

int pdsm_xtra_set_data(uint32_t val0, int client, uint32_t val2, unsigned char *xtra_data_ptr, uint32_t xtra_data_len, uint8_t part_number, uint8_t total_parts, uint32_t xtra_dc_status) 
{
	uint32_t res;
	pdsm_xtra_set_data_args pdsm_xtra_set_data_args;
	pdsm_xtra_set_data_args.pdsm_xtra_cmd_cb_f_type = val0;
	pdsm_xtra_set_data_args.pdsm_client_id_type = client_IDs[client];
	pdsm_xtra_set_data_args.pdsm_xtra_set_data_args_client_data_ptr = val2;
	pdsm_xtra_set_data_args.xtra_data_ptr = xtra_data_ptr;
	pdsm_xtra_set_data_args.xtra_data_len = xtra_data_len;
	pdsm_xtra_set_data_args.part_number = part_number;
	pdsm_xtra_set_data_args.total_parts = total_parts;
	pdsm_xtra_set_data_args.xtra_dc_status = xtra_dc_status;
	if(clnt_call(_clnt, 0x1A, xdr_rpc_pdsm_xtra_set_data_args, &pdsm_xtra_set_data_args, xdr_result_int, &res, timeout)) {
		D("pdsm_xtra_set_data(%u, %d, %u, 0x%x, %u, %u, %u, %u) failed\n", val0, client, val2, (int) xtra_data_ptr, xtra_data_len, part_number, total_parts, xtra_dc_status);
		exit(-1);
	}
	D("pdsm_xtra_set_data(%u, %d, %u, 0x%x, %u, %u, %u, %u)=%u\n", val0, client, val2, (int) xtra_data_ptr, xtra_data_len, part_number, total_parts, xtra_dc_status, res);
	return res;
}

static bool_t xdr_rpc_pdsm_query_data_validity_args(XDR *xdrs, pdsm_xtra_query_data_validity_args *pdsm_xtra_query_data_validity_args) 
{
    if (!xdr_u_long(xdrs, &pdsm_xtra_query_data_validity_args->pdsm_xtra_cmd_cb_f_type))
		return 0;
	if (!xdr_int(xdrs, &pdsm_xtra_query_data_validity_args->pdsm_client_id_type))
		return 0;
	if (!xdr_u_long(xdrs, &pdsm_xtra_query_data_validity_args->pdsm_xtra_query_data_validity_args_client_data_ptr))
		return 0;

	return 1;
}

/*bool_t xdr_pdsm_xtra_time_info(XDR *xdrs, pdsm_xtra_time_info_type *time_info_ptr) {
    //D("%s() is called: %lld, %d", __FUNCTION__, time_info_ptr->time_utc, time_info_ptr->uncertainty);

    if (!xdr_u_quad_t(xdrs, &time_info_ptr->time_utc))
        return 0;
    if (!xdr_u_long(xdrs, &time_info_ptr->uncertainty))
        return 0;
    if (!xdr_u_char(xdrs, &time_info_ptr->ref_to_utc_time))
        return 0;
    if (!xdr_u_char(xdrs, &time_info_ptr->force_flag))
        return 0;

    return 1;
}*/

static bool_t xdr_xtra_time_args(XDR *xdrs, struct xtra_time_params *xtra_time) {
    //D("%s() is called", __FUNCTION__);

    if (!xdr_u_long(xdrs, &xtra_time->data[0]))
        return 0;
    if (!xdr_int(xdrs, &xtra_time->data[1]))
        return 0;
    if (!xdr_u_long(xdrs, &xtra_time->data[2]))
        return 0;
    if (!xdr_pointer(xdrs, (char **)&xtra_time->time_info_ptr, sizeof(pdsm_xtra_time_info_type), (xdrproc_t) xdr_pdsm_xtra_time_info))
        return 0;

    return 1;
}

static bool_t xdr_xtra_validity_args(XDR *xdrs, struct xtra_validity_params *xtra_validity) {
    //Used in two functions pdsm_xtra_query_data_validity and pdsm_xtra_client_initiate_download_request

    if (!xdr_u_long(xdrs, &xtra_validity->data[0]))
        return 0;
    if (!xdr_int(xdrs, &xtra_validity->data[1]))
        return 0;
    if (!xdr_u_long(xdrs, &xtra_validity->data[2]))
        return 0;

    return 1;
}

static bool_t xdr_xtra_auto_args(XDR *xdrs, struct xtra_auto_params *xtra_auto) {

    if (!xdr_u_long(xdrs, &xtra_auto->data[0]))
        return 0;
    if (!xdr_int(xdrs, &xtra_auto->data[1]))
        return 0;
    if (!xdr_u_long(xdrs, &xtra_auto->data[2]))
        return 0;
    if (!xdr_u_char(xdrs, &xtra_auto->boolean))
        return 0;
    if (!xdr_u_short(xdrs, &xtra_auto->interval))
        return 0;

    return 1;
}




int pdsm_xtra_inject_time_info(uint32_t val0, int client, uint32_t val2, pdsm_xtra_time_info_type *pdsm_xtra_time_info_type) 
{
	uint32_t res = -1;
	pdsm_xtra_inject_time_info_args pdsm_xtra_inject_time_info_args;
	pdsm_xtra_inject_time_info_args.pdsm_xtra_cmd_cb_f_type = val0;
	pdsm_xtra_inject_time_info_args.pdsm_client_id_type = client_IDs[client];
	pdsm_xtra_inject_time_info_args.pdsm_xtra_inject_time_info_args_client_data_ptr = val2;
	pdsm_xtra_inject_time_info_args.pdsm_xtra_time_info_type = pdsm_xtra_time_info_type;
	if(clnt_call(_clnt, 0x1E, xdr_rpc_pdsm_xtra_inject_time_info_args, &pdsm_xtra_inject_time_info_args, xdr_result_int, &res, timeout)) {
		D("pdsm_xtra_inject_time_info(%x, %x, %d, %lld, %d) failed\n", val0, client, val2, pdsm_xtra_time_info_type->TimeMsec, pdsm_xtra_time_info_type->TimeUncMsec);
		exit(-1);
	}
	D("pdsm_xtra_inject_time_info(%x, %x, %d, %lld, %d)=%d\n", val0, client, val2, pdsm_xtra_time_info_type->TimeMsec, pdsm_xtra_time_info_type->TimeUncMsec, res);
	return res;
}

int pdsm_xtra_query_data_validity(uint32_t val0, int client, uint32_t val2) 
{
	uint32_t res = -1;
	pdsm_xtra_query_data_validity_args pdsm_xtra_query_data_validity_args;
	pdsm_xtra_query_data_validity_args.pdsm_xtra_cmd_cb_f_type = val0;
	pdsm_xtra_query_data_validity_args.pdsm_client_id_type = client_IDs[client];
	pdsm_xtra_query_data_validity_args.pdsm_xtra_query_data_validity_args_client_data_ptr = val2;
	if(clnt_call(_clnt, 0x1D, xdr_rpc_pdsm_query_data_validity_args, &pdsm_xtra_query_data_validity_args, xdr_result_int, &res, timeout)) {
		D("pdsm_xtra_query_data_validity(%x, %x, %d) failed\n", val0, client, val2);
		exit(-1);
	}
	D("pdsm_xtra_query_data_validity(%x, %x, %d)=%d\n", val0, client, val2, res);
	return res;
}

int pdsm_get_parameters(uint32_t val0, uint32_t val1, uint32_t val2, int client) 
{
    uint32_t res = -1;
	pdsm_get_parameters_args pdsm_get_parameters_args;
	pdsm_get_parameters_args.pdsm_pa_cmd_cb_f_type = val0;
	pdsm_get_parameters_args.pdsm_get_parameters_args_client_data_ptr = val1;
	pdsm_get_parameters_args.pdsm_pa_e_type = val2;
	pdsm_get_parameters_args.pdsm_client_id_type = client_IDs[client];
	if(clnt_call(_clnt, 0x10, xdr_rpc_pdsm_get_parameters_args, &pdsm_get_parameters_args, xdr_result_int, &res, timeout)) {
		D("pdsm_get_parameters(%x, %x, %d, %d) failed\n", val0, val1, val2, client);
		exit(-1);
	}
	D("pdsm_get_parameters(%x, %x, %d, %d)=%d\n", val0, val1, val2, client, res);
	return res;
}

int pdsm_set_parameters(uint32_t val0, uint32_t val1, uint32_t val2, pdsm_pa_info_type *pdsm_pa_info_type, int client) 
{
    uint32_t res = -1;
	pdsm_set_parameters_args pdsm_set_parameters_args;
	pdsm_set_parameters_args.pdsm_pa_cmd_cb_f_type = val0;
	pdsm_set_parameters_args.pdsm_set_parameters_args_client_data_ptr = val1;
	pdsm_set_parameters_args.pdsm_pa_info_type = pdsm_pa_info_type;
	pdsm_set_parameters_args.pdsm_pa_e_type = val2;
	pdsm_set_parameters_args.pdsm_client_id_type = client_IDs[client];
	if(clnt_call(_clnt, 0xF, xdr_rpc_pdsm_set_parameters_args, &pdsm_set_parameters_args, xdr_result_int, &res, timeout)) {
		D("pdsm_set_parameters(%u, %u, %u, %d) failed\n", val0, val1, val2, client);
		exit(-1);
	}
	D("pdsm_set_parameters(%u, %u, %u, %d)=%d\n", val0, val1, val2, client, res);
	return res;
}
int pdsm_xtra_set_auto_download_params(uint32_t val0, int client, uint32_t val2, uint8_t enabled, uint16_t interval) 
{
    uint32_t res = -1;
	pdsm_xtra_set_auto_download_params_args pdsm_xtra_set_auto_download_params_args;
	pdsm_xtra_set_auto_download_params_args.pdsm_xtra_cmd_cb_f_type = val0;
	pdsm_xtra_set_auto_download_params_args.pdsm_client_id_type = client_IDs[client];
	pdsm_xtra_set_auto_download_params_args.pdsm_xtra_set_auto_download_params_args_client_data_ptr = val2;
	pdsm_xtra_set_auto_download_params_args.enabled = enabled;
	pdsm_xtra_set_auto_download_params_args.interval = interval;
	if(clnt_call(_clnt, 0x1C, xdr_rpc_pdsm_xtra_set_auto_download_params_args, &pdsm_xtra_set_auto_download_params_args, xdr_result_int, &res, timeout)) {
		D("pdsm_xtra_set_auto_download_params(%x, %x, %d, %d, %d) failed\n", val0, client, val2, enabled, interval);
		exit(-1);
	}
	D("pdsm_xtra_set_auto_download_params(%x, %x, %d, %d, %d)=%d\n", val0, client, val2, enabled, interval, res);
	return res;
}


int pdsm_xtra_client_initiate_download_request(uint32_t val0, int client, uint32_t val2) 
{
    uint32_t res = -1;
	pdsm_xtra_client_initiate_download_request_args pdsm_xtra_client_initiate_download_request_args;
	pdsm_xtra_client_initiate_download_request_args.pdsm_xtra_cmd_cb_f_type = val0;
	pdsm_xtra_client_initiate_download_request_args.pdsm_client_id_type = client_IDs[client];
	pdsm_xtra_client_initiate_download_request_args.pdsm_xtra_client_initiate_download_request_args_client_data_ptr = val2;
	if(clnt_call(_clnt, 0x1B, xdr_rpc_pdsm_xtra_client_initiate_download_request_args, &pdsm_xtra_client_initiate_download_request_args, xdr_result_int, &res, timeout)) {
		D("pdsm_xtra_client_initiate_download_request(%x, %x, %d) failed\n", val0, client, val2);
		exit(-1);
	}
	D("pdsm_xtra_client_initiate_download_request(%x, %x, %d)=%d\n", val0, client, val2, res);
	return res;
}

int pdsm_get_position(uint32_t val0, uint32_t val1, pdsm_pd_option_s_type *pdsm_pd_option_s_type, pdsm_pd_qos_type *pdsm_pd_qos_type, int client)
{
    uint32_t res;
	pdsm_get_position_args pdsm_get_position_args;
	pdsm_get_position_args.pdsm_pd_cmd_cb_f_type = val0;
	pdsm_get_position_args.pdsm_get_position_args_client_data_ptr = val1;
	pdsm_get_position_args.pdsm_pd_option_s_type = pdsm_pd_option_s_type;
	pdsm_get_position_args.pdsm_pd_qos_type = pdsm_pd_qos_type;
	pdsm_get_position_args.pdsm_client_id_type = client_IDs[client];
	if(clnt_call(_clnt, 0xB, xdr_rpc_pdsm_get_position_args, &pdsm_get_position_args, xdr_result_int, &res, timeout)) {
		D("pdsm_client_get_position() failed\n");
		exit(-1);
	}
	D("pdsm_client_get_position()=%d\n", res);
	
	return res;
}

int pdsm_client_end_session(uint32_t val0, uint32_t val1, uint32_t val2, int client)
{
    uint32_t res;
	pdsm_end_session_args pdsm_end_session_args;
	pdsm_end_session_args.pdsm_pd_cmd_cb_f_type = val0;
	pdsm_end_session_args.pdsm_pd_end_session_e_type = val1;
	pdsm_end_session_args.pdsm_end_session_args_client_data_ptr = val2;
	pdsm_end_session_args.pdsm_client_id_type = client_IDs[client];
	if(clnt_call(_clnt, 0xC, xdr_rpc_pdsm_end_session_args, &pdsm_end_session_args , xdr_result_int, &res, timeout)) {
		D("pdsm_client_end_session(%d, %d, %d, %x) failed\n", val0, val1, val2, client);
		exit(-1);
	}
	D("pdsm_client_end_session(%d, %d, %d, %x)=%x\n", val0, val1, val2, client, res);
	return 0;
}

enum pdsm_pd_events {
    PDSM_PD_EVENT_POSITION = 0x1,
    PDSM_PD_EVENT_VELOCITY = 0x2,
    PDSM_PD_EVENT_HEIGHT = 0x4,
    PDSM_PD_EVENT_DONE = 0x8,
    PDSM_PD_EVENT_END = 0x10,
    PDSM_PD_EVENT_BEGIN = 0x20,
    PDSM_PD_EVENT_COMM_BEGIN = 0x40,
    PDSM_PD_EVENT_COMM_CONNECTED = 0x80,
    PDSM_PD_EVENT_COMM_DONE = 0x200,
    PDSM_PD_EVENT_GPS_BEGIN = 0x4000,
    PDSM_PD_EVENT_GPS_DONE = 0x8000,
    PDSM_PD_EVENT_UPDATE_FAIL = 0x1000000,
};

//From leo-gps.c
extern void update_gps_location(GpsLocation *location);
extern void update_gps_status(GpsStatusValue value);
extern void update_gps_svstatus(GpsSvStatus *svstatus);

void dispatch_pdsm_pd(uint32_t *data) {
    uint32_t event=ntohl(data[2]);
    D("%s(): event=0x%x", __FUNCTION__, event);
    if(event&PDSM_PD_EVENT_BEGIN) {
        D("PDSM_PD_EVENT_BEGIN");
    }
    if(event&PDSM_PD_EVENT_GPS_BEGIN) {
        D("PDSM_PD_EVENT_GPS_BEGIN");
    }
    if(event&PDSM_PD_EVENT_GPS_DONE) {
        D("PDSM_PD_EVENT_GPS_DONE");
        no_fix = 1;
    }
    GpsLocation fix;
    fix.flags = 0;
    if(event&PDSM_PD_EVENT_POSITION) {
        D("PDSM_PD_EVENT_POSITION");
        if (use_nmea) return;

        GpsSvStatus ret;
        int i;
        ret.num_svs=ntohl(data[82]) & 0x1F;

#if DUMP_DATA
        //D("pd %3d: %08x ", 77, ntohl(data[77]));
        for(i=60;i<83;++i) {
            D("pd %3d: %08x ", i, ntohl(data[i]));
        }
        for(i=83;i<83+3*(ret.num_svs-1)+3;++i) {
            D("pd %3d: %d ", i, ntohl(data[i]));
        }
#endif

        for(i=0;i<ret.num_svs;++i) {
            ret.sv_list[i].prn=ntohl(data[83+3*i]);
            ret.sv_list[i].elevation=ntohl(data[83+3*i+1]);
            ret.sv_list[i].azimuth=(float)ntohl(data[83+3*i+2])/100.0f;
            ret.sv_list[i].snr=ntohl(data[83+3*i+2])%100;
        }
        ret.used_in_fix_mask=ntohl(data[77]);
        update_gps_svstatus(&ret);

        fix.timestamp = ntohl(data[8]);
        if (!fix.timestamp) return;

        // convert gps time to epoch time ms
        fix.timestamp += 315964800; // 1/1/1970 to 1/6/1980
        fix.timestamp -= 15; // 15 leap seconds between 1980 and 2011
        fix.timestamp *= 1000; //ms

        fix.flags |= GPS_LOCATION_HAS_LAT_LONG;
        no_fix = 0;

        if (ntohl(data[75])) {
            fix.flags |= GPS_LOCATION_HAS_ACCURACY;
            float hdop = (float)ntohl(data[75]) / 10.0f / 2.0f;
            fix.accuracy = hdop * (float)MEASUREMENT_PRECISION;
        }

        union {
            struct {
                uint32_t lowPart;
                int32_t highPart;
            };
            int64_t int64Part;
        } latitude, longitude;

        latitude.lowPart = ntohl(data[61]);
        latitude.highPart = ntohl(data[60]);
        longitude.lowPart = ntohl(data[63]);
        longitude.highPart = ntohl(data[62]);
        fix.latitude = (double)latitude.int64Part / 1.0E8;
        fix.longitude = (double)longitude.int64Part / 1.0E8;
    }
    if (event&PDSM_PD_EVENT_VELOCITY)
    {
        D("PDSM_PD_EVENT_VELOCITY");
        if (use_nmea) return;
        fix.flags |= GPS_LOCATION_HAS_SPEED|GPS_LOCATION_HAS_BEARING;
        fix.speed = (float)ntohl(data[66]) / 10.0f / 3.6f; // convert kp/h to m/s
        fix.bearing = (float)ntohl(data[67]) / 10.0f;
    }
    if (event&PDSM_PD_EVENT_HEIGHT)
    {
        D("PDSM_PD_EVENT_HEIGHT");
        if (use_nmea) return;
        fix.flags |= GPS_LOCATION_HAS_ALTITUDE;
        fix.altitude = 0;
        double altitude = (double)ntohl(data[64]);
        if (altitude / 10.0f < 1000000.0) // Check if height is not unreasonably high
            fix.altitude = altitude / 10.0f; // Apply height with a division of 10 to correct unit of meters
        else // If unreasonably high then it is a negative height
            fix.altitude = (altitude - (double)4294967295.0) / 10.0f; // Subtract FFFFFFFF to make height negative
    }
    if (fix.flags)
    {
        update_gps_location(&fix);
    }
    if(event&PDSM_PD_EVENT_END)
    {
        D("PDSM_PD_EVENT_END");
    }
    if(event&PDSM_PD_EVENT_DONE)
    {
        D("PDSM_PD_EVENT_DONE");
        pdsm_pd_callback();
    }
}

void dispatch_pdsm_pd_cmd(uint32_t *data) 
{
    uint32_t pd_cmd=ntohl(data[2]);
	uint32_t pd_error=ntohl(data[3]);
	
	D("pd_cmd: %d", pd_cmd);
	D("pd_error: %d", pd_error);
} 

void dump_response(char* path, uint32_t *data) {
    FILE *fp;
	
	fp = fopen(path, "wb");
	if (fp!=NULL) {
		fwrite(data, sizeof(char), 1024, fp);
		fclose(fp);
	}
}

void dispatch_pdsm_ext(uint32_t *data) {
    GpsSvStatus ret;
    int i;

    if (use_nmea) return;

    no_fix++;
    if (no_fix < 2) return;
    
    ret.num_svs=ntohl(data[8]);
    D("%s() is called. num_svs=%d", __FUNCTION__, ret.num_svs);

#if DUMP_DATA
    for(i=0;i<12;++i) {
        D("e %3d: %08x ", i, ntohl(data[i]));
    }
    for(i=101;i<101+12*(ret.num_svs-1)+6;++i) {
        D("e %3d: %d ", i, ntohl(data[i]));
    }
#endif

    for(i=0;i<ret.num_svs;++i) {
        ret.sv_list[i].prn=ntohl(data[101+12*i+1]);
        ret.sv_list[i].elevation=ntohl(data[101+12*i+5]);
        ret.sv_list[i].azimuth=ntohl(data[101+12*i+4]);
        ret.sv_list[i].snr=(float)ntohl(data[101+12*i+2])/10.0f;
    }
    //ret.used_in_fix_mask=ntohl(data[9]);
    ret.used_in_fix_mask=0;
    update_gps_svstatus(&ret);
}

void dispatch_pdsm_pa_cmd(uint32_t *data)
{    
	uint32_t pa_cmd=ntohl(data[2]);
	uint32_t pa_error=ntohl(data[3]);
	
	D("pa_cmd: %d", pa_cmd);
	D("pa_error: %d", pa_error);
}

void dispatch_pdsm_pa(uint32_t *data)
{
    uint32_t pa_client=ntohl(data[0]);
	uint32_t pa_param=ntohl(data[1]);
	
	D("Client: %d", pa_client);
	D("Parameter: %d", pa_param);
	
	if(pa_param == 4)
	{
		delete_params_complete();
	}
}

void dispatch_pdsm_xtra_cmd(uint32_t * data)
{
    uint32_t xtra_cmd=ntohl(data[2]);
	uint32_t xtra_error=ntohl(data[3]);
	
	D("xtra_cmd: %d", xtra_cmd);
	D("xtra_error: %d", xtra_error);
}

void dispatch_pdsm_xtra(uint32_t *data) 
{    
	//Handles download requests from gps chip
	//Have to check if it is a download request because the same procid is multipurpose
	
	unsigned char url[9]; //Stores the filename for the xtra data
	unsigned int i = 0x50;
	
	memcpy(url, &(data[i]), 8); //Copies the filename from the rpc message
	url[8] = '\0'; //Adds the null terminate at the end of the filename to create a string
	
	// Performs comparison with expected string "xtra.bin"
	if (strcmp(url, "xtra.bin") == 0) {
		D("Calling xtra_download_request()");
		//Calls the gps_xtra_download_request callback method
		xtra_download_request();
	}
}

void dispatch_pdsm_xtra_req(uint8_t *data) {
    //Handles download requests from gps chip
    //Have to check if it is a download request because the same procid is multipurpose
    
    unsigned char url[9]; //Stores the filename for the xtra data
    unsigned int i = 0x50;
    
    memcpy(url, &(data[i]), 8); //Copies the filename from the rpc message
    url[8] = '\0'; //Adds the null terminate at the end of the filename to create a string
    
    // Performs comparison with expected string "xtra.bin"
    if (strcmp(url, "xtra.bin") == 0) {
        D("Calling xtra_download_request()");
        //Calls the gps_xtra_download_request callback method
        xtra_download_request();
    }
}

void dispatch_pdsm(uint32_t *data) {
    uint32_t procid=ntohl(data[5]);
	D("%s() is called. data[5]=procid=%d", __FUNCTION__, procid);
	if(procid==1) 
		dispatch_pdsm_pd(&(data[10]));
	else if(procid==2)
		dispatch_pdsm_pa(&(data[14]));
	else if(procid==4) 
		dispatch_pdsm_ext(&(data[10]));
	else if(procid==5)
		dispatch_pdsm_xtra(&(data[10]));
	else if(procid==11)
		dispatch_pdsm_pd_cmd(&(data[10]));
	else if(procid==12)
		dispatch_pdsm_pa_cmd(&(data[10]));
	else if(procid==15)
		dispatch_pdsm_xtra_cmd(&(data[10]));
		
}

void dispatch_atl(uint32_t *data) {
    D("%s() is called", __FUNCTION__);
    // No clue what happens here.
}

void dispatch(struct svc_req* a, registered_server* svc) {
    int i;
    uint32_t *data=svc->xdr->in_msg;
    uint32_t result=0;
    uint32_t svid=ntohl(data[3]);
/*
    D("received some kind of event\n");
    for(i=0;i< svc->xdr->in_len/4;++i) {
        D("%08x ", ntohl(data[i]));
    }
    D("\n");
    for(i=0;i< svc->xdr->in_len/4;++i) {
        D("%010d ", ntohl(data[i]));
    }
    D("\n");
*/
    if(svid==0x3100005b) {
        dispatch_pdsm(data);
    } else if(svid==0x3100001d) {
        dispatch_atl(data);
    } else {
        //Got dispatch for unknown serv id!
    }
    //ACK
    svc_sendreply(svc, xdr_int, &result);
}

uint8_t get_cleanup_value() {
    D("%s() is called: %d", __FUNCTION__, CLEANUP_ENABLED);
    return CLEANUP_ENABLED;
}

uint8_t get_precision_value() {
    D("%s() is called: %d", __FUNCTION__, MEASUREMENT_PRECISION);
    return MEASUREMENT_PRECISION;
}

int parse_gps_conf() {
    FILE *file = fopen("/system/etc/gps.conf", "r");
    if (!file) { 
        D("fopen error\n");
        return 1; 
    }
    
    char *check_auto_download = "GPS1_XTRA_AUTO_DOWNLOAD_ENABLED";
    char *check_interval = "GPS1_XTRA_DOWNLOAD_INTERVAL";
    char *check_cleanup = "GPS1_CLEANUP_ENABLED";
    char *check_timeout = "GPS1_SESSION_TIMEOUT";
    char *check_precision = "GPS1_MEASUREMENT_PRECISION";
    char *result;
    char str[256];
    int i = -1;

    while (fscanf(file, "%s", str) != EOF) {
        //D("%s (%d)\n", str, strlen(str));
        if (!CHECKED[1]) {
            result = strstr(str, check_auto_download);
            if (result != NULL) {
                result = result+strlen(check_auto_download)+1;
                i = atoi(result);
                if (i==0 || i==1)
                    XTRA_AUTO_DOWNLOAD_ENABLED = i;
                CHECKED[1] = 1;
            }
        }
        if (XTRA_AUTO_DOWNLOAD_ENABLED) {
            result = strstr(str, check_interval);
            if (result != NULL) {
                result = result+strlen(check_interval)+1;
                i = atoi(result);
                if (i>0 && i<169)
                    XTRA_DOWNLOAD_INTERVAL = i;
            }
        }
        if (!CHECKED[2]) {
            result = strstr(str, check_cleanup);
            if (result != NULL) {
                result = result+strlen(check_cleanup)+1;
                i = atoi(result);
                if (i==0 || i==1)
                    CLEANUP_ENABLED = i;
                CHECKED[2] = 1;
            }
        }
        if (!CHECKED[3]) {
            result = strstr(str, check_timeout);
            if (result != NULL) {
                result = result+strlen(check_timeout)+1;
                i = atoi(result);
                if (i>1 && i<121)
                    SESSION_TIMEOUT = i;
                CHECKED[3] = 1;
            }
        }
        if (!CHECKED[4]) {
            result = strstr(str, check_precision);
            if (result != NULL) {
                result = result+strlen(check_precision)+1;
                i = atoi(result);
                if (i>0 && i<50)
                    MEASUREMENT_PRECISION = i;
                CHECKED[4] = 1;
            }
        }
    }
    fclose(file);
    LOGD("%s() is called: GPS1_XTRA_AUTO_DOWNLOAD_ENABLED = %d", __FUNCTION__, XTRA_AUTO_DOWNLOAD_ENABLED);
    LOGD("%s() is called: GPS1_XTRA_DOWNLOAD_INTERVAL = %d", __FUNCTION__, XTRA_DOWNLOAD_INTERVAL);
    LOGD("%s() is called: GPS1_CLEANUP_ENABLED = %d", __FUNCTION__, CLEANUP_ENABLED);
    LOGD("%s() is called: GPS1_SESSION_TIMEOUT = %d", __FUNCTION__, SESSION_TIMEOUT);
    LOGD("%s() is called: GPS1_MEASUREMENT_PRECISION = %d", __FUNCTION__, MEASUREMENT_PRECISION);
    return 0;
}

int init_leo() 
{
	struct CLIENT *clnt;
	struct CLIENT *clnt_atl;
	int i;
	SVCXPRT *svc;
	
	svc=svcrtr_create();
	_svc=svc;
	xprt_register(svc);
	svc_register(svc, 0x3100005b, 0x00010001, (__dispatch_fn_t)dispatch, 0);
	svc_register(svc, 0x3100001d, 0x00010001, (__dispatch_fn_t)dispatch, 0);
	
	clnt=clnt_create(NULL, 0x3000005B, 0x00010001, NULL);
	clnt_atl=clnt_create(NULL, 0x3000001D, 0x00010001, NULL);
	_clnt=clnt;
	_clnt_atl = clnt_atl;
	
	if(!clnt) {
		D("Failed creating client\n");
		return -1;
	}
	if(!svc) {
		D("Failed creating server\n");
		return -2;
	}
	
	pdsm_client_init(0x1);
	pdsm_client_init(0xB);
	pdsm_atl_l2_proxy_reg(1,0,0);
	
	pdsm_client_pd_reg(0x1, 0, 0, 0, 0xF310FFFF, 0);
	pdsm_client_pa_reg(0x1, 0, 0, 0, 0xFEFE0, 0);
	pdsm_client_lcs_reg(0x1, 0, 0, 0, 0x3F0, 0);
	pdsm_client_ext_status_reg(0x1, 0, 0, 0, 7, 0);
	pdsm_client_xtra_reg(0xB, 0, 0, 0, 7, 0);
	
	pdsm_client_act(0x1);
	pdsm_client_act(0xB);
	
	set_clients_active(1);
	
	gps_set_gps_lock(0);
    if (!CHECKED[0]) {
        if (use_nmea)
            LOGD("%s() is called: %s version", __FUNCTION__, "NMEA");
        else
            LOGD("%s() is called: %s version", __FUNCTION__, "RPC");

        parse_gps_conf();
        if (XTRA_AUTO_DOWNLOAD_ENABLED)
            gps_xtra_set_auto_params();
        CHECKED[0] = 1;
    }

    return 0;
}

int init_gps_rpc() 
{
    init_leo();
    return 0;
}

int gps_xtra_set_data(unsigned char *xtra_data_ptr, uint32_t xtra_data_len, uint8_t part_number, uint8_t total_parts) 
{
    uint32_t res = -1;
    res = pdsm_xtra_set_data(0, 0xB, 0, xtra_data_ptr, xtra_data_len, part_number, total_parts, 1);
    return res;
}

int gps_xtra_init_down_req() 
{
    //Tell gpsOne to request xtra data
    uint32_t res = -1;
    res = pdsm_xtra_client_initiate_download_request(0, 0xB, 0);
    return res;
}

int gps_xtra_query_data_val() 
{
    //Tell gpsOne to request xtra data
	uint32_t res = -1;
	res =  pdsm_xtra_query_data_validity(0, 0xB, 0);
	return res;
}

int gps_get_parameters() 
{
    uint32_t res = -1;
	res = pdsm_get_parameters(0, 0, 16, 0x1);
	return res;
}

int gps_delete_data(uint32_t flags) {
    uint32_t res = -1;
	uint32_t flag_conv = 0;
	
	pdsm_pa_info_type pdsm_pa_info_type;
	pdsm_delete_parms_type pdsm_delete_parms_type;
	
	if (flags == 34813) 
	{
		flag_conv = 1021;
	}
	else if (flags == 65535)
	{
		flag_conv = 34815;
	}
	else {
		flag_conv = flags;
	}
	
	pdsm_delete_parms_type.val0 = flag_conv;
	pdsm_delete_parms_type.val1 = 0;
	pdsm_delete_parms_type.val2 = 0;
	pdsm_delete_parms_type.val3 = 0;
	pdsm_delete_parms_type.val4 = 0;
	pdsm_delete_parms_type.val5 = 0;
	pdsm_delete_parms_type.val6 = 0;
	pdsm_delete_parms_type.val7 = 0;
	
	pdsm_pa_info_type.pa_set = 4;
	pdsm_pa_info_type.pa_ptr = &pdsm_delete_parms_type;
	
	res = pdsm_set_parameters(0, 0, 0, &pdsm_pa_info_type, 0x1);
	
	return res;
}

void read_gps_xtra_auto_params(xtra_conf_auto_params* xtra_conf_auto_param) 
{
    D("%s() is called", __FUNCTION__);
	
	FILE *fp;
	char *buffer;
	char auto_param[33] = "GPS1_XTRA_AUTO_DOWNLOAD_ENABLED=";
	char auto_char[2];
	char interval_param[29] = "GPS1_XTRA_DOWNLOAD_INTERVAL=";
	char interval_char[4];
	int filesize = 0;
	int result = 0;
	int i;
	
	xtra_conf_auto_param->auto_enable = -1;
	xtra_conf_auto_param->interval = 0;
	
	fp = fopen("/etc/gps.conf", "r");
	if (fp!=NULL) {
		//Obtain Filesize
		fseek(fp , 0 , SEEK_END);
		filesize = ftell(fp);
		rewind(fp);
		
		buffer = malloc(sizeof(char)*filesize);
		result = fread(buffer, sizeof(char), filesize, fp);
		fclose(fp);
		
		if (result == filesize) {
			for (i = 0; i < filesize; i++) {
				if (memcmp(auto_param, &(buffer[i]), sizeof(char)*32) == 0){
					memcpy(auto_char, &(buffer[i+32]), sizeof(char));
					auto_char[1] = '\0';
					xtra_conf_auto_param->auto_enable = atoi(auto_char);
					D("Value of auto: %d", xtra_conf_auto_param->auto_enable);
					break;
				}
			}
			for (i = 0; i < filesize; i++) {
				if (memcmp(interval_param, &(buffer[i]), sizeof(char)*28) == 0){
					memcpy(interval_char, &(buffer[i+28]), sizeof(char)*3);
					interval_char[3] = '\0';
					xtra_conf_auto_param->interval = atoi(interval_char);
					D("Value of interval: %d", xtra_conf_auto_param->interval);
					break;
				}
			}
			free(buffer);
			if (xtra_conf_auto_param->auto_enable == -1 && xtra_conf_auto_param->interval == 0) {
				D("Couldn't Find Parameters");
			}
			else {
		   }
		}
		else {
			D("Incorrect Filesize");
			free(buffer);
		}
	}
	else {
		D("Couldn't Open File");
	}
}

int gps_xtra_set_auto_params()
{
    D("%s() is called", __FUNCTION__);
	//Set xtra auto download parameters
	xtra_conf_auto_params xtra_conf_auto_param;
	uint32_t res = -1;
	uint8_t boolean;
	uint16_t interval;
	int check;
	
	read_gps_xtra_auto_params(&(xtra_conf_auto_param));
	
	boolean = xtra_conf_auto_param.auto_enable;
	interval = xtra_conf_auto_param.interval;
	
	if (boolean < 2 && interval > 0 && interval < 169) {
		check = check_gps_xtra_auto_param_stored(boolean, interval);
	
		if (check == 1) {
			D("pdsm_xtra_set_auto_download_params boolean: %u interval: %u", boolean, interval);
			res = pdsm_xtra_set_auto_download_params(0, 0xB, 0, boolean, interval);
		}
		else if (check == -1) {
			D("Error with check_gps_xtra_auto_param_stored");
			return res;
		}
		else {
			D("Already Set Parameters");
			return res = 1;
		}
	}
	else {
		D("Inavlid Parameters");
		return res;
	}
	return res;
}

int check_gps_xtra_auto_param_stored(int auto_enable, int interval) 
{
    D("%s() is called", __FUNCTION__);
	FILE *fp;
	char *buffer_in;
	char buffer_out[5];
	char auto_enable_char[2];
	char interval_char[4];
	int filesize;
	int read_result;
	int write_result;
	char path[] = "/data/data/xtra_auto.conf";
	
	fp = fopen(path, "r");
	if (fp!=NULL) {
		fseek(fp , 0 , SEEK_END);
		filesize = ftell(fp);
		rewind(fp);
		
		buffer_in = malloc(sizeof(char)*filesize);
		read_result = fread(buffer_in, sizeof(char), filesize, fp);
		fclose(fp);
		
		if (read_result == filesize) {
			memcpy(auto_enable_char, &(buffer_in[0]), sizeof(char));
			memcpy(interval_char, &(buffer_in[1]), sizeof(char)*3);
			auto_enable_char[1] = '\0';
			interval_char[3] = '\0';
		}
		
		if ((auto_enable == atoi(auto_enable_char)) && (interval == atoi(interval_char))) {
			free(buffer_in);
			return 0;
		}
		else {
			fp = fopen(path, "w");
			write_result = sprintf(buffer_out, "%d%d", auto_enable, interval);
			fwrite(buffer_out, sizeof(char), write_result, fp);
			fclose(fp);
			free(buffer_in);
			return 1;
		}
	}
	else {
		fp = fopen(path, "w");
		if (fp!=NULL) {
			write_result = sprintf(buffer_out, "%d%d", auto_enable, interval);
			fwrite(buffer_out, sizeof(char), write_result, fp);
			fclose(fp);
			return 1;
		}
		else {
			return -1;
		}
	}
	return -1;
}



extern int64_t elapsed_realtime();

int gps_xtra_inject_time_info(GpsUtcTime time, int64_t timeReference, int uncertainty)
{	
	uint32_t res = -1;
	pdsm_xtra_time_info_type time_info_ptr;
	
	time_info_ptr.TimeMsec = time;
	time_info_ptr.TimeMsec += (int64_t)(elapsed_realtime() - timeReference);
	time_info_ptr.TimeUncMsec = uncertainty;
	time_info_ptr.b_RefToUtcTime = 1;
	time_info_ptr.b_ForceFlag = 0;
	
	res = pdsm_xtra_inject_time_info(0, 0xB, 0, &time_info_ptr);
	return res;
}

int gps_set_gps_lock(pdsm_gps_lock_e_type pdsm_gps_lock_e_type)
{
    uint32_t res = -1;
	pdsm_pa_info_type pdsm_pa_info_type;
	pdsm_pa_info_type.pa_set = 2;
	pdsm_pa_info_type.pa_ptr = &pdsm_gps_lock_e_type;
	
	res = pdsm_set_parameters(0, 0, 1, &pdsm_pa_info_type, 0x1);
	return res;
}

void gps_get_position() 
{
    D("%s() is called", __FUNCTION__);
	
	pdsm_srch_jgps_prm_info_s_type pdsm_srch_jgps_prm_info_s_type;
	pdsm_srch_jgps_ppm_info_s_type pdsm_srch_jgps_ppm_info_s_type;
	pdsm_pd_meas_mode_info_s_type pdsm_pd_meas_mode_info_s_type;
	pdsm_pd_sec_data_s_type pdsm_pd_sec_data_s_type;
	pdsm_pd_auth_s_type pdsm_pd_auth_s_type;
	pdsm_server_address_u_type pdsm_server_address_u_type;
	pdsm_server_address_s_type pdsm_server_address_s_type;
	pdsm_pd_server_info_s_type pdsm_pd_server_info_s_type;
	pdsm_fix_rate_s_type pdsm_fix_rate_s_type;
	pdsm_server_ipv4_address_type pdsm_server_ipv4_address_type;
	pdsm_pd_option_s_type pdsm_pd_option_s_type;
	pdsm_pd_qos_type pdsm_pd_qos_type;
	unsigned char bytes[20];
	
	bytes[0] = 0;
	bytes[1] = 0;
	bytes[2] = 0;
	bytes[3] = 0;
	bytes[4] = 0;
	bytes[5] = 0;
	bytes[6] = 0;
	bytes[7] = 0;
	bytes[8] = 0;
	bytes[9] = 0;
	bytes[10] = 0;
	bytes[11] = 0;
	bytes[12] = 0;
	bytes[13] = 0;
	bytes[14] = 0;
	bytes[15] = 0;
	bytes[16] = 0;
	bytes[17] = 0;
	bytes[18] = 0;
	bytes[19] = 0;
	
	pdsm_pd_option_s_type.pdsm_pd_session_e_type = 1;
	pdsm_pd_option_s_type.pdsm_pd_session_operation_e_type = 1;
	
	pdsm_pd_option_s_type.pdsm_fix_rate_s_type = &pdsm_fix_rate_s_type;
	pdsm_fix_rate_s_type.num_fixes = 10;
	pdsm_fix_rate_s_type.time_between_fixes = 1;
	
	pdsm_pd_option_s_type.pdsm_pd_server_info_s_type = &pdsm_pd_server_info_s_type;
	pdsm_pd_server_info_s_type.pdsm_server_option_e_type = 1;
	pdsm_pd_server_info_s_type.pdsm_server_address_s_type = &pdsm_server_address_s_type;
	
	pdsm_server_address_s_type.pdsm_server_address_e_type = 0;
	pdsm_server_address_s_type.pdsm_server_address_u_type = &pdsm_server_address_u_type;
	
	pdsm_server_address_u_type.pdsm_server_address_e_type = 0;
	pdsm_server_address_u_type.address_struct = &pdsm_server_ipv4_address_type;
	
	pdsm_server_ipv4_address_type.val0 = 1249725888;
	pdsm_server_ipv4_address_type.val1 = 27676;
	
	pdsm_pd_option_s_type.unknown = 0;
	
	pdsm_pd_option_s_type.pdsm_pd_auth_s_type = &pdsm_pd_auth_s_type;
	pdsm_pd_auth_s_type.pdsm_pd_sec_data_s_type = &pdsm_pd_sec_data_s_type;
	pdsm_pd_sec_data_s_type.val0 = 0;
	pdsm_pd_sec_data_s_type.val1 = 0;
	pdsm_pd_sec_data_s_type.byte_array = bytes;
	
	pdsm_pd_option_s_type.pdsm_pd_meas_mode_info_s_type = &pdsm_pd_meas_mode_info_s_type;
	pdsm_pd_meas_mode_info_s_type.pdsm_sess_jgps_type_e_type = 0;
	pdsm_pd_meas_mode_info_s_type.pdsm_srch_jgps_ppm_info_s_type = &pdsm_srch_jgps_ppm_info_s_type;
	pdsm_pd_meas_mode_info_s_type.pdsm_srch_jgps_prm_info_s_type = &pdsm_srch_jgps_prm_info_s_type;
	
	pdsm_srch_jgps_ppm_info_s_type.val0 = 0;
	pdsm_srch_jgps_ppm_info_s_type.val1 = 0;
	
	pdsm_srch_jgps_prm_info_s_type.val0 = 0;
	pdsm_srch_jgps_prm_info_s_type.val1 = 0;
	
	pdsm_pd_qos_type.accuracy = 50;
	pdsm_pd_qos_type.performance = 120;
	
	pdsm_get_position(0, 0, &pdsm_pd_option_s_type, &pdsm_pd_qos_type, 0x1);
}


void exit_gps_rpc() 
{
    pdsm_client_end_session(0, 0, 0, 0x1);
}

void cleanup_gps_rpc_clients() 
{
    pdsm_client_deact(0x1);
    pdsm_client_deact(0xB);
    
    pdsm_client_release(0x1);
    pdsm_client_release(0xB);
    
    svc_unregister(_svc, 0x3100005b, 0x00010001);
    svc_unregister(_svc, 0x3100005b, 0);
    svc_unregister(_svc, 0x3100001d, 0x00010001);
    svc_unregister(_svc, 0x3100001d, 0);
    xprt_unregister(_svc);
    svc_destroy(_svc);
    
    clnt_destroy(_clnt);
    clnt_destroy(_clnt_atl);
    _clnt = NULL;
    _clnt_atl = NULL;
}

#if DISABLE_CLEANUP
int deactivate_rpc_clients() {
    set_clients_active(0);
	pdsm_client_deact(0x1);
	pdsm_client_deact(0xb);
	return 0;
}
#endif

#if DISABLE_CLEANUP
int reactivate_rpc_clients() {
	pdsm_client_act(0x1);
	pdsm_client_act(0xb);
	set_clients_active(1);
	return 0;
}
#endif

// END OF FILE
