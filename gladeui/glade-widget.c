/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2004 Joaquin Cuenca Abela
 * Copyright (C) 2001, 2002, 2003 Ximian, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Authors:
 *   Joaquin Cuenca Abela <e98cuenc@yahoo.com>
 *   Chema Celorio <chema@celorio.com>
 *   Tristan Van Berkom <tvb@gnome.org>
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

/**
 * SECTION:glade-widget
 * @Short_Description: An object wrapper for the Glade runtime environment.
 *
 * #GladeWidget is the proxy between the instantiated runtime object and
 * the Glade core metadata. This api will be mostly usefull for its
 * convenience api for getting and setting properties (mostly from the plugin).
 */

#include <string.h>
#include <glib-object.h>
#include <gdk/gdkkeysyms.h>
#include <glib/gi18n-lib.h>
#include "glade.h"
#include "glade-accumulators.h"
#include "glade-project.h"
#include "glade-widget-adaptor.h"
#include "glade-widget.h"
#include "glade-marshallers.h"
#include "glade-property.h"
#include "glade-property-class.h"
#include "glade-placeholder.h"
#include "glade-signal.h"
#include "glade-popup.h"
#include "glade-editor.h"
#include "glade-app.h"
#include "glade-design-view.h"
#include "glade-widget-action.h"



static void         glade_widget_set_adaptor           (GladeWidget           *widget,
							GladeWidgetAdaptor    *adaptor);
static void         glade_widget_set_properties        (GladeWidget           *widget,
							GList                 *properties);
static GParameter  *glade_widget_info_params           (GladeWidgetAdaptor    *adaptor,
							GladeWidgetInfo       *info,
							gboolean               construct,
							guint                 *n_params);
static void         glade_widget_fill_from_widget_info (GladeWidgetInfo       *info,
							GladeWidget           *widget,
							gboolean               apply_props);
static GladeWidget *glade_widget_new_from_widget_info  (GladeWidgetInfo       *info,
							GladeProject          *project,
							GladeWidget           *parent);

static gboolean     glade_window_is_embedded           (GtkWindow *window);
static gboolean     glade_widget_embed                 (GladeWidget *widget);

enum
{
	ADD_SIGNAL_HANDLER,
	REMOVE_SIGNAL_HANDLER,
	CHANGE_SIGNAL_HANDLER,
	BUTTON_PRESS_EVENT,
	BUTTON_RELEASE_EVENT,
	MOTION_NOTIFY_EVENT,
	LAST_SIGNAL
};

enum
{
	PROP_0,
	PROP_NAME,
	PROP_INTERNAL,
	PROP_ANARCHIST,
	PROP_OBJECT,
	PROP_ADAPTOR,
	PROP_PROJECT,
	PROP_PROPERTIES,
	PROP_PARENT,
	PROP_INTERNAL_NAME,
	PROP_TEMPLATE,
	PROP_INFO,
	PROP_REASON,
	PROP_TOPLEVEL_WIDTH,
	PROP_TOPLEVEL_HEIGHT
};

static guint         glade_widget_signals[LAST_SIGNAL] = {0};

/* Sometimes we need to use the project deep in the loading code,
 * this is just a shortcut way to get the project.
 */
static GladeProject *loading_project = NULL;
static GQuark        glade_widget_name_quark = 0;


G_DEFINE_TYPE (GladeWidget, glade_widget, G_TYPE_OBJECT)

/*******************************************************************************
                           GladeWidget class methods
 *******************************************************************************/
static void
glade_widget_set_packing_actions (GladeWidget *widget, GladeWidget *parent)
{
	if (widget->packing_actions)
	{
		g_list_foreach (widget->packing_actions, (GFunc)g_object_unref, NULL);
		g_list_free (widget->packing_actions);
		widget->packing_actions = NULL;
	}
	
	if (parent->adaptor->packing_actions)
		widget->packing_actions = glade_widget_adaptor_pack_actions_new (parent->adaptor);
}

static void
glade_widget_add_child_impl (GladeWidget  *widget,
			     GladeWidget  *child,
			     gboolean      at_mouse)
{
	/* Safe to set the parent first... setting it afterwards
	 * creates packing properties, and that is not always
	 * desirable.
	 */
	glade_widget_set_parent (child, widget);

	/* Set packing actions first so we have access from the plugin
	 */
	glade_widget_set_packing_actions (child, widget);

	glade_widget_adaptor_add 
		(widget->adaptor, widget->object, child->object);

	/* XXX FIXME:
	 * We have a fundamental flaw here, we set packing props
	 * after parenting the widget so that we can introspect the
	 * values setup by the runtime widget, in which case the plugin
	 * cannot access its packing properties and set them sensitive
	 * or connect to thier signals etc. maybe its not so important
	 * but its a flaw worthy of note, some kind of double pass api
	 * would be needed to accomadate this.
	 */

	
	/* Setup packing properties here so we can introspect the new
	 * values from the backend.
	 */
	glade_widget_set_packing_properties (child, widget);
}

static void
glade_widget_remove_child_impl (GladeWidget  *widget,
				GladeWidget  *child)
{
	glade_widget_adaptor_remove
		(widget->adaptor, widget->object, child->object);
}

static void
glade_widget_replace_child_impl (GladeWidget *widget,
				 GObject     *old_object,
				 GObject     *new_object)
{
	GladeWidget *gnew_widget = glade_widget_get_from_gobject (new_object);
	GladeWidget *gold_widget = glade_widget_get_from_gobject (old_object);

	if (gnew_widget)
	{
		gnew_widget->parent = widget;

		/* Set packing actions first so we have access from the plugin
		 */
		glade_widget_set_packing_actions (gnew_widget, widget);
	}

	if (gold_widget && gold_widget != gnew_widget)
		gold_widget->parent = NULL;

	glade_widget_adaptor_replace_child 
		(widget->adaptor, widget->object,
		 old_object, new_object);

	/* Setup packing properties here so we can introspect the new
	 * values from the backend.
	 */
	if (gnew_widget)
		glade_widget_set_packing_properties (gnew_widget, widget);
}

static void
glade_widget_add_signal_handler_impl (GladeWidget *widget, GladeSignal *signal_handler)
{
	GPtrArray *signals;
	GladeSignal *new_signal_handler;

	g_return_if_fail (GLADE_IS_WIDGET (widget));
	g_return_if_fail (GLADE_IS_SIGNAL (signal_handler));

	signals = glade_widget_list_signal_handlers (widget, signal_handler->name);
	if (!signals)
	{
		signals = g_ptr_array_new ();
		g_hash_table_insert (widget->signals, g_strdup (signal_handler->name), signals);
	}

	new_signal_handler = glade_signal_clone (signal_handler);
	g_ptr_array_add (signals, new_signal_handler);
}

static void
glade_widget_remove_signal_handler_impl (GladeWidget *widget, GladeSignal *signal_handler)
{
	GPtrArray   *signals;
	GladeSignal *tmp_signal_handler;
	guint        i;

	g_return_if_fail (GLADE_IS_WIDGET (widget));
	g_return_if_fail (GLADE_IS_SIGNAL (signal_handler));

	signals = glade_widget_list_signal_handlers (widget, signal_handler->name);

	/* trying to remove an inexistent signal? */
	g_assert (signals);

	for (i = 0; i < signals->len; i++)
	{
		tmp_signal_handler = g_ptr_array_index (signals, i);
		if (glade_signal_equal (tmp_signal_handler, signal_handler))
		{
			glade_signal_free (tmp_signal_handler);
			g_ptr_array_remove_index (signals, i);
			break;
		}
	}
}

static void
glade_widget_change_signal_handler_impl (GladeWidget *widget,
					 GladeSignal *old_signal_handler,
					 GladeSignal *new_signal_handler)
{
	GPtrArray   *signals;
	GladeSignal *tmp_signal_handler;
	guint        i;
	
	g_return_if_fail (GLADE_IS_WIDGET (widget));
	g_return_if_fail (GLADE_IS_SIGNAL (old_signal_handler));
	g_return_if_fail (GLADE_IS_SIGNAL (new_signal_handler));
	g_return_if_fail (strcmp (old_signal_handler->name, new_signal_handler->name) == 0);

	signals = glade_widget_list_signal_handlers (widget, old_signal_handler->name);

	/* trying to remove an inexistent signal? */
	g_assert (signals);

	for (i = 0; i < signals->len; i++)
	{
		tmp_signal_handler = g_ptr_array_index (signals, i);
		if (glade_signal_equal (tmp_signal_handler, old_signal_handler))
		{
			if (strcmp (old_signal_handler->handler,
				    new_signal_handler->handler) != 0)
			{
				g_free (tmp_signal_handler->handler);
				tmp_signal_handler->handler =
					g_strdup (new_signal_handler->handler);
			}

			/* Handler */
			if (tmp_signal_handler->handler)
				g_free (tmp_signal_handler->handler);
			tmp_signal_handler->handler =
				g_strdup (new_signal_handler->handler);
			
			/* Object */
			if (tmp_signal_handler->userdata)
				g_free (tmp_signal_handler->userdata);
			tmp_signal_handler->userdata = 
				g_strdup (new_signal_handler->userdata);
			
			tmp_signal_handler->after  = new_signal_handler->after;
			tmp_signal_handler->lookup = new_signal_handler->lookup;
			break;
		}
	}
}


static gboolean
glade_widget_button_press_event_impl (GladeWidget    *gwidget,
				      GdkEvent       *base_event)
{
	GtkWidget         *widget;
	GdkEventButton    *event = (GdkEventButton *)base_event;
	gboolean           handled = FALSE;

	/* make sure to grab focus, since we may stop default handlers */
	widget = GTK_WIDGET (glade_widget_get_object (gwidget));
	if (GTK_WIDGET_CAN_FOCUS (widget) && !GTK_WIDGET_HAS_FOCUS (widget))
		gtk_widget_grab_focus (widget);

	/* if it's already selected don't stop default handlers, e.g. toggle button */
	if (event->button == 1)
	{
		if (event->state & GDK_CONTROL_MASK)
		{
			if (glade_project_is_selected (gwidget->project,
						       gwidget->object))
				glade_app_selection_remove 
					(gwidget->object, TRUE);
			else
				glade_app_selection_add
					(gwidget->object, TRUE);
			handled = TRUE;
		}
		else if (glade_project_is_selected (gwidget->project,
						    gwidget->object) == FALSE)
		{
			glade_util_clear_selection ();
			glade_app_selection_set 
				(gwidget->object, TRUE);

			/* Add selection without interrupting event flow 
			 * when shift is down, this allows better behaviour
			 * for GladeFixed children 
			 */
			handled = !(event->state & GDK_SHIFT_MASK);
		}
	}
	else if (event->button == 3)
	{
		glade_popup_widget_pop (gwidget, event, TRUE);
		handled = TRUE;
	}

	return handled;
}

static gboolean
glade_widget_event_impl (GladeWidget *gwidget,
			 GdkEvent    *event)
{
	gboolean handled = FALSE;

	g_return_val_if_fail (GLADE_IS_WIDGET (gwidget), FALSE);

	switch (event->type) 
	{
	case GDK_BUTTON_PRESS:
		g_signal_emit (gwidget, 
			       glade_widget_signals[BUTTON_PRESS_EVENT], 0, 
			       event, &handled);
		break;
	case GDK_BUTTON_RELEASE:
		g_signal_emit (gwidget, 
			       glade_widget_signals[BUTTON_RELEASE_EVENT], 0, 
			       event, &handled);
		break;
	case GDK_MOTION_NOTIFY:
		g_signal_emit (gwidget, 
			       glade_widget_signals[MOTION_NOTIFY_EVENT], 0, 
			       event, &handled);
		break;
	default:
		break;
	}

	return handled;
}


/**
 * glade_widget_event:
 * @event: A #GdkEvent
 *
 * Feed an event to be handled on the project GladeWidget
 * hierarchy.
 *
 * Returns whether the event was handled or not.
 */
gboolean
glade_widget_event (GladeWidget *gwidget,
		    GdkEvent    *event)
{
	gboolean   handled = FALSE;

	/* Lets just avoid some synthetic events (like focus-change) */
	if (((GdkEventAny *)event)->window == NULL) return FALSE;

	handled = GLADE_WIDGET_GET_CLASS (gwidget)->event (gwidget, event);

#if 0
	if (event->type != GDK_EXPOSE)
		g_print ("event widget '%s' handled '%d' event '%d'\n",
			 gwidget->name, handled, event->type);
#endif

	return handled;
}

/*******************************************************************************
                      GObjectClass & Object Construction
 *******************************************************************************/

/*
 * This function creates new GObject parameters based on the GType of the 
 * GladeWidgetAdaptor and its default values.
 *
 * If a GladeWidget is specified, it will be used to apply the
 * values currently in use.
 */
static GParameter *
glade_widget_template_params (GladeWidget      *widget,
			      gboolean          construct,
			      guint            *n_params)
{
	GladeWidgetAdaptor    *klass;
	GArray              *params;
	GObjectClass        *oclass;
	GParamSpec         **pspec;
	GladeProperty       *glade_property;
	GladePropertyClass  *pclass;
	guint                n_props, i;

	g_return_val_if_fail (GLADE_IS_WIDGET (widget), NULL);
	g_return_val_if_fail (n_params != NULL, NULL);

	klass = widget->adaptor;
	
	/* As a slight optimization, we never unref the class
	 */
	oclass = g_type_class_ref (klass->type);
	pspec  = g_object_class_list_properties (oclass, &n_props);
	params = g_array_new (FALSE, FALSE, sizeof (GParameter));

	for (i = 0; i < n_props; i++)
	{
		GParameter parameter = { 0, };

		if ((glade_property = 
		     glade_widget_get_property (widget, pspec[i]->name)) == NULL)
			continue;

		pclass = glade_property->klass;
		
		/* Ignore properties based on some criteria
		 */
		if (!glade_property_get_enabled (glade_property) ||
		    pclass == NULL  || /* Unaccounted for in the builder */
		    pclass->virt    || /* should not be set before 
					  GladeWidget wrapper exists */
		    pclass->ignore)    /* Catalog explicitly ignores the object */
			continue;

		if (construct &&
		    (pspec[i]->flags & 
		     (G_PARAM_CONSTRUCT|G_PARAM_CONSTRUCT_ONLY)) == 0)
			continue;
		else if (!construct &&
			 (pspec[i]->flags & 
			  (G_PARAM_CONSTRUCT|G_PARAM_CONSTRUCT_ONLY)) != 0)
			continue;

		if (g_value_type_compatible (G_VALUE_TYPE (pclass->def),
					     pspec[i]->value_type) == FALSE)
		{
			g_critical ("Type mismatch on %s property of %s",
				    parameter.name, klass->name);
			continue;
		}

		if (g_param_values_cmp (pspec[i], 
					glade_property->value, 
					pclass->orig_def) == 0)
			continue;


		parameter.name = pspec[i]->name; /* These are not copied/freed */
		g_value_init (&parameter.value, pspec[i]->value_type);
		g_value_copy (glade_property->value, &parameter.value);
		
		g_array_append_val (params, parameter);
	}
	g_free (pspec);

	*n_params = params->len;
	return (GParameter *)g_array_free (params, FALSE);
}

static void
free_params (GParameter *params, guint n_params)
{
	gint i;
	for (i = 0; i < n_params; i++)
		g_value_unset (&(params[i].value));
	g_free (params);

}

