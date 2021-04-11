/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2017-2019 Olof Hagsand
  Copyright (C) 2020-2021 Olof Hagsand and Rubicon Communications, LLC(Netgate)

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, 
  indicate your decision by deleting the provisions above and replace them with
  the  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****
  *
  * Pseudo backend plugin for starting restconf daemon
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include "clixon_backend_transaction.h"
#include "backend_plugin_restconf.h"

/*---------------------------------------------------------------------
 * Restconf process pseudo plugin
 */

#define RESTCONF_PROCESS "restconf"

/*! Set current debug flag when starting process using -D <dbg>
 *
 * process argv list including -D is set on start. But the debug flags may change and this is a way
 * to set it dynamically, ie at the time the process is started, not when the backend is started.
 * @param[in]  h      Clixon backend
 * @param[in]  dbg    Debug string , eg "0" or "1"
 */
static int
restconf_pseudo_set_debug(clicon_handle h,
			  char         *dbg)
{
    int    retval = -1;
    char **argv;
    int    argc;
    int    i;

    if (dbg == NULL)
	goto ok;
    if (clixon_process_argv_get(h, RESTCONF_PROCESS, &argv, &argc) < 0)
	goto done;
    for (i=0; i<argc; i++){
	if (argv[i] == NULL)
	    break;
	if (strcmp(argv[i], "-D") == 0 && argc > i+1 && argv[i+1]){
	    free(argv[i+1]);
	    if ((argv[i+1] = strdup(dbg)) == NULL){
		clicon_err(OE_UNIX, errno, "strdup");
		goto done;
	    }
	    break;
	}
    }
 ok:
    retval = 0;
 done:
    return retval;
}

/*! Process rpc callback function 
 * - if RPC op is start, if enable is true, start the service, if false, error or ignore it
 * - if RPC op is stop, stop the service 
 * These rules give that if RPC op is start and enable is false -> change op to none
 */
int
restconf_rpc_wrapper(clicon_handle    h,
		     process_entry_t *pe,
		     proc_operation  *operation)
{
    int    retval = -1;
    cxobj *xt = NULL;
    cxobj *xb;
    
    clicon_debug(1, "%s", __FUNCTION__);
    switch (*operation){
    case PROC_OP_STOP:
	/* if RPC op is stop, stop the service */
	break;
    case PROC_OP_START:
	/* RPC op is start & enable is true, then start the service, 
                           & enable is false, error or ignore it */
	if (xmldb_get(h, "running", NULL,  "/restconf", &xt) < 0)
	    goto done;
	if (xt != NULL &&
	    xpath_first(xt, NULL, "/restconf[enable='false']") != NULL) {
	    *operation = PROC_OP_NONE;
	}
	else{
	    /* Get debug flag of restconf config, set the restconf start -D daemon flag according
	     * to it. The restconf daemon cannoit read its debug flag from config initially,
	     * but in this way it is set directly in its input args.
	     * Its a trick.
	     */
	    if ((xb = xpath_first(xt, NULL, "/restconf/debug")) != NULL){
		if (restconf_pseudo_set_debug(h, xml_body(xb)) < 0)
		    goto done;
	    }
	}
	break;
    default:
	break;
    }
    retval = 0;
 done:
    if (xt)
	xml_free(xt);
    return retval;
}

/*! Enable process-control of restconf daemon, ie start/stop restconf by registering restconf process
 * @param[in]  h  Clicon handle
 * @note Could also look in clixon-restconf and start process if enable is true, but that needs to 
 *       be in start callback using a pseudo plugin.
 *      - Debug flag inheritance only works if backend is started with debug. If debug is set later
 *        this is ignored.
 */
static int
restconf_pseudo_process_control(clicon_handle h)
{
    int    retval = -1;
    char **argv = NULL;
    int    i;
    int    nr;
    cbuf  *cb = NULL;

    nr = 6;
    if ((argv = calloc(nr, sizeof(char *))) == NULL){
	clicon_err(OE_UNIX, errno, "calloc");
	goto done;
    }
    i = 0;
    if ((cb = cbuf_new()) == NULL){
	clicon_err(OE_UNIX, errno, "cbuf_new");
	goto done;
    }
    cprintf(cb, "%s/clixon_restconf", clicon_option_str(h, "CLICON_WWWDIR"));
    argv[i++] = cbuf_get(cb);
    argv[i++] = "-f";
    argv[i++] = clicon_option_str(h, "CLICON_CONFIGFILE");
    /* Add debug if backend has debug. 
     * There is also a debug flag in clixon-restconf.yang but it kicks in after it starts
     */
    argv[i++] = "-D";
    argv[i++] = "0";
    argv[i++] = NULL;
    assert(i==nr);
    if (clixon_process_register(h, RESTCONF_PROCESS,
				"Clixon RESTCONF process",
				NULL /* XXX network namespace */,
				restconf_rpc_wrapper,
				argv, nr) < 0)
	goto done;
    if (argv != NULL)
	free(argv);
    retval = 0;
 done:
    if (cb)
	cbuf_free(cb);
    return retval;
}

