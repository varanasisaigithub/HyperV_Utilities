/*-
 * Copyright (c) 2009-2012 Microsoft Corp.
 * Copyright (c) 2012 NetApp Inc.
 * Copyright (c) 2012 Citrix Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * A common driver for all hyper-V util services.
 */

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/reboot.h>
#include <sys/timetc.h>

#include <dev/hyperv/include/hyperv.h>

#define HV_SHUT_DOWN		0
#define HV_TIME_SYNCH		1
#define HV_HEART_BEAT		2
#define HV_KVP			3
#define HV_MAX_UTIL_SERVICES	4

#define HV_NANO_SEC	1000000000L	/* 10^ 9 nanosecs = 1 sec */

#define HV_WLTIMEDELTA			116444736000000000L /* in 100ns unit */
#define HV_ICTIMESYNCFLAG_PROBE		0
#define HV_ICTIMESYNCFLAG_SYNC		1
#define HV_ICTIMESYNCFLAG_SAMPLE	2

typedef struct hv_vmbus_service {
	hv_guid			guid;		/* Hyper-V GUID		*/
        char*			name;		/* name of service	*/
        boolean_t		enabled;	/* service enabled	*/
        hv_work_queue*		work_queue;	/* background work queue */
				/*
				 * function to initialize service
				 */
        int			(*init)(struct hv_vmbus_service *);
				/*
				 * function to process Hyper-V messages
				 */
        void			(*callback)(void *);
} hv_vmbus_service;


/* Routines called from the loadable KVP module */
void hv_negotiate_version( struct hv_vmbus_icmsg_hdr* icmsghdrp,
	struct hv_vmbus_icmsg_negotiate* negop, uint8_t* buf);

static void hv_shutdown_cb(void *context);
static void hv_heartbeat_cb(void *context);
static void hv_timesync_cb(void *context);

static int hv_timesync_init(hv_vmbus_service *serv);

/**
 * Note: GUID codes below are predefined by the host hypervisor
 * (Hyper-V and Azure)interface and required for correct operation.
 */
static hv_vmbus_service service_table[] = {
	/* Shutdown Service */
	{ .guid.data = {0x31, 0x60, 0x0B, 0X0E, 0x13, 0x52, 0x34, 0x49,
			0x81, 0x8B, 0x38, 0XD9, 0x0C, 0xED, 0x39, 0xDB},
	  .name  = "Hyper-V Shutdown Service\n",
	  .enabled = TRUE,
	  .callback = hv_shutdown_cb,
	},

        /* Time Synch Service */
        { .guid.data = {0x30, 0xe6, 0x27, 0x95, 0xae, 0xd0, 0x7b, 0x49,
			0xad, 0xce, 0xe8, 0x0a, 0xb0, 0x17, 0x5c, 0xaf},
	  .name = "Hyper-V Time Synch Service\n",
	  .enabled = TRUE,
	  .init = hv_timesync_init,
	  .callback = hv_timesync_cb,
	},

        /* Heartbeat Service */
        { .guid.data = {0x39, 0x4f, 0x16, 0x57, 0x15, 0x91, 0x78, 0x4e,
			0xab, 0x55, 0x38, 0x2f, 0x3b, 0xd5, 0x42, 0x2d},
	  .name = "Hyper-V Heartbeat Service\n",
	  .enabled = TRUE,
          .callback = hv_heartbeat_cb,

	},

#if 0
        /* KVP (Key Value Pair) Service */
        { .guid.data = {0xe7, 0xf4, 0xa0, 0xa9, 0x45, 0x5a, 0x96, 0x4d,
			0xb8, 0x27, 0x8a, 0x84, 0x1e, 0x8c, 0x3,  0xe6},
	  .name = "Hyper-V KVP Service\n",
	  .enabled = FALSE,
	  .init = NULL,
	  .callback = NULL,
	},
#endif
};

/**
 * Receive buffer pointers, there is one buffer per utility service. The
 * buffer is allocated during attach().
 */
static uint8_t* receive_buffer[HV_MAX_UTIL_SERVICES];

struct hv_ictimesync_data {
	uint64_t    parenttime;
	uint64_t    childtime;
	uint64_t    roundtriptime;
        uint8_t	    flags;
} __packed;

static int hv_timesync_init(hv_vmbus_service *serv)
{
	serv->work_queue = hv_work_queue_create("Time Sync");
	if (serv->work_queue == NULL)
	    return (ENOMEM);
	return (0);
}

