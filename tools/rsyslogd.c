/* This is the main rsyslogd file.
 * It contains code * that is known to be validly under ASL 2.0,
 * because it was either written from scratch by me (rgerhards) or
 * contributors who agreed to ASL 2.0.
 *
 * Copyright 2004-2014 Rainer Gerhards and Adiscon
 *
 * This file is part of rsyslog.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *       http://www.apache.org/licenses/LICENSE-2.0
 *       -or-
 *       see COPYING.ASL20 in the source distribution
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "config.h"
#include "rsyslog.h"

#include <signal.h>
#include <liblogging/stdlog.h>
#ifdef OS_SOLARIS
#	include <errno.h>
#else
#	include <sys/errno.h>
#endif
#include "sd-daemon.h"

#include "wti.h"
#include "ratelimit.h"
#include "parser.h"
#include "linkedlist.h"
#include "ruleset.h"
#include "action.h"
#include "iminternal.h"
#include "errmsg.h"
#include "threads.h"
#include "dnscache.h"
#include "prop.h"
#include "unicode-helper.h"
#include "net.h"
#include "errmsg.h"
#include "glbl.h"
#include "debug.h"
#include "srUtils.h"
#include "rsconf.h"
#include "cfsysline.h"
#include "datetime.h"
#include "dirty.h"

DEFobjCurrIf(obj)
DEFobjCurrIf(prop)
DEFobjCurrIf(parser)
DEFobjCurrIf(ruleset)
DEFobjCurrIf(net)
DEFobjCurrIf(errmsg)
DEFobjCurrIf(rsconf)
DEFobjCurrIf(module)
DEFobjCurrIf(datetime)
DEFobjCurrIf(glbl)

/* imports from syslogd.c, these should go away over time (as we
 * migrate/replace more and more code to ASL 2.0).
 */
extern int bHadHUP;
extern int bFinished;
extern int doFork;
extern pid_t ppid;
extern char *PidFile;

extern int realMain(int argc, char **argv);
extern rsRetVal queryLocalHostname(void);
void syslogdInit(void);
void syslogd_die(void);
void syslogd_releaseClassPointers(void);
void syslogd_sighup_handler();
char **syslogd_crunch_list(char *list);
void syslogd_printVersion(void);
rsRetVal syslogd_doGlblProcessInit(void);
rsRetVal syslogd_obtainClassPointers(void);
/* end syslogd.c imports */
extern int yydebug; /* interface to flex */


/* forward definitions */
void rsyslogd_submitErrMsg(const int severity, const int iErr, const uchar *msg);


/* global data items */
rsconf_t *ourConf = NULL;	/* our config object */
int MarkInterval = 20 * 60;	/* interval between marks in seconds - read-only after startup */
ratelimit_t *dflt_ratelimiter = NULL; /* ratelimiter for submits without explicit one */
uchar *ConfFile = (uchar*) "/etc/rsyslog.conf";
int bHaveMainQueue = 0;/* set to 1 if the main queue - in queueing mode - is available
			* If the main queue is either not yet ready or not running in 
			* queueing mode (mode DIRECT!), then this is set to 0.
			*/
qqueue_t *pMsgQueue = NULL;	/* default main message queue */
prop_t *pInternalInputName = NULL;	/* there is only one global inputName for all internally-generated messages */
ratelimit_t *internalMsg_ratelimiter = NULL; /* ratelimiter for rsyslog-own messages */
int send_to_all = 0;   /* send message to all IPv4/IPv6 addresses */

static struct queuefilenames_s {
	struct queuefilenames_s *next;
	uchar *name;
} *queuefilenames = NULL;


void
rsyslogd_usage(void)
{
	fprintf(stderr, "usage: rsyslogd [options]\n"
			"use \"man rsyslogd\" for details. To run rsyslog "
			"interactively, use \"rsyslogd -n\""
			"to run it in debug mode use \"rsyslogd -dn\"\n"
			"For further information see http://www.rsyslog.com/doc\n");
	exit(1); /* "good" exit - done to terminate usage() */
}

/* This is a support function for imdiag. It returns back the approximate
 * current number of messages in the main message queue
 * This number includes the messages that reside in an associated DA queue (if
 * it exists) -- rgerhards, 2009-10-14
 * Note that this is imprecise, but needed for the testbench. It should not be used
 * for any other purpose -- impstats is the right tool for all other cases.
 */
rsRetVal
diagGetMainMsgQSize(int *piSize)
{
	DEFiRet;
	assert(piSize != NULL);
	*piSize = (pMsgQueue->pqDA != NULL) ? pMsgQueue->pqDA->iQueueSize : 0;
	*piSize += pMsgQueue->iQueueSize;
	RETiRet;
}


void
rsyslogd_sigttin_handler()
{
	/* this is just a dummy to care for our sigttin input
	 * module cancel interface. The important point is that
	 * it actually does *nothing*.
	 */
}

rsRetVal
rsyslogd_InitStdRatelimiters(void)
{
	DEFiRet;
	CHKiRet(ratelimitNew(&dflt_ratelimiter, "rsyslogd", "dflt"));
	/* TODO: add linux-type limiting capability */
	CHKiRet(ratelimitNew(&internalMsg_ratelimiter, "rsyslogd", "internal_messages"));
	ratelimitSetLinuxLike(internalMsg_ratelimiter, 5, 500);
	/* TODO: make internalMsg ratelimit settings configurable */
finalize_it:
	RETiRet;
}


/* Method to initialize all global classes and use the objects that we need.
 * rgerhards, 2008-01-04
 * rgerhards, 2008-04-16: the actual initialization is now carried out by the runtime
 */
