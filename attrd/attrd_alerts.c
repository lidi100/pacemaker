/*
 * Copyright (C) 2015 Andrew Beekhof <andrew@beekhof.net>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */
#include <crm_internal.h>
#include <crm/crm.h>
#include <crm/cib/internal.h>
#include <crm/msg_xml.h>
#include <crm/cluster/internal.h>
#include <crm/cluster/election.h>
#include <internal.h>
#include "attrd_alerts.h"
#include <crm/common/alerts_internal.h>
#include <crm/common/iso8601_internal.h>
#include <crm/pengine/rules_internal.h>
#include <crm/lrmd_alerts_internal.h>

GHashTable *alert_info_cache = NULL;

lrmd_t *
attrd_lrmd_connect(int max_retry, void callback(lrmd_event_data_t * op))
{
    int ret = -ENOTCONN;
    int fails = 0;

    if (!the_lrmd) {
        the_lrmd = lrmd_api_new();
    }

    while(fails < max_retry) {
        the_lrmd->cmds->set_callback(the_lrmd, callback);

        ret = the_lrmd->cmds->connect(the_lrmd, T_ATTRD, NULL);
        if (ret != pcmk_ok) {
            fails++;
            crm_trace("lrmd_connect RETRY!(%d)", fails);
        } else {
            crm_trace("lrmd_connect OK!");
            break;
        }
    }

    if (ret != pcmk_ok) {
        if (the_lrmd->cmds->is_connected(the_lrmd)) {
            lrmd_api_delete(the_lrmd);
        }
        the_lrmd = NULL;
    }
    return the_lrmd;
}

static void
config_query_callback(xmlNode * msg, int call_id, int rc, xmlNode * output, void *user_data)
{
    crm_time_t *now = crm_time_new(NULL);
    xmlNode *crmalerts = NULL;

    if (rc != pcmk_ok) {
        crm_err("Local CIB query resulted in an error: %s", pcmk_strerror(rc));
        goto bail;
    }

    crmalerts = output;
    if ((crmalerts) &&
        (crm_element_name(crmalerts)) &&
        (strcmp(crm_element_name(crmalerts), XML_CIB_TAG_ALERTS) != 0)) {
        crmalerts = first_named_child(crmalerts, XML_CIB_TAG_ALERTS);
    }
    if (!crmalerts) {
        crm_err("Local CIB query for " XML_CIB_TAG_ALERTS " section failed");
        goto bail;
    }

    pe_unpack_alerts(crmalerts);

  bail:
    crm_time_free(now);
}

gboolean
attrd_read_options(gpointer user_data)
{
    int call_id;
    
    if (the_cib) {
        call_id = the_cib->cmds->query(the_cib,
            "//" XML_CIB_TAG_ALERTS,
            NULL, cib_xpath | cib_scope_local);

        the_cib->cmds->register_callback_full(the_cib, call_id, 120, FALSE,
                                              NULL,
                                              "config_query_callback",
                                              config_query_callback, free);

        crm_trace("Querying the CIB... call %d", call_id);
    } else {
        crm_err("Querying the CIB...CIB connection not active");
    }
    return TRUE;
}

void
attrd_cib_updated_cb(const char *event, xmlNode * msg)
{
    int rc = -1;
    int format= 1;
    xmlNode *patchset = get_message_xml(msg, F_CIB_UPDATE_RESULT);
    xmlNode *change = NULL;
    xmlXPathObject *xpathObj = NULL;

    CRM_CHECK(msg != NULL, return);

    crm_element_value_int(msg, F_CIB_RC, &rc);
    if (rc < pcmk_ok) {
        crm_trace("Filter rc=%d (%s)", rc, pcmk_strerror(rc));
        return;
    }

    crm_element_value_int(patchset, "format", &format);
    if (format == 1) {
        if ((xpathObj = xpath_search(
                 msg,
                 "//" F_CIB_UPDATE_RESULT "//" XML_TAG_DIFF_ADDED "//" XML_CIB_TAG_ALERTS
                 )) != NULL) {
            freeXpathObject(xpathObj);
            mainloop_set_trigger(attrd_config_read);
        }
    } else if (format == 2) {
        for (change = __xml_first_child(patchset); change != NULL; change = __xml_next(change)) {
            const char *xpath = crm_element_value(change, XML_DIFF_PATH);

            if (xpath == NULL) {
                continue;
            }

            /* modifying properties */
            if (!strstr(xpath, "/" XML_TAG_CIB "/" XML_CIB_TAG_CONFIGURATION "/" XML_CIB_TAG_ALERTS)) {
                xmlNode *section = NULL;
                const char *name = NULL;

                /* adding notifications section */
                if ((strcmp(xpath, "/" XML_TAG_CIB "/" XML_CIB_TAG_CONFIGURATION) != 0) ||
                    ((section = __xml_first_child(change)) == NULL) ||
                    ((name = crm_element_name(section)) == NULL) ||
                    (strcmp(name, XML_CIB_TAG_ALERTS) != 0)) {
                    continue;
                }
            }

            mainloop_set_trigger(attrd_config_read);
            break;
        }

    } else {
        crm_warn("Unknown patch format: %d", format);
    }

}