void
hv_negotiate_version(
	struct hv_vmbus_icmsg_hdr*		icmsghdrp,
	struct hv_vmbus_icmsg_negotiate*	negop,
	uint8_t*				buf)
{
	int icframe_vercnt;
        int icmsg_vercnt;
        int i;
#define MAX_SRV_VER 0x7fff

	icmsghdrp->icmsgsize = 0x10;

	negop = (struct hv_vmbus_icmsg_negotiate *) &buf[
		sizeof(struct hv_vmbus_pipe_hdr) +
			sizeof(struct hv_vmbus_icmsg_hdr)];
	printf("In hv_negotiate_version\n");
	icframe_vercnt = negop->icframe_vercnt;
        icmsg_vercnt = negop->icmsg_vercnt;

 /*
         * Select the framework version number we will
         * support.
         */

        for (i = 0; i < negop->icframe_vercnt; i++) {
                if (negop->icversion_data[i].major <= MAX_SRV_VER)
                        icframe_vercnt = negop->icversion_data[i].major;
        }

        for (i = negop->icframe_vercnt;
                 (i < negop->icframe_vercnt + negop->icmsg_vercnt); i++) {
                if (negop->icversion_data[i].major <= MAX_SRV_VER)
                        icmsg_vercnt = negop->icversion_data[i].major;
        }

        /*
         * Respond with the maximum framework and service
         * version numbers we can support.
         */
        negop->icframe_vercnt = 1;
        negop->icmsg_vercnt = 1;
        negop->icversion_data[0].major = icframe_vercnt;
        negop->icversion_data[0].minor = 0;
        negop->icversion_data[1].major = icmsg_vercnt;
        negop->icversion_data[1].minor = 0;

        printf("negop->icversion_data[0].major:%d\n",negop->icversion_data[0].major);
        printf("negop->icversion_data[0].minor:%d\n",negop->icversion_data[0].minor);
        printf("negop->icversion_data[1].major:%d\n",negop->icversion_data[1].major);
        printf("negop->icversion_data[1].minor:%d\n",negop->icversion_data[1].minor);
}


/**
 * Set host time based on time sync message from host
 */
static void
hv_set_host_time(void *context)
{
	uint64_t	hosttime = (uint64_t)context;
	struct timespec	ts, host_ts;
	int64_t		tns, host_tns, tmp, tsec;

	nanotime(&ts);
	tns = ts.tv_sec * HV_NANO_SEC + ts.tv_nsec;
	host_tns = (hosttime - HV_WLTIMEDELTA) * 100;

	tmp = host_tns;
	tsec = tmp / HV_NANO_SEC;
	host_ts.tv_nsec = (long) (tmp - (tsec * HV_NANO_SEC));
	host_ts.tv_sec = tsec;

	/* force time sync with host after reboot, restore, etc. */
	mtx_lock(&Giant);
	tc_setclock(&host_ts);
	resettodr();
	mtx_unlock(&Giant);
}

/**
 * @brief Synchronize time with host after reboot, restore, etc.
 *
 * ICTIMESYNCFLAG_SYNC flag bit indicates reboot, restore events of the VM.
 * After reboot the flag ICTIMESYNCFLAG_SYNC is included in the first time
 * message after the timesync channel is opened. Since the hv_utils module is
 * loaded after hv_vmbus, the first message is usually missed. The other
 * thing is, systime is automatically set to emulated hardware clock which may
 * not be UTC time or in the same time zone. So, to override these effects, we
 * use the first 50 time samples for initial system time setting.
 */
static inline
void hv_adj_guesttime(uint64_t hosttime, uint8_t flags)
{
	static int scnt = 50;

	if ((flags & HV_ICTIMESYNCFLAG_SYNC) != 0) {
		hv_queue_work_item(service_table[HV_TIME_SYNCH].work_queue,
			hv_set_host_time, (void *) hosttime);
		return;
	}

	if ((flags & HV_ICTIMESYNCFLAG_SAMPLE) != 0 && scnt > 0) {
		scnt--;
		hv_queue_work_item(service_table[HV_TIME_SYNCH].work_queue,
			hv_set_host_time, (void *) hosttime);
	}
}

/**
 * Time Sync Channel message handler
 */
