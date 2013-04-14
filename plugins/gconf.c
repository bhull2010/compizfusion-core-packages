/*
 * Copyright © 2005 Novell, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without
 * fee, provided that the above copyright notice appear in all copies
 * and that both that copyright notice and this permission notice
 * appear in supporting documentation, and that the name of
 * Novell, Inc. not be used in advertising or publicity pertaining to
 * distribution of the software without specific, written prior permission.
 * Novell, Inc. makes no representations about the suitability of this
 * software for any purpose. It is provided "as is" without express or
 * implied warranty.
 *
 * NOVELL, INC. DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN
 * NO EVENT SHALL NOVELL, INC. BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION
 * WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Author: David Reveman <davidr@novell.com>
 */

#define _GNU_SOURCE
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include <glib.h>
#include <glib/gprintf.h>
#include <mateconf/mateconf-client.h>

#include <compiz-core.h>

static CompMetadata mateconfMetadata;

#define APP_NAME "compiz"

/* From mateconf-internal.h. Bleah. */
int mateconf_value_compare (const MateConfValue *value_a,
			 const MateConfValue *value_b);

static int corePrivateIndex;

typedef struct _MateConfCore {
    MateConfClient *client;
    guint	cnxn;

    CompTimeoutHandle reloadHandle;

    InitPluginForObjectProc initPluginForObject;
    SetOptionForPluginProc  setOptionForPlugin;
} MateConfCore;

#define GET_MATECONF_CORE(c)				     \
    ((MateConfCore *) (c)->base.privates[corePrivateIndex].ptr)

#define MATECONF_CORE(c)		       \
    MateConfCore *gc = GET_MATECONF_CORE (c)


static gchar *
mateconfGetKey (CompObject  *object,
	     const gchar *plugin,
	     const gchar *option)
{
    const gchar *type;
    gchar	*key, *name, *objectName;

    type = compObjectTypeName (object->type);
    if (strcmp (type, "display") == 0)
	type = "allscreens";

    name = compObjectName (object);
    if (name)
    {
	objectName = g_strdup_printf ("%s%s", type, name);
	free (name);
    }
    else
	objectName = g_strdup (type);

    if (strcmp (plugin, "core") == 0)
	key = g_strjoin ("/", "/apps", APP_NAME, "general", objectName,
			 "options", option, NULL);
    else
	key = g_strjoin ("/", "/apps", APP_NAME, "plugins", plugin, objectName,
			 "options", option, NULL);

    g_free (objectName);

    return key;
}

static MateConfValueType
mateconfTypeFromCompType (CompOptionType type)
{
    switch (type) {
    case CompOptionTypeBool:
    case CompOptionTypeBell:
	return MATECONF_VALUE_BOOL;
    case CompOptionTypeInt:
	return MATECONF_VALUE_INT;
    case CompOptionTypeFloat:
	return MATECONF_VALUE_FLOAT;
    case CompOptionTypeString:
    case CompOptionTypeColor:
    case CompOptionTypeKey:
    case CompOptionTypeButton:
    case CompOptionTypeEdge:
    case CompOptionTypeMatch:
	return MATECONF_VALUE_STRING;
    case CompOptionTypeList:
	return MATECONF_VALUE_LIST;
    default:
	break;
    }

    return MATECONF_VALUE_INVALID;
}

static void
mateconfSetValue (CompObject      *object,
	       CompOptionValue *value,
	       CompOptionType  type,
	       MateConfValue      *gvalue)
{
    switch (type) {
    case CompOptionTypeBool:
	mateconf_value_set_bool (gvalue, value->b);
	break;
    case CompOptionTypeInt:
	mateconf_value_set_int (gvalue, value->i);
	break;
    case CompOptionTypeFloat:
	mateconf_value_set_float (gvalue, value->f);
	break;
    case CompOptionTypeString:
	mateconf_value_set_string (gvalue, value->s);
	break;
    case CompOptionTypeColor: {
	gchar *color;

	color = colorToString (value->c);
	mateconf_value_set_string (gvalue, color);

	free (color);
    } break;
    case CompOptionTypeKey: {
	gchar *action;

	while (object && object->type != COMP_OBJECT_TYPE_DISPLAY)
	    object = object->parent;

	if (!object)
	    return;

	action = keyActionToString (GET_CORE_DISPLAY (object), &value->action);
	mateconf_value_set_string (gvalue, action);

	free (action);
    } break;
    case CompOptionTypeButton: {
	gchar *action;

	while (object && object->type != COMP_OBJECT_TYPE_DISPLAY)
	    object = object->parent;

	if (!object)
	    return;

	action = buttonActionToString (GET_CORE_DISPLAY (object),
				       &value->action);
	mateconf_value_set_string (gvalue, action);

	free (action);
    } break;
    case CompOptionTypeEdge: {
	gchar *edge;

	edge = edgeMaskToString (value->action.edgeMask);
	mateconf_value_set_string (gvalue, edge);

	free (edge);
    } break;
    case CompOptionTypeBell:
	mateconf_value_set_bool (gvalue, value->action.bell);
	break;
    case CompOptionTypeMatch: {
	gchar *match;

	match = matchToString (&value->match);
	mateconf_value_set_string (gvalue, match);

	free (match);
    } break;
    default:
	break;
    }
}