void 
attrd_alert_fini()
{
    if (alert_info_cache) {
        g_hash_table_destroy(alert_info_cache);
        alert_info_cache = NULL;
    }

    if (crm_alert_kind_default) {
       g_strfreev(crm_alert_kind_default);
       crm_alert_kind_default = NULL;
    }
}

static int 
exec_alerts(lrmd_t *lrmd, const char *kind, const char *attribute_name, lrmd_key_value_t * params, GListPtr alert_list, GHashTable *info_cache)
{
    int call_id = 0;
    static int operations = 0;
    GListPtr l;
    crm_time_hr_t *now = crm_time_hr_new(NULL);
    
    params = lrmd_set_alert_key_to_lrmd_params(params, CRM_alert_kind, kind);
    params = lrmd_set_alert_key_to_lrmd_params(params, CRM_alert_version, VERSION);

    for (l = g_list_first(alert_list); l; l = g_list_next(l)) {
        lrmd_rsc_info_t *rsc = NULL;
        crm_alert_entry_t *entry = (crm_alert_entry_t *)(l->data);
        char *timestamp = crm_time_format_hr(entry->tstamp_format, now);
        lrmd_key_value_t * copy_params = NULL;
        lrmd_key_value_t *head, *p;

        if (crm_is_target_alert(entry->select_kind == NULL ? crm_alert_kind_default : entry->select_kind, kind) == FALSE) {
            crm_trace("Cannot sending '%s' alert to '%s' via '%s'(select_kind=%s)", kind, entry->recipient, entry->path, 
                entry->select_kind == NULL ? CRM_ALERT_KIND_DEFAULT : entry->select_kind_orig);
            free(timestamp);
            continue;
        }

        if (crm_is_target_alert(entry->select_attribute_name, attribute_name) == FALSE) {
            crm_trace("Cannot sending '%s' alert to '%s' via '%s'(select_attribute_name=%s attribute_name=%s)", kind, entry->recipient, entry->path, 
                entry->select_attribute_name_orig, attribute_name);
            free(timestamp);
            continue;
        }

        crm_info("Sending '%s' alert to '%s' via '%s'", kind, entry->recipient, entry->path);

        rsc = g_hash_table_lookup(alert_info_cache, entry->id);
        if (rsc == NULL) {
            rsc = lrmd->cmds->get_rsc_info(lrmd, entry->id, 0);
            if (!rsc) {
                lrmd->cmds->register_rsc(lrmd, entry->id, PCMK_ALERT_CLASS,  "pacemaker", entry->path, lrmd_opt_drop_recurring);
                rsc = lrmd->cmds->get_rsc_info(lrmd, entry->id, 0);
                if (!rsc) {
                    crm_err("Could not add alert %s : %s", entry->id, entry->path);
                    return -1; 
                }
                /* cache the result */
                g_hash_table_insert(alert_info_cache, entry->id, rsc);
            }
        }

        /* Because there is a parameter to turn into every transmission, Copy a parameter. */
        head = params;
        while (head) {
            p = head->next;
            copy_params = lrmd_key_value_add(copy_params, head->key, head->value);
            head = p;
        }

        operations++;

        copy_params = lrmd_key_value_add(copy_params, CRM_ALERT_KEY_PATH, entry->path);
        copy_params = lrmd_set_alert_key_to_lrmd_params(copy_params, CRM_alert_recipient, entry->recipient);
        copy_params = lrmd_set_alert_key_to_lrmd_params(copy_params, CRM_alert_node_sequence, crm_itoa(operations));
        copy_params = lrmd_set_alert_key_to_lrmd_params(copy_params, CRM_alert_timestamp, timestamp);

        lrmd_set_alert_envvar_to_lrmd_params(copy_params, entry);
        
        call_id = lrmd->cmds->exec_alert(lrmd, strdup(entry->id), entry->timeout, lrmd_opt_notify_orig_only, copy_params);
        if (call_id <= 0) {
            crm_err("Operation %s on %s failed: %d", "start", rsc->id, call_id);
        } else {
            crm_info("Operation %s on %s compete: %d", "start", rsc->id, call_id);
        }

        free(timestamp);
    }

    if (now) {
        free(now);
    }

    return call_id;
}