rsRetVal
rsyslogd_InitGlobalClasses(void)
{
	DEFiRet;
	char *pErrObj; /* tells us which object failed if that happens (useful for troubleshooting!) */

	/* Intialize the runtime system */
	pErrObj = "rsyslog runtime"; /* set in case the runtime errors before setting an object */
	CHKiRet(rsrtInit(&pErrObj, &obj));
	rsrtSetErrLogger(rsyslogd_submitErrMsg);

	/* Now tell the system which classes we need ourselfs */
	pErrObj = "glbl";
	CHKiRet(objUse(glbl,     CORE_COMPONENT));
	pErrObj = "errmsg";
	CHKiRet(objUse(errmsg,   CORE_COMPONENT));
	pErrObj = "module";
	CHKiRet(objUse(module,   CORE_COMPONENT));
	pErrObj = "datetime";
	CHKiRet(objUse(datetime, CORE_COMPONENT));
	pErrObj = "ruleset";
	CHKiRet(objUse(ruleset,  CORE_COMPONENT));
	/*pErrObj = "conf";
	CHKiRet(objUse(conf,     CORE_COMPONENT));*/
	pErrObj = "prop";
	CHKiRet(objUse(prop,     CORE_COMPONENT));
	pErrObj = "parser";
	CHKiRet(objUse(parser,     CORE_COMPONENT));
	pErrObj = "rsconf";
	CHKiRet(objUse(rsconf,     CORE_COMPONENT));

	/* intialize some dummy classes that are not part of the runtime */
	pErrObj = "action";
	CHKiRet(actionClassInit());
	pErrObj = "template";
	CHKiRet(templateInit());

	/* TODO: the dependency on net shall go away! -- rgerhards, 2008-03-07 */
	pErrObj = "net";
	CHKiRet(objUse(net, LM_NET_FILENAME));

	dnscacheInit();
	initRainerscript();
	ratelimitModInit();

	/* we need to create the inputName property (only once during our lifetime) */
	CHKiRet(prop.Construct(&pInternalInputName));
	CHKiRet(prop.SetString(pInternalInputName, UCHAR_CONSTANT("rsyslogd"), sizeof("rsyslogd") - 1));
	CHKiRet(prop.ConstructFinalize(pInternalInputName));

finalize_it:
	if(iRet != RS_RET_OK) {
		/* we know we are inside the init sequence, so we can safely emit
		 * messages to stderr. -- rgerhards, 2008-04-02
		 */
		fprintf(stderr, "Error during class init for object '%s' - failing...\n", pErrObj);
		fprintf(stderr, "rsyslogd initializiation failed - global classes could not be initialized.\n"
				"Did you do a \"make install\"?\n"
				"Suggested action: run rsyslogd with -d -n options to see what exactly "
				"fails.\n");
	}

	RETiRet;
}

/* preprocess a batch of messages, that is ready them for actual processing. This is done
 * as a first stage and totally in parallel to any other worker active in the system. So
 * it helps us keep up the overall concurrency level.
 * rgerhards, 2010-06-09
 */
static inline rsRetVal
preprocessBatch(batch_t *pBatch, int *pbShutdownImmediate) {
	prop_t *ip;
	prop_t *fqdn;
	prop_t *localName;
	prop_t *propFromHost = NULL;
	prop_t *propFromHostIP = NULL;
	int bIsPermitted;
	msg_t *pMsg;
	int i;
	rsRetVal localRet;
	DEFiRet;

	for(i = 0 ; i < pBatch->nElem  && !*pbShutdownImmediate ; i++) {
		pMsg = pBatch->pElem[i].pMsg;
		if((pMsg->msgFlags & NEEDS_ACLCHK_U) != 0) {
			DBGPRINTF("msgConsumer: UDP ACL must be checked for message (hostname-based)\n");
			if(net.cvthname(pMsg->rcvFrom.pfrominet, &localName, &fqdn, &ip) != RS_RET_OK)
				continue;
			bIsPermitted = net.isAllowedSender2((uchar*)"UDP",
			    (struct sockaddr *)pMsg->rcvFrom.pfrominet, (char*)propGetSzStr(fqdn), 1);
			if(!bIsPermitted) {
				DBGPRINTF("Message from '%s' discarded, not a permitted sender host\n",
					  propGetSzStr(fqdn));
				pBatch->eltState[i] = BATCH_STATE_DISC;
			} else {
				/* save some of the info we obtained */
				MsgSetRcvFrom(pMsg, localName);
				CHKiRet(MsgSetRcvFromIP(pMsg, ip));
				pMsg->msgFlags &= ~NEEDS_ACLCHK_U;
			}
		}
		if((pMsg->msgFlags & NEEDS_PARSING) != 0) {
			if((localRet = parser.ParseMsg(pMsg)) != RS_RET_OK)  {
				DBGPRINTF("Message discarded, parsing error %d\n", localRet);
				pBatch->eltState[i] = BATCH_STATE_DISC;
			}
		}
	}

finalize_it:
	if(propFromHost != NULL)
		prop.Destruct(&propFromHost);
	if(propFromHostIP != NULL)
		prop.Destruct(&propFromHostIP);
	RETiRet;
}


/* The consumer of dequeued messages. This function is called by the
 * queue engine on dequeueing of a message. It runs on a SEPARATE
 * THREAD. It receives an array of pointers, which it must iterate
 * over. We do not do any further batching, as this is of no benefit
 * for the main queue.
 */
static rsRetVal
msgConsumer(void __attribute__((unused)) *notNeeded, batch_t *pBatch, wti_t *pWti)
{
	DEFiRet;
	assert(pBatch != NULL);
	preprocessBatch(pBatch, pWti->pbShutdownImmediate);
	ruleset.ProcessBatch(pBatch, pWti);
//TODO: the BATCH_STATE_COMM must be set somewhere down the road, but we 
//do not have this yet and so we emulate -- 2010-06-10
int i;
	for(i = 0 ; i < pBatch->nElem  && !*pWti->pbShutdownImmediate ; i++) {
		pBatch->eltState[i] = BATCH_STATE_COMM;
	}
	RETiRet;
}


/* create a main message queue, now also used for ruleset queues. This function
 * needs to be moved to some other module, but it is considered acceptable for
 * the time being (remember that we want to restructure config processing at large!).
 * rgerhards, 2009-10-27
 */