static void
mateconfSetOption (CompObject  *object,
		CompOption  *o,
		const gchar *plugin)
{
    MateConfValueType type = mateconfTypeFromCompType (o->type);
    MateConfValue     *gvalue, *existingValue = NULL;
    gchar          *key;

    MATECONF_CORE (&core);

    if (type == MATECONF_VALUE_INVALID)
	return;

    key = mateconfGetKey (object, plugin, o->name);

    existingValue = mateconf_client_get (gc->client, key, NULL);
    gvalue = mateconf_value_new (type);

    if (o->type == CompOptionTypeList)
    {
	GSList     *node, *list = NULL;
	MateConfValue *gv;
	int	   i;

	type = mateconfTypeFromCompType (o->value.list.type);

	for (i = 0; i < o->value.list.nValue; i++)
	{
	    gv = mateconf_value_new (type);
	    mateconfSetValue (object, &o->value.list.value[i],
			   o->value.list.type, gv);
	    list = g_slist_append (list, gv);
	}

	mateconf_value_set_list_type (gvalue, type);
	mateconf_value_set_list (gvalue, list);

	if (!existingValue || mateconf_value_compare (existingValue, gvalue))
	    mateconf_client_set (gc->client, key, gvalue, NULL);

	for (node = list; node; node = node->next)
	    mateconf_value_free ((MateConfValue *) node->data);

	g_slist_free (list);
    }
    else
    {
	mateconfSetValue (object, &o->value, o->type, gvalue);

	if (!existingValue || mateconf_value_compare (existingValue, gvalue))
	    mateconf_client_set (gc->client, key, gvalue, NULL);
    }

    mateconf_value_free (gvalue);

    if (existingValue)
	mateconf_value_free (existingValue);

    g_free (key);
}

static Bool
mateconfGetValue (CompObject      *object,
	       CompOptionValue *value,
	       CompOptionType  type,
	       MateConfValue      *gvalue)

{
    if (type         == CompOptionTypeBool &&
	gvalue->type == MATECONF_VALUE_BOOL)
    {
	value->b = mateconf_value_get_bool (gvalue);
	return TRUE;
    }
    else if (type         == CompOptionTypeInt &&
	     gvalue->type == MATECONF_VALUE_INT)
    {
	value->i = mateconf_value_get_int (gvalue);
	return TRUE;
    }
    else if (type         == CompOptionTypeFloat &&
	     gvalue->type == MATECONF_VALUE_FLOAT)
    {
	value->f = mateconf_value_get_float (gvalue);
	return TRUE;
    }
    else if (type         == CompOptionTypeString &&
	     gvalue->type == MATECONF_VALUE_STRING)
    {
	const char *str;

	str = mateconf_value_get_string (gvalue);
	if (str)
	{
	    value->s = strdup (str);
	    if (value->s)
		return TRUE;
	}
    }
    else if (type         == CompOptionTypeColor &&
	     gvalue->type == MATECONF_VALUE_STRING)
    {
	const gchar *color;

	color = mateconf_value_get_string (gvalue);

	if (stringToColor (color, value->c))
	    return TRUE;
    }
    else if (type         == CompOptionTypeKey &&
	     gvalue->type == MATECONF_VALUE_STRING)
    {
	const gchar *action;

	action = mateconf_value_get_string (gvalue);

	while (object && object->type != COMP_OBJECT_TYPE_DISPLAY)
	    object = object->parent;

	if (!object)
	    return FALSE;

	stringToKeyAction (GET_CORE_DISPLAY (object), action, &value->action);
	return TRUE;
    }
    else if (type         == CompOptionTypeButton &&
	     gvalue->type == MATECONF_VALUE_STRING)
    {
	const gchar *action;

	action = mateconf_value_get_string (gvalue);

	while (object && object->type != COMP_OBJECT_TYPE_DISPLAY)
	    object = object->parent;

	if (!object)
	    return FALSE;

	stringToButtonAction (GET_CORE_DISPLAY (object), action,
			      &value->action);
	return TRUE;
    }
    else if (type         == CompOptionTypeEdge &&
	     gvalue->type == MATECONF_VALUE_STRING)
    {
	const gchar *edge;

	edge = mateconf_value_get_string (gvalue);

	value->action.edgeMask = stringToEdgeMask (edge);
	return TRUE;
    }
    else if (type         == CompOptionTypeBell &&
	     gvalue->type == MATECONF_VALUE_BOOL)
    {
	value->action.bell = mateconf_value_get_bool (gvalue);
	return TRUE;
    }
    else if (type         == CompOptionTypeMatch &&
	     gvalue->type == MATECONF_VALUE_STRING)
    {
	const gchar *match;

	match = mateconf_value_get_string (gvalue);

	matchInit (&value->match);
	matchAddFromString (&value->match, match);
	return TRUE;
    }

    return FALSE;
}

