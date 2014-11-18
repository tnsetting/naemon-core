#include <string.h>
#include "lib/libnaemon.h"
#include "nm_alloc.h"
#include "query-handler.h"
#include "qh-oconf.h"
#include "objects.h"

#define OCONF_ACTION_INSERT 0
#define OCONF_ACTION_CLONE  1
#define OCONF_ACTION_UPDATE 2

static int count_objectlist_items(struct objectlist *base)
{
	int count = 0;
	struct objectlist *list;

	for (list = base; list; list = list->next);
		count++;
	return count;
}

static void count_host_slaves(struct host *h, int *escalations, int *deps)
{
	(*escalations) += count_objectlist_items(h->escalation_list);
	(*deps) += count_objectlist_items(h->exec_deps);
	(*deps) += count_objectlist_items(h->notify_deps);
}

static void count_service_slaves(struct service *s, int *escalations, int *deps)
{
	(*escalations) += count_objectlist_items(s->escalation_list);
	(*deps) += count_objectlist_items(s->exec_deps);
	(*deps) += count_objectlist_items(s->notify_deps);
}

static contactgroupsmember *copy_cg_members(struct contactgroupsmember *orig)
{
	struct contactgroupsmember *ret = NULL;
	return ret;
}

static contactsmember *copy_contact_members(struct contactsmember *orig)
{
	struct contactsmember *ret = NULL;
	return ret;
}

static void *objlist_deepcopy_hostescalation(void *ptr)
{
	struct hostescalation *ret;
	ret = malloc(sizeof(*ret));
	memcpy(ret, ptr, sizeof(*ret));
	return ret;
}

static void *objlist_deepcopy_hostdependency(void *ptr)
{
	struct hostdependency *ret;
	ret = malloc(sizeof(*ret));
	memcpy(ret, ptr, sizeof(*ret));
	return ret;
}

static struct contact *clone_contact(struct contact *orig)
{
	struct contact *c;

	c = malloc(sizeof(*c));
	memcpy(c, orig, sizeof(*c));
	c->name = NULL;

	return c;
}

/** XXX FIXME TO NOT CRASH ON EXIT (deepcopy_objectlist etc) */
static struct host *clone_host(struct host *orig)
{
	struct host *h;

	h = malloc(sizeof(*h));
	memcpy(h, orig, sizeof(*h));
	h->name = NULL;
	h->contact_groups = copy_cg_members(orig->contact_groups);
	h->contacts = copy_contact_members(orig->contacts);
	h->escalation_list = deepcopy_objectlist(orig->escalation_list, objlist_deepcopy_hostescalation);
	h->exec_deps = deepcopy_objectlist(orig->exec_deps, objlist_deepcopy_hostdependency);
	h->notify_deps = deepcopy_objectlist(orig->notify_deps, objlist_deepcopy_hostdependency);

	return h;
}

static struct service *clone_service(struct service *orig)
{
	struct service *s;

	s = malloc(sizeof(*s));
	memcpy(s, orig, sizeof(*s));
	s->description = NULL;

	return s;
}

static int oconf_contact(int sd, int action, char *base, struct kvvec *kvv)
{
	int i = 0, offset = 0;
	struct contact *orig = NULL, *c = NULL;
	struct contact **replacement_ary;

	if (base && !(orig = find_contact(base))) {
		qh_error(sd, 404, "Unable to find contact '%s'", base);
		return 0;
	}

	if (action == OCONF_ACTION_CLONE) {
		c = clone_contact(orig);
	} else if (action == OCONF_ACTION_UPDATE) {
		c = orig;
	} else {
		c = nm_calloc(1, sizeof(*c));
	}

	for (i = offset; i < kvv->kv_pairs; i++) {
		struct key_value *kv = &kvv->kv[i];
		if (!strcmp(kv->key, "contact_name")) {
			c->name = strdup(kv->value);
		} else if (!strcmp(kv->key, "email")) {
			nm_free(c->email);
			c->email = strdup(kv->value);
		} else if (!strcmp(kv->key, "pager")) {
			nm_free(c->pager);
			c->pager = strdup(kv->value);
		}
	}

	if (action == OCONF_ACTION_INSERT) {
		if (!c->name) {
			qh_error(sd, 400, "No contact_name specified in variables");
			nm_free(c);
			return 0;
		}
		if (DKHASH_EDUPE == dkhash_insert(object_hash_tables[OBJTYPE_CONTACT], c->name, NULL, c)) {
			qh_error(sd, 400, "Contact '%s' already exists", c->name);
			nm_free(c);
		}
		replacement_ary = nm_malloc((num_objects.contacts + 2) * sizeof(*c));
		if (!replacement_ary) {
			nm_free(c);
			qh_error(sd, 500, "malloc()");
			return 0;
		}

		memcpy(replacement_ary, contact_ary, sizeof(*c) * (num_objects.contacts + 1));
		replacement_ary[num_objects.contacts - 1]->next = c;
		replacement_ary[num_objects.contacts] = c;
		contact_ary = replacement_ary;
		c->next = NULL;
		c->id = num_objects.contacts++;
	}

	return 0;
}

