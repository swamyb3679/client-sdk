/*
 * Copyright 2020 Intel Corporation
 * SPDX-License-Identifier: Apache 2.0
 */

/*!
 * \file
 * \brief Implementation of SDO protocol spec. The APIs in this file realize
 * various aspects of SDO protcol.
 */

#include "sdoCryptoApi.h"
#include "util.h"
#include "sdoprot.h"
#include "load_credentials.h"
#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include "safe_lib.h"
#include "snprintf_s.h"

/* This is a test mode to skip the CEC1702 signing and present a constant n4
 * nonce and signature.  These were generated in the server. */
//#define CONSTANT_N4

#ifndef asizeof
#define asizeof(x) (sizeof(x) / sizeof(x)[0])
#endif

#define DI_ID_TO_STATE_FN(id) (id - SDO_STATE_DI_APP_START)
#define TO1_ID_TO_STATE_FN(id) (id - SDO_STATE_T01_SND_HELLO_SDO)
#define TO2_ID_TO_STATE_FN(id) (id - SDO_STATE_T02_SND_HELLO_DEVICE)

typedef int (*state_func)(SDOProt_t *ps);

/*
 * State functions for DI
 */
static state_func di_state_fn[] = {
    msg10, /* DI.AppStart */
    msg11, /* DI.SetCredentials */
    msg12, /* DI.SetHMAC */
    msg13, /* DI.Done */
};

/*
 * State functions for TO1
 */
static state_func to1_state_fn[] = {
    msg30, /* TO1.HelloSDO */
    msg31, /* TO1.HelloSDOAck */
    msg32, /* TO1.ProveToSDO */
    msg33, /* TO1.SDORedirect */
};

/*
 * State functions for TO2
 */
static state_func to2_state_fn[] = {
    msg40, /* TO2.HelloDevice */
    msg41, /* TO2.ProveOPHdr */
    msg42, /* TO2.GetOPNextEntry */
    msg43, /* TO2.OPNextEntry */
    msg44, /* TO2.ProveDevice */
    msg45, /* TO2.GetNextDeviceServiceInfo */
    msg46, /* TO2.NextDeviceServiceInfo */
    msg47, /* TO2.SetupDevice */
    msg48, /* TO2.GetNextOwnerServiceInfo */
    msg49, /* TO2.OwnerServiceInfo */
    msg50, /* TO2.Done */
    msg51, /* TO2.Done2 */
};

/**
 * ps_free() - free all the protocol state
 * ps stores the message data which gets used in the next messages, so,
 * this function needs to be called in:
 * a. Error handling to free all state data
 * b. When the state machine is completed successfully
 */
static void ps_free(SDOProt_t *ps)
{
	if (ps->SDORedirect.plainText) {
		sdoByteArrayFree(ps->SDORedirect.plainText);
		ps->SDORedirect.plainText = NULL;
	}
	if (ps->SDORedirect.Obsig) {
		sdoByteArrayFree(ps->SDORedirect.Obsig);
		ps->SDORedirect.Obsig = NULL;
	}
	if (ps->n5) {
		sdoByteArrayFree(ps->n5);
		ps->n5 = NULL;
	}
	if (ps->n5r) {
		sdoByteArrayFree(ps->n5r);
		ps->n5r = NULL;
	}
	if (ps->newOVHdrHMAC) {
		sdoHashFree(ps->newOVHdrHMAC);
		ps->newOVHdrHMAC = NULL;
	}
	if (ps->n6) {
		sdoByteArrayFree(ps->n6);
		ps->n6 = NULL;
	}
	if (ps->n7r) {
		sdoByteArrayFree(ps->n7r);
		ps->n7r = NULL;
	}
}

/**
 * Allocate memory for resources required to run DI protocol and set state
 * variables to init state.
 *
 * @param ps
 *        Pointer to the database containtain all protocol state variables.
 * @param devCred
 *        Pointer to the database containtain Device credentials.
 * @return ret
 *         None.
 */
void sdoProtDIInit(SDOProt_t *ps, SDODevCred_t *devCred)
{
	ps->state = SDO_STATE_DI_INIT;
	ps->devCred = devCred;
	ps->success = false;
}

/**
 * Manage the protocol state machine
 *
 * @param ps
 *        Pointer to the database containtain all protocol state variables.
 * @return
 *        "true" in case of success. "false" if failed.
 */