static void
hv_timesync_cb(void *context)
{
	hv_vmbus_channel*	channel = context;
	hv_vmbus_icmsg_hdr*	icmsghdrp;
	uint32_t		recvlen;
	uint64_t		requestId;
	int			ret;
	uint8_t*		time_buf;
	struct hv_ictimesync_data* timedatap;

	time_buf = receive_buffer[HV_TIME_SYNCH];

	ret = hv_vmbus_channel_recv_packet(channel, time_buf,
		PAGE_SIZE, &recvlen, &requestId);

	if ((ret == 0) && recvlen > 0) {
		icmsghdrp = (struct hv_vmbus_icmsg_hdr *) &time_buf[
			sizeof(struct hv_vmbus_pipe_hdr)];

		if (icmsghdrp->icmsgtype == HV_ICMSGTYPE_NEGOTIATE) {
			hv_negotiate_version(icmsghdrp, NULL, time_buf);
		} else {
			timedatap = (struct hv_ictimesync_data *) &time_buf[
				sizeof(struct hv_vmbus_pipe_hdr) +
					sizeof(struct hv_vmbus_icmsg_hdr)];
			hv_adj_guesttime(
				timedatap->parenttime,
				timedatap->flags);
		}

		icmsghdrp->icflags = HV_ICMSGHDRFLAG_TRANSACTION
			| HV_ICMSGHDRFLAG_RESPONSE;

		hv_vmbus_channel_send_packet(channel, time_buf,
			recvlen, requestId,
			HV_VMBUS_PACKET_TYPE_DATA_IN_BAND, 0);
	}
}

/**
 * Shutdown
 */
static void
hv_shutdown_cb(void *context)
{
	uint8_t*			buf;
	hv_vmbus_channel*		channel = context;
	uint8_t				execute_shutdown = 0;
	hv_vmbus_icmsg_hdr*		icmsghdrp;
	uint32_t			recv_len;
	uint64_t			request_id;
	int				ret;
	hv_vmbus_shutdown_msg_data*	shutdown_msg;

	buf = receive_buffer[HV_SHUT_DOWN];

	ret = hv_vmbus_channel_recv_packet(channel, buf, PAGE_SIZE,
		&recv_len, &request_id);

	if ((ret == 0) && recv_len > 0) {

		icmsghdrp = (struct hv_vmbus_icmsg_hdr *)
			&buf[sizeof(struct hv_vmbus_pipe_hdr)];

		if (icmsghdrp->icmsgtype == HV_ICMSGTYPE_NEGOTIATE) {
			hv_negotiate_version(icmsghdrp, NULL, buf);

		} else {
			shutdown_msg =
				(struct hv_vmbus_shutdown_msg_data *)
				&buf[sizeof(struct hv_vmbus_pipe_hdr) +
					sizeof(struct hv_vmbus_icmsg_hdr)];

			switch (shutdown_msg->flags) {
			case 0:
				case 1:
				icmsghdrp->status = HV_S_OK;
				execute_shutdown = 1;
				if (bootverbose)
					printf(
						"Shutdown request received -"
							" graceful shutdown initiated\n");
				break;
			default:
				icmsghdrp->status = HV_E_FAIL;
				execute_shutdown = 0;
				printf("Shutdown request received -"
					" Invalid request\n");
				break;
			}
		}

		icmsghdrp->icflags = HV_ICMSGHDRFLAG_TRANSACTION |
			HV_ICMSGHDRFLAG_RESPONSE;

		hv_vmbus_channel_send_packet(channel, buf,
			recv_len, request_id,
			HV_VMBUS_PACKET_TYPE_DATA_IN_BAND, 0);
	}

	if (execute_shutdown)
		shutdown_nice(RB_POWEROFF);
}

/**
 * Process heartbeat message
 */
static void
hv_heartbeat_cb(void *context)
{
	uint8_t*		buf;
	hv_vmbus_channel*	channel = context;
	uint32_t		recvlen;
	uint64_t		requestid;
	int			ret;

	struct hv_vmbus_heartbeat_msg_data*	heartbeat_msg;
	struct hv_vmbus_icmsg_hdr*		icmsghdrp;

	buf = receive_buffer[HV_HEART_BEAT];

	ret = hv_vmbus_channel_recv_packet(channel, buf, PAGE_SIZE, &recvlen,
		&requestid);

	if ((ret == 0) && recvlen > 0) {

		icmsghdrp = (struct hv_vmbus_icmsg_hdr *)
			&buf[sizeof(struct hv_vmbus_pipe_hdr)];

		if (icmsghdrp->icmsgtype == HV_ICMSGTYPE_NEGOTIATE) {
			hv_negotiate_version(icmsghdrp, NULL, buf);

		} else {
			heartbeat_msg =
				(struct hv_vmbus_heartbeat_msg_data *)
				&buf[sizeof(struct hv_vmbus_pipe_hdr) +
					sizeof(struct hv_vmbus_icmsg_hdr)];

			heartbeat_msg->seq_num += 1;
		}

		icmsghdrp->icflags = HV_ICMSGHDRFLAG_TRANSACTION |
			HV_ICMSGHDRFLAG_RESPONSE;

		hv_vmbus_channel_send_packet(channel, buf, recvlen, requestid,
			HV_VMBUS_PACKET_TYPE_DATA_IN_BAND, 0);
	}
}