static Bool
mateconfReadOptionValue (CompObject      *object,
		      MateConfEntry      *entry,
		      CompOption      *o,
		      CompOptionValue *value)
{
    MateConfValue *gvalue;

    gvalue = mateconf_entry_get_value (entry);
    if (!gvalue)
	return FALSE;

    compInitOptionValue (value);

    if (o->type      == CompOptionTypeList &&
	gvalue->type == MATECONF_VALUE_LIST)
    {
	MateConfValueType type;
	GSList	       *list;
	int	       i, n;

	type = mateconf_value_get_list_type (gvalue);
	if (mateconfTypeFromCompType (o->value.list.type) != type)
	    return FALSE;

	list = mateconf_value_get_list (gvalue);
	n    = g_slist_length (list);

	value->list.value  = NULL;
	value->list.nValue = 0;
	value->list.type   = o->value.list.type;

	if (n)
	{
	    value->list.value = malloc (sizeof (CompOptionValue) * n);
	    if (value->list.value)
	    {
		for (i = 0; i < n; i++)
		{
		    if (!mateconfGetValue (object,
					&value->list.value[i],
					o->value.list.type,
					(MateConfValue *) list->data))
			break;

		    value->list.nValue++;

		    list = g_slist_next (list);
		}

		if (value->list.nValue != n)
		{
		    compFiniOptionValue (value, o->type);
		    return FALSE;
		}
	    }
	}
    }
    else
    {
	if (!mateconfGetValue (object, value, o->type, gvalue))
	    return FALSE;
    }

    return TRUE;
}

static void
mateconfGetOption (CompObject *object,
		CompOption *o,
		const char *plugin)
{
    MateConfEntry *entry;
    gchar      *key;

    MATECONF_CORE (&core);

    key = mateconfGetKey (object, plugin, o->name);

    entry = mateconf_client_get_entry (gc->client, key, NULL, TRUE, NULL);
    if (entry)
    {
	CompOptionValue value;

	if (mateconfReadOptionValue (object, entry, o, &value))
	{
	    (*core.setOptionForPlugin) (object, plugin, o->name, &value);
	    compFiniOptionValue (&value, o->type);
	}
	else
	{
	    mateconfSetOption (object, o, plugin);
	}

	mateconf_entry_free (entry);
    }

    g_free (key);
}

static CompBool
mateconfReloadObjectTree (CompObject *object,
			 void       *closure);

static CompBool
mateconfReloadObjectsWithType (CompObjectType type,
			      CompObject     *parent,
			      void	     *closure)
{
    compObjectForEach (parent, type, mateconfReloadObjectTree, closure);

    return TRUE;
}

static CompBool
mateconfReloadObjectTree (CompObject *object,
		       void       *closure)
{
    CompPlugin *p = (CompPlugin *) closure;
    CompOption  *option;
    int		nOption;

    option = (*p->vTable->getObjectOptions) (p, object, &nOption);
    while (nOption--)
	mateconfGetOption (object, option++, p->vTable->name);

    compObjectForEachType (object, mateconfReloadObjectsWithType, closure);

    return TRUE;
}