bool sdo_process_states(SDOProt_t *ps)
{
	bool status = false;
	int prevState = 0;
	state_func state_fn = NULL;

	for (;;) {

		/*
		 * Retaining the older logic of state machine. For the states to
		 * process, the message processor has to be update ps->state. In
		 * case the state is not changed and no error has been reported,
		 * it means that the data read from network is pending, so, we
		 * read data and come back here for the same message processing
		 */
		prevState = ps->state;

		switch (ps->state) {
		/* DI states */
		case SDO_STATE_DI_APP_START:
		case SDO_STATE_DI_SET_CREDENTIALS:
		case SDO_STATE_DI_SET_HMAC:
		case SDO_STATE_DI_DONE:
			state_fn = di_state_fn[DI_ID_TO_STATE_FN(ps->state)];
			break;

		/* TO1 states */
		case SDO_STATE_T01_SND_HELLO_SDO:
		case SDO_STATE_TO1_RCV_HELLO_SDOACK:
		case SDO_STATE_TO1_SND_PROVE_TO_SDO:
		case SDO_STATE_TO1_RCV_SDO_REDIRECT:
			state_fn = to1_state_fn[TO1_ID_TO_STATE_FN(ps->state)];
			break;

		/* TO2 states */
		case SDO_STATE_T02_SND_HELLO_DEVICE:
		case SDO_STATE_TO2_RCV_PROVE_OVHDR:
		case SDO_STATE_TO2_SND_GET_OP_NEXT_ENTRY:
		case SDO_STATE_T02_RCV_OP_NEXT_ENTRY:
		case SDO_STATE_TO2_SND_PROVE_DEVICE:
		case SDO_STATE_TO2_RCV_GET_NEXT_DEVICE_SERVICE_INFO:
		case SDO_STATE_TO2_SND_NEXT_DEVICE_SERVICE_INFO:
		case SDO_STATE_TO2_RCV_SETUP_DEVICE:
		case SDO_STATE_T02_SND_GET_NEXT_OWNER_SERVICE_INFO:
		case SDO_STATE_T02_RCV_NEXT_OWNER_SERVICE_INFO:
		case SDO_STATE_TO2_SND_DONE:
		case SDO_STATE_TO2_RCV_DONE_2:
			state_fn = to2_state_fn[TO2_ID_TO_STATE_FN(ps->state)];
			break;

		case SDO_STATE_ERROR:
		case SDO_STATE_DONE:
		default:
			break;
		}

		/*
		 * FIXME: ps->state cannot start with a junk state. It is for
		 * unit test to pass
		 */
		if (!state_fn)
			break;

		if (state_fn && state_fn(ps)) {
			char err_msg[64];

			(void)snprintf_s_i(err_msg, sizeof(err_msg),
					   "msg%d: message parse error",
					   ps->state);
			ps->state = SDO_STATE_ERROR;
			sdoSendErrorMessage(&ps->sdow, MESSAGE_BODY_ERROR,
					    ps->state, err_msg);
			ps_free(ps);
			break;
		}

		/* If we reached with msg51 as ps->state, we are done */
		if (prevState == SDO_STATE_TO2_RCV_DONE_2 &&
		    ps->state == SDO_STATE_DONE) {
			ps_free(ps);
		}

		/* Break for reading network data */
		if (prevState == ps->state) {
			status = true;
			break;
		}
	}

	return status;
}

/**
 * Allocate memory for resources required to run TO1 protocol and set state
 * variables to init state.
 *
 * @param ps
 *        Pointer to the database containtain all protocol state variables.
 * @param devCred
 *        Pointer to the database containtain Device credentials.
 * @return ret
 *         0 on success and -1 on failure
 */
int32_t sdoProtTO1Init(SDOProt_t *ps, SDODevCred_t *devCred)
{
	if (!ps || !devCred || !devCred->ownerBlk) {
		return -1;
	}
	ps->state = SDO_STATE_TO1_INIT;
	ps->g2 = devCred->ownerBlk->guid;
	ps->devCred = devCred;
	ps->success = false;
	return 0;
}

/**
 * Allocate memory for resources required to run TO2 protocol and set state
 * variables to init state.
 *
 * @param ps
 *        Pointer to the database containtain all protocol state variables.
 * @param si
 *        Pointer to device service info database.
 * @param devCred
 *        Pointer to the database containtain Device credentials.
 * @param moduleList
 *        Global Module List Head Pointer.
 * @return
 *        true if success, false otherwise.
 *
 */
bool sdoProtTO2Init(SDOProt_t *ps, SDOServiceInfo_t *si, SDODevCred_t *devCred,
		    sdoSdkServiceInfoModuleList_t *moduleList)
{
	ps->state = SDO_STATE_T02_INIT;
	ps->keyEncoding = SDO_OWNER_ATTEST_PK_ENC;

	ps->success = false;
	ps->serviceInfo = si;
	ps->devCred = devCred;
	ps->g2 = devCred->ownerBlk->guid;
	ps->RoundTripCount = 0;
	ps->iv = sdoAlloc(sizeof(SDOIV_t));
	if (!ps->iv) {
		LOG(LOG_ERROR, "Malloc failed!\n");
		return false;
	}

	/* Initialize svinfo related data */
	if (moduleList) {
		ps->SvInfoModListHead = moduleList;
		ps->dsiInfo = sdoAlloc(sizeof(sdoSvInfoDsiInfo_t));
		if (!ps->dsiInfo) {
			return false;
		}

		ps->dsiInfo->list_dsi = ps->SvInfoModListHead;
		ps->dsiInfo->moduleDsiIndex = 0;

		/* Execute SvInfo type=START */
		if (!sdoModExecSvInfotype(ps->SvInfoModListHead,
					  SDO_SI_START)) {
			LOG(LOG_ERROR,
			    "SvInfo: One or more module's START failed\n");
			sdoFree(ps->iv);
			sdoFree(ps->dsiInfo);
			return false;
		}
	} else
		LOG(LOG_DEBUG,
		    "SvInfo: no modules are registered to the SDO!\n");

	//	LOG(LOG_DEBUG, "Key Exchange Mode: %s\n", ps->kx->bytes);
	//	LOG(LOG_DEBUG, "Cipher Suite: %s\n", ps->cs->bytes);

	return true;
}

