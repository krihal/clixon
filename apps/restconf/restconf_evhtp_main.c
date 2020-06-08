/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand
  Copyright (C) 2020 Olof Hagsand and Rubicon Communications, LLC(Netgate)

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
  
 */

/* XXX temp constant should go away, */
#undef _EVHTP_NYI 

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif
/* compilation withotu threading support 
 * XXX: could be disabled already in configure?
 */
#define EVHTP_DISABLE_EVTHR
#define EVHTP_DISABLE_REGEX

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>
#include <fcntl.h>
#include <time.h>
#include <limits.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <assert.h>
#include <sys/stat.h> /* chmod */

/* evhtp */
#include <evhtp/evhtp.h>
#include <evhtp/sslutils.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

/* restconf */

#include "restconf_lib.h"
#if 0 /* These are all dependent on FCGX */
#include "restconf_methods.h"
#include "restconf_methods_get.h"
#include "restconf_methods_post.h"
#include "restconf_stream.h"
#endif

/* Command line options to be passed to getopt(3) */
#define RESTCONF_OPTS "hD:f:l:p:d:y:a:u:o:P:c:k:"

/* Need global variable to for signal handler XXX */
static clicon_handle _CLICON_HANDLE = NULL;

/*! Signall terminates process
 */
static void
restconf_sig_term(int arg)
{
    static int i=0;

    if (i++ == 0)
	clicon_log(LOG_NOTICE, "%s: %s: pid: %u Signal %d", 
		   __PROGRAM__, __FUNCTION__, getpid(), arg);
    else
	exit(-1);
    if (_CLICON_HANDLE){
#ifdef _EVHTP_NYI 
	stream_child_freeall(_CLICON_HANDLE);
#endif
	restconf_terminate(_CLICON_HANDLE);
    }
    clicon_exit_set(); /* checked in clixon_event_loop() */
    exit(-1);
}

static void
restconf_sig_child(int arg)
{
    int status;
    int pid;

    if ((pid = waitpid(-1, &status, 0)) != -1 && WIFEXITED(status)){
#ifdef _EVHTP_NYI
	;
#endif
    }
}

static void
cx_gencb(evhtp_request_t *req,
	 void            *arg)
{
    evhtp_connection_t *conn;
    //    clicon_handle       h = arg;

    fprintf(stderr, "%s\n", __FUNCTION__);    
    if (req == NULL){
	errno = EINVAL;
	return;
    }
    if ((conn = evhtp_request_get_connection(req)) == NULL)
	goto done;
    htp_sslutil_add_xheaders(
        req->headers_out,
        conn->ssl,
        HTP_SSLUTILS_XHDR_ALL);
    evhtp_send_reply(req, EVHTP_RES_NOTFOUND);
 done:
    return; /* void */
}

static evhtp_res
cx_pre_accept(evhtp_connection_t *conn,
	      void               *arg)
{
    fprintf(stderr, "%s\n", __FUNCTION__);
    return EVHTP_RES_OK;
}

static evhtp_res
cx_post_accept(evhtp_connection_t *conn,
	       void               *arg)
{
    fprintf(stderr, "%s\n", __FUNCTION__);
    return EVHTP_RES_OK;
}

static int
print_header_(evhtp_header_t * header, void * arg) {
    fprintf(stderr, "%s: %s\n", header->key, header->val);
    return 0;
}

/*! Generic callback called if no other callbacks are matched
 */
static void
cx_path_restconf(evhtp_request_t *req,
		 void            *arg)
{
    evhtp_connection_t *conn;
    //    clicon_handle       h = arg;
    struct evbuffer    *b = NULL; 
    htp_method          meth;

    fprintf(stderr, "%s\n", __FUNCTION__);    
    if (req == NULL){
	errno = EINVAL;
	goto done;
    }
    if ((conn = evhtp_request_get_connection(req)) == NULL)
	goto done;
    meth = evhtp_request_get_method(req);
    fprintf(stderr, "%s method:%d\n", __FUNCTION__, meth);    
    evhtp_headers_for_each(req->headers_in, print_header_, NULL);

    if ((b = evbuffer_new()) == NULL){
	goto done;
    }
    htp_sslutil_add_xheaders(
        req->headers_out,
        conn->ssl,
        HTP_SSLUTILS_XHDR_ALL);
    evhtp_send_reply_start(req, EVHTP_RES_OK);
    evbuffer_add(b, "hej\n", strlen("hej\n\n"));
    evhtp_send_reply_body(req, b);
    evhtp_send_reply_end(req);

    //   evhtp_headers_add_header(request->headers_out, evhtp_header_new("Host", "localhost", 0, 0)); evhtp_headers_add_headers(request->headers_out, headers);
    


 done:
    return; /* void */
}