static Bool
mateconfReload (void *closure)
{
    CompPlugin  *p;

    MATECONF_CORE (&core);

    for (p = getPlugins (); p; p = p->next)
    {
	if (!p->vTable->getObjectOptions)
	    continue;

	mateconfReloadObjectTree (&core.base, (void *) p);
    }

    gc->reloadHandle = 0;

    return FALSE;
}

static Bool
mateconfSetOptionForPlugin (CompObject      *object,
			 const char	 *plugin,
			 const char	 *name,
			 CompOptionValue *value)
{
    CompBool status;

    MATECONF_CORE (&core);

    UNWRAP (gc, &core, setOptionForPlugin);
    status = (*core.setOptionForPlugin) (object, plugin, name, value);
    WRAP (gc, &core, setOptionForPlugin, mateconfSetOptionForPlugin);

    if (status && !gc->reloadHandle)
    {
	CompPlugin *p;

	p = findActivePlugin (plugin);
	if (p && p->vTable->getObjectOptions)
	{
	    CompOption *option;
	    int	       nOption;

	    option = (*p->vTable->getObjectOptions) (p, object, &nOption);
	    option = compFindOption (option, nOption, name, 0);
	    if (option)
		mateconfSetOption (object, option, p->vTable->name);
	}
    }

    return status;
}

static CompBool
mateconfInitPluginForObject (CompPlugin *p,
			  CompObject *o)
{
    CompBool status;

    MATECONF_CORE (&core);

    UNWRAP (gc, &core, initPluginForObject);
    status = (*core.initPluginForObject) (p, o);
    WRAP (gc, &core, initPluginForObject, mateconfInitPluginForObject);

    if (status && p->vTable->getObjectOptions)
    {
	CompOption *option;
	int	   nOption;

	option = (*p->vTable->getObjectOptions) (p, o, &nOption);
	while (nOption--)
	    mateconfGetOption (o, option++, p->vTable->name);
    }

    return status;
}

/* MULTIDPYERROR: only works with one or less displays present */
static void
mateconfKeyChanged (MateConfClient *client,
		 guint	     cnxn_id,
		 MateConfEntry  *entry,
		 gpointer    user_data)
{
    CompPlugin *plugin;
    CompObject *object;
    CompOption *option = NULL;
    int	       nOption = 0;
    gchar      **token;
    int	       objectIndex = 4;

    token = g_strsplit (entry->key, "/", 8);

    if (g_strv_length (token) < 7)
    {
	g_strfreev (token);
	return;
    }

    if (strcmp (token[0], "")	    != 0 ||
	strcmp (token[1], "apps")   != 0 ||
	strcmp (token[2], APP_NAME) != 0)
    {
	g_strfreev (token);
	return;
    }

    if (strcmp (token[3], "general") == 0)
    {
	plugin = findActivePlugin ("core");
    }
    else
    {
	if (strcmp (token[3], "plugins") != 0 || g_strv_length (token) < 8)
	{
	    g_strfreev (token);
	    return;
	}

	objectIndex = 5;
	plugin = findActivePlugin (token[4]);
    }

    if (!plugin)
    {
	g_strfreev (token);
	return;
    }

    object = compObjectFind (&core.base, COMP_OBJECT_TYPE_DISPLAY, NULL);
    if (!object)
    {
	g_strfreev (token);
	return;
    }

    if (strncmp (token[objectIndex], "screen", 6) == 0)
    {
	object = compObjectFind (object, COMP_OBJECT_TYPE_SCREEN,
				 token[objectIndex] + 6);
	if (!object)
	{
	    g_strfreev (token);
	    return;
	}
    }
    else if (strcmp (token[objectIndex], "allscreens") != 0)
    {
	g_strfreev (token);
	return;
    }

    if (strcmp (token[objectIndex + 1], "options") != 0)
    {
	g_strfreev (token);
	return;
    }

    if (plugin->vTable->getObjectOptions)
	option = (*plugin->vTable->getObjectOptions) (plugin, object,
						      &nOption);

    option = compFindOption (option, nOption, token[objectIndex + 2], 0);
    if (option)
    {
	CompOptionValue value;

	if (mateconfReadOptionValue (object, entry, option, &value))
	{
	    (*core.setOptionForPlugin) (object,
					plugin->vTable->name,
					option->name,
					&value);

	    compFiniOptionValue (&value, option->type);
	}
    }

    g_strfreev (token);
}