static int
hv_util_probe(device_t dev)
{
	int i;
	int rtn_value = ENXIO;

	for (i = 0; i < HV_MAX_UTIL_SERVICES; i++) {
		const char *p = vmbus_get_type(dev);
		if (service_table[i].enabled
			&& !memcmp(p, &service_table[i].guid, sizeof(hv_guid))) {
			device_set_softc(dev, (void *) (&service_table[i]));
			rtn_value = 0;
		}
	}

	return rtn_value;
}


static int
hv_util_attach(device_t dev)
{
	struct hv_device*		hv_dev;
	struct hv_vmbus_service*	service;
	int				ret;
	size_t				receive_buffer_offset;

	hv_dev = vmbus_get_devctx(dev);
	service = device_get_softc(dev);
	receive_buffer_offset = service - &service_table[0];
	device_printf(dev, "Hyper-V Service attaching: %s\n", service->name);

	/* Allocate twice PAGE_SIZE for KVP */
	if (receive_buffer_offset == HV_KVP) {
#if 0
		/* Do not allocate buffer for KVP service here */
		receive_buffer[receive_buffer_offset] =
			malloc(PAGE_SIZE*2, M_DEVBUF, M_WAITOK | M_ZERO);
#endif
		printf("hv_util_attach: HV_KVP should not be called\n");
		return 0;
	}
	else {
		receive_buffer[receive_buffer_offset] =
			malloc(PAGE_SIZE, M_DEVBUF, M_WAITOK | M_ZERO);
	}

	if (service->init != NULL) {
		ret = service->init(service);
		if (ret) {
			ret = ENODEV;
			goto error0;
		}
	}

	ret = hv_vmbus_channel_open(hv_dev->channel, 2 * PAGE_SIZE,
		2 * PAGE_SIZE, NULL, 0,
		service->callback, hv_dev->channel);

	if (ret)
		goto error0;

	return (0);

	error0:

	free(receive_buffer[receive_buffer_offset], M_DEVBUF);
	receive_buffer[receive_buffer_offset] = NULL;

	return (ret);
}

static int
hv_util_detach(device_t dev)
{
	struct hv_device* hv_dev;
	struct hv_vmbus_service* service;
	size_t receive_buffer_offset;

	hv_dev = vmbus_get_devctx(dev);

	hv_vmbus_channel_close(hv_dev->channel);
	service = device_get_softc(dev);

	receive_buffer_offset = service - &service_table[0];

	/* KVP related work is done in kvp_unload and not here */
	if (receive_buffer_offset == HV_KVP) {
		printf("hv_util_detach: HV_KVP should not be called\n");
		return 0;
	}

	if (service->work_queue != NULL)
		hv_work_queue_close(service->work_queue);

	free(receive_buffer[receive_buffer_offset], M_DEVBUF);
	receive_buffer[receive_buffer_offset] = NULL;

	return (0);
}

static void hv_util_init(void)
{
}

static int hv_util_modevent(module_t mod, int event, void *arg)
{
	switch (event) {
	case MOD_LOAD:
		break;
	case MOD_UNLOAD:
		break;
	default:
		break;
	}
	return (0);
}

static device_method_t util_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe, hv_util_probe),
	DEVMETHOD(device_attach, hv_util_attach),
	DEVMETHOD(device_detach, hv_util_detach),
	DEVMETHOD(device_shutdown, bus_generic_shutdown),
	{ 0, 0 } }
;

static driver_t util_driver = { "hyperv-utils", util_methods, 0 };

static devclass_t util_devclass;

DRIVER_MODULE(hv_utils, vmbus, util_driver, util_devclass, hv_util_modevent, 0);
MODULE_VERSION(hv_utils, 1);
MODULE_DEPEND(hv_utils, vmbus, 1, 1, 1);

SYSINIT(hv_util_initx, SI_SUB_RUN_SCHEDULER, SI_ORDER_MIDDLE + 1,
	hv_util_init, NULL);