static GObject *
glade_widget_build_object (GladeWidgetAdaptor *adaptor, GladeWidget *widget, GladeWidgetInfo *info)
{
	GParameter          *params;
	GObject             *object;
	guint                n_params, i;

	if (widget)
		params = glade_widget_template_params (widget, TRUE, &n_params);
	else if (info)
		params = glade_widget_info_params (adaptor, info, TRUE, &n_params);
	else
		params = glade_widget_adaptor_default_params (adaptor, TRUE, &n_params);

	/* Create the new object with the correct parameters.
	 */
	object = g_object_newv (adaptor->type, n_params, params);

	free_params (params, n_params);

	if (widget)
		params = glade_widget_template_params (widget, FALSE, &n_params);
	else if (info)
		params = glade_widget_info_params (adaptor, info, FALSE, &n_params);
	else
		params = glade_widget_adaptor_default_params (adaptor, FALSE, &n_params);

	for (i = 0; i < n_params; i++)
	{
		g_object_set_property (object, params[i].name, &(params[i].value));
	}

	free_params (params, n_params);

	return object;
}

/**
 * glade_widget_dup_properties:
 * @template_props: the #GladeProperty list to copy
 * @as_load: whether to behave as if loading the project
 *
 * Copies a list of properties, if @as_load is specified, then
 * properties that are not saved to the glade file are ignored.
 *
 * Returns: A newly allocated #GList of new #GladeProperty objects.
 */
GList *
glade_widget_dup_properties (GList *template_props, gboolean as_load)
{
	GList *list, *properties = NULL;

	for (list = template_props; list && list->data; list = list->next)
	{
		GladeProperty *prop = list->data;
		
		if (prop->klass->save == FALSE && as_load)
			continue;

		properties = g_list_prepend (properties, glade_property_dup (prop, NULL));
	}
	return g_list_reverse (properties);
}

/**
 * glade_widget_remove_property:
 * @widget: A #GladeWidget
 * @id_property: the name of the property
 *
 * Removes the #GladeProperty indicated by @id_property
 * from @widget (this is intended for use in the plugin, to
 * remove properties from composite children that dont make
 * sence to allow the user to specify, notably - properties
 * that are proxied through the composite widget's properties or
 * style properties).
 */
void
glade_widget_remove_property (GladeWidget  *widget,
			      const gchar  *id_property)
{
	GladeProperty *prop;

	g_return_if_fail (GLADE_IS_WIDGET (widget));
	g_return_if_fail (id_property);

	if ((prop = glade_widget_get_property (widget, id_property)) != NULL)
	{
		widget->properties = g_list_remove (widget->properties, prop);
		g_object_unref (prop);
	}
	else
		g_critical ("Couldnt find property %s on widget %s\n",
			    id_property, widget->name);
}

static void
glade_widget_set_catalog_defaults (GList *list)
{
	GList *l;
	for (l = list; l && l->data; l = l->next)
	{
		GladeProperty *prop  = l->data;
		GladePropertyClass *klass = prop->klass;
		
		if (glade_property_equals_value (prop, klass->orig_def) &&
		     g_param_values_cmp (klass->pspec, klass->orig_def, klass->def))
			glade_property_reset (prop);
	}
}

static void
glade_widget_sync_custom_props (GladeWidget *widget)
{
	GList *l;
	for (l = widget->properties; l && l->data; l = l->next)
	{
		GladeProperty *prop  = GLADE_PROPERTY(l->data);

		/* XXX We need a better option to this hack.
		 *
		 * This used to be based on whether a function was
		 * provided by the backend to treat the said property, now
		 * that function is classwide so we dont know, so currently
		 * we are just syncing all properties for the sake of those
		 * properties.
		 */
		if (!prop->klass->construct_only)
			glade_property_sync (prop);

	}
}

static void
glade_widget_sync_packing_props (GladeWidget *widget)
{
	GList *l;
	for (l = widget->packing_properties; l && l->data; l = l->next) {
		GladeProperty *prop  = GLADE_PROPERTY(l->data);
		glade_property_sync (prop);
	}
}


static GObject *
glade_widget_constructor (GType                  type,
			  guint                  n_construct_properties,
			  GObjectConstructParam *construct_properties)
{
	GladeWidget      *gwidget;
	GObject          *ret_obj, *object;
	GList            *properties = NULL, *list;

	ret_obj = G_OBJECT_CLASS (glade_widget_parent_class)->constructor
		(type, n_construct_properties, construct_properties);

	gwidget = GLADE_WIDGET (ret_obj);

	if (gwidget->name == NULL)
	{
		if (gwidget->internal)
		{
			gchar *name_base = g_strdup_printf ("%s-%s", 
							    gwidget->construct_internal, 
							    gwidget->internal);
			
			if (gwidget->project)
			{
				gwidget->name = 
					glade_project_new_widget_name (gwidget->project, 
								       name_base);
				g_free (name_base);
			}
			else
				gwidget->name = name_base;

		}
		else if (gwidget->project)
			gwidget->name = glade_project_new_widget_name
				(GLADE_PROJECT (gwidget->project), 
				 gwidget->adaptor->generic_name);
		else
			gwidget->name = 
				g_strdup (gwidget->adaptor->generic_name);
	}

	if (gwidget->construct_template)
	{
		properties = glade_widget_dup_properties
			(gwidget->construct_template->properties, FALSE);
		
		glade_widget_set_properties (gwidget, properties);
	}

	if (gwidget->object == NULL)
	{
		object = glade_widget_build_object(gwidget->adaptor, 
						   gwidget->construct_template, 
						   gwidget->construct_info);
		glade_widget_set_object (gwidget, object);
	}

	/* Setup width/height */
	gwidget->width  = GWA_DEFAULT_WIDTH (gwidget->adaptor);
	gwidget->height = GWA_DEFAULT_HEIGHT (gwidget->adaptor);

	/* Introspect object properties before passing it to post_create,
	 * but only when its freshly created (depend on glade file at
	 * load time and copying properties at dup time).
	 */
	if (gwidget->construct_reason == GLADE_CREATE_USER)
		for (list = gwidget->properties; list; list = list->next)
			glade_property_load (GLADE_PROPERTY (list->data));
	
	/* We only use catalog defaults when the widget was created by the user! */
	if (gwidget->construct_reason == GLADE_CREATE_USER)
		glade_widget_set_catalog_defaults (gwidget->properties);
	
	/* Only call this once the GladeWidget is completely built
	 * (but before calling custom handlers...)
	 */
	glade_widget_adaptor_post_create (gwidget->adaptor, 
					  gwidget->object,
					  gwidget->construct_reason);

	/* Virtual properties need to be explicitly synchronized.
	 */
	if (gwidget->construct_reason == GLADE_CREATE_USER)
		glade_widget_sync_custom_props (gwidget);

	if (gwidget->parent && gwidget->packing_properties == NULL)
		glade_widget_set_packing_properties (gwidget, gwidget->parent);
	
	if (GTK_IS_WIDGET (gwidget->object) && !GTK_WIDGET_TOPLEVEL (gwidget->object))
	{
		gwidget->visible = TRUE;
		gtk_widget_show_all (GTK_WIDGET (gwidget->object));
	}
	else if (GTK_IS_WIDGET (gwidget->object) == FALSE)
		gwidget->visible = TRUE;
	
	return ret_obj;
}

static void
glade_widget_finalize (GObject *object)
{
	GladeWidget *widget = GLADE_WIDGET (object);

	g_return_if_fail (GLADE_IS_WIDGET (object));

	g_free (widget->name);
	g_free (widget->internal);
	g_hash_table_destroy (widget->signals);

	G_OBJECT_CLASS(glade_widget_parent_class)->finalize(object);
}

static void
glade_widget_dispose (GObject *object)
{
	GladeWidget *widget = GLADE_WIDGET (object);

	g_return_if_fail (GLADE_IS_WIDGET (object));

	/* We do not keep a reference to internal widgets */
	if (widget->internal == NULL)
	{
		if (GTK_IS_OBJECT (widget->object))
			gtk_object_destroy (GTK_OBJECT (widget->object));
		else 
			g_object_unref (widget->object);
	}

	if (widget->properties)
	{
		g_list_foreach (widget->properties, (GFunc)g_object_unref, NULL);
		g_list_free (widget->properties);
	}
	
	if (widget->packing_properties)
	{
		g_list_foreach (widget->packing_properties, (GFunc)g_object_unref, NULL);
		g_list_free (widget->packing_properties);
	}
	
	if (widget->actions)
	{
		g_list_foreach (widget->actions, (GFunc)g_object_unref, NULL);
		g_list_free (widget->actions);
	}
	
	if (widget->packing_actions)
	{
		g_list_foreach (widget->packing_actions, (GFunc)g_object_unref, NULL);
		g_list_free (widget->packing_actions);
	}
	
	G_OBJECT_CLASS (glade_widget_parent_class)->dispose (object);
}