static void
mateconfSendGLibNotify (CompScreen *s)
{
    Display *dpy = s->display->display;
    XEvent  xev;

    xev.xclient.type    = ClientMessage;
    xev.xclient.display = dpy;
    xev.xclient.format  = 32;

    xev.xclient.message_type = XInternAtom (dpy, "_COMPIZ_GLIB_NOTIFY", 0);
    xev.xclient.window	     = s->root;

    memset (xev.xclient.data.l, 0, sizeof (xev.xclient.data.l));

    XSendEvent (dpy,
		s->root,
		FALSE,
		SubstructureRedirectMask | SubstructureNotifyMask,
		&xev);
}

static Bool
mateconfInitCore (CompPlugin *p,
	       CompCore   *c)
{
    MateConfCore *gc;

    if (!checkPluginABI ("core", CORE_ABIVERSION))
	return FALSE;

    gc = malloc (sizeof (MateConfCore));
    if (!gc)
	return FALSE;

    g_type_init ();

    gc->client = mateconf_client_get_default ();

    mateconf_client_add_dir (gc->client, "/apps/" APP_NAME,
			  MATECONF_CLIENT_PRELOAD_NONE, NULL);

    gc->reloadHandle = compAddTimeout (0, 0, mateconfReload, 0);

    gc->cnxn = mateconf_client_notify_add (gc->client, "/apps/" APP_NAME,
					mateconfKeyChanged, c, NULL, NULL);

    WRAP (gc, c, initPluginForObject, mateconfInitPluginForObject);
    WRAP (gc, c, setOptionForPlugin, mateconfSetOptionForPlugin);

    c->base.privates[corePrivateIndex].ptr = gc;

    return TRUE;
}

static void
mateconfFiniCore (CompPlugin *p,
	       CompCore   *c)
{
    MATECONF_CORE (c);

    UNWRAP (gc, c, initPluginForObject);
    UNWRAP (gc, c, setOptionForPlugin);

    if (gc->reloadHandle)
	compRemoveTimeout (gc->reloadHandle);

    if (gc->cnxn)
	mateconf_client_notify_remove (gc->client, gc->cnxn);

    mateconf_client_remove_dir (gc->client, "/apps/" APP_NAME, NULL);
    mateconf_client_clear_cache (gc->client);

    free (gc);
}

static Bool
mateconfInitScreen (CompPlugin *p,
		 CompScreen *s)
{
    mateconfSendGLibNotify (s);

    return TRUE;
}

static CompBool
mateconfInitObject (CompPlugin *p,
		 CompObject *o)
{
    static InitPluginObjectProc dispTab[] = {
	(InitPluginObjectProc) mateconfInitCore,
	(InitPluginObjectProc) 0, /* InitDisplay */
	(InitPluginObjectProc) mateconfInitScreen
    };

    RETURN_DISPATCH (o, dispTab, ARRAY_SIZE (dispTab), TRUE, (p, o));
}

static void
mateconfFiniObject (CompPlugin *p,
		 CompObject *o)
{
    static FiniPluginObjectProc dispTab[] = {
	(FiniPluginObjectProc) mateconfFiniCore
    };

    DISPATCH (o, dispTab, ARRAY_SIZE (dispTab), (p, o));
}

static Bool
mateconfInit (CompPlugin *p)
{
    if (!compInitPluginMetadataFromInfo (&mateconfMetadata, p->vTable->name,
					 0, 0, 0, 0))
	return FALSE;

    corePrivateIndex = allocateCorePrivateIndex ();
    if (corePrivateIndex < 0)
    {
	compFiniMetadata (&mateconfMetadata);
	return FALSE;
    }

    compAddMetadataFromFile (&mateconfMetadata, p->vTable->name);

    return TRUE;
}

static void
mateconfFini (CompPlugin *p)
{
    freeCorePrivateIndex (corePrivateIndex);
    compFiniMetadata (&mateconfMetadata);
}

static CompMetadata *
mateconfGetMetadata (CompPlugin *plugin)
{
    return &mateconfMetadata;
}

CompPluginVTable mateconfVTable = {
    "mateconf",
    mateconfGetMetadata,
    mateconfInit,
    mateconfFini,
    mateconfInitObject,
    mateconfFiniObject,
    0, /* GetObjectOptions */
    0  /* SetObjectOption */
};

CompPluginVTable *
getCompPluginInfo20070830 (void)
{
    return &mateconfVTable;
}