/*! Restconf pseduo-plugin process validate
 */
static int
restconf_pseudo_process_validate(clicon_handle    h,
				 transaction_data td)
{
    int    retval = -1;
    cxobj *xtarget;

    clicon_debug(1, "%s", __FUNCTION__);
    xtarget = transaction_target(td);
    /* If ssl-enable is true and (at least a) socket has ssl,
     * then server-cert-path and server-key-path must exist */
    if (xpath_first(xtarget, NULL, "restconf/enable[.='true']") &&
	xpath_first(xtarget, NULL, "restconf/socket[ssl='true']")){
	/* Should filepath be checked? One could claim this is a runtime system,... */
	if (xpath_first(xtarget, 0, "restconf/server-cert-path") == NULL){
	    clicon_err(OE_CFG, 0, "SSL enabled but server-cert-path not set");
	    return -1; /* induce fail */
	}
	if (xpath_first(xtarget, 0, "restconf/server-key-path") == NULL){
	    clicon_err(OE_CFG, 0, "SSL enabled but server-key-path not set");
	    return -1; /* induce fail */
	}
    }
    retval = 0;
    return retval;
}

/*! Restconf pseduo-plugin process commit
 */
static int
restconf_pseudo_process_commit(clicon_handle    h,
			       transaction_data td)
{
    int    retval = -1;
    cxobj *xtarget;
    cxobj *xsource;
    cxobj *cx;
    int    enabled = 0;
    cxobj *xb;
    
    clicon_debug(1, "%s", __FUNCTION__);
    xtarget = transaction_target(td);
    xsource = transaction_src(td);
    if (xpath_first(xtarget, NULL, "/restconf[enable='true']") != NULL)
	enabled++;
    /* Get debug flag of restconf config, set the restconf start -D daemon flag according
     * to it. The restconf daemon cannoit read its debug flag from config initially,
     * but in this way it is set directly in its input args.
     * Its a trick.
     */
    if ((xb = xpath_first(xtarget, NULL, "/restconf/debug")) != NULL){
	if (restconf_pseudo_set_debug(h, xml_body(xb)) < 0)
	    goto done;
    }
    /* Toggle start/stop if enable flag changed */
    if ((cx = xpath_first(xtarget, NULL, "/restconf/enable")) != NULL &&
	xml_flag(cx, XML_FLAG_CHANGE|XML_FLAG_ADD)){
	if (clixon_process_operation(h, RESTCONF_PROCESS,
				     enabled?PROC_OP_START:PROC_OP_STOP, 0) < 0)
	    goto done;
    }
    else if (enabled){     /* If something changed and running, restart process */
	if (transaction_dlen(td) != 0 ||
	    transaction_alen(td) != 0 ||
	    transaction_clen(td) != 0){
	    if ((cx = xpath_first(xtarget, NULL, "/restconf")) != NULL &&
		xml_flag(cx, XML_FLAG_CHANGE|XML_FLAG_ADD)){
		/* A restart can terminate a restconf connection (cut the tree limb you are sitting on)
		 * Specifically, the socket is terminated where the reply is sent, which will
		 * cause the curl to fail.
		 * Note that it should really be a START if the process is stopped, but the
		 * commit code need not know any of that
		 */
		if (clixon_process_operation(h, RESTCONF_PROCESS, PROC_OP_RESTART, 0) < 0)
		    goto done;
	    }
	    else if ((cx = xpath_first(xsource, NULL, "/restconf")) != NULL &&
		     xml_flag(cx, XML_FLAG_CHANGE|XML_FLAG_DEL)){
		/* Or something deleted */
		if (clixon_process_operation(h, RESTCONF_PROCESS, PROC_OP_RESTART, 0) < 0)
		    goto done;
	    }
	}
    }
    retval = 0;
 done:
    return retval;
}

/*! Register start/stop restconf RPC and create pseudo-plugin to monitor enable flag
 * @param[in]  h  Clixon handle
 */
int
backend_plugin_restconf_register(clicon_handle h,
				 yang_stmt    *yspec)
{
    int            retval = -1;
    clixon_plugin *cp = NULL;

    if (clixon_pseudo_plugin(h, "restconf pseudo plugin", &cp) < 0)
	goto done;

    cp->cp_api.ca_trans_validate = restconf_pseudo_process_validate;
    cp->cp_api.ca_trans_commit = restconf_pseudo_process_commit;

    /* Register generic process-control of restconf daemon, ie start/stop restconf */
    if (restconf_pseudo_process_control(h) < 0)
	goto done;
    retval = 0;
 done:
    return retval;
}