rsRetVal createMainQueue(qqueue_t **ppQueue, uchar *pszQueueName, struct nvlst *lst)
{
	struct queuefilenames_s *qfn;
	uchar *qfname = NULL;
	static int qfn_renamenum = 0;
	uchar qfrenamebuf[1024];
	DEFiRet;

	/* create message queue */
	CHKiRet_Hdlr(qqueueConstruct(ppQueue, ourConf->globals.mainQ.MainMsgQueType, ourConf->globals.mainQ.iMainMsgQueueNumWorkers, ourConf->globals.mainQ.iMainMsgQueueSize, msgConsumer)) {
		/* no queue is fatal, we need to give up in that case... */
		errmsg.LogError(0, iRet, "could not create (ruleset) main message queue"); \
	}
	/* name our main queue object (it's not fatal if it fails...) */
	obj.SetName((obj_t*) (*ppQueue), pszQueueName);

	if(lst == NULL) { /* use legacy parameters? */
		/* ... set some properties ... */
	#	define setQPROP(func, directive, data) \
		CHKiRet_Hdlr(func(*ppQueue, data)) { \
			errmsg.LogError(0, NO_ERRCODE, "Invalid " #directive ", error %d. Ignored, running with default setting", iRet); \
		}
	#	define setQPROPstr(func, directive, data) \
		CHKiRet_Hdlr(func(*ppQueue, data, (data == NULL)? 0 : strlen((char*) data))) { \
			errmsg.LogError(0, NO_ERRCODE, "Invalid " #directive ", error %d. Ignored, running with default setting", iRet); \
		}

		if(ourConf->globals.mainQ.pszMainMsgQFName != NULL) {
			/* check if the queue file name is unique, else emit an error */
			for(qfn = queuefilenames ; qfn != NULL ; qfn = qfn->next) {
				dbgprintf("check queue file name '%s' vs '%s'\n", qfn->name, ourConf->globals.mainQ.pszMainMsgQFName );
				if(!ustrcmp(qfn->name, ourConf->globals.mainQ.pszMainMsgQFName)) {
					snprintf((char*)qfrenamebuf, sizeof(qfrenamebuf), "%d-%s-%s",
						 ++qfn_renamenum, ourConf->globals.mainQ.pszMainMsgQFName,  
						 (pszQueueName == NULL) ? "NONAME" : (char*)pszQueueName);
					qfname = ustrdup(qfrenamebuf);
					errmsg.LogError(0, NO_ERRCODE, "Error: queue file name '%s' already in use "
						" - using '%s' instead", ourConf->globals.mainQ.pszMainMsgQFName, qfname);
					break;
				}
			}
			if(qfname == NULL)
				qfname = ustrdup(ourConf->globals.mainQ.pszMainMsgQFName);
			qfn = malloc(sizeof(struct queuefilenames_s));
			qfn->name = qfname;
			qfn->next = queuefilenames;
			queuefilenames = qfn;
		}

		setQPROP(qqueueSetMaxFileSize, "$MainMsgQueueFileSize", ourConf->globals.mainQ.iMainMsgQueMaxFileSize);
		setQPROP(qqueueSetsizeOnDiskMax, "$MainMsgQueueMaxDiskSpace", ourConf->globals.mainQ.iMainMsgQueMaxDiskSpace);
		setQPROP(qqueueSetiDeqBatchSize, "$MainMsgQueueDequeueBatchSize", ourConf->globals.mainQ.iMainMsgQueDeqBatchSize);
		setQPROPstr(qqueueSetFilePrefix, "$MainMsgQueueFileName", qfname);
		setQPROP(qqueueSetiPersistUpdCnt, "$MainMsgQueueCheckpointInterval", ourConf->globals.mainQ.iMainMsgQPersistUpdCnt);
		setQPROP(qqueueSetbSyncQueueFiles, "$MainMsgQueueSyncQueueFiles", ourConf->globals.mainQ.bMainMsgQSyncQeueFiles);
		setQPROP(qqueueSettoQShutdown, "$MainMsgQueueTimeoutShutdown", ourConf->globals.mainQ.iMainMsgQtoQShutdown );
		setQPROP(qqueueSettoActShutdown, "$MainMsgQueueTimeoutActionCompletion", ourConf->globals.mainQ.iMainMsgQtoActShutdown);
		setQPROP(qqueueSettoWrkShutdown, "$MainMsgQueueWorkerTimeoutThreadShutdown", ourConf->globals.mainQ.iMainMsgQtoWrkShutdown);
		setQPROP(qqueueSettoEnq, "$MainMsgQueueTimeoutEnqueue", ourConf->globals.mainQ.iMainMsgQtoEnq);
		setQPROP(qqueueSetiHighWtrMrk, "$MainMsgQueueHighWaterMark", ourConf->globals.mainQ.iMainMsgQHighWtrMark);
		setQPROP(qqueueSetiLowWtrMrk, "$MainMsgQueueLowWaterMark", ourConf->globals.mainQ.iMainMsgQLowWtrMark);
		setQPROP(qqueueSetiDiscardMrk, "$MainMsgQueueDiscardMark", ourConf->globals.mainQ.iMainMsgQDiscardMark);
		setQPROP(qqueueSetiDiscardSeverity, "$MainMsgQueueDiscardSeverity", ourConf->globals.mainQ.iMainMsgQDiscardSeverity);
		setQPROP(qqueueSetiMinMsgsPerWrkr, "$MainMsgQueueWorkerThreadMinimumMessages", ourConf->globals.mainQ.iMainMsgQWrkMinMsgs);
		setQPROP(qqueueSetbSaveOnShutdown, "$MainMsgQueueSaveOnShutdown", ourConf->globals.mainQ.bMainMsgQSaveOnShutdown);
		setQPROP(qqueueSetiDeqSlowdown, "$MainMsgQueueDequeueSlowdown", ourConf->globals.mainQ.iMainMsgQDeqSlowdown);
		setQPROP(qqueueSetiDeqtWinFromHr,  "$MainMsgQueueDequeueTimeBegin", ourConf->globals.mainQ.iMainMsgQueueDeqtWinFromHr);
		setQPROP(qqueueSetiDeqtWinToHr,    "$MainMsgQueueDequeueTimeEnd", ourConf->globals.mainQ.iMainMsgQueueDeqtWinToHr);

	#	undef setQPROP
	#	undef setQPROPstr
	} else { /* use new style config! */
		qqueueSetDefaultsRulesetQueue(*ppQueue);
		qqueueApplyCnfParam(*ppQueue, lst);
	}
	RETiRet;
}

rsRetVal
startMainQueue(qqueue_t *pQueue)
{
	DEFiRet;
	CHKiRet_Hdlr(qqueueStart(pQueue)) {
		/* no queue is fatal, we need to give up in that case... */
		errmsg.LogError(0, iRet, "could not start (ruleset) main message queue"); \
	}
	RETiRet;
}


/* this is a special function used to submit an error message. This
 * function is also passed to the runtime library as the generic error
 * message handler. -- rgerhards, 2008-04-17
 */
void
rsyslogd_submitErrMsg(const int severity, const int iErr, const uchar *msg)
{
	logmsgInternal(iErr, LOG_SYSLOG|(severity & 0x07), msg, 0);
}

static inline rsRetVal
submitMsgWithDfltRatelimiter(msg_t *pMsg)
{
	return ratelimitAddMsg(dflt_ratelimiter, NULL, pMsg);
}


/* This function logs a message to rsyslog itself, using its own
 * internal structures. This means external programs (like the
 * system journal) will never see this message.
 */
static rsRetVal
logmsgInternalSelf(const int iErr, const int pri, const size_t lenMsg,
	const char *__restrict__ const msg, int flags)
{
	uchar pszTag[33];
	msg_t *pMsg;
	DEFiRet;

	CHKiRet(msgConstruct(&pMsg));
	MsgSetInputName(pMsg, pInternalInputName);
	MsgSetRawMsg(pMsg, (char*)msg, lenMsg);
	MsgSetHOSTNAME(pMsg, glbl.GetLocalHostName(), ustrlen(glbl.GetLocalHostName()));
	MsgSetRcvFrom(pMsg, glbl.GetLocalHostNameProp());
	MsgSetRcvFromIP(pMsg, glbl.GetLocalHostIP());
	MsgSetMSGoffs(pMsg, 0);
	/* check if we have an error code associated and, if so,
	 * adjust the tag. -- rgerhards, 2008-06-27
	 */
	if(iErr == NO_ERRCODE) {
		MsgSetTAG(pMsg, UCHAR_CONSTANT("rsyslogd:"), sizeof("rsyslogd:") - 1);
	} else {
		size_t len = snprintf((char*)pszTag, sizeof(pszTag), "rsyslogd%d:", iErr);
		pszTag[32] = '\0'; /* just to make sure... */
		MsgSetTAG(pMsg, pszTag, len);
	}
	pMsg->iFacility = LOG_FAC(pri);
	pMsg->iSeverity = LOG_PRI(pri);
	flags |= INTERNAL_MSG;
	pMsg->msgFlags  = flags;

	if(bHaveMainQueue == 0) { /* not yet in queued mode */
		iminternalAddMsg(pMsg);
	} else {
               /* we have the queue, so we can simply provide the
		 * message to the queue engine.
		 */
		ratelimitAddMsg(internalMsg_ratelimiter, NULL, pMsg);
	}
finalize_it:
	RETiRet;
}



/* rgerhards 2004-11-09: the following is a function that can be used
 * to log a message orginating from the syslogd itself.
 */
rsRetVal
logmsgInternal(int iErr, int pri, const uchar *const msg, int flags)
{
	size_t lenMsg;
	unsigned i;
	char *bufModMsg = NULL; /* buffer for modified message, should we need to modify */
	DEFiRet;

	/* we first do a path the remove control characters that may have accidently
	 * introduced (program error!). This costs performance, but we do not expect
	 * to be called very frequently in any case ;) -- rgerhards, 2013-12-19.
	 */
	lenMsg = ustrlen(msg);
	for(i = 0 ; i < lenMsg ; ++i) {
		if(msg[i] < 0x20 || msg[i] == 0x7f) {
			if(bufModMsg == NULL) {
				CHKmalloc(bufModMsg = strdup((char*) msg));
			}
			bufModMsg[i] = ' ';
		}
	}

	if(bProcessInternalMessages) {
		CHKiRet(logmsgInternalSelf(iErr, pri, lenMsg,
					   (bufModMsg == NULL) ? (char*)msg : bufModMsg,
					   flags));
	} else {
		stdlog_log(stdlog_hdl, LOG_PRI(pri), "%s",
			   (bufModMsg == NULL) ? (char*)msg : bufModMsg);
	}

	/* we now check if we should print internal messages out to stderr. This was
	 * suggested by HKS as a way to help people troubleshoot rsyslog configuration
	 * (by running it interactively. This makes an awful lot of sense, so I add
	 * it here. -- rgerhards, 2008-07-28
	 * Note that error messages can not be disabled during a config verify. This
	 * permits us to process unmodified config files which otherwise contain a
	 * supressor statement.
	 */
	if(((Debug == DEBUG_FULL || !doFork) && ourConf->globals.bErrMsgToStderr) || iConfigVerify) {
		if(LOG_PRI(pri) == LOG_ERR)
			fprintf(stderr, "rsyslogd: %s\n", (bufModMsg == NULL) ? (char*)msg : bufModMsg);
	}

finalize_it:
	free(bufModMsg);
	RETiRet;
}

rsRetVal
submitMsg(msg_t *pMsg)
{
	return submitMsgWithDfltRatelimiter(pMsg);
}

/* submit a message to the main message queue.   This is primarily
 * a hook to prevent the need for callers to know about the main message queue
 * rgerhards, 2008-02-13
 */
rsRetVal
submitMsg2(msg_t *pMsg)
{
	qqueue_t *pQueue;
	ruleset_t *pRuleset;
	DEFiRet;

	ISOBJ_TYPE_assert(pMsg, msg);

	pRuleset = MsgGetRuleset(pMsg);
	pQueue = (pRuleset == NULL) ? pMsgQueue : ruleset.GetRulesetQueue(pRuleset);

	/* if a plugin logs a message during shutdown, the queue may no longer exist */
	if(pQueue == NULL) {
		DBGPRINTF("submitMsg2() could not submit message - "
			  "queue does (no longer?) exist - ignored\n");
		FINALIZE;
	}

	qqueueEnqMsg(pQueue, pMsg->flowCtlType, pMsg);

finalize_it:
	RETiRet;
}

/* submit multiple messages at once, very similar to submitMsg, just
 * for multi_submit_t. All messages need to go into the SAME queue!
 * rgerhards, 2009-06-16
 */
rsRetVal
multiSubmitMsg2(multi_submit_t *pMultiSub)
{
	qqueue_t *pQueue;
	ruleset_t *pRuleset;
	DEFiRet;
	assert(pMultiSub != NULL);

	if(pMultiSub->nElem == 0)
		FINALIZE;

	pRuleset = MsgGetRuleset(pMultiSub->ppMsgs[0]);
	pQueue = (pRuleset == NULL) ? pMsgQueue : ruleset.GetRulesetQueue(pRuleset);

	/* if a plugin logs a message during shutdown, the queue may no longer exist */
	if(pQueue == NULL) {
		DBGPRINTF("multiSubmitMsg() could not submit message - "
			  "queue does (no longer?) exist - ignored\n");
		FINALIZE;
	}

	iRet = pQueue->MultiEnq(pQueue, pMultiSub);
	pMultiSub->nElem = 0;

finalize_it:
	RETiRet;
}
rsRetVal
multiSubmitMsg(multi_submit_t *pMultiSub) /* backward compat. level */
{
	return multiSubmitMsg2(pMultiSub);
}


/* flush multiSubmit, e.g. at end of read records */
rsRetVal
multiSubmitFlush(multi_submit_t *pMultiSub)
{
	DEFiRet;
	if(pMultiSub->nElem > 0) {
		iRet = multiSubmitMsg2(pMultiSub);
	}
	RETiRet;
}


/* some support for command line option parsing. Any non-trivial options must be
 * buffered until the complete command line has been parsed. This is necessary to
 * prevent dependencies between the options. That, in turn, means we need to have
 * something that is capable of buffering options and there values. The follwing
 * functions handle that.
 * rgerhards, 2008-04-04
 */
typedef struct bufOpt {
	struct bufOpt *pNext;
	char optchar;
	char *arg;
} bufOpt_t;
static bufOpt_t *bufOptRoot = NULL;
static bufOpt_t *bufOptLast = NULL;

/* add option buffer */
static rsRetVal
bufOptAdd(char opt, char *arg)
{
	DEFiRet;
	bufOpt_t *pBuf;

	if((pBuf = MALLOC(sizeof(bufOpt_t))) == NULL)
		ABORT_FINALIZE(RS_RET_OUT_OF_MEMORY);

	pBuf->optchar = opt;
	pBuf->arg = arg;
	pBuf->pNext = NULL;

	if(bufOptLast == NULL) {
		bufOptRoot = pBuf; /* then there is also no root! */
	} else {
		bufOptLast->pNext = pBuf;
	}
	bufOptLast = pBuf;

finalize_it:
	RETiRet;
}


/* remove option buffer from top of list, return values and destruct buffer itself.
 * returns RS_RET_END_OF_LINKEDLIST when no more options are present.
 * (we use int *opt instead of char *opt to keep consistent with getopt())
 */
static rsRetVal
bufOptRemove(int *opt, char **arg)
{
	DEFiRet;
	bufOpt_t *pBuf;

	if(bufOptRoot == NULL)
		ABORT_FINALIZE(RS_RET_END_OF_LINKEDLIST);
	pBuf = bufOptRoot;

	*opt = pBuf->optchar;
	*arg = pBuf->arg;

	bufOptRoot = pBuf->pNext;
	free(pBuf);

finalize_it:
	RETiRet;
}


rsRetVal
rsyslogdInit(void)
{
	char bufStartUpMsg[512];
	struct sigaction sigAct;
	DEFiRet;

	memset(&sigAct, 0, sizeof (sigAct));
	sigemptyset(&sigAct.sa_mask);
	sigAct.sa_handler = syslogd_sighup_handler;
	sigaction(SIGHUP, &sigAct, NULL);

	CHKiRet(rsconf.Activate(ourConf));
	DBGPRINTF(" started.\n");

	if(ourConf->globals.bLogStatusMsgs) {
               snprintf(bufStartUpMsg, sizeof(bufStartUpMsg)/sizeof(char),
			 " [origin software=\"rsyslogd\" " "swVersion=\"" VERSION \
			 "\" x-pid=\"%d\" x-info=\"http://www.rsyslog.com\"] start",
			 (int) glblGetOurPid());
		logmsgInternal(NO_ERRCODE, LOG_SYSLOG|LOG_INFO, (uchar*)bufStartUpMsg, 0);
	}

finalize_it:
	RETiRet;
}


/* This is the main entry point into rsyslogd. Over time, we should try to
 * modularize it a bit more...
 */
void
initAll(int argc, char **argv)
{
	rsRetVal localRet;
	int ch;
	extern int optind;
	extern char *optarg;
	int bEOptionWasGiven = 0;
	int iHelperUOpt;
	int bChDirRoot = 1; /* change the current working directory to "/"? */
	char *arg;	/* for command line option processing */
	char cwdbuf[128]; /* buffer to obtain/display current working directory */
	DEFiRet;

	/* first, parse the command line options. We do not carry out any actual work, just
	 * see what we should do. This relieves us from certain anomalies and we can process
	 * the parameters down below in the correct order. For example, we must know the
	 * value of -M before we can do the init, but at the same time we need to have
	 * the base classes init before we can process most of the options. Now, with the
	 * split of functionality, this is no longer a problem. Thanks to varmofekoj for
	 * suggesting this algo.
	 * Note: where we just need to set some flags and can do so without knowledge
	 * of other options, we do this during the inital option processing.
	 * rgerhards, 2008-04-04
	 */
	while((ch = getopt(argc, argv, "46a:Ac:dDef:g:hi:l:m:M:nN:op:qQr::s:S:t:T:u:vwx")) != EOF) {
		switch((char)ch) {
                case '4':
                case '6':
                case 'A':
                case 'a':
		case 'f': /* configuration file */
		case 'h':
		case 'i': /* pid file name */
		case 'l':
		case 'm': /* mark interval */
		case 'n': /* don't fork */
		case 'N': /* enable config verify mode */
                case 'o':
                case 'p':
		case 'q': /* add hostname if DNS resolving has failed */
		case 'Q': /* dont resolve hostnames in ACL to IPs */
		case 's':
		case 'S': /* Source IP for local client to be used on multihomed host */
		case 'T': /* chroot on startup (primarily for testing) */
		case 'u': /* misc user settings */
		case 'w': /* disable disallowed host warnings */
		case 'x': /* disable dns for remote messages */
		case 'g': /* enable tcp gssapi logging */
		case 'r': /* accept remote messages */
		case 't': /* enable tcp logging */
			CHKiRet(bufOptAdd(ch, optarg));
			break;
		case 'c':		/* compatibility mode */
			fprintf(stderr, "rsyslogd: error: option -c is no longer supported - ignored\n");
			break;
		case 'd': /* debug - must be handled now, so that debug is active during init! */
			debugging_on = 1;
			Debug = 1;
			yydebug = 1;
			break;
		case 'D': /* BISON debug */
			yydebug = 1;
			break;
		case 'e':		/* log every message (no repeat message supression) */
			bEOptionWasGiven = 1;
			break;
		case 'M': /* default module load path -- this MUST be carried out immediately! */
			glblModPath = (uchar*) optarg;
			break;
		case 'v': /* MUST be carried out immediately! */
			syslogd_printVersion();
			exit(0); /* exit for -v option - so this is a "good one" */
		case '?':
		default:
			rsyslogd_usage();
		}
	}

	if(argc - optind)
		rsyslogd_usage();

	DBGPRINTF("rsyslogd %s startup, module path '%s', cwd:%s\n",
		  VERSION, glblModPath == NULL ? "" : (char*)glblModPath,
		  getcwd(cwdbuf, sizeof(cwdbuf)));

	/* we are done with the initial option parsing and processing. Now we init the system. */

	ppid = getpid();

	CHKiRet(rsyslogd_InitGlobalClasses());
	CHKiRet(syslogd_obtainClassPointers());

	/* doing some core initializations */

	/* get our host and domain names - we need to do this early as we may emit
	 * error log messages, which need the correct hostname. -- rgerhards, 2008-04-04
	 */
	queryLocalHostname();

	/* initialize the objects */
	if((iRet = modInitIminternal()) != RS_RET_OK) {
		fprintf(stderr, "fatal error: could not initialize errbuf object (error code %d).\n",
			iRet);
		exit(1); /* "good" exit, leaving at init for fatal error */
	}


	/* END core initializations - we now come back to carrying out command line options*/

	while((iRet = bufOptRemove(&ch, &arg)) == RS_RET_OK) {
		DBGPRINTF("deque option %c, optarg '%s'\n", ch, (arg == NULL) ? "" : arg);
		switch((char)ch) {
                case '4':
	                glbl.SetDefPFFamily(PF_INET);
                        break;
                case '6':
                        glbl.SetDefPFFamily(PF_INET6);
                        break;
                case 'A':
                        send_to_all++;
                        break;
                case 'a':
			fprintf(stderr, "rsyslogd: error -a is no longer supported, use module imuxsock instead");
                        break;
		case 'S':		/* Source IP for local client to be used on multihomed host */
			if(glbl.GetSourceIPofLocalClient() != NULL) {
				fprintf (stderr, "rsyslogd: Only one -S argument allowed, the first one is taken.\n");
			} else {
				glbl.SetSourceIPofLocalClient((uchar*)arg);
			}
			break;
		case 'f':		/* configuration file */
			ConfFile = (uchar*) arg;
			break;
		case 'g':		/* enable tcp gssapi logging */
			fprintf(stderr,	"rsyslogd: -g option no longer supported - ignored\n");
		case 'h':
			fprintf(stderr, "rsyslogd: error -h is no longer supported - ignored");
			break;
		case 'i':		/* pid file name */
			PidFile = arg;
			break;
		case 'l':
			if(glbl.GetLocalHosts() != NULL) {
				fprintf (stderr, "rsyslogd: Only one -l argument allowed, the first one is taken.\n");
			} else {
				glbl.SetLocalHosts(syslogd_crunch_list(arg));
			}
			break;
		case 'm':		/* mark interval */
			fprintf(stderr, "rsyslogd: error -m is no longer supported - use immark instead");
			break;
		case 'n':		/* don't fork */
			doFork = 0;
			break;
		case 'N':		/* enable config verify mode */
			iConfigVerify = atoi(arg);
			break;
                case 'o':
			fprintf(stderr, "error -o is no longer supported, use module imuxsock instead");
                        break;
                case 'p':
			fprintf(stderr, "error -p is no longer supported, use module imuxsock instead");
			break;
		case 'q':               /* add hostname if DNS resolving has failed */
		        *(net.pACLAddHostnameOnFail) = 1;
		        break;
		case 'Q':               /* dont resolve hostnames in ACL to IPs */
		        *(net.pACLDontResolve) = 1;
		        break;
		case 'r':		/* accept remote messages */
			fprintf(stderr, "rsyslogd: error option -r is no longer supported - ignored");
			break;
		case 's':
			if(glbl.GetStripDomains() != NULL) {
				fprintf (stderr, "rsyslogd: Only one -s argument allowed, the first one is taken.\n");
			} else {
				glbl.SetStripDomains(syslogd_crunch_list(arg));
			}
			break;
		case 't':		/* enable tcp logging */
			fprintf(stderr, "rsyslogd: error option -t is no longer supported - ignored");
			break;
		case 'T':/* chroot() immediately at program startup, but only for testing, NOT security yet */
			if(chroot(arg) != 0) {
				perror("chroot");
				exit(1);
			}
			break;
		case 'u':		/* misc user settings */
			iHelperUOpt = atoi(arg);
			if(iHelperUOpt & 0x01)
				glbl.SetParseHOSTNAMEandTAG(0);
			if(iHelperUOpt & 0x02)
				bChDirRoot = 0;
			break;
		case 'w':		/* disable disallowed host warnigs */
			glbl.SetOption_DisallowWarning(0);
			break;
		case 'x':		/* disable dns for remote messages */
			glbl.SetDisableDNS(1);
			break;
               case '?':
		default:
			rsyslogd_usage();
		}
	}

	if(iRet != RS_RET_END_OF_LINKEDLIST)
		FINALIZE;

	if(iConfigVerify) {
		fprintf(stderr, "rsyslogd: version %s, config validation run (level %d), master config %s\n",
			VERSION, iConfigVerify, ConfFile);
	}

	localRet = rsconf.Load(&ourConf, ConfFile);

	syslogdInit();

	if(localRet == RS_RET_NONFATAL_CONFIG_ERR) {
		if(loadConf->globals.bAbortOnUncleanConfig) {
			fprintf(stderr, "rsyslogd: $AbortOnUncleanConfig is set, and config is not clean.\n"
					"Check error log for details, fix errors and restart. As a last\n"
					"resort, you may want to remove $AbortOnUncleanConfig to permit a\n"
					"startup with a dirty config.\n");
			exit(2);
		}
		if(iConfigVerify) {
			/* a bit dirty, but useful... */
			exit(1);
		}
		localRet = RS_RET_OK;
	}
	CHKiRet(localRet);
	
	CHKiRet(rsyslogd_InitStdRatelimiters());

	if(bChDirRoot) {
		if(chdir("/") != 0)
			fprintf(stderr, "Can not do 'cd /' - still trying to run\n");
	}

	/* process compatibility mode settings */
	if(bEOptionWasGiven) {
		errmsg.LogError(0, NO_ERRCODE, "WARNING: \"message repeated n times\" feature MUST be turned on in "
					    "rsyslog.conf - CURRENTLY EVERY MESSAGE WILL BE LOGGED. Visit "
					    "http://www.rsyslog.com/rptdmsgreduction to learn "
					    "more and cast your vote if you want us to keep this feature.");
	}

	if(!iConfigVerify)
		CHKiRet(syslogd_doGlblProcessInit());

	/* Send a signal to the parent so it can terminate.  */
	if(glblGetOurPid() != ppid)
		kill(ppid, SIGTERM);

	CHKiRet(rsyslogdInit());

	if(Debug && debugging_on) {
		dbgprintf("Debugging enabled, SIGUSR1 to turn off debugging.\n");
	}

	/* END OF INTIALIZATION */
	DBGPRINTF("initialization completed, transitioning to regular run mode\n");

	/* close stderr and stdout if they are kept open during a fork. Note that this
	 * may introduce subtle security issues: if we are in a jail, one may break out of
	 * it via these descriptors. But if I close them earlier, error messages will (once
	 * again) not be emitted to the user that starts the daemon. As root jail support
	 * is still in its infancy (and not really done), we currently accept this issue.
	 * rgerhards, 2009-06-29
	 */
	if(doFork) {
		close(1);
		close(2);
		ourConf->globals.bErrMsgToStderr = 0;
	}

finalize_it:
	if(iRet == RS_RET_VALIDATION_RUN) {
		fprintf(stderr, "rsyslogd: End of config validation run. Bye.\n");
		exit(0);
	} else if(iRet != RS_RET_OK) {
		fprintf(stderr, "rsyslogd: run failed with error %d (see rsyslog.h "
				"or try http://www.rsyslog.com/e/%d to learn what that number means)\n", iRet, iRet*-1);
		exit(1);
	}

	ENDfunc
}

void
rsyslogdDebugSwitch()
{
	time_t tTime;
	struct tm tp;
	struct sigaction sigAct;

	datetime.GetTime(&tTime);
	localtime_r(&tTime, &tp);
	if(debugging_on == 0) {
		debugging_on = 1;
		dbgprintf("\n");
		dbgprintf("\n");
		dbgprintf("********************************************************************************\n");
		dbgprintf("Switching debugging_on to true at %2.2d:%2.2d:%2.2d\n",
			  tp.tm_hour, tp.tm_min, tp.tm_sec);
		dbgprintf("********************************************************************************\n");
	} else {
		dbgprintf("********************************************************************************\n");
		dbgprintf("Switching debugging_on to false at %2.2d:%2.2d:%2.2d\n",
			  tp.tm_hour, tp.tm_min, tp.tm_sec);
		dbgprintf("********************************************************************************\n");
		dbgprintf("\n");
		dbgprintf("\n");
		debugging_on = 0;
	}

	memset(&sigAct, 0, sizeof (sigAct));
	sigemptyset(&sigAct.sa_mask);
	sigAct.sa_handler = rsyslogdDebugSwitch;
	sigaction(SIGUSR1, &sigAct, NULL);
}


/* this function pulls all internal messages from the buffer
 * and puts them into the processing engine.
 * We can only do limited error handling, as this would not
 * really help us. TODO: add error messages?
 * rgerhards, 2007-08-03
 */
static inline void processImInternal(void)
{
	msg_t *pMsg;

	while(iminternalRemoveMsg(&pMsg) == RS_RET_OK) {
		ratelimitAddMsg(dflt_ratelimiter, NULL, pMsg);
	}
}


/* This takes a received message that must be decoded and submits it to
 * the main message queue. This is a legacy function which is being provided
 * to aid older input plugins that do not support message creation via
 * the new interfaces themselves. It is not recommended to use this
 * function for new plugins. -- rgerhards, 2009-10-12
 */
rsRetVal
parseAndSubmitMessage(uchar *hname, uchar *hnameIP, uchar *msg, int len, int flags, flowControl_t flowCtlType,
	prop_t *pInputName, struct syslogTime *stTime, time_t ttGenTime, ruleset_t *pRuleset)
{
	prop_t *pProp = NULL;
	msg_t *pMsg;
	DEFiRet;

	/* we now create our own message object and submit it to the queue */
	if(stTime == NULL) {
		CHKiRet(msgConstruct(&pMsg));
	} else {
		CHKiRet(msgConstructWithTime(&pMsg, stTime, ttGenTime));
	}
	if(pInputName != NULL)
		MsgSetInputName(pMsg, pInputName);
	MsgSetRawMsg(pMsg, (char*)msg, len);
	MsgSetFlowControlType(pMsg, flowCtlType);
	MsgSetRuleset(pMsg, pRuleset);
	pMsg->msgFlags  = flags | NEEDS_PARSING;

	MsgSetRcvFromStr(pMsg, hname, ustrlen(hname), &pProp);
	CHKiRet(prop.Destruct(&pProp));
	CHKiRet(MsgSetRcvFromIPStr(pMsg, hnameIP, ustrlen(hnameIP), &pProp));
	CHKiRet(prop.Destruct(&pProp));
	CHKiRet(submitMsg2(pMsg));

finalize_it:
	RETiRet;
}


/* helper to doHUP(), this "HUPs" each action. The necessary locking
 * is done inside the action class and nothing we need to take care of.
 * rgerhards, 2008-10-22
 */
DEFFUNC_llExecFunc(doHUPActions)
{
	BEGINfunc
	actionCallHUPHdlr((action_t*) pData);
	ENDfunc
	return RS_RET_OK; /* we ignore errors, we can not do anything either way */
}


/* This function processes a HUP after one has been detected. Note that this
 * is *NOT* the sighup handler. The signal is recorded by the handler, that record
 * detected inside the mainloop and then this function is called to do the
 * real work. -- rgerhards, 2008-10-22
 * Note: there is a VERY slim chance of a data race when the hostname is reset.
 * We prefer to take this risk rather than sync all accesses, because to the best
 * of my analysis it can not really hurt (the actual property is reference-counted)
 * but the sync would require some extra CPU for *each* message processed.
 * rgerhards, 2012-04-11
 */
static inline void
doHUP(void)
{
	char buf[512];

	if(ourConf->globals.bLogStatusMsgs) {
		snprintf(buf, sizeof(buf) / sizeof(char),
			 " [origin software=\"rsyslogd\" " "swVersion=\"" VERSION
			 "\" x-pid=\"%d\" x-info=\"http://www.rsyslog.com\"] rsyslogd was HUPed",
			 (int) glblGetOurPid());
			errno = 0;
		logmsgInternal(NO_ERRCODE, LOG_SYSLOG|LOG_INFO, (uchar*)buf, 0);
	}

	queryLocalHostname(); /* re-read our name */
	ruleset.IterateAllActions(ourConf, doHUPActions, NULL);
	lookupDoHUP();
}

/* rsyslogdDoDie() is a signal handler. If called, it sets the bFinished variable
 * to indicate the program should terminate. However, it does not terminate
 * it itself, because that causes issues with multi-threading. The actual
 * termination is then done on the main thread. This solution might introduce
 * a minimal delay, but it is much cleaner than the approach of doing everything
 * inside the signal handler.
 * rgerhards, 2005-10-26
 * Note:
 * - we do not call DBGPRINTF() as this may cause us to block in case something
 *   with the threading is wrong.
 * - we do not really care about the return state of write(), but we need this
 *   strange check we do to silence compiler warnings (thanks, Ubuntu!)
 */
void
rsyslogdDoDie(int sig)
{
#	define MSG1 "DoDie called.\n"
#	define MSG2 "DoDie called 5 times - unconditional exit\n"
	static int iRetries = 0; /* debug aid */
	dbgprintf(MSG1);
	if(Debug == DEBUG_FULL) {
		if(write(1, MSG1, sizeof(MSG1) - 1)) {}
	}
	if(iRetries++ == 4) {
		if(Debug == DEBUG_FULL) {
			if(write(1, MSG2, sizeof(MSG2) - 1)) {}
		}
		abort();
	}
	bFinished = sig;
	if(glblDebugOnShutdown) {
		/* kind of hackish - set to 0, so that debug_swith will enable
		 * and AND emit the "start debug log" message.
		 */
		debugging_on = 0;
		rsyslogdDebugSwitch();
	}
#	undef MSG1
#	undef MSG2
}


/* This is the main processing loop. It is called after successful initialization.
 * When it returns, the syslogd terminates.
 * Its sole function is to provide some housekeeping things. The real work is done
 * by the other threads spawned.
 */
static void
mainloop(void)
{
	struct timeval tvSelectTimeout;

	BEGINfunc
	/* first check if we have any internal messages queued and spit them out. We used
	 * to do that on any loop iteration, but that is no longer necessry. The reason
	 * is that once we reach this point here, we always run on multiple threads and
	 * thus the main queue is properly initialized. -- rgerhards, 2008-06-09
	 */
	processImInternal();

	while(!bFinished){
		/* this is now just a wait - please note that we do use a near-"eternal"
		 * timeout of 1 day. This enables us to help safe the environment
		 * by not unnecessarily awaking rsyslog on a regular tick (just think
		 * powertop, for example). In that case, we primarily wait for a signal,
		 * but a once-a-day wakeup should be quite acceptable. -- rgerhards, 2008-06-09
		 */
		tvSelectTimeout.tv_sec = 86400 /*1 day*/;
		tvSelectTimeout.tv_usec = 0;
		select(1, NULL, NULL, NULL, &tvSelectTimeout);
		if(bFinished)
			break;	/* exit as quickly as possible */

		if(bHadHUP) {
			doHUP();
			bHadHUP = 0;
			continue;
		}
	}
	ENDfunc
}


/* Finalize and destruct all actions.
 */
void
rsyslogd_destructAllActions(void)
{
	ruleset.DestructAllActions(runConf);
	bHaveMainQueue = 0; /* flag that internal messages need to be temporarily stored */
}


/* de-initialize everything, make ready for termination */
static void
deinitAll(void)
{
	char buf[256];

	DBGPRINTF("exiting on signal %d\n", bFinished);

	/* IMPORTANT: we should close the inputs first, and THEN send our termination
	 * message. If we do it the other way around, logmsgInternal() may block on
	 * a full queue and the inputs still fill up that queue. Depending on the
	 * scheduling order, we may end up with logmsgInternal being held for a quite
	 * long time. When the inputs are terminated first, that should not happen
	 * because the queue is drained in parallel. The situation could only become
	 * an issue with extremely long running actions in a queue full environment.
	 * However, such actions are at least considered poorly written, if not
	 * outright wrong. So we do not care about this very remote problem.
	 * rgerhards, 2008-01-11
	 */

	/* close the inputs */
	DBGPRINTF("Terminating input threads...\n");
	glbl.SetGlobalInputTermination();
	thrdTerminateAll();

	/* and THEN send the termination log message (see long comment above) */
	if(bFinished && runConf->globals.bLogStatusMsgs) {
		(void) snprintf(buf, sizeof(buf) / sizeof(char),
		 " [origin software=\"rsyslogd\" " "swVersion=\"" VERSION \
		 "\" x-pid=\"%d\" x-info=\"http://www.rsyslog.com\"]" " exiting on signal %d.",
		 (int) glblGetOurPid(), bFinished);
		errno = 0;
		logmsgInternal(NO_ERRCODE, LOG_SYSLOG|LOG_INFO, (uchar*)buf, 0);
	}
	/* we sleep for 50ms to give the queue a chance to pick up the exit message;
	 * otherwise we have seen cases where the message did not make it to log
	 * files, even on idle systems.
	 */
	srSleep(0, 50);

	/* drain queue (if configured so) and stop main queue worker thread pool */
	DBGPRINTF("Terminating main queue...\n");
	qqueueDestruct(&pMsgQueue);
	pMsgQueue = NULL;

	/* Free ressources and close connections. This includes flushing any remaining
	 * repeated msgs.
	 */
	DBGPRINTF("Terminating outputs...\n");
	rsyslogd_destructAllActions();

	DBGPRINTF("all primary multi-thread sources have been terminated - now doing aux cleanup...\n");

	DBGPRINTF("destructing current config...\n");
	rsconf.Destruct(&runConf);

	modExitIminternal();

	if(pInternalInputName != NULL)
		prop.Destruct(&pInternalInputName);

	/* the following line cleans up CfSysLineHandlers that were not based on loadable
	 * modules. As such, they are not yet cleared.  */
	unregCfSysLineHdlrs();

	/*dbgPrintAllDebugInfo(); / * this is the last spot where this can be done - below output modules are unloaded! */

	syslogd_releaseClassPointers();

	parserClassExit();
	rsconfClassExit();
	strExit();
	ratelimitModExit();
	dnscacheDeinit();
	thrdExit();

	module.UnloadAndDestructAll(eMOD_LINK_ALL);

	rsrtExit(); /* runtime MUST always be deinitialized LAST (except for debug system) */
	DBGPRINTF("Clean shutdown completed, bye\n");

	/* dbgClassExit MUST be the last one, because it de-inits the debug system */
	dbgClassExit();

	/* NO CODE HERE - dbgClassExit() must be the last thing before exit()! */
	syslogd_die();
}

/* This is the main entry point into rsyslogd. This must be a function in its own
 * right in order to intialize the debug system in a portable way (otherwise we would
 * need to have a statement before variable definitions.
 * rgerhards, 20080-01-28
 */
int
main(int argc, char **argv)
{
	dbgClassInit();
	initAll(argc, argv);
	sd_notify(0, "READY=1");

	mainloop();
	deinitAll();
	return 0;
}
