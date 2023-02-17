/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren
  Copyright (C) 2017-2019 Olof Hagsand
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
  use your version of this file under the terms of Apache License version 2, 
  indicate your decision by deleting the provisions above and replace them with
  the  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****

 *
 * Given an existing XML tree, bind YANG specs to XML nodes according to different
 * algorithms
 */
#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <syslog.h>
#include <fcntl.h>
#include <assert.h>
#include <arpa/inet.h>
#include <sys/param.h>
#include <netinet/in.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */

#include "clixon_string.h"
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_string.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_options.h"
#include "clixon_data.h"
#include "clixon_yang_module.h"
#include "clixon_yang_schema_mount.h"
#include "clixon_plugin.h"
#include "clixon_xml_nsctx.h"
#include "clixon_xpath_ctx.h"
#include "clixon_xpath.h"
#include "clixon_log.h"
#include "clixon_err.h"
#include "clixon_netconf_lib.h"
#include "clixon_plugin.h"
#include "clixon_xml_sort.h"
#include "clixon_yang_type.h"
#include "clixon_xml_map.h"
#include "clixon_xml_bind.h"

/*
 * Local variables
 */
static int _yang_unknown_anydata = 0;
static int _netconf_message_id_optional = 0;

/*! Kludge to equate unknown XML with anydata
 * The problem with this is that its global and should be bound to a handle
 */
int
xml_bind_yang_unknown_anydata(int val)
{
    _yang_unknown_anydata = val;
    return 0;
}

/*! Kludge to set message_id_optional
 * The problem with this is that its global and should be bound to a handle
 */
int
xml_bind_netconf_message_id_optional(int val)
{
    _netconf_message_id_optional = val;
    return 0;
}

/*! After yang binding, bodies of containers and lists are stripped from XML bodies
 * May apply to other nodes?
 * Exception for bodies marked with XML_FLAG_BODYKEY, see text syntax parsing
 * @see text_mark_bodies
 */
static int
strip_body_objects(cxobj *xt)
{
    yang_stmt    *yt;
    enum rfc_6020 keyword;
    cxobj        *xb;
    
    if ((yt = xml_spec(xt)) != NULL){
        keyword = yang_keyword_get(yt);
        if (keyword == Y_LIST || keyword == Y_CONTAINER){
            xb = NULL;
            /* Quits if marked object, assume all are same */
            while ((xb = xml_find_type(xt, NULL, "body", CX_BODY)) != NULL &&
                   !xml_flag(xb, XML_FLAG_BODYKEY)){
                xml_purge(xb);
            }
        }
    }

    return 0;
}

/*! Associate XML node x with x:s parents yang:s matching child
 *
 * @param[in]   xt     XML tree node
 * @param[in]   xsibling
 * @param[in]   yspec  Top-level YANG spec / mount-point
 * @param[out]  xerr   Reason for failure, or NULL
 * @retval      2      OK Yang assignment not made because yang parent is anyxml or anydata
 * @retval      1      OK Yang assignment made
 * @retval      0      Yang assigment not made and xerr set
 * @retval     -1      Error
 * @note retval = 2 is special
 * @see populate_self_top
 */