static void
free_alert_info(gpointer value)
{
    lrmd_rsc_info_t *rsc_info = value;

    lrmd_free_rsc_info(rsc_info);
}

static void
attrd_alert_lrm_op_callback(lrmd_event_data_t * op)
{
    CRM_CHECK(op != NULL, return);

    if (op->type == lrmd_event_disconnect) {
        crm_info("Lost connection to LRMD service!");
        if (the_lrmd->cmds->is_connected(the_lrmd)) {
            the_lrmd->cmds->disconnect(the_lrmd);
            lrmd_api_delete(the_lrmd);
        }
        the_lrmd = NULL;
        return;
    } else if (op->type != lrmd_event_exec_complete) {
        return;
    }

    if (op->params != NULL) {
        void *value_tmp1, *value_tmp2;

        value_tmp1 = g_hash_table_lookup(op->params, CRM_ALERT_KEY_PATH);
        if (value_tmp1 != NULL) {
            value_tmp2 = g_hash_table_lookup(op->params, CRM_ALERT_NODE_SEQUENCE);
            if(op->rc == 0) {
                crm_info("Alert %s (%s) complete", value_tmp2, value_tmp1);
            } else {
                crm_warn("Alert %s (%s) failed: %d", value_tmp2, value_tmp1, op->rc);
            }
        }
    }
}

int 
attrd_send_alerts(lrmd_t *lrmd, const char *node, uint32_t nodeid, const char *attribute_name, const char *attribute_value, GListPtr alert_list)
{
    int ret = pcmk_ok;
    lrmd_key_value_t *params = NULL;

    if (lrmd == NULL) {
        lrmd = attrd_lrmd_connect(10, attrd_alert_lrm_op_callback);
        if (lrmd == NULL) {
            crm_warn("LRMD connection not active");
            return ret;
        }
    }

    crm_trace("LRMD connection active");

    if (alert_info_cache == NULL) {
        alert_info_cache = g_hash_table_new_full(crm_str_hash,
                                                g_str_equal, NULL, free_alert_info);
    }
   
    params = lrmd_set_alert_key_to_lrmd_params(params, CRM_alert_node, node);
    params = lrmd_set_alert_key_to_lrmd_params(params, CRM_alert_nodeid, crm_itoa(nodeid));
    params = lrmd_set_alert_key_to_lrmd_params(params, CRM_alert_attribute_name, attribute_name);
    params = lrmd_set_alert_key_to_lrmd_params(params, CRM_alert_attribute_value, attribute_value == NULL ? "null" : attribute_value);

    ret = exec_alerts(lrmd, "attribute", attribute_name, params, alert_list, alert_info_cache); 
    crm_trace("ret : %d, node : %s, nodeid: %s,  name: %s, value : %s", 
                  ret, node, crm_itoa(nodeid), attribute_name, attribute_value); 

    if (params) {
        lrmd_key_value_freeall(params);
    }

    return ret;
}

#if HAVE_ATOMIC_ATTRD
void
set_alert_attribute_value(GHashTable *t, attribute_value_t *v)
{
    attribute_value_t *a_v = NULL;
    a_v = calloc(1, sizeof(attribute_value_t));
    CRM_ASSERT(a_v != NULL);

    a_v->nodeid = v->nodeid;
    a_v->nodename = strdup(v->nodename);

    if (v->current != NULL) {
        a_v->current = strdup(v->current);
    }

    g_hash_table_replace(t, a_v->nodename, a_v);
}

void
send_alert_attributes_value(attribute_t *a, GHashTable *t)
{
    int call_id = 0;
    attribute_value_t *at = NULL;
    GHashTableIter vIter;

    g_hash_table_iter_init(&vIter, t);

    while (g_hash_table_iter_next(&vIter, NULL, (gpointer *) & at)) {
        call_id = attrd_send_alerts(the_lrmd, at->nodename, at->nodeid, a->id, at->current, crm_alert_list);
        crm_trace("call_id : %d, nodename : %s, nodeid: %d,  name: %s, value : %s", 
                  call_id, at->nodename, at->nodeid, a->id, at->current); 
    }
}
#endif