/**
 * Check total number of round trips in TO2 exceeded the limit.
 *
 * @param ps
 *        Pointer to the database containtain all protocol state variables.
 * @return
 *        false if roundtrip limit exceeded, true otherwise.
 */
bool sdoCheckTO2RoundTrips(SDOProt_t *ps)
{
	if (ps->RoundTripCount > MAX_TO2_ROUND_TRIPS) {
		LOG(LOG_ERROR, "Exceeded maximum number of TO2 rounds\n");
		sdoSendErrorMessage(&ps->sdow, INTERNAL_SERVER_ERROR, ps->state,
				    "Exceeded max number of rounds");
		ps->state = SDO_STATE_ERROR;
		return false;
	}
	ps->RoundTripCount++;
	return true;
}

/**
 * Check if we have received a REST message.
 *
 * @param sdor
 *        Pointer to received JSON packet.
 * @param sdow
 *        Pointer to outgoing JSON packet which has been composed by Protocol
 * APIs(DI_Run/TO1_Run/TO2_Run).
 * @param protName
 *        Name of Protocol(DI/TO1/TO2).
 * @param statep
 *        Current state of Protocol state machine.
 * @return
 *        true in case of new message received. flase if no message to read.
 */
bool sdoProtRcvMsg(SDOR_t *sdor, SDOW_t *sdow, char *protName, int *statep)
{
	uint32_t mtype;

	if (sdor->receive == NULL && !sdoRHaveBlock(sdor))
		return false;

	if (!sdoRNextBlock(sdor, &mtype)) {
		LOG(LOG_ERROR, "expecting another block\n");
		*statep = SDO_STATE_ERROR;
		return false;
	}
	LOG(LOG_DEBUG, "%s: Received message type %" PRIu32 " : %d bytes\n",
	    protName, mtype, sdor->b.blockSize);

	return true;
}

/**
 * Internal API
 */
void sdoSendErrorMessage(SDOW_t *sdow, int ecode, int msgnum,
			 const char *errmsg)
{
	LOG(LOG_ERROR, "Sending Error Message\n");

	sdoWNextBlock(sdow, SDO_TYPE_ERROR);
	sdoWBeginObject(sdow);
	sdoWriteTag(sdow, "ec");
	sdoWriteUInt(sdow, ecode);
	sdoWriteTag(sdow, "emsg");
	sdoWriteUInt(sdow, msgnum);
	sdoWriteTag(sdow, "em");
	sdoWriteString(sdow, errmsg);
	sdoWEndObject(sdow);
}

#if 0
/**
 * Receive the error message
 * @param sdor - pointer to the input buffer
 * @param ecode - error code
 * @param msgnum - pointer to the SDO message number
 * @param errmsg - pointer to the error message string
 * @param errmsgSz - size of error message string
 */
void sdoReceiveErrorMessage(SDOR_t *sdor, int *ecode, int *msgnum, char *errmsg,
			    int errmsgSz)
{
	// Called after SDONextBlock...
	// SDONextBlock(sdor, &mtype, &majVer, &minVer);
	if (!sdoRBeginObject(sdor)) {
		LOG(LOG_ERROR, "Begin Object not found.\n");
		goto fail;
	}
	*ecode = 0;
	*msgnum = 255;
	if (strncpy_s(errmsg, errmsgSz, "error message parse failed",
		      errmsgSz) != 0) {
		LOG(LOG_ERROR, "strcpy() failed!\n");
	}
	if (!sdoReadExpectedTag(sdor, "ec"))
		goto fail;
	*ecode = sdoReadUInt(sdor);
	if (!sdoReadExpectedTag(sdor, "emsg"))
		goto fail;
	*msgnum = sdoReadUInt(sdor);
	if (!sdoReadExpectedTag(sdor, "em"))
		goto fail;
	if (!sdoReadString(sdor, errmsg, errmsgSz)) {
		LOG(LOG_ERROR, "sdoReceiveErrorMessage(): sdoReadString() "
			       "returned NULL!\n");
		goto fail;
	}
	if (!sdoREndObject(sdor)) {
		LOG(LOG_ERROR, "End Object not found.\n");
		goto fail;
	}
fail:
	sdoRFlush(sdor);
}
#endif