static int oconf_host(int sd, int action, char *base, struct kvvec *vars)
{
	struct host *orig = NULL, *h;
	int num_escalations = 0, num_deps = 0;
	int num_s_escalations = 0, num_s_deps = 0;
	int num_services = 0;
	struct servicesmember *sm;

	if (base) {
		if (!(orig = find_host(base))) {
			qh_error(sd, 404, "Unable to find host '%s'", base);
			return 0;
		}
	}

	if (action == OCONF_ACTION_CLONE) {
		h = clone_host(orig);
	} else if (action == OCONF_ACTION_UPDATE) {
		h = orig;
	} else {
		h = nm_calloc(1, sizeof(*h));
	}

	count_host_slaves(h, &num_escalations, &num_deps);
	for (sm = h->services; sm; sm = sm->next) {
		num_services++;
		count_service_slaves(sm->service_ptr, &num_s_escalations, &num_s_deps);
	}
	num_objects.services += num_services;
	num_objects.hostescalations += num_escalations;
	num_objects.hostdependencies += num_deps;
	num_objects.serviceescalations += num_s_escalations;
	num_objects.servicedependencies += num_s_deps;

	if (action == OCONF_ACTION_INSERT) {
		struct host **replacement_ary;

		if (!h->name) {
			qh_error(sd, 400, "No host_name specified in variables");
			nm_free(h);
			return 0;
		}
		if (DKHASH_EDUPE == dkhash_insert(object_hash_tables[OBJTYPE_CONTACT], h->name, NULL, h)) {
			qh_error(sd, 400, "Host '%s' already exists", h->name);
			nm_free(h);
		}
		replacement_ary = nm_malloc((num_objects.hosts + 2) * sizeof(*h));
		if (!replacement_ary) {
			nm_free(h);
			qh_error(sd, 500, "malloc()");
			return 0;
		}

		memcpy(replacement_ary, host_ary, sizeof(*h) * (num_objects.hosts + 1));
		replacement_ary[num_objects.hosts - 1]->next = h;
		replacement_ary[num_objects.hosts] = h;
		host_ary = replacement_ary;
		h->next = NULL;
		h->id = num_objects.contacts++;
	}
	post_process_hosts();
	post_process_services();

	return 0;
}

static int oconf_service(int sd, int action, char *base, struct kvvec *vars)
{
	struct service *orig, *s;

	if (base) {
		char *host_name, *description;

		description = strchr(base, ';');
		if (!description) {
			qh_error(sd, 400, "Service names must be given in the form of 'host_name;service_description'");
			return 0;
		}
		host_name = base;
		*(description)++ = 0;
		if (!(orig = find_service(host_name, description))) {
			qh_error(sd, 404, "Unable to find service '%s' on host '%s' to clone",
			         host_name, description);
			return 0;
		}
	}

	if (action == OCONF_ACTION_CLONE) {
		s = clone_service(orig);
	} else if (action == OCONF_ACTION_UPDATE) {
		s = orig;
	} else {
		s = nm_calloc(1, sizeof(*s));
	}

	post_process_services();
	return 0;
}

int oconf_qhandler(int sd, char *buf, unsigned int len)
{
	char *action, *table = NULL, *base = NULL, *vars;
	int action_id, ret;
	struct kvvec *kvv = NULL;

	if (!*buf || !strcmp(buf, "help")) {
		nsock_printf_nul
			(sd,
				"Query handler for object configuration\n"
				"Syntax:\n"
				"  <action> <objecttype> <var1=key1>;<var2;key2><varN;keyN>\n"
			);
		return 0;
	}

	nsock_printf(sd, "buf=%s\n", buf);
	action = buf;
	while (*action == ' ')
		action++;

	if (!*action) {
		qh_error(sd, 500, "No action provided for oconf handler");
		return 0;
	}

	if ((table = strchr(action, ' '))) {
		*(table++) = 0;
		if (!strncasecmp(table, "table", 5)) {
			table += 6;
		}
	} else {
		qh_error(sd, 500, "No object type provided for oconf handler");
		return 0;
	}

	if (!strcasecmp(action, "clone")) {
		action_id = OCONF_ACTION_CLONE;
	} else if (!strcasecmp(action, "insert")) {
		action_id = OCONF_ACTION_INSERT;
	} else if (!strcasecmp(action, "update")) {
		action_id = OCONF_ACTION_UPDATE;
	} else if (!strcasecmp(action, "delete")) {
		qh_error(sd, 501, "Action '%s' is not implemented", action);
		return 0;
	} else {
		qh_error(sd, 400, "No action '%s' available (or planned)", action);
		return 0;
	}

	if (action_id == OCONF_ACTION_UPDATE || action_id == OCONF_ACTION_CLONE) {
		if ((base = strchr(table, ' '))) {
			*(base++) = 0;
		} else {
			qh_error(sd, 500, "No object selected to clone");
			return 0;
		}

		/* single- or double quotes escapes object names */
		if (*base == '\'') {
			vars = strchr(base + 1, '\'');
			*(base++) = 0;
			*(vars++) = 0;
		} else if (*base == '"') {
			vars = strchr(base + 1, '"');
			*(base++) = 0;
			*(vars++) = 0;
		} else {
			vars = strchr(base, ' ');
		}
	} else {
		vars = strchr(table, ' ');
	}

	if (!vars) {
		qh_error(sd, 400, "No variables specified for oconf action");
		return 0;
	}

	*(vars++) = 0;
	nsock_printf(sd, "Applying action '%s' to %s object '%s'\nVARS:\n%s\n", action, table, base, vars);
	kvv = buf2kvvec(vars, strlen(vars), '=', ';', KVVEC_ASSIGN);

	if (!strcasecmp(table, "host")) {
		ret = oconf_host(sd, action_id, base, kvv);
	} else if (!strcasecmp(table, "service")) {
		ret = oconf_service(sd, action_id, base, kvv);
	} else if (!strcasecmp(table, "contact")) {
		ret = oconf_contact(sd, action_id, base, kvv);
	} else {
		qh_error(sd, 500, "Unknown table: '%s'", table);
	}

	kvvec_destroy(kvv, 0);

	return ret;
}