static void
glade_widget_set_real_property (GObject         *object,
				guint            prop_id,
				const GValue    *value,
				GParamSpec      *pspec)
{
	GladeWidget *widget;

	widget = GLADE_WIDGET (object);

	switch (prop_id)
	{
	case PROP_NAME:
		glade_widget_set_name (widget, g_value_get_string (value));
		break;
	case PROP_INTERNAL:
		glade_widget_set_internal (widget, g_value_get_string (value));
		break;
	case PROP_ANARCHIST:
		widget->anarchist = g_value_get_boolean (value);
		break;
	case PROP_OBJECT:
		if (g_value_get_object (value))
			glade_widget_set_object (widget, g_value_get_object (value));
		break;
	case PROP_PROJECT:
		glade_widget_set_project (widget, GLADE_PROJECT (g_value_get_object (value)));
		break;
	case PROP_ADAPTOR:
		glade_widget_set_adaptor (widget, GLADE_WIDGET_ADAPTOR
					  (g_value_get_object (value)));
		break;
	case PROP_PROPERTIES:
		glade_widget_set_properties (widget, (GList *)g_value_get_pointer (value));
		break;
	case PROP_PARENT:
		glade_widget_set_parent (widget, GLADE_WIDGET (g_value_get_object (value)));
		break;
	case PROP_INTERNAL_NAME:
		if (g_value_get_string (value))
			widget->construct_internal = g_value_dup_string (value);
		break;
	case PROP_TEMPLATE:
		widget->construct_template = g_value_get_object (value);
		break;
	case PROP_INFO:
		widget->construct_info = g_value_get_pointer (value);
		break;
	case PROP_REASON:
		widget->construct_reason = g_value_get_int (value);
		break;
	case PROP_TOPLEVEL_WIDTH:
		widget->width = g_value_get_int (value);
		break;
	case PROP_TOPLEVEL_HEIGHT:
		widget->height = g_value_get_int (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
glade_widget_get_real_property (GObject         *object,
				guint            prop_id,
				GValue          *value,
				GParamSpec      *pspec)
{
	GladeWidget *widget;

	widget = GLADE_WIDGET (object);

	switch (prop_id)
	{
	case PROP_NAME:
		g_value_set_string (value, widget->name);
		break;
	case PROP_INTERNAL:
		g_value_set_string (value, widget->internal);
		break;
	case PROP_ANARCHIST:
		g_value_set_boolean (value, widget->anarchist);
		break;
	case PROP_ADAPTOR:
		g_value_set_object (value, widget->adaptor);
		break;
	case PROP_PROJECT:
		g_value_set_object (value, G_OBJECT (widget->project));
		break;
	case PROP_OBJECT:
		g_value_set_object (value, widget->object);
		break;
	case PROP_PROPERTIES:
		g_value_set_pointer (value, widget->properties);
		break;
	case PROP_PARENT:
		g_value_set_object (value, widget->parent);
		break;
	case PROP_TOPLEVEL_WIDTH:
		g_value_set_int (value, widget->width);
		break;
	case PROP_TOPLEVEL_HEIGHT:
		g_value_set_int (value, widget->height);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
free_signals (gpointer value)
{
	GPtrArray *signals = (GPtrArray*) value;
	guint i;
	guint nb_signals;

	if (signals == NULL)
		return;

	/* g_ptr_array_foreach (signals, (GFunc) glade_signal_free, NULL);
	 * only available in modern versions of Gtk+ */
	nb_signals = signals->len;
	for (i = 0; i < nb_signals; i++)
		glade_signal_free (g_ptr_array_index (signals, i));

	g_ptr_array_free (signals, TRUE);
}

static void
glade_widget_init (GladeWidget *widget)
{
	widget->adaptor = NULL;
	widget->project = NULL;
	widget->name = NULL;
	widget->internal = NULL;
	widget->object = NULL;
	widget->properties = NULL;
	widget->packing_properties = NULL;
	widget->prop_refs = NULL;
	widget->prop_refs_readonly = FALSE;
	widget->signals = g_hash_table_new_full
		(g_str_hash, g_str_equal,
		 (GDestroyNotify) g_free,
		 (GDestroyNotify) free_signals);
	
	/* Initial invalid values */
	widget->width  = -1;
	widget->height = -1;
}

static void
glade_widget_class_init (GladeWidgetClass *klass)
{
	GObjectClass *object_class;

	if (glade_widget_name_quark == 0)
		glade_widget_name_quark = 
			g_quark_from_static_string ("GladeWidgetDataTag");

	object_class = G_OBJECT_CLASS (klass);

	object_class->constructor     = glade_widget_constructor;
	object_class->finalize        = glade_widget_finalize;
	object_class->dispose         = glade_widget_dispose;
	object_class->set_property    = glade_widget_set_real_property;
	object_class->get_property    = glade_widget_get_real_property;

	klass->add_child              = glade_widget_add_child_impl;
	klass->remove_child           = glade_widget_remove_child_impl;
	klass->replace_child          = glade_widget_replace_child_impl;
	klass->event                  = glade_widget_event_impl;

	klass->add_signal_handler     = glade_widget_add_signal_handler_impl;
	klass->remove_signal_handler  = glade_widget_remove_signal_handler_impl;
	klass->change_signal_handler  = glade_widget_change_signal_handler_impl;

	klass->button_press_event     = glade_widget_button_press_event_impl;
	klass->button_release_event   = NULL;
	klass->motion_notify_event    = NULL;

	g_object_class_install_property
		(object_class, PROP_NAME,
		 g_param_spec_string ("name", _("Name"),
				      _("The name of the widget"),
				      NULL,
				      G_PARAM_READWRITE |
				      G_PARAM_CONSTRUCT));

	g_object_class_install_property
		(object_class, PROP_INTERNAL,
		 g_param_spec_string ("internal", _("Internal name"),
				      _("The internal name of the widget"),
				      NULL, G_PARAM_READWRITE |
				      G_PARAM_CONSTRUCT));
	
	g_object_class_install_property
		(object_class, PROP_ANARCHIST,
		 g_param_spec_boolean ("anarchist", _("Anarchist"),
				       _("Whether this composite child is "
					 "an ancestral child or an anarchist child"),
				       FALSE, G_PARAM_READWRITE |
				       G_PARAM_CONSTRUCT_ONLY));

	g_object_class_install_property
		(object_class, PROP_OBJECT,
		 g_param_spec_object ("object", _("Object"),
				      _("The object associated"),
				      G_TYPE_OBJECT,
				      G_PARAM_READWRITE |
				      G_PARAM_CONSTRUCT));

	g_object_class_install_property
		(object_class, PROP_ADAPTOR,
		   g_param_spec_object ("adaptor", _("Adaptor"),
					_("The class adaptor for the associated widget"),
					GLADE_TYPE_WIDGET_ADAPTOR,
					G_PARAM_READWRITE |
					G_PARAM_CONSTRUCT_ONLY));

	g_object_class_install_property
		(object_class, PROP_PROJECT,
		 g_param_spec_object ("project", _("Project"),
				      _("The glade project that "
					"this widget belongs to"),
				      GLADE_TYPE_PROJECT,
				      G_PARAM_READWRITE |
				      G_PARAM_CONSTRUCT));

	g_object_class_install_property
		(object_class, PROP_PROPERTIES,
		 g_param_spec_pointer ("properties", _("Properties"),
				       _("A list of GladeProperties"),
				       G_PARAM_READWRITE |
				       G_PARAM_CONSTRUCT_ONLY));

	g_object_class_install_property
		(object_class, 	PROP_PARENT,
		 g_param_spec_object ("parent", _("Parent"),
				      _("A pointer to the parenting GladeWidget"),
				      GLADE_TYPE_WIDGET,
				      G_PARAM_READWRITE |
				      G_PARAM_CONSTRUCT));

	g_object_class_install_property
		(object_class, 	PROP_INTERNAL_NAME,
		 g_param_spec_string ("internal-name", _("Internal Name"),
				      _("A generic name prefix for internal widgets"),
				      NULL, G_PARAM_CONSTRUCT_ONLY|G_PARAM_WRITABLE));

	g_object_class_install_property
		(object_class, 	PROP_TEMPLATE,
		 g_param_spec_object ("template", _("Template"),
				       _("A GladeWidget template to base a new widget on"),
				      GLADE_TYPE_WIDGET,
				      G_PARAM_CONSTRUCT_ONLY|G_PARAM_WRITABLE));

	g_object_class_install_property
		(object_class, 	PROP_INFO,
		 g_param_spec_pointer ("info", _("Info"),
				       _("A GladeWidgetInfo struct to base a new widget on"),
				       G_PARAM_CONSTRUCT_ONLY|G_PARAM_WRITABLE));

	g_object_class_install_property
		(object_class, 	PROP_REASON,
		 g_param_spec_int ("reason", _("Reason"),
				   _("A GladeCreateReason for this creation"),
				   GLADE_CREATE_USER,
				   GLADE_CREATE_REASONS - 1,
				   GLADE_CREATE_USER,
				   G_PARAM_CONSTRUCT_ONLY|G_PARAM_WRITABLE));

	g_object_class_install_property
		(object_class, 	PROP_TOPLEVEL_WIDTH,
		 g_param_spec_int ("toplevel-width", _("Toplevel Width"),
				   _("The width of the widget when toplevel in "
				     "the GladeDesignLayout"),
				   -1,
				   G_MAXINT,
				   -1,
				   G_PARAM_READWRITE));

	g_object_class_install_property
		(object_class, 	PROP_TOPLEVEL_HEIGHT,
		 g_param_spec_int ("toplevel-height", _("Toplevel Height"),
				   _("The height of the widget when toplevel in "
				     "the GladeDesignLayout"),
				   -1,
				   G_MAXINT,
				   -1,
				   G_PARAM_READWRITE));

	/**
	 * GladeWidget::add-signal-handler:
	 * @gladewidget: the #GladeWidget which received the signal.
	 * @arg1: the #GladeSignal that was added to @gladewidget.
	 */
	glade_widget_signals[ADD_SIGNAL_HANDLER] =
		g_signal_new ("add-signal-handler",
			      G_TYPE_FROM_CLASS (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (GladeWidgetClass, add_signal_handler),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__POINTER,
			      G_TYPE_NONE,
			      1,
			      G_TYPE_POINTER);

	/**
	 * GladeWidget::remove-signal-handler:
	 * @gladewidget: the #GladeWidget which received the signal.
	 * @arg1: the #GladeSignal that was removed from @gladewidget.
	 */
	glade_widget_signals[REMOVE_SIGNAL_HANDLER] =
		g_signal_new ("remove-signal-handler",
			      G_TYPE_FROM_CLASS (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (GladeWidgetClass, remove_signal_handler),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__POINTER,
			      G_TYPE_NONE,
			      1,
			      G_TYPE_POINTER);


	/**
	 * GladeWidget::change-signal-handler:
	 * @gladewidget: the #GladeWidget which received the signal.
	 * @arg1: the old #GladeSignal
	 * @arg2: the new #GladeSignal
	 */
	glade_widget_signals[CHANGE_SIGNAL_HANDLER] =
		g_signal_new ("change-signal-handler",
			      G_TYPE_FROM_CLASS (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (GladeWidgetClass, change_signal_handler),
			      NULL, NULL,
			      glade_marshal_VOID__POINTER_POINTER,
			      G_TYPE_NONE,
			      2,
			      G_TYPE_POINTER, G_TYPE_POINTER);


	/**
	 * GladeWidget::button-press-event:
	 * @gladewidget: the #GladeWidget which received the signal.
	 * @arg1: the #GdkEvent
	 */
	glade_widget_signals[BUTTON_PRESS_EVENT] =
		g_signal_new ("button-press-event",
			      G_TYPE_FROM_CLASS (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (GladeWidgetClass, button_press_event),
			      glade_boolean_handled_accumulator, NULL,
			      glade_marshal_BOOLEAN__BOXED,
			      G_TYPE_BOOLEAN, 1,
			      GDK_TYPE_EVENT | G_SIGNAL_TYPE_STATIC_SCOPE);

	/**
	 * GladeWidget::button-relese-event:
	 * @gladewidget: the #GladeWidget which received the signal.
	 * @arg1: the #GdkEvent
	 */
	glade_widget_signals[BUTTON_RELEASE_EVENT] =
		g_signal_new ("button-release-event",
			      G_TYPE_FROM_CLASS (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (GladeWidgetClass, button_release_event),
			      glade_boolean_handled_accumulator, NULL,
			      glade_marshal_BOOLEAN__BOXED,
			      G_TYPE_BOOLEAN, 1,
			      GDK_TYPE_EVENT | G_SIGNAL_TYPE_STATIC_SCOPE);


	/**
	 * GladeWidget::motion-notify-event:
	 * @gladewidget: the #GladeWidget which received the signal.
	 * @arg1: the #GdkEvent
	 */
	glade_widget_signals[MOTION_NOTIFY_EVENT] =
		g_signal_new ("motion-notify-event",
			      G_TYPE_FROM_CLASS (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (GladeWidgetClass, motion_notify_event),
			      glade_boolean_handled_accumulator, NULL,
			      glade_marshal_BOOLEAN__BOXED,
			      G_TYPE_BOOLEAN, 1,
			      GDK_TYPE_EVENT | G_SIGNAL_TYPE_STATIC_SCOPE);
}

/*******************************************************************************
                                Static stuff....
 *******************************************************************************/
static void
glade_widget_copy_packing_props (GladeWidget *parent,
				 GladeWidget *child,
				 GladeWidget *template_widget)
{
	GladeProperty *dup_prop, *orig_prop;
	GList         *l;

	g_return_if_fail (child->parent == parent);

	glade_widget_set_packing_properties (child, parent);

	for (l = child->packing_properties; l && l->data; l = l->next)
	{
		dup_prop  = GLADE_PROPERTY(l->data);
		orig_prop =
			glade_widget_get_property (template_widget, dup_prop->klass->id);
		glade_property_set_value (dup_prop, orig_prop->value);
	}
}

static void
glade_widget_set_default_packing_properties (GladeWidget *container,
					     GladeWidget *child)
{
	GladePropertyClass *property_class;
	GList              *l;

	for (l = container->adaptor->packing_props; l; l = l->next) 
	{
		const gchar  *def;
		GValue       *value;

		property_class = l->data;

		if ((def = 
		     glade_widget_adaptor_get_packing_default
		     (child->adaptor, container->adaptor, property_class->id)) == NULL)
			continue;
		
		value = glade_property_class_make_gvalue_from_string (property_class,
								      def,
								      child->project);
		
		glade_widget_child_set_property (container, child,
						 property_class->id, value);
		g_value_unset (value);
		g_free (value);
	}
}

/**
 * When looking for an internal child we have to walk up the hierarchy...
 */
static GObject *
glade_widget_get_internal_child (GladeWidget *parent,
				 const gchar *internal)                
{
	while (parent)
	{
		GladeWidgetAdaptorClass *adaptor_class =
			GLADE_WIDGET_ADAPTOR_GET_CLASS (parent->adaptor);

		if (adaptor_class->get_internal_child)
			return glade_widget_adaptor_get_internal_child
				(parent->adaptor, parent->object, internal);

		parent = glade_widget_get_parent (parent);
	}
	return NULL;
}

static GladeGetInternalFunc
glade_widget_get_internal_func (GladeWidget *parent, GladeWidget **parent_ret)
{
	GladeWidget *gwidget;
	
	g_return_val_if_fail (GLADE_IS_WIDGET (parent), NULL);
	
	for (gwidget = parent; gwidget; gwidget = gwidget->parent)
	{
		GladeWidgetAdaptorClass *adaptor_class =
			GLADE_WIDGET_ADAPTOR_GET_CLASS (gwidget->adaptor);

		if (adaptor_class->get_internal_child)
		{
			if (parent_ret) *parent_ret = gwidget;
			return adaptor_class->get_internal_child;
		}
	}
	g_error ("No internal child search function "
		 "provided for widget class %s (or any parents)",
		 parent->adaptor->name);

	return NULL;
}


static GladeWidget *
glade_widget_dup_internal (GladeWidget *parent,
			   GladeWidget *template_widget,
			   gboolean     exact)
{
	GladeGetInternalFunc  get_internal;
	GladeWidget *gwidget = NULL, *internal_parent;
	GList       *list, *children;
	GtkWidget   *placeholder;
	gchar       *child_type;
	
	g_return_val_if_fail (template_widget != NULL && GLADE_IS_WIDGET(template_widget), NULL);
	g_return_val_if_fail (parent == NULL || GLADE_IS_WIDGET (parent), NULL);

	/* Dont actually duplicate internal widgets, but recurse through them anyway. */
	if (template_widget->internal)
	{
		GObject *internal_object = NULL;

		if (parent && 
		    (get_internal = 
		     glade_widget_get_internal_func (parent, &internal_parent)) != NULL)
		{
			/* We cant use "parent" here, we have to recurse up the hierarchy to find
			 * the "parent" that has `get_internal_child' support (i.e. internal children
			 * may have depth).
			 */
			internal_object = get_internal (internal_parent->adaptor,
							internal_parent->object, 
							template_widget->internal);
			g_assert (internal_object);
			
			gwidget = glade_widget_get_from_gobject (internal_object);
			g_assert (gwidget);
		}
	}
	else
	{
		gchar *name = g_strdup (template_widget->name);
fprintf(stderr,"template name[%s]\n",name);
		gwidget = glade_widget_adaptor_create_widget
			(template_widget->adaptor, FALSE,
			 "name", name,
			 "parent", parent, 
			 "project", template_widget->project,
			 "template", template_widget, 
			 "reason", GLADE_CREATE_COPY, NULL);
		g_free (name);
		glade_widget_copy_properties (gwidget, template_widget);
	}

	/* Copy signals over here regardless of internal or not... */
	if (exact)
		glade_widget_copy_signals (gwidget, template_widget);
	
	if ((children =
	     glade_widget_adaptor_get_children (template_widget->adaptor,
						template_widget->object)) != NULL)
	{
		for (list = children; list && list->data; list = list->next)
		{
			GObject     *child = G_OBJECT (list->data);
			GladeWidget *child_gwidget, *child_dup;

			child_type = g_object_get_data (child, "special-child-type");

			if ((child_gwidget = glade_widget_get_from_gobject (child)) == NULL)
			{
				/* Bring the placeholders along ...
				 * but not unmarked internal children */
				if (GLADE_IS_PLACEHOLDER (child))
				{
					placeholder = glade_placeholder_new ();

					g_object_set_data_full (G_OBJECT (placeholder),
								"special-child-type",
								g_strdup (child_type),
								g_free);
					
					glade_widget_adaptor_add (gwidget->adaptor,
								  gwidget->object,
								  G_OBJECT (placeholder));
				}
			}
			else
			{
				/* Recurse through every GladeWidget (internal or not) */
				child_dup = glade_widget_dup_internal (gwidget, child_gwidget, exact);

				if (child_gwidget->internal == NULL)
				{
					g_object_set_data_full (child_dup->object,
								"special-child-type",
								g_strdup (child_type),
								g_free);

					glade_widget_add_child (gwidget, child_dup, FALSE);
				}

				/* Internal children that are not heirarchic children
				 * need to avoid copying these packing props (like popup windows
				 * created on behalf of composite widgets).
				 */
				if (glade_widget_adaptor_has_child (gwidget->adaptor,
								    gwidget->object,
								    child_dup->object))
					glade_widget_copy_packing_props (gwidget,
									 child_dup,
									 child_gwidget);
			}
		}
		g_list_free (children);
	}

	if (gwidget->internal)
		glade_widget_copy_properties (gwidget, template_widget);

	if (gwidget->packing_properties == NULL)
		gwidget->packing_properties = glade_widget_dup_properties (template_widget->packing_properties , FALSE);
	
	/* If custom properties are still at thier
	 * default value, they need to be synced.
	 */
	glade_widget_sync_custom_props (gwidget);

	/* Special case GtkWindow here and ensure the pasted window
	 * has the same size as the 'Cut' one.
	 */
	if (GTK_IS_WINDOW (gwidget->object))
	{
		gint width, height;
		g_assert (GTK_IS_WINDOW (template_widget->object));

		gtk_window_get_size (GTK_WINDOW (template_widget->object), 
				     &width, &height);
		gtk_window_resize (GTK_WINDOW (gwidget->object),
				   width, height);
	} else {
		gint width, height;
		gtk_widget_get_size_request(GTK_WIDGET(template_widget->object),
			&width,&height);
		gtk_widget_set_size_request(GTK_WIDGET(gwidget->object),
			width,height);
	}

	return gwidget;
}


typedef struct {
	GladeWidget *widget;
	GtkWidget   *placeholder;
	GList       *properties;
	
	gchar       *internal_name;
	GList       *internal_list;
} GladeChildExtract;

static GList *
glade_widget_extract_children (GladeWidget *gwidget)
{
	GladeChildExtract *extract;
	GList             *extract_list = NULL;
	GList             *children, *list;
	
	children = glade_widget_adaptor_get_children
		(gwidget->adaptor, gwidget->object);

	for (list = children; list && list->data; list = list->next)
	{
		GObject     *child   = G_OBJECT(list->data);
		GladeWidget *gchild  = glade_widget_get_from_gobject (child);
#if 0
		g_print ("Extracting %s from %s\n",
			 gchild ? gchild->name : 
			 GLADE_IS_PLACEHOLDER (child) ? "placeholder" : "unknown widget", 
			 gwidget->name);
#endif
		if (gchild && gchild->internal)
		{
			/* Recurse and collect any deep child hierarchies
			 * inside composite widgets.
			 */
			extract = g_new0 (GladeChildExtract, 1);
			extract->internal_name = g_strdup (gchild->internal);
			extract->internal_list = glade_widget_extract_children (gchild);
			extract->properties    = 
				glade_widget_dup_properties (gchild->properties, TRUE);
			
			extract_list = g_list_prepend (extract_list, extract);
		
		}
		else if (gchild || GLADE_IS_PLACEHOLDER (child))
		{
			extract = g_new0 (GladeChildExtract, 1);

			if (gchild)
			{
				extract->widget = g_object_ref (gchild);
				
				/* Make copies of the packing properties
				 */
				extract->properties = 
					glade_widget_dup_properties 
					(gchild->packing_properties, TRUE);

				glade_widget_remove_child (gwidget, gchild);
			}
			else
			{
				/* need to handle placeholders by hand here */
				extract->placeholder = g_object_ref (child);
				glade_widget_adaptor_remove (gwidget->adaptor,
							     gwidget->object, child);
			}
			extract_list =
				g_list_prepend (extract_list, extract);
		}
	}

	if (children)
		g_list_free (children);

	return g_list_reverse (extract_list);
}

static void
glade_widget_insert_children (GladeWidget *gwidget, GList *children)
{
	GladeChildExtract *extract;
	GladeWidget       *gchild;
	GObject           *internal_object;
	GList             *list, *l;
	
	for (list = children; list; list = list->next)
	{
		extract = list->data;
		
		if (extract->internal_name)
		{
			GladeGetInternalFunc   get_internal;
			GladeWidget           *internal_parent;


			/* Recurse and add deep widget hierarchies to internal
			 * widgets.
			 */
			get_internal = glade_widget_get_internal_func
				(gwidget, &internal_parent);

			internal_object = get_internal (internal_parent->adaptor,
							internal_parent->object,
							extract->internal_name);

			gchild = glade_widget_get_from_gobject (internal_object);
			
			/* This will free the list... */
			glade_widget_insert_children (gchild, extract->internal_list);

			/* Set the properties after inserting the children */
			for (l = extract->properties; l; l = l->next)
			{
				GValue         value = { 0, };
				GladeProperty *saved_prop = l->data;
				GladeProperty *widget_prop = 
					glade_widget_get_property (gchild,
								   saved_prop->klass->id);
				
				glade_property_get_value (saved_prop, &value);
				glade_property_set_value (widget_prop, &value);
				g_value_unset (&value);

				/* Free them as we go ... */
				g_object_unref (saved_prop);
			}

			if (extract->properties)
				g_list_free (extract->properties);

			g_free (extract->internal_name);
		}
		else if (extract->widget)
		{
			glade_widget_add_child (gwidget, extract->widget, FALSE);
			g_object_unref (extract->widget);
			
			for (l = extract->properties; l; l = l->next)
			{
				GValue         value = { 0, };
				GladeProperty *saved_prop = l->data;
				GladeProperty *widget_prop = 
					glade_widget_get_pack_property (extract->widget,
									saved_prop->klass->id);
				
				glade_property_get_value (saved_prop, &value);
				glade_property_set_value (widget_prop, &value);
				g_value_unset (&value);

				/* Free them as we go ... */
				g_object_unref (saved_prop);
			}
			if (extract->properties)
				g_list_free (extract->properties);
		}
		else
		{
			glade_widget_adaptor_add (gwidget->adaptor,
						  gwidget->object,
						  G_OBJECT (extract->placeholder));
			g_object_unref (extract->placeholder);
		}
		g_free (extract);
	}
	
	if (children)
		g_list_free (children);
}

static void
glade_widget_set_properties (GladeWidget *widget, GList *properties)
{
	GladeProperty *property;
	GList         *list;

	if (properties)
	{
		if (widget->properties)
		{
			g_list_foreach (widget->properties, (GFunc)g_object_unref, NULL);
			g_list_free (widget->properties);
		}
		widget->properties = properties;
		
		for (list = properties; list; list = list->next)
		{
			property = list->data;

			property->widget = widget;
		}
	}
}

static void
glade_widget_set_actions (GladeWidget *widget, GladeWidgetAdaptor *adaptor)
{
	GList *l;
	
	for (l = adaptor->actions; l; l = g_list_next (l))
	{
		GWActionClass *action = l->data;
		GObject *obj = g_object_new (GLADE_TYPE_WIDGET_ACTION,
					     "class", action, NULL);
		
		widget->actions = g_list_prepend (widget->actions,
						  GLADE_WIDGET_ACTION (obj));
	}
	widget->actions = g_list_reverse (widget->actions);
}

static void
glade_widget_set_adaptor (GladeWidget *widget, GladeWidgetAdaptor *adaptor)
{
	GladePropertyClass *property_class;
	GladeProperty *property;
	GList *list;

	g_return_if_fail (GLADE_IS_WIDGET (widget));
	g_return_if_fail (GLADE_IS_WIDGET_ADAPTOR (adaptor));
	/* calling set_class out of the constructor? */
	g_return_if_fail (widget->adaptor == NULL);
	
	widget->adaptor = adaptor;

	/* If we have no properties; we are not in the process of loading
	 */
	if (!widget->properties)
	{
		for (list = adaptor->properties; list; list = list->next)
		{
			property_class = GLADE_PROPERTY_CLASS(list->data);
			if ((property = glade_property_new (property_class, 
							    widget, NULL)) == NULL)
			{
				g_warning ("Failed to create [%s] property",
					   property_class->id);
				continue;
			}

			widget->properties = g_list_prepend (widget->properties, property);
		}
		widget->properties = g_list_reverse (widget->properties);
	}
	
	/* Create actions from adaptor */
	glade_widget_set_actions (widget, adaptor);
}

static gboolean
expose_draw_selection (GtkWidget       *widget_gtk,
		       GdkEventExpose  *event,
		       GladeWidget     *gwidget)
{
	glade_util_draw_selection_nodes (event->window);
        return FALSE;
}


/* Connects a signal handler to the 'event' signal for a widget and
   all its children recursively. We need this to draw the selection
   rectangles and to get button press/release events reliably. */
static void
glade_widget_connect_signal_handlers (GtkWidget   *widget_gtk, 
				      GCallback    callback, 
				      GladeWidget *gwidget)
{
	GList *children, *list;

	/* Check if we've already connected an event handler. */
	if (!g_object_get_data (G_OBJECT (widget_gtk),
				GLADE_TAG_EVENT_HANDLER_CONNECTED)) 
	{
		/* Make sure we can recieve the kind of events we're
		 * connecting for 
		 */
		gtk_widget_add_events (widget_gtk,
				       GDK_POINTER_MOTION_MASK      | /* Handle pointer events */
				       GDK_POINTER_MOTION_HINT_MASK | /* for drag/resize and   */
				       GDK_BUTTON_PRESS_MASK        | /* managing selection.   */
				       GDK_BUTTON_RELEASE_MASK);

		g_signal_connect (G_OBJECT (widget_gtk), "event",
				  callback, gwidget);

		g_signal_connect_after (G_OBJECT (widget_gtk), "expose-event",
					G_CALLBACK (expose_draw_selection), gwidget);
		
		
		g_object_set_data (G_OBJECT (widget_gtk),
				   GLADE_TAG_EVENT_HANDLER_CONNECTED,
				   GINT_TO_POINTER (1));
	}

	/* We also need to get expose events for any children.
	 */
	if (GTK_IS_CONTAINER (widget_gtk)) 
	{
		if ((children = 
		     glade_util_container_get_all_children (GTK_CONTAINER
							    (widget_gtk))) != NULL)
		{
			for (list = children; list; list = list->next)
				glade_widget_connect_signal_handlers 
					(GTK_WIDGET (list->data), callback, gwidget);
			g_list_free (children);
		}
	}
}

/*
 * Returns a list of GladeProperties from a list for the correct
 * child type for this widget of this container.
 */
static GList *
glade_widget_create_packing_properties (GladeWidget *container, GladeWidget *widget)
{
	GladePropertyClass   *property_class;
	GladeProperty        *property;
	GList                *list, *packing_props = NULL;

	/* XXX TODO: by checking with some GladePropertyClass metadata, decide
	 * which packing properties go on which type of children.
	 */
	for (list = container->adaptor->packing_props; 
	     list && list->data; list = list->next)
	{
		property_class = list->data;
		property       = glade_property_new (property_class, widget, NULL);
		packing_props  = g_list_prepend (packing_props, property);

	}
	return g_list_reverse (packing_props);
}

/*******************************************************************************
                           GladeInterface Parsing code
 *******************************************************************************/
static gint
glade_widget_set_child_type_from_child_info (GladeChildInfo     *child_info,
					     GladeWidgetAdaptor *parent_adaptor,
					     GObject *child)
{
	guint           i;
	GladePropInfo  *prop_info;
	gchar          *special_child_type;

	g_object_get (parent_adaptor, "special-child-type", &special_child_type, NULL);
	
	if (!special_child_type)
		return -1;

	for (i = 0; i < child_info->n_properties; ++i)
	{
		prop_info = child_info->properties + i;
		if (!strcmp (prop_info->name, special_child_type))
		{
			g_free (special_child_type);
			
			g_object_set_data_full (child,
						"special-child-type",
						g_strdup (prop_info->value),
						g_free);
			return i;
		}
	}
	g_free (special_child_type);
	return -1;
}

static gboolean
glade_widget_new_child_from_child_info (GladeChildInfo *info,
					GladeProject   *project,
					GladeWidget    *parent)
{
	GladeWidget    *child;
	GList          *list;

	g_return_val_if_fail (info != NULL, FALSE);
	g_return_val_if_fail (project != NULL, FALSE);

	/* is it a placeholder? */
	if (!info->child)
	{
		GObject *palaceholder = G_OBJECT (glade_placeholder_new ());
		glade_widget_set_child_type_from_child_info 
			(info, parent->adaptor, palaceholder);
		glade_widget_adaptor_add (parent->adaptor,
					  parent->object,
					  palaceholder);
		return TRUE;
	}

	/* is it an internal child? */
	if (info->internal_child)
	{
		GObject *child_object =
			glade_widget_get_internal_child (parent, info->internal_child);

		if (!child_object)
                {
			g_warning ("Failed to locate internal child %s of %s",
				   info->internal_child, glade_widget_get_name (parent));
			return FALSE;
		}

		if ((child = glade_widget_get_from_gobject (child_object)) == NULL)
			g_error ("Unable to get GladeWidget for internal child %s\n",
				 info->internal_child);

		/* Apply internal widget name from here */
		glade_widget_set_name (child, info->child->name);
		glade_widget_fill_from_widget_info (info->child, child, TRUE);
		glade_widget_sync_custom_props (child);
	}
        else
        {
		child = glade_widget_new_from_widget_info (info->child, project, parent);
		if (!child)
			return FALSE;

		child->parent = parent;

		glade_widget_set_child_type_from_child_info 
			(info, parent->adaptor, child->object);
		
		glade_widget_add_child (parent, child, FALSE);

		glade_widget_sync_packing_props (child);
	}

	/* Get the packing properties */
	for (list = child->packing_properties; list; list = list->next)
	{
		GladeProperty *property = list->data;
		glade_property_read (property, property->klass, 
				     loading_project, info, TRUE);
	}
	return TRUE;
}

static void
glade_widget_fill_from_widget_info (GladeWidgetInfo *info,
				    GladeWidget     *widget,
				    gboolean         apply_props)
{
	GladeProperty *property;
	GList         *list;
	guint          i;

	g_return_if_fail (GLADE_IS_WIDGET (widget));
	g_return_if_fail (info != NULL);
	
	g_assert (strcmp (info->classname, widget->adaptor->name) == 0);

	/* Children */
	for (i = 0; i < info->n_children; ++i)
	{
		if (!glade_widget_new_child_from_child_info (info->children + i,
							     widget->project, widget))
		{
			g_warning ("Failed to read child of %s",
				   glade_widget_get_name (widget));
			continue;
		}
	}

	/* Signals */
	for (i = 0; i < info->n_signals; ++i)
	{
		GladeSignal *signal;

		signal = glade_signal_new_from_signal_info (info->signals + i);
		if (!signal)
		{
			g_warning ("Failed to read signal");
			continue;
		}
		glade_widget_add_signal_handler (widget, signal);
	}

	/* Properties */
	if (apply_props)
	{
 		for (list = widget->properties; list; list = list->next)
  		{
			property = list->data;
			glade_property_read (property, property->klass, 
					     loading_project, info, TRUE);
		}
	}
}



static GList *
glade_widget_properties_from_widget_info (GladeWidgetAdaptor *klass,
					  GladeWidgetInfo  *info)
{
	GList  *properties = NULL, *list;
	
	for (list = klass->properties; list && list->data; list = list->next)
	{
		GladePropertyClass *pclass = list->data;
		GladeProperty      *property;

		/* If there is a value in the XML, initialize property with it,
		 * otherwise initialize property to default.
		 */
		property = glade_property_new (pclass, NULL, NULL);
		
		glade_property_original_reset (property);
		
		glade_property_read (property, property->klass, 
				     loading_project, info, TRUE);

		properties = g_list_prepend (properties, property);
	}

	return g_list_reverse (properties);
}

static GladeWidget *
glade_widget_new_from_widget_info (GladeWidgetInfo *info,
                                   GladeProject    *project,
                                   GladeWidget     *parent)
{
	GladeWidgetAdaptor *adaptor;
	GladeWidget        *widget;
	GList              *properties;
	
	g_return_val_if_fail (info != NULL, NULL);
	g_return_val_if_fail (project != NULL, NULL);

	if ((adaptor = 
	     glade_widget_adaptor_get_by_name (info->classname)) == NULL)
	{
		g_warning ("Widget class %s unknown.", info->classname);
		return NULL;
	}

	properties = glade_widget_properties_from_widget_info (adaptor, info);
	widget = glade_widget_adaptor_create_widget
		(adaptor, FALSE,
		 "name", info->name, 
		 "parent", parent, 
		 "project", project, 
		 "info", info, 
		 "properties", properties, 
		 "reason", GLADE_CREATE_LOAD, NULL);

	/* create the packing_properties list, without setting them */
	if (parent)
		widget->packing_properties =
			glade_widget_create_packing_properties (parent, widget);

	/* Load children first */
	glade_widget_fill_from_widget_info (info, widget, FALSE);

	/* Now sync custom props, things like "size" on GtkBox need
	 * this to be done afterwards.
	 */
	glade_widget_sync_custom_props (widget);

	return widget;
}

static GParameter *
glade_widget_info_params (GladeWidgetAdaptor *adaptor,
			  GladeWidgetInfo    *info,
			  gboolean            construct,
			  guint              *n_params)
{
	GladePropertyClass   *glade_property_class;
	GObjectClass         *oclass;
	GParamSpec          **pspec;
	GArray               *params;
	guint                 i, n_props;
	
	oclass = g_type_class_ref (adaptor->type);
	pspec  = g_object_class_list_properties (oclass, &n_props);
	params = g_array_new (FALSE, FALSE, sizeof (GParameter));

	/* prepare parameters that have glade_property_class->def */
	for (i = 0; i < n_props; i++)
	{
		GParameter  parameter = { 0, };
		GValue     *value;
		
		glade_property_class =
			glade_widget_adaptor_get_property_class (adaptor,
								 pspec[i]->name);
		if (glade_property_class == NULL  ||
		    glade_property_class->virt    ||
		    glade_property_class->ignore)
			continue;

		if (construct &&
		    (pspec[i]->flags & 
		     (G_PARAM_CONSTRUCT|G_PARAM_CONSTRUCT_ONLY)) == 0)
			continue;
		else if (!construct &&
			 (pspec[i]->flags & 
			  (G_PARAM_CONSTRUCT|G_PARAM_CONSTRUCT_ONLY)) != 0)
			continue;


		/* Try filling parameter with value from widget info.
		 */
		if ((value = glade_property_read (NULL, glade_property_class,
						  loading_project, info, FALSE)) != NULL)
		{
			parameter.name = pspec[i]->name;
			g_value_init (&parameter.value, pspec[i]->value_type);
			
			g_value_copy (value, &parameter.value);
			g_value_unset (value);
			g_free (value);

			g_array_append_val (params, parameter);
		}
	}
	g_free(pspec);

	g_type_class_unref (oclass);

	*n_params = params->len;
	return (GParameter *)g_array_free (params, FALSE);
}

/*******************************************************************************
                                     API
 *******************************************************************************/
GladeWidget *
glade_widget_get_from_gobject (gpointer object)
{
	g_return_val_if_fail (G_IS_OBJECT (object), NULL);
	
	return g_object_get_qdata (G_OBJECT (object), glade_widget_name_quark);
}

static void
glade_widget_debug_real (GladeWidget *widget, int indent)
{
	g_print ("%*sGladeWidget at %p\n", indent, "", widget);
	g_print ("%*sname = [%s]\n", indent, "", widget->name ? widget->name : "-");
	g_print ("%*sinternal = [%s]\n", indent, "", widget->internal ? widget->internal : "-");
	g_print ("%*sgobject = %p [%s]\n",
		 indent, "", widget->object, G_OBJECT_TYPE_NAME (widget->object));
	if (GTK_IS_WIDGET (widget->object))
		g_print ("%*sgtkwidget->parent = %p\n", indent, "",
			 gtk_widget_get_parent (GTK_WIDGET(widget->object)));
	if (GTK_IS_CONTAINER (widget->object)) {
		GList *children, *l;
		children = glade_util_container_get_all_children
			(GTK_CONTAINER (widget->object));
		for (l = children; l; l = l->next) {
			GtkWidget *widget_gtk = GTK_WIDGET (l->data);
			GladeWidget *widget = glade_widget_get_from_gobject (widget_gtk);
			if (widget) {
				glade_widget_debug_real (widget, indent + 2);
			} else if (GLADE_IS_PLACEHOLDER (widget_gtk)) {
				g_print ("%*sGtkWidget child %p is a placeholder.\n",
					 indent + 2, "", widget_gtk);
			} else {
				g_print ("%*sGtkWidget child %p [%s] has no glade widget.\n",
					 indent + 2, "",
					 widget_gtk, G_OBJECT_TYPE_NAME (widget_gtk));
			}
		}
		if (!children)
			g_print ("%*shas no children\n", indent, "");
		g_list_free (children);
	} else {
		g_print ("%*snot a container\n", indent, "");
	}
	g_print ("\n");
}

/**
 * glade_widget_debug:
 * @widget: a #GladeWidget
 *
 * Prints some information about a #GladeWidget, currently
 * this is unmaintained.
 */
static void
glade_widget_debug (GladeWidget *widget)
{
	glade_widget_debug_real (widget, 0);
}

static gboolean
glade_widget_show_idle (GladeWidget *widget)
{
	/* This could be dangerous */ 
	if (GLADE_IS_WIDGET (widget))
		glade_widget_show (widget);

	return FALSE;
}

/**
 * glade_widget_show:
 * @widget: A #GladeWidget
 *
 * Display @widget in it's project's GladeDesignView
 */
void
glade_widget_show (GladeWidget *widget)
{
	GladeDesignView *view;
	GtkWidget *layout;

	g_return_if_fail (GLADE_IS_WIDGET (widget));

	/* Position window at saved coordinates or in the center */
	if (GTK_IS_WINDOW (widget->object) && glade_widget_embed (widget))
	{
		view = glade_design_view_get_from_project (glade_widget_get_project (widget));
		layout = GTK_WIDGET (glade_design_view_get_layout (view));

		/* This case causes a black window */
		if (layout && !GTK_WIDGET_REALIZED (layout))
		{
			/* XXX Dangerous !!! give her a little kick */
			g_idle_add (glade_widget_show_idle, widget);
			return;
		}
		else if (!layout)
			return;
		
		if (gtk_bin_get_child (GTK_BIN (layout)) != NULL)
			gtk_container_remove (GTK_CONTAINER (layout), gtk_bin_get_child (GTK_BIN (layout)));
		
		gtk_container_add (GTK_CONTAINER (layout), GTK_WIDGET (widget->object));

		gtk_widget_show_all (GTK_WIDGET (widget->object));

	} else if (GTK_IS_WIDGET (widget->object))
	{
		gtk_widget_show_all (GTK_WIDGET (widget->object));
	}
	widget->visible = TRUE;
}

/**
 * glade_widget_hide:
 * @widget: A #GladeWidget
 *
 * Hide @widget
 */
void
glade_widget_hide (GladeWidget *widget)
{
	g_return_if_fail (GLADE_IS_WIDGET (widget));
	if (GTK_IS_WINDOW (widget->object))
	{
		/* Save coordinates */
		gtk_widget_hide (GTK_WIDGET (widget->object));
	}
	widget->visible = FALSE;
}

/**
 * glade_widget_add_prop_ref:
 * @widget: A #GladeWidget
 * @property: the #GladeProperty
 *
 * Adds @property to @widget 's list of referenced properties.
 *
 * Note: this is used to track object reference properties that
 *       go in and out of the project.
 */
void
glade_widget_add_prop_ref (GladeWidget *widget, GladeProperty *property)
{
	g_return_if_fail (GLADE_IS_WIDGET (widget));
	g_return_if_fail (GLADE_IS_PROPERTY (property));

	if (property && !widget->prop_refs_readonly &&
	    !g_list_find (widget->prop_refs, property))
		widget->prop_refs = g_list_prepend (widget->prop_refs, property);
}

/**
 * glade_widget_remove_prop_ref:
 * @widget: A #GladeWidget
 * @property: the #GladeProperty
 *
 * Removes @property from @widget 's list of referenced properties.
 *
 * Note: this is used to track object reference properties that
 *       go in and out of the project.
 */
void
glade_widget_remove_prop_ref (GladeWidget *widget, GladeProperty *property)
{
	g_return_if_fail (GLADE_IS_WIDGET (widget));
	g_return_if_fail (GLADE_IS_PROPERTY (property));

	if (!widget->prop_refs_readonly)
		widget->prop_refs = g_list_remove (widget->prop_refs, property);
}

/**
 * glade_widget_project_notify:
 * @widget: A #GladeWidget
 * @project: The #GladeProject (or %NULL)
 *
 * Notifies @widget that it is now in @project.
 *
 * Note that this doesnt really set the project; the project is saved
 * for internal reasons even when the widget is on the clipboard.
 * (also used for property references).
 */
void
glade_widget_project_notify (GladeWidget *widget, GladeProject *project)
{
	GList         *l;
	GladeProperty *property;

	g_return_if_fail (GLADE_IS_WIDGET (widget));

	/* Since glade_property_set() will try to modify list,
	 * we protect it with the 'prop_refs_readonly' flag.
	 */
	widget->prop_refs_readonly = TRUE;
	for (l = widget->prop_refs; l && l->data; l = l->next)
	{
		property = GLADE_PROPERTY (l->data);

		if (project != NULL && 
		    project == property->widget->project)
			glade_property_add_object (property, widget->object);
		else
			glade_property_remove_object (property, widget->object);
	}
	widget->prop_refs_readonly = FALSE;
}

static void
glade_widget_copy_signal_foreach (const gchar *key,
				  GPtrArray   *signals,
				  GladeWidget *dest)
{
	GladeSignal *signal;
	gint i;

	for (i = 0; i < signals->len; i++)
	{
		signal = (GladeSignal *)signals->pdata[i];
		glade_widget_add_signal_handler	(dest, signal);
	}
}

/**
 * glade_widget_copy_signals:
 * @widget:   a 'dest' #GladeWidget
 * @template_widget: a 'src' #GladeWidget
 *
 * Sets signals in @widget based on the values of
 * matching signals in @template_widget
 */
void
glade_widget_copy_signals (GladeWidget *widget,
			   GladeWidget *template_widget)
{
	g_return_if_fail (GLADE_IS_WIDGET (widget));
	g_return_if_fail (GLADE_IS_WIDGET (template_widget));

	g_hash_table_foreach (template_widget->signals,
			      (GHFunc)glade_widget_copy_signal_foreach,
			      widget);
}

/**
 * glade_widget_copy_properties:
 * @widget:   a 'dest' #GladeWidget
 * @template_widget: a 'src' #GladeWidget
 *
 * Sets properties in @widget based on the values of
 * matching properties in @template_widget
 */
void
glade_widget_copy_properties (GladeWidget *widget,
			      GladeWidget *template_widget)
{
	GList *l;

	g_return_if_fail (GLADE_IS_WIDGET (widget));
	g_return_if_fail (GLADE_IS_WIDGET (template_widget));

	for (l = widget->properties; l && l->data; l = l->next)
	{
		GladeProperty *widget_prop  = GLADE_PROPERTY(l->data);
		GladeProperty *template_prop;
	
		/* Check if they share the same class definition, different
		 * properties may have the same name (support for
		 * copying properties across "not-quite" compatible widget
		 * classes, like GtkImageMenuItem --> GtkCheckMenuItem).
		 */
		if ((template_prop =
		     glade_widget_get_property (template_widget, 
						widget_prop->klass->id)) != NULL &&
		    glade_property_class_match (template_prop->klass, widget_prop->klass))
			glade_property_set_value (widget_prop, template_prop->value);
	}
}

/**
 * glade_widget_add_child:
 * @parent: A #GladeWidget
 * @child: the #GladeWidget to add
 * @at_mouse: whether the added widget should be added
 *            at the current mouse position
 *
 * Adds @child to @parent in a generic way for this #GladeWidget parent.
 */
void
glade_widget_add_child (GladeWidget      *parent,
			GladeWidget      *child,
			gboolean          at_mouse)
{
	g_return_if_fail (GLADE_IS_WIDGET (parent));
	g_return_if_fail (GLADE_IS_WIDGET (child));

	GLADE_WIDGET_GET_CLASS (parent)->add_child (parent, child, at_mouse);
}

/**
 * glade_widget_remove_child:
 * @parent: A #GladeWidget
 * @child: the #GladeWidget to add
 *
 * Removes @child from @parent in a generic way for this #GladeWidget parent.
 */
void
glade_widget_remove_child (GladeWidget      *parent,
			   GladeWidget      *child)
{
	g_return_if_fail (GLADE_IS_WIDGET (parent));
	g_return_if_fail (GLADE_IS_WIDGET (child));

	GLADE_WIDGET_GET_CLASS (parent)->remove_child (parent, child);
}

/**
 * glade_widget_dup:
 * @template_widget: a #GladeWidget
 * @exact: whether or not to creat an exact duplicate
 *
 * Creates a deep copy of #GladeWidget. if @exact is specified,
 * the widget name is preserved and signals are carried over
 * (this is used to maintain names & signals in Cut/Paste context
 * as opposed to Copy/Paste contexts).
 *
 * Returns: The newly created #GladeWidget
 */
GladeWidget *
glade_widget_dup (GladeWidget *template_widget,
		  gboolean     exact)
{
	GladeWidget *widget;

	g_return_val_if_fail (GLADE_IS_WIDGET (template_widget), NULL);
	
	glade_widget_push_superuser ();
	widget = glade_widget_dup_internal (NULL, template_widget, exact);
	glade_widget_pop_superuser ();

	return widget;
}

/**
 * glade_widget_rebuild:
 * @gwidget: a #GladeWidget
 *
 * Replaces the current widget instance with
 * a new one while preserving all properties children and
 * takes care of reparenting.
 *
 */
void
glade_widget_rebuild (GladeWidget *gwidget)
{
	GObject            *new_object, *old_object;
	GladeWidgetAdaptor *adaptor;
	GList              *children;
	gboolean            reselect = FALSE, inproject;
	
	g_return_if_fail (GLADE_IS_WIDGET (gwidget));


	adaptor = gwidget->adaptor;

	/* Here we take care removing the widget from the project and
	 * the selection before rebuilding the instance.
	 */
	inproject = gwidget->project ?
		(glade_project_has_object
		 (gwidget->project, gwidget->object) ? TRUE : FALSE) : FALSE;

	if (inproject)
	{
		if (glade_project_is_selected (gwidget->project, 
					       gwidget->object))
		{
			reselect = TRUE;
			glade_project_selection_remove
				(gwidget->project, gwidget->object, FALSE);
		}
		glade_project_remove_object (gwidget->project, gwidget->object);
	}

	/* Extract and keep the child hierarchies aside... */
	children = glade_widget_extract_children (gwidget);

	/* Hold a reference to the old widget while we transport properties
	 * and children from it
	 */
	new_object = glade_widget_build_object(adaptor, gwidget, NULL);
	old_object = g_object_ref(glade_widget_get_object (gwidget));

	glade_widget_set_object (gwidget, new_object);

	/* Only call this once the object has a proper GladeWidget */
	glade_widget_adaptor_post_create (adaptor, new_object, GLADE_CREATE_REBUILD);

	/* Replace old object with new object in parent
	 */
	if (gwidget->parent)
		glade_widget_replace (gwidget->parent,
				      old_object, new_object);

	/* Reparent any children of the old object to the new object
	 * (this function will consume and free the child list).
	 */
	glade_widget_push_superuser ();
	glade_widget_insert_children (gwidget, children);
	glade_widget_pop_superuser ();
		
	/* Custom properties aren't transfered in build_object, since build_object
	 * is only concerned with object creation.
	 */
	glade_widget_sync_custom_props (gwidget);

	/* Sync packing.
	 */
	glade_widget_sync_packing_props (gwidget);
	
	if (g_type_is_a (adaptor->type, GTK_TYPE_WIDGET))
	{
		/* Must use gtk_widget_destroy here for cases like dialogs and toplevels
		 * (otherwise I'd prefer g_object_unref() )
		 */
		gtk_widget_destroy  (GTK_WIDGET(old_object));
	}
	else
		g_object_unref (old_object);

	/* If the widget was in a project (and maybe the selection), then
	 * restore that stuff.
	 */
	if (inproject)
	{
		glade_project_add_object (gwidget->project, NULL,
					  gwidget->object);
		if (reselect)
			glade_project_selection_add
				(gwidget->project, gwidget->object, TRUE);
	}

 	/* We shouldnt show if its not already visible */
	if (gwidget->visible)
		glade_widget_show (gwidget);
}

/**
 * glade_widget_add_signal_handler:
 * @widget: A #GladeWidget
 * @signal_handler: The #GladeSignal
 *
 * Adds a signal handler for @widget 
 */
void
glade_widget_add_signal_handler	(GladeWidget *widget, GladeSignal *signal_handler)
{
	g_return_if_fail (GLADE_IS_WIDGET (widget));

	g_signal_emit (widget, glade_widget_signals[ADD_SIGNAL_HANDLER], 0, signal_handler);
}


/**
 * glade_widget_remove_signal_handler:
 * @widget: A #GladeWidget
 * @signal_handler: The #GladeSignal
 *
 * Removes a signal handler from @widget 
 */
void
glade_widget_remove_signal_handler (GladeWidget *widget, GladeSignal *signal_handler)
{
	g_return_if_fail (GLADE_IS_WIDGET (widget));

	g_signal_emit (widget, glade_widget_signals[REMOVE_SIGNAL_HANDLER], 0, signal_handler);
}

/**
 * glade_widget_change_signal_handler:
 * @widget: A #GladeWidget
 * @old_signal_handler: the old #GladeSignal
 * @new_signal_handler: the new #GladeSignal
 *
 * Changes a #GladeSignal on @widget 
 */
void
glade_widget_change_signal_handler (GladeWidget *widget,
				    GladeSignal *old_signal_handler,
				    GladeSignal *new_signal_handler)
{
	g_return_if_fail (GLADE_IS_WIDGET (widget));

	g_signal_emit (widget, glade_widget_signals[CHANGE_SIGNAL_HANDLER], 0,
		       old_signal_handler, new_signal_handler);
}

/**
 * glade_widget_list_signal_handlers:
 * @widget: a #GladeWidget
 * @signal_name: the name of the signal
 *
 * Returns: A #GPtrArray of #GladeSignal for @signal_name
 */
GPtrArray *
glade_widget_list_signal_handlers (GladeWidget *widget,
				   const gchar *signal_name) /* array of GladeSignal* */
{
	g_return_val_if_fail (GLADE_IS_WIDGET (widget), NULL);
	return g_hash_table_lookup (widget->signals, signal_name);
}

/**
 * glade_widget_set_name:
 * @widget: a #GladeWidget
 * @name: a string
 *
 * Sets @widget's name to @name.
 */
void
glade_widget_set_name (GladeWidget *widget, const gchar *name)
{
	g_return_if_fail (GLADE_IS_WIDGET (widget));
	if (widget->name != name) {

		if (widget->project && 
		    glade_project_get_widget_by_name (widget->project, name))
		{
			/* print a warning ? */
			return;
		}

		if (widget->name)
			g_free (widget->name);
		widget->name = g_strdup (name);
		g_object_notify (G_OBJECT (widget), "name");
	}
}

/**
 * glade_widget_get_name:
 * @widget: a #GladeWidget
 *
 * Returns: a pointer to @widget's name
 */
const gchar *
glade_widget_get_name (GladeWidget *widget)
{
	g_return_val_if_fail (GLADE_IS_WIDGET (widget), NULL);
	return widget->name;
}

/**
 * glade_widget_set_internal:
 * @widget: A #GladeWidget
 * @internal: The internal name
 *
 * Sets the internal name of @widget to @internal
 */
void
glade_widget_set_internal (GladeWidget *widget, const gchar *internal)
{
	g_return_if_fail (GLADE_IS_WIDGET (widget));
	if (widget->internal != internal) {
		g_free (widget->internal);
		widget->internal = g_strdup (internal);
		g_object_notify (G_OBJECT (widget), "internal");
	}
}

/**
 * glade_widget_get_internal:
 * @widget: a #GladeWidget
 *
 * Returns: the internal name of @widget
 */
G_CONST_RETURN gchar *
glade_widget_get_internal (GladeWidget *widget)
{
	g_return_val_if_fail (GLADE_IS_WIDGET (widget), NULL);
	return widget->internal;
}

/**
 * glade_widget_get_adaptor:
 * @widget: a #GladeWidget
 *
 * Returns: the #GladeWidgetAdaptor of @widget
 */
GladeWidgetAdaptor *
glade_widget_get_adaptor (GladeWidget *widget)
{
	g_return_val_if_fail (GLADE_IS_WIDGET (widget), NULL);
	return widget->adaptor;
}

/**
 * glade_widget_set_project:
 * @widget: a #GladeWidget
 * @project: a #GladeProject
 *
 * Makes @widget belong to @project.
 */
void
glade_widget_set_project (GladeWidget *widget, GladeProject *project)
{
	if (widget->project != project)
	{
		widget->project = project;
		g_object_notify (G_OBJECT (widget), "project");
	}
}

/**
 * glade_widget_get_project:
 * @widget: a #GladeWidget
 * 
 * Returns: the #GladeProject that @widget belongs to
 */
GladeProject *
glade_widget_get_project (GladeWidget *widget)
{
	g_return_val_if_fail (GLADE_IS_WIDGET (widget), NULL);
	return widget->project;
}

/**
 * glade_widget_get_property:
 * @widget: a #GladeWidget
 * @id_property: a string naming a #GladeProperty
 *
 * Returns: the #GladeProperty in @widget named @id_property
 */
GladeProperty *
glade_widget_get_property (GladeWidget *widget, const gchar *id_property)
{
	static gchar   id_buffer[GPC_PROPERTY_NAMELEN] = { 0, };
	GList         *list;
	GladeProperty *property;

	g_return_val_if_fail (GLADE_IS_WIDGET (widget), NULL);
	g_return_val_if_fail (id_property != NULL, NULL);

	/* "-1" to always leave a trailing '\0' charachter */
	strncpy (id_buffer, id_property, GPC_PROPERTY_NAMELEN - 1);
	glade_util_replace (id_buffer, '_', '-');
		
	for (list = widget->properties; list; list = list->next) {
		property = list->data;
		if (strcmp (property->klass->id, id_buffer) == 0)
			return property;
	}
	return glade_widget_get_pack_property (widget, id_property);
}

/**
 * glade_widget_get_pack_property:
 * @widget: a #GladeWidget
 * @id_property: a string naming a #GladeProperty
 *
 * Returns: the #GladeProperty in @widget named @id_property
 */
GladeProperty *
glade_widget_get_pack_property (GladeWidget *widget, const gchar *id_property)
{
	static gchar   id_buffer[GPC_PROPERTY_NAMELEN] = { 0, };
	GList         *list;
	GladeProperty *property;

	g_return_val_if_fail (GLADE_IS_WIDGET (widget), NULL);
	g_return_val_if_fail (id_property != NULL, NULL);

	/* "-1" to always leave a trailing '\0' charachter */
	strncpy (id_buffer, id_property, GPC_PROPERTY_NAMELEN - 1);
	glade_util_replace (id_buffer, '_', '-');

	for (list = widget->packing_properties; list; list = list->next) {
		property = list->data;
		if (strcmp (property->klass->id, id_buffer) == 0)
			return property;
	}
	return NULL;
}


/**
 * glade_widget_property_get:
 * @widget: a #GladeWidget
 * @id_property: a string naming a #GladeProperty
 * @...: The return location for the value of the said #GladeProperty
 *
 * Gets the value of @id_property in @widget
 *
 * Returns: whether @id_property was found or not.
 */
gboolean
glade_widget_property_get (GladeWidget      *widget,
			   const gchar      *id_property,
			   ...)
{
	GladeProperty *property;
	va_list        vl;
	
	g_return_val_if_fail (GLADE_IS_WIDGET (widget), FALSE);
	g_return_val_if_fail (id_property != NULL, FALSE);

	if ((property = glade_widget_get_property (widget, id_property)) != NULL)
	{
		va_start (vl, id_property);
		glade_property_get_va_list (property, vl);
		va_end (vl);
		return TRUE;
	}
	return FALSE;
}

/**
 * glade_widget_property_set:
 * @widget: a #GladeWidget
 * @id_property: a string naming a #GladeProperty
 * @...: A value of the correct type for the said #GladeProperty
 *
 * Sets the value of @id_property in @widget
 *
 * Returns: whether @id_property was found or not.
 */
gboolean
glade_widget_property_set (GladeWidget      *widget,
			   const gchar      *id_property,
			   ...)
{
	GladeProperty *property;
	va_list        vl;
	
	g_return_val_if_fail (GLADE_IS_WIDGET (widget), FALSE);
	g_return_val_if_fail (id_property != NULL, FALSE);

	if ((property = glade_widget_get_property (widget, id_property)) != NULL)
	{
		va_start (vl, id_property);
		glade_property_set_va_list (property, vl);
		va_end (vl);
		return TRUE;
	}
	return FALSE;
}

/**
 * glade_widget_pack_property_get:
 * @widget: a #GladeWidget
 * @id_property: a string naming a #GladeProperty
 * @...: The return location for the value of the said #GladeProperty
 *
 * Gets the value of @id_property in @widget packing properties
 *
 * Returns: whether @id_property was found or not.
 */
gboolean
glade_widget_pack_property_get (GladeWidget      *widget,
				const gchar      *id_property,
				...)
{
	GladeProperty *property;
	va_list        vl;
	
	g_return_val_if_fail (GLADE_IS_WIDGET (widget), FALSE);
	g_return_val_if_fail (id_property != NULL, FALSE);

	if ((property = glade_widget_get_pack_property (widget, id_property)) != NULL)
	{
		va_start (vl, id_property);
		glade_property_get_va_list (property, vl);
		va_end (vl);
		return TRUE;
	}
	return FALSE;
}

/**
 * glade_widget_pack_property_set:
 * @widget: a #GladeWidget
 * @id_property: a string naming a #GladeProperty
 * @...: The return location for the value of the said #GladeProperty
 *
 * Sets the value of @id_property in @widget packing properties
 *
 * Returns: whether @id_property was found or not.
 */
gboolean
glade_widget_pack_property_set (GladeWidget      *widget,
				const gchar      *id_property,
				...)
{
	GladeProperty *property;
	va_list        vl;
	
	g_return_val_if_fail (GLADE_IS_WIDGET (widget), FALSE);
	g_return_val_if_fail (id_property != NULL, FALSE);

	if ((property = glade_widget_get_pack_property (widget, id_property)) != NULL)
	{
		va_start (vl, id_property);
		glade_property_set_va_list (property, vl);
		va_end (vl);
		return TRUE;
	}
	return FALSE;
}

/**
 * glade_widget_property_set_sensitive:
 * @widget: a #GladeWidget
 * @id_property: a string naming a #GladeProperty
 * @sensitive: setting sensitive or insensitive
 * @reason: a description of why the user cant edit this property
 *          which will be used as a tooltip
 *
 * Sets the sensitivity of @id_property in @widget
 *
 * Returns: whether @id_property was found or not.
 */
gboolean
glade_widget_property_set_sensitive (GladeWidget      *widget,
				     const gchar      *id_property,
				     gboolean          sensitive,
				     const gchar      *reason)
{
	GladeProperty *property;
	
	g_return_val_if_fail (GLADE_IS_WIDGET (widget), FALSE);
	g_return_val_if_fail (id_property != NULL, FALSE);

	if ((property = glade_widget_get_property (widget, id_property)) != NULL)
	{
		glade_property_set_sensitive (property, sensitive, reason);
		return TRUE;
	}
	return FALSE;
}

/**
 * glade_widget_pack_property_set_sensitive:
 * @widget: a #GladeWidget
 * @id_property: a string naming a #GladeProperty
 * @sensitive: setting sensitive or insensitive
 * @reason: a description of why the user cant edit this property
 *          which will be used as a tooltip
 *
 * Sets the sensitivity of @id_property in @widget's packing properties.
 *
 * Returns: whether @id_property was found or not.
 */
gboolean
glade_widget_pack_property_set_sensitive (GladeWidget      *widget,
					  const gchar      *id_property,
					  gboolean          sensitive,
					  const gchar      *reason)
{
	GladeProperty *property;
	
	g_return_val_if_fail (GLADE_IS_WIDGET (widget), FALSE);
	g_return_val_if_fail (id_property != NULL, FALSE);

	if ((property = glade_widget_get_pack_property (widget, id_property)) != NULL)
	{
		glade_property_set_sensitive (property, sensitive, reason);
		return TRUE;
	}
	return FALSE;
}


/**
 * glade_widget_property_set_enabled:
 * @widget: a #GladeWidget
 * @id_property: a string naming a #GladeProperty
 * @enabled: setting enabled or disabled
 *
 * Sets the enabled state of @id_property in @widget; this is
 * used for optional properties.
 *
 * Returns: whether @id_property was found or not.
 */
gboolean
glade_widget_property_set_enabled (GladeWidget      *widget,
				   const gchar      *id_property,
				   gboolean          enabled)
{
	GladeProperty *property;
	
	g_return_val_if_fail (GLADE_IS_WIDGET (widget), FALSE);
	g_return_val_if_fail (id_property != NULL, FALSE);

	if ((property = glade_widget_get_property (widget, id_property)) != NULL)
	{
		glade_property_set_enabled (property, enabled);
		return TRUE;
	}
	return FALSE;
}

/**
 * glade_widget_pack_property_set_enabled:
 * @widget: a #GladeWidget
 * @id_property: a string naming a #GladeProperty
 * @enabled: setting enabled or disabled
 *
 * Sets the enabled state of @id_property in @widget's packing 
 * properties; this is used for optional properties.
 *
 * Returns: whether @id_property was found or not.
 */
gboolean
glade_widget_pack_property_set_enabled (GladeWidget      *widget,
					const gchar      *id_property,
					gboolean          enabled)
{
	GladeProperty *property;
	
	g_return_val_if_fail (GLADE_IS_WIDGET (widget), FALSE);
	g_return_val_if_fail (id_property != NULL, FALSE);

	if ((property = glade_widget_get_pack_property (widget, id_property)) != NULL)
	{
		glade_property_set_enabled (property, enabled);
		return TRUE;
	}
	return FALSE;
}

/**
 * glade_widget_property_set_save_always:
 * @widget: a #GladeWidget
 * @id_property: a string naming a #GladeProperty
 * @setting: the setting 
 *
 * Sets whether @id_property in @widget should be special cased
 * to always be saved regardless of its default value.
 * (used for some special cases like properties
 * that are assigned initial values in composite widgets
 * or derived widget code).
 *
 * Returns: whether @id_property was found or not.
 */
gboolean
glade_widget_property_set_save_always (GladeWidget      *widget,
				       const gchar      *id_property,
				       gboolean          setting)
{
	GladeProperty *property;
	
	g_return_val_if_fail (GLADE_IS_WIDGET (widget), FALSE);
	g_return_val_if_fail (id_property != NULL, FALSE);

	if ((property = glade_widget_get_property (widget, id_property)) != NULL)
	{
		glade_property_set_save_always (property, setting);
		return TRUE;
	}
	return FALSE;
}

/**
 * glade_widget_pack_property_set_save_always:
 * @widget: a #GladeWidget
 * @id_property: a string naming a #GladeProperty
 * @setting: the setting 
 *
 * Sets whether @id_property in @widget should be special cased
 * to always be saved regardless of its default value.
 * (used for some special cases like properties
 * that are assigned initial values in composite widgets
 * or derived widget code).
 *
 * Returns: whether @id_property was found or not.
 */
gboolean
glade_widget_pack_property_set_save_always (GladeWidget      *widget,
					    const gchar      *id_property,
					    gboolean          setting)
{
	GladeProperty *property;
	
	g_return_val_if_fail (GLADE_IS_WIDGET (widget), FALSE);
	g_return_val_if_fail (id_property != NULL, FALSE);

	if ((property = glade_widget_get_pack_property (widget, id_property)) != NULL)
	{
		glade_property_set_save_always (property, setting);
		return TRUE;
	}
	return FALSE;
}

/**
 * glade_widget_property_string:
 * @widget: a #GladeWidget
 * @id_property: a string naming a #GladeProperty
 * @value: the #GValue to print or %NULL
 *
 * Creates a printable string representing @id_property in
 * @widget, if @value is specified it will be used in place
 * of @id_property's real value (this is a convinience
 * function to print/debug properties usually from plugin
 * backends).
 *
 * Returns: A newly allocated string representing @id_property
 */
gchar *
glade_widget_property_string (GladeWidget      *widget,
			      const gchar      *id_property,
			      const GValue     *value)
{
	GladeProperty *property;
	gchar         *ret_string = NULL;
	
	g_return_val_if_fail (GLADE_IS_WIDGET (widget), NULL);
	g_return_val_if_fail (id_property != NULL, NULL);

	if ((property = glade_widget_get_property (widget, id_property)) != NULL)
		ret_string =
			glade_property_class_make_string_from_gvalue (property->klass,
								      value ? value : property->value);

	return ret_string;
}

/**
 * glade_widget_pack_property_string:
 * @widget: a #GladeWidget
 * @id_property: a string naming a #GladeProperty
 * @value: the #GValue to print or %NULL
 *
 * Same as glade_widget_property_string() but for packing
 * properties.
 *
 * Returns: A newly allocated string representing @id_property
 */
gchar *
glade_widget_pack_property_string (GladeWidget      *widget,
				   const gchar      *id_property,
				   const GValue     *value)
{
	GladeProperty *property;
	gchar         *ret_string = NULL;
	
	g_return_val_if_fail (GLADE_IS_WIDGET (widget), NULL);
	g_return_val_if_fail (id_property != NULL, NULL);

	if ((property = glade_widget_get_pack_property (widget, id_property)) != NULL)
		ret_string =
			glade_property_class_make_string_from_gvalue (property->klass,
								      value ? value : property->value);

	return ret_string;
}


/**
 * glade_widget_property_reset:
 * @widget: a #GladeWidget
 * @id_property: a string naming a #GladeProperty
 *
 * Resets @id_property in @widget to it's default value
 *
 * Returns: whether @id_property was found or not.
 */
gboolean
glade_widget_property_reset (GladeWidget   *widget,
			     const gchar   *id_property)
{
	GladeProperty *property;
	
	g_return_val_if_fail (GLADE_IS_WIDGET (widget), FALSE);

	if ((property = glade_widget_get_property (widget, id_property)) != NULL)
	{
		glade_property_reset (property);
		return TRUE;
	}
	return FALSE;
}

/**
 * glade_widget_pack_property_reset:
 * @widget: a #GladeWidget
 * @id_property: a string naming a #GladeProperty
 *
 * Resets @id_property in @widget's packing properties to it's default value
 *
 * Returns: whether @id_property was found or not.
 */
gboolean      
glade_widget_pack_property_reset (GladeWidget   *widget,
				  const gchar   *id_property)
{
	GladeProperty *property;
	
	g_return_val_if_fail (GLADE_IS_WIDGET (widget), FALSE);

	if ((property = glade_widget_get_pack_property (widget, id_property)) != NULL)
	{
		glade_property_reset (property);
		return TRUE;
	}
	return FALSE;
}

static gboolean
glade_widget_property_default_common (GladeWidget *widget,
			       const gchar *id_property, gboolean original)
{
	GladeProperty *property;
	
	g_return_val_if_fail (GLADE_IS_WIDGET (widget), FALSE);

	if ((property = glade_widget_get_property (widget, id_property)) != NULL)
		return (original) ? glade_property_original_default (property) :
			glade_property_default (property);

	return FALSE;
}

/**
 * glade_widget_property_default:
 * @widget: a #GladeWidget
 * @id_property: a string naming a #GladeProperty
 *
 * Returns: whether whether @id_property was found and is 
 * currently set to it's default value.
 */
gboolean
glade_widget_property_default (GladeWidget *widget,
			       const gchar *id_property)
{
	return glade_widget_property_default_common (widget, id_property, FALSE);
}

/**
 * glade_widget_property_original_default:
 * @widget: a #GladeWidget
 * @id_property: a string naming a #GladeProperty
 *
 * Returns: whether whether @id_property was found and is 
 * currently set to it's original default value.
 */
gboolean
glade_widget_property_original_default (GladeWidget *widget,
			       const gchar *id_property)
{
	return glade_widget_property_default_common (widget, id_property, TRUE);
}

/**
 * glade_widget_pack_property_default:
 * @widget: a #GladeWidget
 * @id_property: a string naming a #GladeProperty
 *
 * Returns: whether whether @id_property was found and is 
 * currently set to it's default value.
 */
gboolean 
glade_widget_pack_property_default (GladeWidget *widget,
				    const gchar *id_property)
{
	GladeProperty *property;
	
	g_return_val_if_fail (GLADE_IS_WIDGET (widget), FALSE);

	if ((property = glade_widget_get_pack_property (widget, id_property)) != NULL)
		return glade_property_default (property);

	return FALSE;
}


/**
 * glade_widget_object_set_property:
 * @widget:        A #GladeWidget
 * @property_name: The property identifier
 * @value:         The #GValue
 *
 * This function applies @value to the property @property_name on
 * the runtime object of @widget.
 */
void
glade_widget_object_set_property (GladeWidget      *widget,
				  const gchar      *property_name,
				  const GValue     *value)
{
	g_return_if_fail (GLADE_IS_WIDGET (widget));
	g_return_if_fail (property_name != NULL && value != NULL);

	glade_widget_adaptor_set_property (widget->adaptor,
					   widget->object,
					   property_name, value);
}


/**
 * glade_widget_object_get_property:
 * @widget:        A #GladeWidget
 * @property_name: The property identifier
 * @value:         The #GValue
 *
 * This function retrieves the value of the property @property_name on
 * the runtime object of @widget and sets it in @value.
 */
void
glade_widget_object_get_property (GladeWidget      *widget,
				  const gchar      *property_name,
				  GValue           *value)
{
	g_return_if_fail (GLADE_IS_WIDGET (widget));
	g_return_if_fail (property_name != NULL && value != NULL);

	glade_widget_adaptor_get_property (widget->adaptor,
					   widget->object,
					   property_name, value);
}

/**
 * glade_widget_child_set_property:
 * @widget:        A #GladeWidget
 * @child:         The #GladeWidget child
 * @property_name: The id of the property
 * @value:         The @GValue
 *
 * Sets @child's packing property identified by @property_name to @value.
 */
void
glade_widget_child_set_property (GladeWidget      *widget,
				 GladeWidget      *child,
				 const gchar      *property_name,
				 const GValue     *value)
{
	g_return_if_fail (GLADE_IS_WIDGET (widget));
	g_return_if_fail (GLADE_IS_WIDGET (child));
	g_return_if_fail (property_name != NULL && value != NULL);

	glade_widget_adaptor_child_set_property (widget->adaptor,
						 widget->object,
						 child->object,
						 property_name, value);
}

/**
 * glade_widget_child_get_property:
 * @widget:        A #GladeWidget
 * @child:         The #GladeWidget child
 * @property_name: The id of the property
 * @value:         The @GValue
 *
 * Gets @child's packing property identified by @property_name.
 */
void
glade_widget_child_get_property (GladeWidget      *widget,
				 GladeWidget      *child,
				 const gchar      *property_name,
				 GValue           *value)
{
	g_return_if_fail (GLADE_IS_WIDGET (widget));
	g_return_if_fail (GLADE_IS_WIDGET (child));
	g_return_if_fail (property_name != NULL && value != NULL);

	glade_widget_adaptor_child_get_property (widget->adaptor,
						 widget->object,
						 child->object,
						 property_name, value);

}

static gboolean 
glade_widget_event_private (GtkWidget   *widget,
			    GdkEvent    *event,
			    GladeWidget *gwidget)
{
	GtkWidget *layout = widget;

	/* Find the parenting layout container */
	while (layout && !GLADE_IS_DESIGN_LAYOUT (layout))
		layout = layout->parent;

	/* Event outside the logical heirarchy, could be a menuitem
	 * or other such popup window, we'll presume to send it directly
	 * to the GladeWidget that connected here.
	 */
	if (!layout)
		return glade_widget_event (gwidget, event);

	/* Let the parenting GladeDesignLayout decide which GladeWidget to
	 * marshall this event to.
	 */
	if (GLADE_IS_DESIGN_LAYOUT (layout))
		return glade_design_layout_widget_event (GLADE_DESIGN_LAYOUT (layout),
							 gwidget, event);
	else
		return FALSE;
}

/**
 * glade_widget_set_object:
 * @gwidget: A #GladeWidget
 * @new_object: the new #GObject for @gwidget
 *
 * Set the runtime object for this GladeWidget wrapper
 * (this is used deep in the core and is probably unsafe
 * to use elsewhere).
 */
void
glade_widget_set_object (GladeWidget *gwidget, GObject *new_object)
{
	GladeWidgetAdaptor *adaptor;
	GObject            *old_object;
	
	g_return_if_fail (GLADE_IS_WIDGET (gwidget));
	g_return_if_fail (G_IS_OBJECT     (new_object));
	g_return_if_fail (gwidget->adaptor);
	g_return_if_fail (g_type_is_a (G_OBJECT_TYPE (new_object),
				       gwidget->adaptor->type));

	adaptor    = gwidget->adaptor;
	old_object = gwidget->object;
	
	/* Add internal reference to new widget if its not internal */
	if (gwidget->internal)
		gwidget->object = G_OBJECT(new_object);
	else
		gwidget->object = g_object_ref (G_OBJECT(new_object));
	
	g_object_set_qdata (G_OBJECT (new_object), glade_widget_name_quark, gwidget);

	if (g_type_is_a (gwidget->adaptor->type, GTK_TYPE_WIDGET))
	{
		/* Disable any built-in DnD
		 */
		gtk_drag_dest_unset (GTK_WIDGET (new_object));
		gtk_drag_source_unset (GTK_WIDGET (new_object));

		/* Take care of drawing selection directly on widgets
		 * for the time being
		 */
		glade_widget_connect_signal_handlers
			(GTK_WIDGET(new_object),
			 G_CALLBACK (glade_widget_event_private),
			 gwidget);
	}

	/* Remove internal reference to old widget */
	if (gwidget->internal == NULL && old_object) {
		g_object_set_qdata (G_OBJECT (old_object), glade_widget_name_quark, NULL);
		g_object_unref (G_OBJECT (old_object));
	}
	g_object_notify (G_OBJECT (gwidget), "object");
}

/**
 * glade_widget_get_object:
 * @widget: a #GladeWidget
 *
 * Returns: the #GObject associated with @widget
 */
GObject *
glade_widget_get_object (GladeWidget *widget)
{
	g_return_val_if_fail (GLADE_IS_WIDGET (widget), NULL);
	return widget->object;
}

/**
 * glade_widget_get_parent:
 * @widget: A #GladeWidget
 *
 * Returns: The parenting #GladeWidget
 */
GladeWidget *
glade_widget_get_parent (GladeWidget *widget)
{
	g_return_val_if_fail (GLADE_IS_WIDGET (widget), NULL);
	return widget->parent;
}

/**
 * glade_widget_set_parent:
 * @widget: A #GladeWidget
 * @parent: the parenting #GladeWidget (or %NULL)
 *
 * sets the parenting #GladeWidget
 */
void
glade_widget_set_parent (GladeWidget *widget,
			 GladeWidget *parent)
{
	GladeWidget *old_parent;

	g_return_if_fail (GLADE_IS_WIDGET (widget));

	old_parent     = widget->parent;
	widget->parent = parent;

	/* Set packing props only if the object is actually parented by 'parent'
	 * (a subsequent call should come from glade_command after parenting).
	 */
	if (widget->object && parent != NULL &&
	    glade_widget_adaptor_has_child 
	    (parent->adaptor, parent->object, widget->object))
	{
		if (old_parent == NULL || widget->packing_properties == NULL ||
		    old_parent->adaptor->type != parent->adaptor->type)
			glade_widget_set_packing_properties (widget, parent);
		else
			glade_widget_sync_packing_props (widget);
	}
	
	if (parent) glade_widget_set_packing_actions (widget, parent);

	g_object_notify (G_OBJECT (widget), "parent");
}

/**
 * glade_widget_set_packing_properties:
 * @widget:     A #GladeWidget
 * @container:  The parent #GladeWidget
 *
 * Generates the packing_properties list of the widget, given
 * the class of the container we are adding the widget to.
 * If the widget already has packing_properties, but the container
 * has changed, the current list is freed and replaced.
 */
void
glade_widget_set_packing_properties (GladeWidget *widget,
				     GladeWidget *container)
{
	GList *list;

	g_return_if_fail (GLADE_IS_WIDGET (widget));
	g_return_if_fail (GLADE_IS_WIDGET (container));

	g_list_foreach (widget->packing_properties, (GFunc)g_object_unref, NULL);
	g_list_free (widget->packing_properties);
	widget->packing_properties = NULL;

	/* We have to detect whether this is an anarchist child of a composite
	 * widget or not, in otherwords; whether its really a direct child or
	 * a child of a popup window created on the composite widget's behalf.
	 */
	if (widget->anarchist) return;

	widget->packing_properties = glade_widget_create_packing_properties (container, widget);

	/* Dont introspect on properties that are not parented yet.
	 */
	if (glade_widget_adaptor_has_child (container->adaptor,
					    container->object,
					    widget->object))
	{
		glade_widget_set_default_packing_properties (container, widget);

		/* update the values of the properties to the ones we get from gtk */
		for (list = widget->packing_properties;
		     list && list->data;
		     list = list->next)
		{
			GladeProperty *property = list->data;
			g_value_reset (property->value);
			glade_widget_child_get_property 
				(container,
				 widget, property->klass->id, property->value);
		}
	}
}

/**
 * glade_widget_has_decendant:
 * @widget: a #GladeWidget
 * @type: a  #GType
 * 
 * Returns: whether this GladeWidget has any decendants of type @type
 *          or any decendants that implement the @type interface
 */
gboolean
glade_widget_has_decendant (GladeWidget *widget, GType type)
{
	GladeWidget   *child;
	GList         *children, *l;
	gboolean       found = FALSE;

	if (G_TYPE_IS_INTERFACE (type) &&
	    glade_util_class_implements_interface
	    (widget->adaptor->type, type))
		return TRUE;
	else if (G_TYPE_IS_INTERFACE (type) == FALSE &&
		 g_type_is_a (widget->adaptor->type, type))
		return TRUE;

	if ((children = glade_widget_adaptor_get_children
	     (widget->adaptor, widget->object)) != NULL)
	{
		for (l = children; l; l = l->next)
			if ((child = glade_widget_get_from_gobject (l->data)) != NULL &&
			    (found = glade_widget_has_decendant (child, type)))
				break;
		g_list_free (children);
	}	
	return found;
}

/**
 * glade_widget_replace:
 * @old_object: a #GObject
 * @new_object: a #GObject
 * 
 * Replaces a GObject with another GObject inside a GObject which
 * behaves as a container.
 *
 * Note that both GObjects must be owned by a GladeWidget.
 */
void
glade_widget_replace (GladeWidget *parent, GObject *old_object, GObject *new_object)
{
	g_return_if_fail (G_IS_OBJECT (old_object));
	g_return_if_fail (G_IS_OBJECT (new_object));

	GLADE_WIDGET_GET_CLASS (parent)->replace_child (parent, old_object, new_object);
}

/* XML Serialization */
static gboolean
glade_widget_write_child (GArray *children, GladeWidget *parent, GObject *object, GladeInterface *interface);

typedef struct _WriteSignalsContext
{
	GladeInterface *interface;
	GArray *signals;
} WriteSignalsContext;

static void
glade_widget_write_signals (gpointer key, gpointer value, gpointer user_data)
{
	WriteSignalsContext *write_signals_context;
        GPtrArray *signals;
	guint i;

	write_signals_context = (WriteSignalsContext *) user_data;
	signals = (GPtrArray *) value;
	for (i = 0; i < signals->len; i++)
	{
		GladeSignal *signal = g_ptr_array_index (signals, i);
		GladeSignalInfo signalinfo;

		glade_signal_write (&signalinfo, signal,
				    write_signals_context->interface);
		g_array_append_val (write_signals_context->signals, 
				    signalinfo);
	}
}

/**
 * glade_widget_write:
 * @widget: a #GladeWidget
 * @interface: a #GladeInterface
 *
 * TODO: write me
 *
 * Returns: 
 */
GladeWidgetInfo*
glade_widget_write (GladeWidget *widget, GladeInterface *interface)
{
	WriteSignalsContext write_signals_context;
	GladeWidgetInfo *info;
	GArray *props, *atk_props, *atk_actions, *atk_relations, *accels, *children;
	GList *list;

	g_return_val_if_fail (GLADE_IS_WIDGET (widget), NULL);

	info = g_new0 (GladeWidgetInfo, 1);

	info->classname = glade_xml_alloc_string (interface, widget->adaptor->name);
	info->name      = glade_xml_alloc_string (interface, widget->name);

	/* Write the properties */
	props         = g_array_new (FALSE, FALSE, sizeof (GladePropInfo));
	atk_props     = g_array_new (FALSE, FALSE, sizeof (GladePropInfo));
	atk_relations = g_array_new (FALSE, FALSE, sizeof (GladeAtkRelationInfo));
	atk_actions   = g_array_new (FALSE, FALSE, sizeof (GladeAtkActionInfo));
	accels        = g_array_new (FALSE, FALSE, sizeof (GladeAccelInfo));

	for (list = widget->properties; list; list = list->next)
	{
		GladeProperty *property = list->data;

		/* This should never happen */
		if (property->klass->packing)
			continue;

		switch (property->klass->type)
		{
		case GPC_NORMAL:
			glade_property_write (property, interface, props);
			break;
		case GPC_ATK_PROPERTY:
			glade_property_write (property, interface, atk_props);
			break;
		case GPC_ATK_RELATION:
			glade_property_write (property, interface, atk_relations);
			break;
		case GPC_ATK_ACTION:
			glade_property_write (property, interface, atk_actions);
			break;
		case GPC_ACCEL_PROPERTY:
			glade_property_write (property, interface, accels);
			break;
		default:
			break;
		}
	}

	/* Properties */
	info->properties = (GladePropInfo *) props->data;
	info->n_properties = props->len;
	g_array_free(props, FALSE);

	/* Atk Properties */
	info->atk_props = (GladePropInfo *) atk_props->data;
	info->n_atk_props = atk_props->len;
	g_array_free(atk_props, FALSE);

	/* Atk Relations */
	info->relations = (GladeAtkRelationInfo *) atk_relations->data;
	info->n_relations = atk_relations->len;
	g_array_free(atk_relations, FALSE);

	/* Atk Actions */
	info->atk_actions = (GladeAtkActionInfo *) atk_actions->data;
	info->n_atk_actions = atk_actions->len;
	g_array_free(atk_actions, FALSE);

	/* Accels */
	info->accels = (GladeAccelInfo *) accels->data;
	info->n_accels = accels->len;
	g_array_free(accels, FALSE);

	/* Signals */
	write_signals_context.interface = interface;
	write_signals_context.signals = g_array_new (FALSE, FALSE,
	                                              sizeof (GladeSignalInfo));
	g_hash_table_foreach (widget->signals,
			      glade_widget_write_signals,
			      &write_signals_context);
	info->signals = (GladeSignalInfo *)
				write_signals_context.signals->data;
	info->n_signals = write_signals_context.signals->len;
	g_array_free (write_signals_context.signals, FALSE);

	/* Children */
	if ((list =
	     glade_widget_adaptor_get_children (widget->adaptor,
						widget->object)) != NULL)
	{
		children = g_array_new (FALSE, FALSE, sizeof (GladeChildInfo));
		while (list && list->data)
		{
			GObject *child = list->data;
			glade_widget_write_child (children, widget, child, interface);
			list = list->next;
		}
		info->children   = (GladeChildInfo *) children->data;
		info->n_children = children->len;
		
		g_array_free (children, FALSE);
		g_list_free (list);
	}
	g_hash_table_insert(interface->names, info->name, info);

	return info;
}


static gboolean
glade_widget_write_special_child_prop (GArray *props, 
				       GladeWidget *parent, 
				       GObject *object, 
				       GladeInterface *interface)
{
	GladePropInfo         info = { 0 };
	gchar                *buff, *special_child_type;

	buff = g_object_get_data (object, "special-child-type");

	g_object_get (parent->adaptor, "special-child-type", &special_child_type, NULL);

	if (special_child_type && buff)
	{
		info.name  = glade_xml_alloc_propname (interface, 
						       special_child_type);
		info.value = glade_xml_alloc_string (interface, buff);
		g_array_append_val (props, info);

		g_free (special_child_type);
		return TRUE;
	}
	g_free (special_child_type);
	return FALSE;
}

gboolean
glade_widget_write_child (GArray         *children, 
			  GladeWidget    *parent, 
			  GObject        *object,
			  GladeInterface *interface)
{
	GladeChildInfo info = { 0 };
	GladeWidget *child_widget;
	GList *list;
	GArray *props;

	if (GLADE_IS_PLACEHOLDER (object))
	{
		props = g_array_new (FALSE, FALSE,
				     sizeof (GladePropInfo));
		/* Here we have to add the "special-child-type" packing property */
		glade_widget_write_special_child_prop (props, parent, 
						       object, interface);

		info.properties = (GladePropInfo *) props->data;
		info.n_properties = props->len;
		g_array_free(props, FALSE);

		g_array_append_val (children, info);

		return TRUE;
	}

	child_widget = glade_widget_get_from_gobject (object);
	if (!child_widget)
		return FALSE;
	
	if (child_widget->internal)
		info.internal_child = glade_xml_alloc_string(interface, child_widget->internal);

	info.child = glade_widget_write (child_widget, interface);
	if (!info.child)
	{
		g_warning ("Failed to write child widget");
		return FALSE;
	}

	/* Append the packing properties */
	props = g_array_new (FALSE, FALSE, sizeof (GladePropInfo));
	
	/* Here we have to add the "special-child-type" packing property */
	glade_widget_write_special_child_prop (props, parent, 
					       child_widget->object,
					       interface);

	if (child_widget->packing_properties != NULL) 
	{
		for (list = child_widget->packing_properties;
		     list; list = list->next) 
		{
			GladeProperty *property;

			property = list->data;
			g_assert (property->klass->packing != FALSE);
			glade_property_write (property, interface, props);
		}
	}

	info.properties = (GladePropInfo *) props->data;
	info.n_properties = props->len;
	g_array_free(props, FALSE);
	
	g_array_append_val (children, info);

	return TRUE;
}

/**
 * glade_widget_read:
 * @project: a #GladeProject
 * @info: a #GladeWidgetInfo
 *
 * Returns: a new #GladeWidget for @project, based on @info
 */
GladeWidget *
glade_widget_read (GladeProject *project, GladeWidgetInfo *info)
{
	GladeWidget *widget;

	glade_widget_push_superuser ();
	loading_project = project;
		
	if ((widget = glade_widget_new_from_widget_info
	     (info, project, NULL)) != NULL)
	{
#if 0
		if (glade_verbose)
			glade_widget_debug (widget);
#endif
	}	

	loading_project = NULL;
	glade_widget_pop_superuser ();

	return widget;
}

static gint glade_widget_su_stack = 0;

/**
 * glade_widget_superuser:
 *
 * Checks if we are in superuser mode.
 *
 * Superuser mode is when we are
 *   - Loading a project
 *   - Dupping a widget recursively
 *   - Rebuilding an instance for a construct-only property
 *
 * In these cases, we must act like a load, this should be checked
 * from the plugin when implementing containers, when undo/redo comes
 * around, the plugin is responsable for maintaining the same container
 * size when widgets are added/removed.
 */
gboolean
glade_widget_superuser (void)
{
	return glade_widget_su_stack > 0;
}

/**
 * glade_widget_push_superuser:
 *
 * Sets superuser mode
 */
void
glade_widget_push_superuser (void)
{
	glade_property_push_superuser ();
	glade_widget_su_stack++;
}


/**
 * glade_widget_pop_superuser:
 *
 * Unsets superuser mode
 */
void
glade_widget_pop_superuser (void)
{
	if (--glade_widget_su_stack < 0)
	{
		g_critical ("Bug: widget super user stack is corrupt.\n");
	}
	glade_property_pop_superuser ();
}


/**
 * glade_widget_placeholder_relation:
 * @parent: A #GladeWidget
 * @widget: The child #GladeWidget
 *
 * Returns whether placeholders should be used
 * in operations concerning this parent & child.
 *
 * Currently that criteria is whether @parent is a
 * GtkContainer, @widget is a GtkWidget and the parent
 * adaptor has been marked to use placeholders.
 *
 * Returns: whether to use placeholders for this relationship.
 */
gboolean
glade_widget_placeholder_relation (GladeWidget *parent, 
				   GladeWidget *widget)
{
	g_return_val_if_fail (GLADE_IS_WIDGET (parent), FALSE);
	g_return_val_if_fail (GLADE_IS_WIDGET (widget), FALSE);

	return (GTK_IS_CONTAINER (parent->object) &&
		GTK_IS_WIDGET (widget->object) &&
		GWA_USE_PLACEHOLDERS (parent->adaptor));
}

static GladeWidgetAction *
glade_widget_action_lookup (GList **actions, const gchar *path, gboolean remove)
{
	GList *l;
	
	for (l = *actions; l; l = g_list_next (l))
	{
		GladeWidgetAction *action = l->data;
		
		if (strcmp (action->klass->path, path) == 0)
		{
			if (remove)
			{
				*actions = g_list_remove (*actions, action);
				g_object_unref (action);
				return NULL;
			}
			return action;
		}
		
		if (action->actions &&
		    g_str_has_prefix (path, action->klass->path) &&
		    (action = glade_widget_action_lookup (&action->actions, path, remove)))
			return action;
	}
	
	return NULL;
}

/**
 * glade_widget_get_action:
 * @widget: a #GladeWidget
 * @action_path: a full action path including groups
 *
 * Returns a #GladeWidgetAction object indentified by @action_path.
 *
 * Returns: the action or NULL if not found.
 */
GladeWidgetAction *
glade_widget_get_action (GladeWidget *widget, const gchar *action_path)
{
	g_return_val_if_fail (GLADE_IS_WIDGET (widget), NULL);
	g_return_val_if_fail (action_path != NULL, NULL);
	
	return glade_widget_action_lookup (&widget->actions, action_path, FALSE);
}

/**
 * glade_widget_get_pack_action:
 * @widget: a #GladeWidget
 * @action_path: a full action path including groups
 *
 * Returns a #GladeWidgetAction object indentified by @action_path.
 *
 * Returns: the action or NULL if not found.
 */
GladeWidgetAction *
glade_widget_get_pack_action (GladeWidget *widget, const gchar *action_path)
{
	g_return_val_if_fail (GLADE_IS_WIDGET (widget), NULL);
	g_return_val_if_fail (action_path != NULL, NULL);
	
	return glade_widget_action_lookup (&widget->packing_actions, action_path, FALSE);
}



/**
 * glade_widget_set_action_sensitive:
 * @widget: a #GladeWidget
 * @action_path: a full action path including groups
 * @sensitive: setting sensitive or insensitive
 *
 * Sets the sensitivity of @action_path in @widget
 *
 * Returns: whether @action_path was found or not.
 */
gboolean
glade_widget_set_action_sensitive (GladeWidget *widget,
				   const gchar *action_path,
				   gboolean     sensitive)
{
	GladeWidgetAction *action;
	
	g_return_val_if_fail (GLADE_IS_WIDGET (widget), FALSE);

	if ((action = glade_widget_get_action (widget, action_path)) != NULL)
	{
		glade_widget_action_set_sensitive (action, sensitive);
		return TRUE;
	}
	return FALSE;
}

/**
 * glade_widget_set_pack_action_sensitive:
 * @widget: a #GladeWidget
 * @action_path: a full action path including groups
 * @sensitive: setting sensitive or insensitive
 *
 * Sets the sensitivity of @action_path in @widget
 *
 * Returns: whether @action_path was found or not.
 */
gboolean
glade_widget_set_pack_action_sensitive (GladeWidget *widget,
					const gchar *action_path,
					gboolean     sensitive)
{
	GladeWidgetAction *action;
	
	g_return_val_if_fail (GLADE_IS_WIDGET (widget), FALSE);

	if ((action = glade_widget_get_pack_action (widget, action_path)) != NULL)
	{
		glade_widget_action_set_sensitive (action, sensitive);
		return TRUE;
	}
	return FALSE;
}


/**
 * glade_widget_remove_action:
 * @widget: a #GladeWidget
 * @action_path: a full action path including groups
 *
 * Remove an action.
 */
void
glade_widget_remove_action (GladeWidget *widget, const gchar *action_path)
{
	g_return_if_fail (GLADE_IS_WIDGET (widget));
	g_return_if_fail (action_path != NULL);
	
	glade_widget_action_lookup (&widget->actions, action_path, TRUE);
}

/**
 * glade_widget_remove_pack_action:
 * @widget: a #GladeWidget
 * @action_path: a full action path including groups
 *
 * Remove a packing action.
 */
void
glade_widget_remove_pack_action (GladeWidget *widget, const gchar *action_path)
{
	g_return_if_fail (GLADE_IS_WIDGET (widget));
	g_return_if_fail (action_path != NULL);
	
	glade_widget_action_lookup (&widget->packing_actions, action_path, TRUE);
}

/**
 * glade_widget_create_action_menu:
 * @widget: a #GladeWidget
 * @action_path: an action path or NULL to include every @widget action.
 *
 * Create a new GtkMenu with every action in it. 
 *
 */
GtkWidget *
glade_widget_create_action_menu (GladeWidget *widget, const gchar *action_path)
{
	GladeWidgetAction *action = NULL;
	GtkWidget *menu;

	g_return_val_if_fail (GLADE_IS_WIDGET (widget), NULL);

	if (action_path)
	{
		action = glade_widget_action_lookup (&widget->actions, action_path, FALSE);
		if (action == NULL)
			action = glade_widget_action_lookup (&widget->packing_actions, action_path, FALSE);
	}
	
	menu = gtk_menu_new ();
	if (glade_popup_action_populate_menu (menu, widget, action, TRUE))
		return menu;
	
	g_object_unref (G_OBJECT (menu));
	
	return NULL;
}

/*******************************************************************************
 *                           Toplevel GladeWidget Embedding                    *
 ******************************************************************************
 *
 * Overrides realize() and size_allocate() by signal connection on GtkWindows.
 *
 * This is high crack code and should be replaced by a more robust implementation
 * in GTK+ proper. 
 *
 */

static GQuark
embedded_window_get_quark ()
{
	static GQuark embedded_window_quark = 0;

	if (embedded_window_quark == 0)
		embedded_window_quark = g_quark_from_string ("GladeEmbedWindow");
	
	return embedded_window_quark;
}

static gboolean
glade_window_is_embedded (GtkWindow *window)
{
	return GPOINTER_TO_INT (g_object_get_qdata ((GObject *) window, embedded_window_get_quark ()));	 
}

static void
embedded_window_realize_handler (GtkWidget *widget)
{
	GtkWindow *window;
	GdkWindowAttr attributes;
	gint attributes_mask;

	window = GTK_WINDOW (widget);

	GTK_WIDGET_SET_FLAGS (widget, GTK_REALIZED);

	attributes.window_type = GDK_WINDOW_CHILD;
	attributes.wclass = GDK_INPUT_OUTPUT;
	attributes.visual = gtk_widget_get_visual (widget);
	attributes.colormap = gtk_widget_get_colormap (widget);

	attributes.x = widget->allocation.x;
	attributes.y = widget->allocation.y;
	attributes.width = widget->allocation.width;
	attributes.height = widget->allocation.height;

	attributes.event_mask = gtk_widget_get_events (widget) |
				GDK_EXPOSURE_MASK              |
                                GDK_FOCUS_CHANGE_MASK          |
			        GDK_KEY_PRESS_MASK             |
			        GDK_KEY_RELEASE_MASK           |
			        GDK_ENTER_NOTIFY_MASK          |
			        GDK_LEAVE_NOTIFY_MASK          |
				GDK_STRUCTURE_MASK;

	attributes_mask = GDK_WA_X | GDK_WA_Y | GDK_WA_VISUAL | GDK_WA_COLORMAP;

	/* destroy the previously created window */
	if (GDK_IS_WINDOW (widget->window))
	{
		gdk_window_hide (widget->window);
	}

	widget->window = gdk_window_new (gtk_widget_get_parent_window (widget),
					 &attributes, attributes_mask);

	gdk_window_enable_synchronized_configure (widget->window);

	gdk_window_set_user_data (widget->window, window);

	widget->style = gtk_style_attach (widget->style, widget->window);
	gtk_style_set_background (widget->style, widget->window, GTK_STATE_NORMAL);

}

static void
embedded_window_size_allocate_handler (GtkWidget *widget)
{
	if (GTK_WIDGET_REALIZED (widget))
	{
		gdk_window_move_resize (widget->window,
					widget->allocation.x,
					widget->allocation.y,
					widget->allocation.width,
					widget->allocation.height);
	}
}

/**
 * glade_widget_embed:
 * @window: a #GtkWindow
 *
 * Embeds a window by signal connection method
 */
static gboolean
glade_widget_embed (GladeWidget *gwidget)
{
	GtkWindow *window;
	GtkWidget *widget;
	
	g_return_val_if_fail (GLADE_IS_WIDGET (gwidget), FALSE);
	g_return_val_if_fail (GTK_IS_WINDOW (gwidget->object), FALSE);
	
	window = GTK_WINDOW (gwidget->object);
	widget = GTK_WIDGET (window);
	
	if (glade_window_is_embedded (window)) return TRUE;
	
	if (GTK_WIDGET_REALIZED (widget)) gtk_widget_unrealize (widget);

	GTK_WIDGET_UNSET_FLAGS (widget, GTK_TOPLEVEL);
	gtk_container_set_resize_mode (GTK_CONTAINER (window), GTK_RESIZE_PARENT);

	g_signal_connect (window, "realize",
			  G_CALLBACK (embedded_window_realize_handler), NULL);
	g_signal_connect (window, "size-allocate",
			  G_CALLBACK (embedded_window_size_allocate_handler), NULL);

	/* mark window as embedded */
	g_object_set_qdata (G_OBJECT (window), embedded_window_get_quark (),
			    GINT_TO_POINTER (TRUE));
	
	return TRUE;
}