/*! Usage help routine
 * @param[in]  argv0  command line
 * @param[in]  h      Clicon handle
 */
static void
usage(clicon_handle h,
      char         *argv0)

{
    fprintf(stderr, "usage:%s [options]\n"
	    "where options are\n"
            "\t-h \t\t  Help\n"
	    "\t-D <level>\t  Debug level\n"
    	    "\t-f <file>\t  Configuration file (mandatory)\n"
	    "\t-l <s|f<file>> \t  Log on (s)yslog, (f)ile (syslog is default)\n"
	    "\t-p <dir>\t  Yang directory path (see CLICON_YANG_DIR)\n"
	    "\t-d <dir>\t  Specify restconf plugin directory dir (default: %s)\n"
	    "\t-y <file>\t  Load yang spec file (override yang main module)\n"
    	    "\t-a UNIX|IPv4|IPv6 Internal backend socket family\n"
    	    "\t-u <path|addr>\t  Internal socket domain path or IP addr (see -a)\n"
	    "\t-o \"<option>=<value>\" Give configuration option overriding config file (see clixon-config.yang)\n"
    	    "\t-P <port>\t  HTTPS port (default 443)\n"
	    "\t-c <cert>\t  SSL server certificate - pemfile (mandatory)\n"
	    "\t-k <key>\t  SSL private key - privfile (mandatory)\n"
	    ,
	    argv0,
	    clicon_restconf_dir(h)
	    );
    exit(0);
}

/*! Main routine for libevhtp restconf
 */
