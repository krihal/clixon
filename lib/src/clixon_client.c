/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2020-2022 Olof Hagsand and Rubicon Communications, LLC(Netgate)

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
  use your version of this file under the terms of Apache License version 2, indicate
  your decision by deleting the provisions above and replace them with the 
  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****
  *
  */
#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <unistd.h>
#include <syslog.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>

/* cligen */
#include <cligen/cligen.h>

#include "clixon_queue.h"
#include "clixon_string.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_log.h"
#include "clixon_err.h"
#include "clixon_yang.h"
#include "clixon_options.h"
#include "clixon_proc.h"
#include "clixon_xml.h"
#include "clixon_xml_nsctx.h"
#include "clixon_xml_io.h"
#include "clixon_xpath_ctx.h"
#include "clixon_xpath.h"
#include "clixon_netconf_lib.h"
#include "clixon_proto.h"
#include "clixon_proto_client.h"
#include "clixon_client.h"

/*
 * Constants
 */
/* Netconf binary default, override with environment variable: CLIXON_NETCONF_BIN 
 * Could try to get path from install/makefile data
 */
#define CLIXON_NETCONF_BIN CLIXON_CONFIG_BINDIR "/clixon_netconf"

#define CLIXON_CLIENT_MAGIC 0x54fe649a

#define chandle(ch) (assert(clixon_client_handle_check(ch)==0),(struct clixon_client_handle *)(ch))

/*! Internal structure of clixon client handle. 
 */
struct clixon_client_handle{
    uint32_t           cch_magic;  /* magic number */
    clicon_handle      cch_h;      /* Clixon handle */
    clixon_client_type cch_type;   /* Clixon socket type */
    int                cch_socket; /* Input/output socket */
    char              *cch_descr;  /* Description of socket / peer for logging  XXX NYI */
    int                cch_pid;    /* Sub-process-id Only applies for NETCONF/SSH */
    int                cch_locked; /* State variable: 1 means locked */
};

/*! Check struct magic number for sanity checks
 *
 * @param[in]  h   Clixon client handle
 * @retval     0   Sanity check OK
 * @retval    -1   Sanity check failed
 */
static int
clixon_client_handle_check(clixon_client_handle ch)
{
    /* Dont use handle macro to avoid recursion */
    struct clixon_client_handle *cch = (struct clixon_client_handle *)(ch);

    return cch->cch_magic == CLIXON_CLIENT_MAGIC ? 0 : -1;
}

/*! Initialize Clixon client API
 *
 * @param[in]  config_file Clixon configuration file, or NULL for default
 * @retval     h           Clixon handler
 * @retval     NULL        Error
 * @see clixon_client_close
 */
clixon_handle
clixon_client_init(const char *config_file)
{
    clicon_handle  h;

    clixon_debug(CLIXON_DBG_DEFAULT, "%s", __FUNCTION__);
    /* Initiate CLICON handle. CLIgen is also initialized */
    if ((h = clicon_handle_init()) == NULL)
        return NULL;
    /* Set clixon config file - reuse the one in the main installation */
    clicon_option_str_set(h, "CLICON_CONFIGFILE",
                          config_file?(char*)config_file:CLIXON_DEFAULT_CONFIG);
    /* Find, read and parse configfile */
    if (clicon_options_main(h) < 0)
        return NULL;
    return h;
}

/*! Deallocate everything from client_init
 *
 * @param[in]  h     Clixon handle
 * @see clixon_client_init
 */
int
clixon_client_terminate(clicon_handle h)
{
    clixon_debug(CLIXON_DBG_DEFAULT, "%s", __FUNCTION__);
    clicon_handle_exit(h);
    return 0;
}

/*! Send a lock request (internal)
 *
 * @param[in]  h      Clixon handle
 * @param[in]  sock   Open socket
 * @param[in]  descr  Description of peer for logging
 * @param[in]  lock   0: unlock, 1: lock  
 * @param[in]  db     Datastore name
 * @retval     0      OK
 * @retval    -1      Error
 */