static int
populate_self_parent(cxobj     *xt,
                     cxobj     *xsibling,
                     yang_stmt *yspec,
                     cxobj    **xerr)
{
    int        retval = -1;
    yang_stmt *y = NULL;     /* yang node */
    yang_stmt *yparent;      /* yang parent */
    cxobj     *xp = NULL;    /* xml parent */
    char      *name;
    char      *ns = NULL;    /* XML namespace of xt */
    char      *nsy = NULL;   /* Yang namespace of xt */
    cbuf      *cb = NULL;

    name = xml_name(xt);
    /* optimization for massive lists - use the first element as role model */
    if (xsibling &&
        xml_child_nr_type(xt, CX_ATTR) == 0){
        y = xml_spec(xsibling);
        goto set;
    }
    xp = xml_parent(xt);
    if (xp == NULL){
        if (xerr &&
            netconf_bad_element_xml(xerr, "application", name, "Missing parent") < 0)
            goto done;
        goto fail;
    }
    if ((yparent = xml_spec(xp)) == NULL){
        if (xerr &&
            netconf_bad_element_xml(xerr, "application", name, "Missing parent yang node") < 0)
            goto done;
        goto fail;
    }
    if (yang_keyword_get(yparent) == Y_ANYXML || yang_keyword_get(yparent) == Y_ANYDATA){
        retval = 2;
        goto done;
    }
    if (xml2ns(xt, xml_prefix(xt), &ns) < 0)
        goto done;
    /* Special case since action is not a datanode */
    if ((y = yang_find(yparent, Y_ACTION, name)) == NULL)
        if ((y = yang_find_datanode(yparent, name)) == NULL){
            if (_yang_unknown_anydata){
                /* Add dummy Y_ANYDATA yang stmt, see ysp_add */
                if ((y = yang_anydata_add(yparent, name)) < 0)
                    goto done;
                xml_spec_set(xt, y);
                retval = 2; /* treat as anydata */
                clicon_log(LOG_WARNING,
                           "%s: %d: No YANG spec for %s, anydata used",
                           __FUNCTION__, __LINE__, name);
                goto done;
            }
            if ((cb = cbuf_new()) == NULL){
                clicon_err(OE_UNIX, errno, "cbuf_new");
                goto done;
            }
            cprintf(cb, "Failed to find YANG spec of XML node: %s", name);
            cprintf(cb, " with parent: %s", xml_name(xp));
            if (ns)
                cprintf(cb, " in namespace: %s", ns);
            if (xerr &&
                netconf_unknown_element_xml(xerr, "application", name, cbuf_get(cb)) < 0)
                goto done;
            goto fail;
        }
    nsy = yang_find_mynamespace(y);
    if (ns == NULL || nsy == NULL){
        if (xerr &&
            netconf_bad_element_xml(xerr, "application", name, "Missing namespace") < 0)
            goto done;
        goto fail;
    }
    /* Assign spec only if namespaces match */
    if (strcmp(ns, nsy) != 0){
        if (xerr &&
            netconf_bad_element_xml(xerr, "application", name, "Namespace mismatch") < 0)
            goto done;
        goto fail;
    }
 set:
    xml_spec_set(xt, y);
#ifdef XML_EXPLICIT_INDEX
    if (xml_search_index_p(xt))
        xml_search_child_insert(xp, xt);
#endif
    retval = 1;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Associate XML node x with yang spec y by going through all top-level modules and finding match
 *
 * @param[in]   xt     XML tree node
 * @param[in]   yspec  Yang spec
 * @param[out]  xerr   Reason for failure, or NULL
 * @retval      1      OK yang assignment made
 * @retval      0      yang assigment not made and xerr set
 * @retval     -1      Error
 * @see populate_self_parent
 */
static int
populate_self_top(cxobj     *xt, 
                  yang_stmt *yspec,
                  cxobj    **xerr)
{
    int        retval = -1;
    yang_stmt *y = NULL;     /* yang node */
    yang_stmt *ymod;         /* yang module */
    char      *name;
    char      *ns = NULL;    /* XML namespace of xt */
    char      *nsy = NULL;   /* Yang namespace of xt */
    cbuf      *cb = NULL;
    cxobj     *xp;

    name = xml_name(xt);
    if (yspec == NULL){
        if (xerr &&
            netconf_bad_element_xml(xerr, "application", name, "Missing yang spec") < 0)
            goto done;
        goto fail;
    }
    if (ys_module_by_xml(yspec, xt, &ymod) < 0)
        goto done;
    if (xml2ns(xt, xml_prefix(xt), &ns) < 0)
        goto done;
    /* ymod is "real" module, name may belong to included submodule */
    if (ymod == NULL){
        if (xerr){
            if ((cb = cbuf_new()) == NULL){
                clicon_err(OE_UNIX, errno, "cbuf_new");
                goto done;
            }
            cprintf(cb, "Failed to find YANG spec of XML node: %s", name);
            if ((xp = xml_parent(xt)) != NULL)
                cprintf(cb, " with parent: %s", xml_name(xp));
            if (ns)
                cprintf(cb, " in namespace: %s", ns);
            if (netconf_unknown_element_xml(xerr, "application", name, cbuf_get(cb)) < 0)
                goto done;
        }
        goto fail;
    }

    if ((y = yang_find_schemanode(ymod, name)) == NULL){ /* also rpc */
        if (_yang_unknown_anydata){
            /* Add dummy Y_ANYDATA yang stmt, see ysp_add */
            if ((y = yang_anydata_add(ymod, name)) < 0)
                goto done;
            xml_spec_set(xt, y);
            retval = 2; /* treat as anydata */
            clicon_log(LOG_WARNING,
                       "%s: %d: No YANG spec for %s, anydata used",
                       __FUNCTION__, __LINE__, name);
            goto done;
        }
        if ((cb = cbuf_new()) == NULL){
            clicon_err(OE_UNIX, errno, "cbuf_new");
            goto done;
        }
        cprintf(cb, "Failed to find YANG spec of XML node: %s", name);
        if ((xp = xml_parent(xt)) != NULL)
            cprintf(cb, " with parent: %s", xml_name(xp));
        if (ns)
            cprintf(cb, " in namespace: %s", ns);
        if (xerr &&
            netconf_unknown_element_xml(xerr, "application", name, cbuf_get(cb)) < 0)
            goto done;
        goto fail;
    }
    nsy = yang_find_mynamespace(y);
    if (ns == NULL || nsy == NULL){
        if (xerr &&
            netconf_bad_element_xml(xerr, "application", name, "Missing namespace") < 0)
            goto done;
        goto fail;
    }
    /* Assign spec only if namespaces match */
    if (strcmp(ns, nsy) != 0){
        if (xerr &&
            netconf_bad_element_xml(xerr, "application", name, "Namespace mismatch") < 0)
            goto done;
        goto fail;
    }
    xml_spec_set(xt, y);
    retval = 1;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Find yang spec association of tree of XML nodes
 *
 * Populate xt:s children as top-level symbols
 * This may be unnecessary if yspec is set on manual creation: x=xml_new(); xml_spec_set(x,y)
 * @param[in]   h      Clixon handle (sometimes NULL)
 * @param[in]   xt     XML tree node
 * @param[in]   yb     How to bind yang to XML top-level when parsing
 * @param[in]   yspec  Yang spec
 * @param[out]  xerr   Reason for failure, or NULL
 * @retval      1      OK yang assignment made
 * @retval      0      Partial or no yang assigment made (at least one failed) and xerr set
 * @retval     -1      Error
 * @code
 *   cxobj *xerr = NULL;
 *   if (xml_bind_yang(h, x, YB_MODULE, yspec, &xerr) < 0)
 *     err;
 * @endcode
 * There are several functions in the API family
 * @see xml_bind_yang_rpc     for incoming rpc 
 * @see xml_bind_yang0        If the calling xml object should also be populated
 * @note For subs to anyxml nodes will not have spec set
 */
int
xml_bind_yang(clicon_handle h,
              cxobj        *xt, 
              yang_bind     yb,
              yang_stmt    *yspec,
              cxobj       **xerr)
{
    int    retval = -1;
    cxobj *xc;         /* xml child */
    int    ret;

    strip_body_objects(xt);
    xc = NULL;     /* Apply on children */
    while ((xc = xml_child_each(xt, xc, CX_ELMNT)) != NULL) {
        if ((ret = xml_bind_yang0(h, xc, yb, yspec, xerr)) < 0)
            goto done;
        if (ret == 0)
            goto fail;
    }
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*!
 *
 * @param[in]   h      Clixon handle (sometimes NULL)
 * @param[in]   xt     XML tree node
 * @param[in]   yb     How to bind yang to XML top-level when parsing
 * @param[in]   yspec  Yang spec
 * @param[in]   xsibling
 * @param[out]  xerr   Reason for failure, or NULL
 * @retval      1      OK yang assignment made
 * @retval      0      Partial or no yang assigment made (at least one failed) and xerr set
 * @retval     -1      Error
 */
static int
xml_bind_yang0_opt(clicon_handle h,
                   cxobj        *xt, 
                   yang_bind     yb,
                   yang_stmt    *yspec,
                   cxobj        *xsibling,
                   cxobj       **xerr)
{
    int        retval = -1;
    cxobj     *xc;           /* xml child */
    int        ret;
    yang_stmt *yc0 = NULL;
    cxobj     *xc0 = NULL;
    cxobj     *xs;
    char      *name0 = NULL;
    char      *prefix0 = NULL;
    char      *name;
    yang_bind  ybc;
    char      *prefix;
    yang_stmt *yspec1 = NULL;

    switch (yb){
    case YB_MODULE:
        if ((ret = populate_self_top(xt, yspec, xerr)) < 0)
            goto done;
        break;
    case YB_PARENT:
        if ((ret = populate_self_parent(xt, xsibling, yspec, xerr)) < 0)
            goto done;
        break;
    default:
        clicon_err(OE_XML, EINVAL, "Invalid yang binding: %d", yb);
        goto done;
        break;
    }
    if (ret == 0)
        goto fail;
    else if (ret == 2)     /* ret=2 for anyxml from parent^ */
        goto ok;
    strip_body_objects(xt);
    ybc = YB_PARENT;
    if (h && clicon_option_bool(h, "CLICON_YANG_SCHEMA_MOUNT")){
        yspec1 = NULL;
        if ((ret = xml_yang_mount_get(h, xt, NULL, &yspec1)) < 0)
            goto done;
        if (ret == 0)
            yspec1 = yspec;
        else{
            if (yspec1)
                ybc = YB_MODULE;
            else if (h == NULL)
                goto ok; /* treat as anydata */
            else{
                if ((ret = yang_schema_yanglib_parse_mount(h, xt)) < 0)
                    goto done;
                if (ret == 0)
                    goto ok;
                /* Try again */
                if ((ret = xml_yang_mount_get(h, xt, NULL, &yspec1)) < 0)
                    goto done;
                if (yspec1)
                    ybc = YB_MODULE;
                else
                    goto ok;
            }
        }
    }
    else
        yspec1 = yspec;
    xc = NULL;     /* Apply on children */
    while ((xc = xml_child_each(xt, xc, CX_ELMNT)) != NULL) {
        /* It is xml2ns in populate_self_parent that needs improvement */
        /* cache previous + prefix */
        name = xml_name(xc);
        prefix = xml_prefix(xc);
        if (yc0 != NULL &&
            clicon_strcmp(name0, name) == 0 &&
            clicon_strcmp(prefix0, prefix) == 0){
            if ((ret = xml_bind_yang0_opt(h, xc, ybc, yspec1, xc0, xerr)) < 0)
                goto done;
        }
        else if (xsibling &&
                 (xs = xml_find_type(xsibling, prefix, name, CX_ELMNT)) != NULL){
            if ((ret = xml_bind_yang0_opt(h, xc, ybc, yspec1, xs, xerr)) < 0)
                goto done;
        }
        else if ((ret = xml_bind_yang0_opt(h, xc, ybc, yspec1, NULL, xerr)) < 0)
            goto done;
        if (ret == 0)
            goto fail;
        xc0 = xc;
        yc0 = xml_spec(xc); /* cache */
        name0 = xml_name(xc);
        prefix0 = xml_prefix(xc);
    }
 ok:
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Find yang spec association of tree of XML nodes
 *
 * @param[in]   h      Clixon handle (sometimes NULL)
 * @param[in]   xt     XML tree node
 * @param[in]   yb     How to bind yang to XML top-level when parsing
 * @param[in]   yspec  Yang spec
 * @param[out]  xerr   Reason for failure, or NULL
 * @retval      1      OK yang assignment made
 * @retval      0      Partial or no yang assigment made (at least one failed) and xerr set
 * @retval     -1      Error
 * Populate xt as top-level node
 * @see xml_bind_yang  If only children of xt should be populated, not xt itself
 */
int
xml_bind_yang0(clicon_handle h,
               cxobj        *xt, 
               yang_bind     yb,
               yang_stmt    *yspec,
               cxobj       **xerr)
{
    int        retval = -1;
    cxobj     *xc;           /* xml child */
    int        ret;

    switch (yb){
    case YB_MODULE:
        if ((ret = populate_self_top(xt, yspec, xerr)) < 0) 
            goto done;
        break;
    case YB_PARENT:
        if ((ret = populate_self_parent(xt, NULL, yspec, xerr)) < 0)
            goto done;
        break;
    case YB_NONE:
        ret = 1;
        break;
    default:
        clicon_err(OE_XML, EINVAL, "Invalid yang binding: %d", yb);
        goto done;
        break;
    }
    if (ret == 0)
        goto fail;
    else if (ret == 2)     /* ret=2 for anyxml from parent^ */
        goto ok;
    strip_body_objects(xt);
    xc = NULL;     /* Apply on children */
    while ((xc = xml_child_each(xt, xc, CX_ELMNT)) != NULL) {
        if ((ret = xml_bind_yang0_opt(h, xc, YB_PARENT, yspec, NULL, xerr)) < 0)
            goto done;
        if (ret == 0)
            goto fail;
    }
 ok:
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! RPC-specific
 *
 * @param[in]   h      Clixon handle
 * @param[in]   xn     XML action node
 * @param[in]   yspec  Yang spec
 * @param[out]  xerr   Reason for failure, or NULL
 * @retval      1      OK yang assignment made
 * @retval      0      Partial or no yang assigment made (at least one failed) and xerr set
 * @retval     -1      Error
 */
static int
xml_bind_yang_rpc_rpc(clicon_handle h,
                      cxobj        *x,
                      yang_stmt    *yrpc,
                      char         *rpcname,
                      cxobj       **xerr)
{
    int        retval = -1;
    cbuf      *cb = NULL;
    char      *name;
    cxobj     *xc;
    yang_stmt *yi = NULL; /* input */
    int        ret;

    xml_spec_set(x, yrpc); /* required for validate */
    if ((yi = yang_find(yrpc, Y_INPUT, NULL)) == NULL){
        /* If no yang input spec but RPC has elements, return unknown element */
        if (xml_child_nr_type(x, CX_ELMNT) != 0){
            xc = xml_child_i_type(x, 0, CX_ELMNT); /* Pick first */
            name = xml_name(xc);
            if ((cb = cbuf_new()) == NULL){
                clicon_err(OE_UNIX, errno, "cbuf_new");
                goto done;
            }
            cprintf(cb, "Unrecognized parameter: %s in rpc: %s", name, rpcname);
            if (xerr &&
                netconf_unknown_element_xml(xerr, "application", name, cbuf_get(cb)) < 0)
                goto done;
            goto fail;
        }
    }
    else{
        /* xml_bind_yang need to have parent with yang spec for
         * recursive population to work. Therefore, assign input yang
         * to rpc level although not 100% intuitive */
        xml_spec_set(x, yi); 
        if ((ret = xml_bind_yang(h, x, YB_PARENT, NULL, xerr)) < 0)
            goto done;
        if (ret == 0)
            goto fail;
    }
    retval = 1;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Action-specific
 *
 * Find the innermost container or list containing an XML element that carries the name of the
 * defined action.
 * Only one action can be invoked in one rpc
 * @param[in]   h      Clixon handle
 * @param[in]   xn     XML action node
 * @param[in]   yspec  Yang spec
 * @param[out]  xerr   Reason for failure, or NULL
 * @retval      1      OK yang assignment made
 * @retval      0      Partial or no yang assigment made (at least one failed) and xerr set
 * @retval     -1      Error
 * XXX if not more action, consider folding into calling function
 */
static int
xml_bind_yang_rpc_action(clicon_handle h,
                         cxobj        *xn,
                         yang_stmt    *yspec,
                         cxobj       **xerr)
{
    int        retval = -1;
    int        ret;
    cxobj     *xi;
    yang_stmt *yi;;

    if ((ret = xml_bind_yang(h, xn, YB_MODULE, yspec, xerr)) < 0)
        goto done;
    if (ret == 0)
        goto fail;
    /* Special case: bind "action" node to module for validate code to work */
    if ((xi = xml_child_i_type(xn, 0, CX_ELMNT)) != NULL &&
        (yi = xml_spec(xi))){
        xml_spec_set(xn, ys_module(yi));
    }
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Find yang spec association of XML node for incoming RPC starting with <rpc>
 * 
 * Incoming RPC has an "input" structure that is not taken care of by xml_bind_yang
 * @param[in]   h      Clixon handle
 * @param[in]   xrpc   XML rpc node
 * @param[in]   yspec  Yang spec
 * @param[out]  xerr   Reason for failure, or NULL
 * @retval      1      OK yang assignment made
 * @retval      0      Partial or no yang assigment made (at least one failed) and xerr set
 * @retval     -1      Error
 * The 
 * @code
 *   if ((ret = xml_bind_yang_rpc(x, NULL, &xerr)) < 0)
 *      err;
 * @endcode
 * @see xml_bind_yang  For other generic cases
 * @see xml_bind_yang_rpc_reply 
 */
int
xml_bind_yang_rpc(clicon_handle h,
                  cxobj        *xrpc,
                  yang_stmt    *yspec,
                  cxobj       **xerr)
{
    int        retval = -1;
    yang_stmt *yrpc = NULL;    /* yang node */
    yang_stmt *ymod=NULL; /* yang module */
    cxobj     *x;
    int        ret;
    char      *opname;  /* top-level netconf operation */
    char      *rpcname; /* RPC name */
    char      *name;
    cxobj     *xc;
    
    opname = xml_name(xrpc);
    if ((strcmp(opname, "hello")) == 0){ 
        /* Hello: dont bind, dont appear in any yang spec, just ensure there is nothing apart from
         * session-id or capabilities/capability tags
         * If erro, just log, drop and close, rpc-error should not be sent since it is not rpc
         * Actually, there are no error replies to hello messages according to any RFC, so
         * rpc error reply here is non-standard, but may be useful.
         */
        x = NULL;
        while ((x = xml_child_each(xrpc, x, CX_ELMNT)) != NULL) {
            name = xml_name(x);
            if (strcmp(name, "session-id") == 0)
                continue;
            else if (strcmp(name, "capabilities") == 0){
                xc = NULL;
                while ((xc = xml_child_each(x, xc, CX_ELMNT)) != NULL) {
                    if (strcmp(xml_name(xc), "capability") != 0){
                        if (xerr &&
                            netconf_unknown_element_xml(xerr, "protocol", xml_name(xc),
                                                        "Unrecognized hello/capabilities element") < 0)
                            goto done;
                        goto fail;
                    }
                }
            }
            else {
                if (xerr &&
                    netconf_unknown_element_xml(xerr, "protocol", name, "Unrecognized hello element") < 0)
                    goto done;
                clicon_err(OE_XML, EFAULT, "Unrecognized hello element: %s", name);
                goto fail;
            }
        }
        goto ok;
    }
    else if ((strcmp(opname, "notification")) == 0)
        goto ok;
    else if ((strcmp(opname, "rpc")) == 0)
        ; /* continue */
    else {   /* Notify, rpc-reply? */
        if (xerr &&
            netconf_unknown_element_xml(xerr, "protocol", opname, "Unrecognized netconf operation") < 0)
            goto done;
        goto fail;
    }
    if (_netconf_message_id_optional == 0){
        /* RFC 6241 4.1:
         *    The <rpc> element has a mandatory attribute "message-id"
         */
        if (xml_find_type(xrpc, NULL, "message-id", CX_ATTR) == NULL){
            if (xerr &&
                netconf_missing_attribute_xml(xerr, "rpc", "message-id", "Incoming rpc") < 0)
                goto done;
            goto fail;
        }
    }
    x = NULL;
    while ((x = xml_child_each(xrpc, x, CX_ELMNT)) != NULL) {
        rpcname = xml_name(x);
        if ((ret = xml_rpc_isaction(x)) < 0)
            goto done;
        if (ret == 1){
            if ((ret = xml_bind_yang_rpc_action(h, x, yspec, xerr)) < 0)
                goto done;
            if (ret == 0)
                goto fail;
            goto ok;
        } /* if not action fall through */
        if (ys_module_by_xml(yspec, x, &ymod) < 0)
            goto done;
        if (ymod == NULL){
            if (xerr &&
                netconf_unknown_element_xml(xerr, "application", rpcname, "Unrecognized RPC (wrong namespace?)") < 0)
                goto done;
            goto fail;
        }
        if ((yrpc = yang_find(ymod, Y_RPC, rpcname)) == NULL){
            if (xerr &&
                netconf_unknown_element_xml(xerr, "application", rpcname, "Unrecognized RPC") < 0)
                goto done;
            goto fail;
        }
        if ((ret = xml_bind_yang_rpc_rpc(h, x, yrpc, rpcname, xerr)) < 0)
            goto done;
        if (ret == 0)
            goto fail;
    }
 ok:
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Find yang spec association of XML node for outgoing RPC starting with <rpc-reply>
 * 
 * Outgoing RPC has an "output" structure that is not taken care of by xml_bind_yang
 * @param[in]   h      Clixon handle (sometimes NULL)
 * @param[in]   xrpc   XML rpc node
 * @param[in]   name   Name of RPC (not seen in output/reply)
 * @param[in]   yspec  Yang spec
 * @param[out]  xerr   Reason for failure, or NULL
 * @retval      1      OK yang assignment made
 * @retval      0      Partial or no yang assigment made (at least one failed) and xerr set
 * @retval     -1      Error
 *
 * @code
 *   if ((ret = xml_bind_yang_rpc_reply(x, "get-config", yspec, &xerr)) < 0)
 *      err;
 * @endcode
 * @see xml_bind_yang  For other generic cases
 */
int
xml_bind_yang_rpc_reply(clicon_handle h,
                        cxobj        *xrpc,
                        char         *name,
                        yang_stmt    *yspec,
                        cxobj       **xerr)
{
    int        retval = -1;
    yang_stmt *yrpc = NULL;    /* yang node */
    yang_stmt *ymod=NULL;      /* yang module */
    yang_stmt *yo = NULL;      /* output */
    cxobj     *x;
    int        ret;
    cxobj     *xerr1 = NULL;
    char      *opname;
    cbuf      *cberr = NULL;
    cxobj     *xc;
    
    opname = xml_name(xrpc);
    if (strcmp(opname, "rpc-reply")){
        if ((cberr = cbuf_new()) == NULL){
            clicon_err(OE_UNIX, errno, "cbuf_new");
            goto done;
        }
        cprintf(cberr, "Internal error, unrecognized netconf operation in backend reply, expected rpc-reply but received: %s", opname);
        if (xerr && netconf_operation_failed_xml(xerr, "application", cbuf_get(cberr)) < 0)
            goto done;
        goto fail;
    }
    x = NULL;
    while ((x = xml_child_each(xrpc, x, CX_ELMNT)) != NULL) {
        if (ys_module_by_xml(yspec, x, &ymod) < 0)
            goto done;
        if (ymod == NULL)
            continue;
        if ((yrpc = yang_find(ymod, Y_RPC, name)) == NULL)
            continue;
        //      xml_spec_set(xrpc, yrpc);
        if ((yo = yang_find(yrpc, Y_OUTPUT, NULL)) == NULL)
            continue;
        /* xml_bind_yang need to have parent with yang spec for
         * recursive population to work. Therefore, assign input yang
         * to rpc level although not 100% intuitive */
        break;
    }
    if (yo != NULL){
        xml_spec_set(xrpc, yo); 
        /* Special case for ok and rpc-error */
        if ((xc = xml_child_i_type(xrpc, 0, CX_ELMNT)) != NULL &&
            (strcmp(xml_name(xc),"rpc-error") == 0
             || strcmp(xml_name(xc),"ok") == 0
             )){
            goto ok;
        }
        /* Use a temporary xml error tree since it is stringified in the original error on error */
        if ((ret = xml_bind_yang(h, xrpc, YB_PARENT, NULL, &xerr1)) < 0)
            goto done;
        if (ret == 0){
            if ((cberr = cbuf_new()) == NULL){
                clicon_err(OE_UNIX, errno, "cbuf_new");
                goto done;
            }
            cprintf(cberr, "Internal error in backend reply: ");
            if (netconf_err2cb(xerr1, cberr) < 0)
                goto done;
            if (xerr && netconf_operation_failed_xml(xerr, "application", cbuf_get(cberr)) < 0)
                goto done;
            goto fail;
        }
    }
 ok:
    retval = 1;
 done:
    if (cberr)
        cbuf_free(cberr);
    if (xerr1)
        xml_free(xerr1);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Special case explicit binding 
 */
int
xml_bind_special(cxobj     *xd,
                 yang_stmt *yspec,
                 char      *schema_nodeid)
{
    int        retval = -1;
    yang_stmt *yd;
    
    if (yang_abs_schema_nodeid(yspec, schema_nodeid, &yd) < 0)
        goto done;
    if (yd)
        xml_spec_set(xd, yd);
    retval = 0;
 done:
    return retval;
}