int
main(int    argc,
     char **argv)
{
    int            retval = -1;
    char	  *argv0 = argv[0];
    int            c;
    clicon_handle  h;
    char          *dir;
    int            logdst = CLICON_LOG_SYSLOG;
    yang_stmt     *yspec = NULL;
    char          *str;
    clixon_plugin *cp = NULL;
    cvec          *nsctx_global = NULL; /* Global namespace context */
    size_t         cligen_buflen;
    size_t         cligen_bufthreshold;
    uint16_t       port = 443;
#ifdef _EVHTP_NYI 
    char          *stream_path;
#endif
    evhtp_t           *htp = NULL;
    struct event_base *evbase = NULL;
    evhtp_ssl_cfg_t   *ssl_config = NULL;
    struct stat        f_stat;

    /* In the startup, logs to stderr & debug flag set later */
    clicon_log_init(__PROGRAM__, LOG_INFO, logdst); 

    /* Create handle */
    if ((h = clicon_handle_init()) == NULL)
	goto done;

    _CLICON_HANDLE = h; /* for termination handling */

    while ((c = getopt(argc, argv, RESTCONF_OPTS)) != -1)
	switch (c) {
	case 'h':
	    usage(h, argv0);
	    break;
	case 'D' : /* debug */
	    if (sscanf(optarg, "%d", &debug) != 1)
		usage(h, argv0);
	    break;
	 case 'f': /* override config file */
	    if (!strlen(optarg))
		usage(h, argv0);
	    clicon_option_str_set(h, "CLICON_CONFIGFILE", optarg);
	    break;
	 case 'l': /* Log destination: s|e|o */
	     if ((logdst = clicon_log_opt(optarg[0])) < 0)
		usage(h, argv0);
	    if (logdst == CLICON_LOG_FILE &&
		strlen(optarg)>1 &&
		clicon_log_file(optarg+1) < 0)
		goto done;
	   break;
	} /* switch getopt */

    /* 
     * Logs, error and debug to stderr or syslog, set debug level
     */
    clicon_log_init(__PROGRAM__, debug?LOG_DEBUG:LOG_INFO, logdst); 

    clicon_debug_init(debug, NULL); 
    clicon_log(LOG_NOTICE, "%s: %u Started", __PROGRAM__, getpid());
    if (set_signal(SIGTERM, restconf_sig_term, NULL) < 0){
	clicon_err(OE_DAEMON, errno, "Setting signal");
	goto done;
    }
    if (set_signal(SIGINT, restconf_sig_term, NULL) < 0){
	clicon_err(OE_DAEMON, errno, "Setting signal");
	goto done;
    }
    if (set_signal(SIGCHLD, restconf_sig_child, NULL) < 0){
	clicon_err(OE_DAEMON, errno, "Setting signal");
	goto done;
    }

    /* Find and read configfile */
    if (clicon_options_main(h) < 0)
	goto done;
#ifdef _EVHTP_NYI 
    stream_path = clicon_option_str(h, "CLICON_STREAM_PATH");
#endif
    /* Init evhtp ssl config struct */
    if ((ssl_config = malloc(sizeof(evhtp_ssl_cfg_t))) == NULL){
	clicon_err(OE_UNIX, errno, "malloc");
	goto done;
    }
    memset(ssl_config, 0, sizeof(evhtp_ssl_cfg_t));
    ssl_config->ssl_opts = SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1;
    
    /* Now rest of options, some overwrite option file */
    optind = 1;
    opterr = 0;
    while ((c = getopt(argc, argv, RESTCONF_OPTS)) != -1)
	switch (c) {
	case 'h' : /* help */
	case 'D' : /* debug */
	case 'f':  /* config file */
	case 'l':  /* log  */
	    break; /* see above */
	case 'p' : /* yang dir path */
	    if (clicon_option_add(h, "CLICON_YANG_DIR", optarg) < 0)
		goto done;
	    break;
	case 'd':  /* Plugin directory */
	    if (!strlen(optarg))
		usage(h, argv0);
	    clicon_option_str_set(h, "CLICON_RESTCONF_DIR", optarg);
	    break;
	case 'y' : /* Load yang spec file (override yang main module) */
	    clicon_option_str_set(h, "CLICON_YANG_MAIN_FILE", optarg);
	    break;
	case 'a': /* internal backend socket address family */
	    clicon_option_str_set(h, "CLICON_SOCK_FAMILY", optarg);
	    break;
	case 'u': /* internal backend socket unix domain path or ip host */
	    if (!strlen(optarg))
		usage(h, argv0);
	    clicon_option_str_set(h, "CLICON_SOCK", optarg);
	    break;
	case 'o':{ /* Configuration option */
	    char          *val;
	    if ((val = index(optarg, '=')) == NULL)
		usage(h, argv0);
	    *val++ = '\0';
	    if (clicon_option_add(h, optarg, val) < 0)
		goto done;
	    break;
	}
	case 'P': /* http port */
	    if (!strlen(optarg))
		usage(h, argv0);
	    port=atoi(optarg);
	    break;
	case 'c': /* SSL Server Certificate */
	    ssl_config->pemfile = optarg;
	    break;
	case 'k': /* SSL private key */
	    ssl_config->privfile = optarg;
	    break;
        default:
            usage(h, argv0);
            break;
	}
    argc -= optind;
    argv += optind;

    /* Check ssl mandatory options */
    if (ssl_config->pemfile == NULL || ssl_config->privfile == NULL)
	usage(h, argv0);
    /* Verify SSL files */
    if (ssl_config->pemfile == NULL)
	usage(h, argv0);
    if (stat(ssl_config->pemfile, &f_stat) != 0) {
	clicon_err(OE_FATAL, errno, "Cannot load SSL cert '%s'", ssl_config->pemfile);
	goto done;
    }
    if (ssl_config->privfile == NULL) 
	usage(h, argv0);
    if (stat(ssl_config->privfile, &f_stat) != 0) {
	clicon_err(OE_FATAL, errno, "Cannot load SSL key '%s'", ssl_config->privfile);
	goto done;
    }
    //    ssl_verify_mode             = htp_sslutil_verify2opts(optarg);
    assert(SSL_VERIFY_NONE == 0);
    /* Access the remaining argv/argc options (after --) w clicon-argv_get() */
    clicon_argv_set(h, argv0, argc, argv);
    
    /* Init cligen buffers */
    cligen_buflen = clicon_option_int(h, "CLICON_CLI_BUF_START");
    cligen_bufthreshold = clicon_option_int(h, "CLICON_CLI_BUF_THRESHOLD");
    cbuf_alloc_set(cligen_buflen, cligen_bufthreshold);

    /* Add (hardcoded) netconf features in case ietf-netconf loaded here
     * Otherwise it is loaded in netconf_module_load below
     */
    if (netconf_module_features(h) < 0)
	goto done;

    /* Create top-level yang spec and store as option */
    if ((yspec = yspec_new()) == NULL)
	goto done;
    clicon_dbspec_yang_set(h, yspec);
    /* Treat unknown XML as anydata */
    if (clicon_option_bool(h, "CLICON_YANG_UNKNOWN_ANYDATA") == 1)
	xml_bind_yang_unknown_anydata(1);
    
    /* Load restconf plugins before yangs are loaded (eg extension callbacks) */
    if ((dir = clicon_restconf_dir(h)) != NULL)
	if (clixon_plugins_load(h, CLIXON_PLUGIN_INIT, dir, NULL) < 0)
	    return -1;
    /* Create a pseudo-plugin to create extension callback to set the ietf-routing
     * yang-data extension for api-root top-level restconf function.
     */
    if (clixon_pseudo_plugin(h, "pseudo restconf", &cp) < 0)
	goto done;
    cp->cp_api.ca_extension = restconf_main_extension_cb;

    /* Load Yang modules
     * 1. Load a yang module as a specific absolute filename */
    if ((str = clicon_yang_main_file(h)) != NULL){
	if (yang_spec_parse_file(h, str, yspec) < 0)
	    goto done;
    }
    /* 2. Load a (single) main module */
    if ((str = clicon_yang_module_main(h)) != NULL){
	if (yang_spec_parse_module(h, str, clicon_yang_module_revision(h),
				   yspec) < 0)
	    goto done;
    }
    /* 3. Load all modules in a directory */
    if ((str = clicon_yang_main_dir(h)) != NULL){
	if (yang_spec_load_dir(h, str, yspec) < 0)
	    goto done;
    }
    /* Load clixon lib yang module */
    if (yang_spec_parse_module(h, "clixon-lib", NULL, yspec) < 0)
	goto done;
     /* Load yang module library, RFC7895 */
    if (yang_modules_init(h) < 0)
	goto done;

    /* Load yang restconf module */
    if (yang_spec_parse_module(h, "ietf-restconf", NULL, yspec)< 0)
	goto done;
    
    /* Add netconf yang spec, used as internal protocol */
    if (netconf_module_load(h) < 0)
	goto done;
    
    /* Add system modules */
     if (clicon_option_bool(h, "CLICON_STREAM_DISCOVERY_RFC8040") &&
	 yang_spec_parse_module(h, "ietf-restconf-monitoring", NULL, yspec)< 0)
	 goto done;
     if (clicon_option_bool(h, "CLICON_STREAM_DISCOVERY_RFC5277") &&
	 yang_spec_parse_module(h, "clixon-rfc5277", NULL, yspec)< 0)
	 goto done;

     /* Here all modules are loaded 
      * Compute and set canonical namespace context
      */
     if (xml_nsctx_yangspec(yspec, &nsctx_global) < 0)
	 goto done;
     if (clicon_nsctx_global_set(h, nsctx_global) < 0)
	 goto done;

     /* Dump configuration options on debug */
    if (debug)      
	clicon_option_dump(h, debug);

    /* Call start function in all plugins before we go interactive 
     */
     if (clixon_plugin_start_all(h) < 0)
	 goto done;

    /* Find and read configfile */
    if (clicon_options_main(h) < 0)
	goto done;

    /* Init evhtp */
    if ((evbase = event_base_new()) == NULL){
	clicon_err(OE_UNIX, errno, "event_base_new");
	goto done;
    }
    /* create a new evhtp_t instance */
    if ((htp = evhtp_new(evbase, NULL)) == NULL){
	clicon_err(OE_UNIX, errno, "evhtp_new");
	goto done;
    }
    if (evhtp_ssl_init(htp, ssl_config) < 0){
	clicon_err(OE_UNIX, errno, "evhtp_new");
	goto done;
    }
    /* Generic callback called if no other callbacks are matched */
    evhtp_set_gencb(htp, cx_gencb, h);

    /* Callback before the connection is accepted. */
    evhtp_set_pre_accept_cb(htp, cx_pre_accept, h);

    /* Callback right after a connection is accepted. */
    evhtp_set_post_accept_cb(htp, cx_post_accept, h);

    /* Callback to be executed on a specific path */
    if (evhtp_set_cb(htp, "/" RESTCONF_API, cx_path_restconf, h) == NULL){
	clicon_err(OE_EVENTS, errno, "evhtp_set_cb");
	goto done;
    }

    /* bind to a socket, optionally with specific protocol support formatting */
    if (evhtp_bind_socket(htp, "127.0.0.1", port, 128) < 0){
	clicon_err(OE_UNIX, errno, "evhtp_bind_socket");
	goto done;
    }

    event_base_loop(evbase, 0);
    
    evhtp_unbind_socket(htp);

    //    evhtp_safe_free(htp, evhtp_free);
    //    evhtp_safe_free(evbase, event_base_free);

    retval = 0;
 done:
#ifdef _EVHTP_NYI 
    stream_child_freeall(h);
#endif
    restconf_terminate(h);    
    return retval;
}