int
clixon_client_lock(clixon_handle h,
                   int           sock,
                   const char   *descr,
                   const int     lock,
                   const char   *db)
{
    int    retval = -1;
    cxobj *xret = NULL;
    cxobj *xd;
    cbuf  *msg = NULL;
    cbuf  *msgret = NULL;
    int    eof = 0;

    clixon_debug(CLIXON_DBG_DEFAULT, "%s", __FUNCTION__);
    if (db == NULL){
        clicon_err(OE_XML, EINVAL, "Expected db");
        goto done;
    }
    if ((msg = cbuf_new()) == NULL){
        clicon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    if ((msgret = cbuf_new()) == NULL){
        clicon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(msg, "<rpc xmlns=\"%s\" %s>"
            "<%slock><target><%s/></target></%slock></rpc>",
            NETCONF_BASE_NAMESPACE,
            NETCONF_MESSAGE_ID_ATTR,
            lock?"":"un", db, lock?"":"un");
    if (clicon_rpc1(sock, descr, msg, msgret, &eof) < 0)
        goto done;
    if (eof){
        close(sock);
        clicon_err(OE_PROTO, ESHUTDOWN, "Unexpected close of CLICON_SOCK. Clixon backend daemon may have crashed.");
        goto done;
    }
    if (clixon_xml_parse_string(cbuf_get(msgret), YB_NONE, NULL, &xret, NULL) < 0)
        goto done;
    if ((xd = xpath_first(xret, NULL, "/rpc-reply/rpc-error")) != NULL){
        xd = xml_parent(xd); /* point to rpc-reply */
        clixon_netconf_error(h, xd, "Get config", NULL);
        goto done; /* Not fatal */
    }
    retval = 0;
 done:
    clixon_debug(CLIXON_DBG_DEFAULT, "%s retval:%d", __FUNCTION__, retval);
    if (xret)
        xml_free(xret);
    if (msg)
        cbuf_free(msg);
    if (msgret)
        cbuf_free(msgret);
    return retval;
}

/*! Internal function to construct the encoding and hello message
 *
 * @param[in]  sock    Socket to netconf server
 * @param[in]  descr Description of peer for logging
 * @param[in]  version Netconf version for capability announcement
 * @retval     0       OK
 * @retval    -1       Error
 */
int
clixon_client_hello(int         sock,
                    const char *descr,
                    int         version)
{
    int   retval = -1;
    cbuf *msg = NULL;

    clixon_debug(CLIXON_DBG_DEFAULT, "%s", __FUNCTION__);
    if ((msg = cbuf_new()) == NULL){
        clicon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    //    cprintf(msg, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    cprintf(msg, "<hello xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);
    cprintf(msg, "<capabilities>");
    cprintf(msg, "<capability>%s</capability>", version==0?NETCONF_BASE_CAPABILITY_1_0:NETCONF_BASE_CAPABILITY_1_1);
    cprintf(msg, "</capabilities>");
    cprintf(msg, "</hello>");
    cprintf(msg, "]]>]]>");
    if (clicon_msg_send1(sock, descr, msg) < 0)
        goto done;
    retval = 0;
 done:
    clixon_debug(CLIXON_DBG_DEFAULT, "%s retval:%d", __FUNCTION__, retval);
    if (msg)
        cbuf_free(msg);
    return retval;
}

/*!
 */
static int
clixon_client_connect_netconf(clicon_handle                h,
                              struct clixon_client_handle *cch)
{
    int         retval = -1;
    int         nr;
    int         i;
    char      **argv = NULL;
    char       *netconf_bin = NULL;
    struct stat st = {0,};
    char        dbgstr[8];

    nr = 7;
    if (clixon_debug_get() != 0)
        nr += 2;
    if ((argv = calloc(nr, sizeof(char *))) == NULL){
        clicon_err(OE_UNIX, errno, "calloc");
        goto done;
    }
    i = 0;
    if ((netconf_bin = getenv("CLIXON_NETCONF_BIN")) == NULL)
        netconf_bin = CLIXON_NETCONF_BIN;
    if (stat(netconf_bin, &st) < 0){
        clicon_err(OE_NETCONF, errno, "netconf binary %s. Set with CLIXON_NETCONF_BIN=",
                   netconf_bin);
        goto done;
    }
    argv[i++] = netconf_bin;
    argv[i++] = "-q";
    argv[i++] = "-f";
    argv[i++] = clicon_option_str(h, "CLICON_CONFIGFILE");
    argv[i++] = "-l"; /* log to syslog */
    argv[i++] = "s";
    if (clixon_debug_get() != 0){
        argv[i++] = "-D";
        snprintf(dbgstr, sizeof(dbgstr)-1, "%d", clixon_debug_get());
        argv[i++] = dbgstr;
    }
    argv[i++] = NULL;
    assert(i==nr);
    if (clixon_proc_socket(argv, SOCK_DGRAM, &cch->cch_pid, &cch->cch_socket) < 0){
        goto done;
    }
    retval = 0;
 done:
    return retval;
}

/*!
 */
static int
clixon_client_connect_ssh(clicon_handle                h,
                          struct clixon_client_handle *cch,
                          const char                  *dest)
{
    int         retval = -1;
    int         nr;
    int         i;
    char      **argv = NULL;
    char       *ssh_bin = SSH_BIN;
    struct stat st = {0,};

    clixon_debug(CLIXON_DBG_DEFAULT, "%s", __FUNCTION__);
    nr = 5;
    if ((argv = calloc(nr, sizeof(char *))) == NULL){
        clicon_err(OE_UNIX, errno, "calloc");
        goto done;
    }
    i = 0;
    if (stat(ssh_bin, &st) < 0){
        clicon_err(OE_NETCONF, errno, "ssh binary %s", ssh_bin);
        goto done;
    }
    argv[i++] = ssh_bin;
    argv[i++] = (char*)dest;
    argv[i++] = "-s";
    argv[i++] = "netconf";
    argv[i++] = NULL;
    assert(i==nr);
    for (i=0;i<nr;i++)
        clixon_debug(CLIXON_DBG_DEFAULT, "%s: argv[%d]:%s", __FUNCTION__, i, argv[i]);
    if (clixon_proc_socket(argv, SOCK_STREAM, &cch->cch_pid, &cch->cch_socket) < 0){
        goto done;
    }
    retval = 0;
 done:
    return retval;
}

/*! Connect client to clixon backend according to config and return a socket
 *
 * @param[in]  h        Clixon handle
 * @param[in]  socktype Type of socket, internal/external/netconf/ssh
 * @param[in]  dest     Destination for some types
 * @retval     ch       Clixon session handler
 * @retval     NULL     Error
 * @see clixon_client_disconnect  Close the socket returned here
 */
clixon_client_handle
clixon_client_connect(clicon_handle      h,
                      clixon_client_type socktype,
                      const char        *dest)
{
    struct clixon_client_handle *cch = NULL;
    size_t                       sz = sizeof(struct clixon_client_handle);

    clixon_debug(CLIXON_DBG_DEFAULT, "%s", __FUNCTION__);
    if ((cch = malloc(sz)) == NULL){
        clicon_err(OE_NETCONF, errno, "malloc");
        goto done;
    }
    memset(cch, 0, sz);
    cch->cch_magic   = CLIXON_CLIENT_MAGIC;
    cch->cch_type = socktype;
    cch->cch_h = h;
    switch (socktype){
    case CLIXON_CLIENT_IPC:
        if (clicon_rpc_connect(h, &cch->cch_socket) < 0)
            goto err;
        break;
    case CLIXON_CLIENT_NETCONF:
        if (clixon_client_connect_netconf(h, cch) < 0)
            goto err;
        break;
#ifdef SSH_BIN
    case CLIXON_CLIENT_SSH:
        if (clixon_client_connect_ssh(h, cch, dest) < 0)
            goto err;
#else
        clicon_err(OE_UNIX, 0, "No ssh bin");
        goto done;
#endif
        break;
    } /* switch */
 done:
    clixon_debug(CLIXON_DBG_DEFAULT, "%s retval:%p", __FUNCTION__, cch);
    return cch;
 err:
    if (cch)
        clixon_client_disconnect(cch);
    cch = NULL;
    goto done;
}

/*! Connect client to clixon backend according to config and return a socket
 *
 * @param[in]  ch        Clixon client session handle
 * @retval     0         OK
 * @retval    -1         Error
 * @see clixon_client_connect where the handle is created
 * The handle is deallocated
 */
int
clixon_client_disconnect(clixon_client_handle ch)
{
    int                          retval = -1;
    struct clixon_client_handle *cch = chandle(ch);

    clixon_debug(CLIXON_DBG_DEFAULT, "%s", __FUNCTION__);
    if (cch == NULL){
        clicon_err(OE_XML, EINVAL, "Expected cch handle");
        goto done;
    }
    /* unlock (if locked) */
    if (cch->cch_locked)
        ;//     (void)clixon_client_lock(cch->cch_socket, 0, "running");

    switch(cch->cch_type){
    case CLIXON_CLIENT_IPC:
        close(cch->cch_socket);
        break;
    case CLIXON_CLIENT_SSH:
    case CLIXON_CLIENT_NETCONF:
        if (clixon_proc_socket_close(cch->cch_pid,
                                     cch->cch_socket) < 0)
            goto done;
        break;
    }
    free(cch);
    retval = 0;
 done:
    return retval;
}

/*! Get the bottom-most leaf in an xml tree being a result of xpath
 *
 * @param[in]  xtop    Pointer to XML top-of-tree
 * @param[out] xbotp   Pointer to XML bottom node
 * @retval     0       OK
 */
static int
clixon_xml_bottom(cxobj  *xtop,
                  cxobj **xbotp)
{
    int retval = -1;
    cxobj *x;
    cxobj *xp;
    cxobj *xc = NULL;

    xp = xtop;
    while (xp != NULL){
        /* Find child, if many, one which is not a key, if any */
        xc = NULL;
        x = NULL;
        while ((x = xml_child_each(xp, x, CX_ELMNT)) != NULL)
            xc = x;
        /* xc is last XXX */
        if (xc == NULL)
            break;
        xp = xc;
    }
    retval = 0;
    *xbotp = xp;
    retval = 0;
    // done:
    return retval;
}

/*! Internal function to construct a get-config and query a value from the backend
 *
 * @param[in]  h         Clixon handle
 * @param[in]  sock      Socket
 * @param[in]  descr     Description of peer for logging
 * @param[in]  namespace Default namespace used for non-prefixed entries in xpath. (Alt use nsc)
 * @param[in]  xpath     XPath
 * @param[out] xdata     XML data tree (may or may not include the intended data)
 * @retval     0         OK
 * @retval    -1         Error
 * @note configurable netconf framing type, now hardwired to 0
 */
static int
clixon_client_get_xdata(clicon_handle h,
                        int         sock,
                        const char *descr,
                        const char *namespace,
                        const char *xpath,
                        cxobj     **xdata)
{
    int          retval = -1;
    cxobj       *xret = NULL;
    cxobj       *xd;
    cbuf        *msg = NULL;
    cbuf        *msgret = NULL;
    const char  *db = "running";
    cvec        *nsc = NULL;
    int          eof = 0;

    clixon_debug(CLIXON_DBG_DEFAULT, "%s", __FUNCTION__);
    if ((msg = cbuf_new()) == NULL){
        clicon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    if ((msgret = cbuf_new()) == NULL){
        clicon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(msg, "<rpc xmlns=\"%s\"", NETCONF_BASE_NAMESPACE);
    cprintf(msg, " xmlns:%s=\"%s\"",
            NETCONF_BASE_PREFIX, NETCONF_BASE_NAMESPACE);
    cprintf(msg, " %s", NETCONF_MESSAGE_ID_ATTR);
    cprintf(msg, "><get-config><source><%s/></source>", db);
    if (xpath && strlen(xpath)){
        cprintf(msg, "<%s:filter %s:type=\"xpath\" xmlns=\"%s\" %s:select=\"%s\"",
                NETCONF_BASE_PREFIX,
                NETCONF_BASE_PREFIX,
                namespace,
                NETCONF_BASE_PREFIX,
                xpath);
        if (xml_nsctx_cbuf(msg, nsc) < 0)
            goto done;
        cprintf(msg, "/>");
    }
    cprintf(msg, "</get-config></rpc>");
    if (netconf_output_encap(0, msg) < 0) // XXX configurable session
        goto done;
    if (clicon_msg_send1(sock, descr, msg) < 0)
        goto done;
    if (clicon_msg_rcv1(sock, descr, msgret, &eof) < 0)
        goto done;
    if (eof){
        close(sock);
        clicon_err(OE_PROTO, ESHUTDOWN, "Unexpected close of CLICON_SOCK. Clixon backend daemon may have crashed.");
        goto done;
    }
    if (clixon_xml_parse_string(cbuf_get(msgret), YB_NONE, NULL, &xret, NULL) < 0)
        goto done;
    if ((xd = xpath_first(xret, NULL, "/rpc-reply/rpc-error")) != NULL){
        xd = xml_parent(xd); /* point to rpc-reply */
        clixon_netconf_error(h, xd, "Get config", NULL);
        goto done; /* Not fatal */
    }
    else if ((xd = xpath_first(xret, NULL, "/rpc-reply/data")) == NULL){
        if ((xd = xml_new(NETCONF_OUTPUT_DATA, NULL, CX_ELMNT)) == NULL)
            goto done;
    }
    else{
        if (xml_rm(xd) < 0)
            goto done;
    }
    *xdata = xd;
    retval = 0;
 done:
    clixon_debug(CLIXON_DBG_DEFAULT, "%s retval:%d", __FUNCTION__, retval);
    if (xret)
        xml_free(xret);
    if (msg)
        cbuf_free(msg);
    if (msgret)
        cbuf_free(msgret);
    return retval;
}

/*! Generic get value of body
 *
 * @param[in]  h         Clixon handle
 * @param[in]  sock      Open socket
 * @param[in]  descr     Description of peer for logging
 * @param[in]  namespace Default namespace used for non-prefixed entries in xpath.
 * @param[in]  xpath     XPath
 * @param[out] val       Output value
 * @retval     0         OK
 * @retval    -1         Error
 */
static int
clixon_client_get_body_val(clicon_handle h,
                           int         sock,
                           const char *descr,
                           const char *namespace,
                           const char *xpath,
                           char      **val)
{
    int    retval = -1;
    cxobj *xdata = NULL;
    cxobj *xobj = NULL;

    clixon_debug(CLIXON_DBG_DEFAULT, "%s", __FUNCTION__);
    if (val == NULL){
        clicon_err(OE_XML, EINVAL, "Expected val");
        goto done;
    }
    if (clixon_client_get_xdata(h, sock, descr, namespace, xpath, &xdata) < 0)
        goto done;
    if (xdata == NULL){
        clicon_err(OE_XML, EINVAL, "No xml obj found");
        goto done;
    }
    /* Is this an error, maybe an "unset" retval ? */
    if (xml_child_nr_type(xdata, CX_ELMNT) == 0){
        clicon_err(OE_XML, EINVAL, "Value not found");
        goto done;
    }
    if (clixon_xml_bottom(xdata, &xobj) < 0)
        goto done;
    if (xobj == NULL){
        clicon_err(OE_XML, EINVAL, "No xml value found");
        goto done;
    }
    *val = xml_body(xobj);
    retval = 0;
 done:
    clixon_debug(CLIXON_DBG_DEFAULT, "%s retval:%d", __FUNCTION__, retval);
    return retval;
}

/*! Client-api get boolean
 *
 * @param[in]  ch        Clixon client handle
 * @param[out] rval      Return value 
 * @param[in]  namespace Default namespace used for non-prefixed entries in xpath. (Alt use nsc)
 * @param[in]  xpath     XPath
 * @retval     0         OK
 * @retval    -1         Error
 */
int
clixon_client_get_bool(clixon_client_handle ch,
                       int                 *rval,
                       const char          *namespace,
                       const char          *xpath)
{
    int                          retval = -1;
    struct clixon_client_handle *cch = chandle(ch);
    char                        *val = NULL;
    char                        *reason = NULL;
    int                          ret;
    uint8_t                      val0=0;

    clixon_debug(CLIXON_DBG_DEFAULT, "%s", __FUNCTION__);
    if (clixon_client_get_body_val(cch->cch_h, cch->cch_socket, cch->cch_descr,
                                   namespace, xpath, &val) < 0)
        goto done;
    if ((ret = parse_bool(val, &val0, &reason)) < 0){
        clicon_err(OE_XML, errno, "parse_bool");
        goto done;
    }
    if (ret == 0){
        clicon_err(OE_XML, EINVAL, "%s", reason);
        goto done;
    }
    *rval = (int)val0;
    retval = 0;
 done:
    if (reason)
        free(reason);
    return retval;
}

/*! Client-api get string
 *
 * @param[in]  ch        Clixon client handle
 * @param[out] rval      Return value string  
 * @param[in]  n         Length of string
 * @param[in]  namespace Default namespace used for non-prefixed entries in xpath. (Alt use nsc)
 * @param[in]  xpath     XPath
 * @retval     0         OK
 * @retval    -1         Error
 */
int
clixon_client_get_str(clixon_client_handle ch,
                      char                *rval,
                      int                  n,
                      const char          *namespace,
                      const char          *xpath)
{
    int                          retval = -1;
    struct clixon_client_handle *cch = chandle(ch);
    char                        *val = NULL;

    clixon_debug(CLIXON_DBG_DEFAULT, "%s", __FUNCTION__);
    if (clixon_client_get_body_val(cch->cch_h, cch->cch_socket, cch->cch_descr,
                                   namespace, xpath, &val) < 0)
        goto done;
    strncpy(rval, val, n-1);
    rval[n-1]= '\0';
    retval = 0;
 done:
    return retval;
}

/*! Client-api get uint8
 *
 * @param[in]  ch        Clixon client handle
 * @param[out] rval      Return value  
 * @param[in]  namespace Default namespace used for non-prefixed entries in xpath. (Alt use nsc)
 * @param[in]  xpath     XPath
 * @retval     0         OK
 * @retval    -1         Error
 */
int
clixon_client_get_uint8(clixon_client_handle ch,
                        uint8_t             *rval,
                        const char          *namespace,
                        const char          *xpath)
{
    int                          retval = -1;
    struct clixon_client_handle *cch = chandle(ch);
    char                        *val = NULL;
    char                        *reason = NULL;
    int                          ret;

    clixon_debug(CLIXON_DBG_DEFAULT, "%s", __FUNCTION__);
    if (clixon_client_get_body_val(cch->cch_h, cch->cch_socket, cch->cch_descr,
                                   namespace, xpath, &val) < 0)
        goto done;
    if ((ret = parse_uint8(val, rval, &reason)) < 0){
        clicon_err(OE_XML, errno, "parse_bool");
        goto done;
    }
    if (ret == 0){
        clicon_err(OE_XML, EINVAL, "%s", reason);
        goto done;
    }
    retval = 0;
 done:
    if (reason)
        free(reason);
    return retval;
}

/*! Client-api get uint16
 *
 * @param[in]  ch        Clixon client handle
 * @param[out] rval      Return value  
 * @param[in]  namespace Default namespace used for non-prefixed entries in xpath. (Alt use nsc)
 * @param[in]  xpath     XPath
 * @retval     0         OK
 * @retval    -1         Error
 */
int
clixon_client_get_uint16(clixon_client_handle ch,
                         uint16_t            *rval,
                         const char          *namespace,
                         const char          *xpath)
{
    int                          retval = -1;
    struct clixon_client_handle *cch = chandle(ch);
    char                        *val = NULL;
    char                        *reason = NULL;
    int                          ret;

    clixon_debug(CLIXON_DBG_DEFAULT, "%s", __FUNCTION__);
    if (clixon_client_get_body_val(cch->cch_h, cch->cch_socket, cch->cch_descr,
                                   namespace, xpath, &val) < 0)
        goto done;
    if ((ret = parse_uint16(val, rval, &reason)) < 0){
        clicon_err(OE_XML, errno, "parse_bool");
        goto done;
    }
    if (ret == 0){
        clicon_err(OE_XML, EINVAL, "%s", reason);
        goto done;
    }
    retval = 0;
 done:
    if (reason)
        free(reason);
    return retval;
}

/*! Client-api get uint32
 *
 * @param[in]  ch        Clixon client handle
 * @param[out] rval      Return value  
 * @param[in]  namespace Default namespace used for non-prefixed entries in xpath. (Alt use nsc)
 * @param[in]  xpath     XPath
 * @retval     0         OK
 * @retval    -1         Error
 */
int
clixon_client_get_uint32(clixon_client_handle ch,
                         uint32_t            *rval,
                         const char          *namespace,
                         const char          *xpath)
{
    int                          retval = -1;
    struct clixon_client_handle *cch = chandle(ch);
    char                        *val = NULL;
    char                        *reason = NULL;
    int                          ret;

    clixon_debug(CLIXON_DBG_DEFAULT, "%s", __FUNCTION__);
    if (clixon_client_get_body_val(cch->cch_h, cch->cch_socket, cch->cch_descr,
                                   namespace, xpath, &val) < 0)
        goto done;
    if (val == NULL){
        clicon_err(OE_XML, EFAULT, "val is NULL");
        goto done;
    }
    if ((ret = parse_uint32(val, rval, &reason)) < 0){
        clicon_err(OE_XML, errno, "parse_bool");
        goto done;
    }
    if (ret == 0){
        clicon_err(OE_XML, EINVAL, "%s", reason);
        goto done;
    }
    retval = 0;
 done:
    clixon_debug(CLIXON_DBG_DEFAULT, "%s retval:%d", __FUNCTION__, retval);
    if (reason)
        free(reason);
    return retval;
}

/*! Client-api get uint64
 *
 * @param[in]  ch        Clixon client handle
 * @param[out] rval      Return value  
 * @param[in]  namespace Default namespace used for non-prefixed entries in xpath. (Alt use nsc)
 * @param[in]  xpath     XPath
 * @retval     0         OK
 * @retval    -1         Error
 */
int
clixon_client_get_uint64(clixon_client_handle  ch,
                         uint64_t             *rval,
                         const char           *namespace,
                         const char           *xpath)
{
    int                          retval = -1;
    struct clixon_client_handle *cch = chandle(ch);
    char                        *val = NULL;
    char                        *reason = NULL;
    int                          ret;

    clixon_debug(CLIXON_DBG_DEFAULT, "%s", __FUNCTION__);
    if (clixon_client_get_body_val(cch->cch_h, cch->cch_socket, cch->cch_descr,
                                   namespace, xpath, &val) < 0)
        goto done;
    if ((ret = parse_uint64(val, rval, &reason)) < 0){
        clicon_err(OE_XML, errno, "parse_bool");
        goto done;
    }
    if (ret == 0){
        clicon_err(OE_XML, EINVAL, "%s", reason);
        goto done;
    }
    retval = 0;
 done:
    if (reason)
        free(reason);
    return retval;
}

/* Access functions */
/*! Client-api get uint64
 *
 * @param[in]  ch     Clixon client handle
 * @retval     s      Open socket
 * @retval    -1      No/closed socket
 */
int
clixon_client_socket_get(clixon_client_handle ch)
{
    struct clixon_client_handle *cch = chandle(ch);

    return cch->cch_socket;
}
