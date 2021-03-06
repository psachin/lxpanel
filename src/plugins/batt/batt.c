/*
 * ACPI battery monitor plugin for LXPanel
 *
 * Copyright (C) 2007 by Greg McNew <gmcnew@gmail.com>
 * Copyright (C) 2008 by Hong Jen Yee <pcman.tw@gmail.com>
 * Copyright (C) 2009 by Juergen Hoetzel <juergen@archlinux.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 *
 * This plugin monitors battery usage on ACPI-enabled systems by reading the
 * battery information found in /sys/class/power_supply. The update interval is
 * user-configurable and defaults to 3 second.
 *
 * The battery's remaining life is estimated from its current charge and current
 * rate of discharge. The user may configure an alarm command to be run when
 * their estimated remaining battery life reaches a certain level.
 */

/* FIXME:
 *  Here are somethings need to be improvec:
 *  1. Replace pthread stuff with gthread counterparts for portability.
 *  3. Add an option to hide the plugin when AC power is used or there is no battery.
 *  4. Handle failure gracefully under systems other than Linux.
*/

#include <glib.h>
#include <glib/gi18n.h>
#include <pthread.h> /* used by pthread_create() and alarmThread */
#include <semaphore.h> /* used by update() and alarmProcess() for alarms */
#include <stdlib.h>
#include <string.h>

#include "dbg.h"
#include "batt_sys.h"
#include "misc.h" /* used for the line struct */
#include "panel.h" /* used to determine panel orientation */
#include "plugin.h"

/* The last MAX_SAMPLES samples are averaged when charge rates are evaluated.
   This helps prevent spikes in the "time left" values the user sees. */
#define MAX_SAMPLES 10

typedef struct {
    char *alarmCommand,
        *backgroundColor,
        *chargingColor1,
        *chargingColor2,
        *dischargingColor1,
        *dischargingColor2;
    GdkColor background,
        charging1,
        charging2,
        discharging1,
        discharging2;
    cairo_surface_t *pixmap;
    GtkWidget *drawingArea;
    int orientation;
    unsigned int alarmTime,
        border,
        height,
        length,
        numSamples,
        requestedBorder,
        *rateSamples,
        rateSamplesSum,
        thickness,
        timer,
        state_elapsed_time,
        info_elapsed_time,
        wasCharging,
        width,
        hide_if_no_battery;
    sem_t alarmProcessLock;
    battery* b;
    gboolean has_ac_adapter;
} lx_battery;

typedef struct {
     batt_cap* b1;
} lx_batt_cap;

typedef struct {
    char *command;
    sem_t *lock;
} Alarm;

static void destructor(Plugin *p);
static void update_display(lx_battery *lx_b, lx_batt_cap *lx_b1, gboolean repaint);

/* alarmProcess takes the address of a dynamically allocated alarm struct (which
   it must free). It ensures that alarm commands do not run concurrently. */
static void * alarmProcess(void *arg) {
    Alarm *a = (Alarm *) arg;

    sem_wait(a->lock);
    system(a->command);
    sem_post(a->lock);

    g_free(a);
    return NULL;
}


/* FIXME:
   Don't repaint if percentage of remaining charge and remaining time aren't changed. */
void update_display(lx_battery *lx_b, lx_batt_cap *lx_b1, gboolean repaint) {
    cairo_t *cr;
    char tooltip[ 256 ];
    battery *b = lx_b->b;
    batt_cap *b1 = lx_b1->b1;
    /* unit: mW */
    int rate;
    gboolean isCharging;

    if (! lx_b->pixmap )
        return;

    cr = cairo_create(lx_b->pixmap);
    cairo_set_line_width (cr, 1.0);

    /* no battery is found */
    if( b == NULL ) 
    {
	gtk_widget_set_tooltip_text( lx_b->drawingArea, _("No batteries found") );
	return;
    }
    
    /* draw background */
    gdk_cairo_set_source_color(cr, &lx_b->background);
    cairo_rectangle(cr, 0, 0, lx_b->width, lx_b->height);
    cairo_fill(cr);

    /* fixme: only one battery supported */

    rate = lx_b->b->current_now;
    isCharging = battery_is_charging ( b );
    
    /* Consider running the alarm command */
    if ( !isCharging && rate > 0 &&
	( ( battery_get_remaining( b ) / 60 ) < lx_b->alarmTime ) )
    {
	/* Shrug this should be done using glibs process functions */
	/* Alarms should not run concurrently; determine whether an alarm is
	   already running */
	int alarmCanRun;
	sem_getvalue(&(lx_b->alarmProcessLock), &alarmCanRun);
	
	/* Run the alarm command if it isn't already running */
	if (alarmCanRun) {
	    
	    Alarm *a = (Alarm *) malloc(sizeof(Alarm));
	    a->command = lx_b->alarmCommand;
	    a->lock = &(lx_b->alarmProcessLock);
	    
	    /* Manage the alarm process in a new thread, which which will be
	       responsible for freeing the alarm struct it's given */
	    pthread_t alarmThread;
	    pthread_create(&alarmThread, NULL, alarmProcess, a);
	    
	}
    }

    /* Make a tooltip string, and display remaining charge time if the battery
       is charging or remaining life if it's discharging */
    if (isCharging) {
	int hours = lx_b->b->seconds / 3600;
	int left_seconds = b->seconds - 3600 * hours;
	int minutes = left_seconds / 60;
	//printf("precentage : %d\n", percentage);
	printf("hours: %d\n", hours);
	printf("minutes: %02d\n", minutes);
	printf("precentage: %02d\n", lx_b1->b1->per);

	snprintf(tooltip, 256,
		_("Battery: %d%% charged, %d:%02d until full"),
		lx_b->b->percentage,
		hours,
		minutes );
    } else {
	/* if we have enough rate information for battery */
	if (lx_b->b->percentage != 100) {
	    int hours = lx_b->b->seconds / 3600;
	    int left_seconds = b->seconds - 3600 * hours;
	    int minutes = left_seconds / 60;
	    snprintf(tooltip, 256,
		    _("Battery: %d%% charged, %d:%02d left"),
		    lx_b->b->percentage,
		    hours,
		    minutes );
	} else {
	    snprintf(tooltip, 256,
		    _("Battery: %d%% charged"),
		    100 );
	}
    }

    gtk_widget_set_tooltip_text(lx_b->drawingArea, tooltip);

    int chargeLevel = lx_b->b->percentage * (lx_b->length - 2 * lx_b->border) / 100;

    if (lx_b->orientation == ORIENT_HORIZ) {

	/* Draw the battery bar vertically, using color 1 for the left half and
	   color 2 for the right half */
        gdk_cairo_set_source_color(cr,
                isCharging ? &lx_b->charging1 : &lx_b->discharging1);
        cairo_rectangle(cr, lx_b->border,
                lx_b->height - lx_b->border - chargeLevel, lx_b->width / 2
                - lx_b->border, chargeLevel);
        cairo_fill(cr);
        gdk_cairo_set_source_color(cr,
                isCharging ? &lx_b->charging2 : &lx_b->discharging2);
        cairo_rectangle(cr, lx_b->width / 2,
                lx_b->height - lx_b->border - chargeLevel, (lx_b->width + 1) / 2
                - lx_b->border, chargeLevel);
        cairo_fill(cr);

    }
    else {

	/* Draw the battery bar horizontally, using color 1 for the top half and
	   color 2 for the bottom half */
        gdk_cairo_set_source_color(cr,
                isCharging ? &lx_b->charging1 : &lx_b->discharging1);
        cairo_rectangle(cr, lx_b->border,
                lx_b->border, chargeLevel, lx_b->height / 2 - lx_b->border);
        cairo_fill(cr);
        gdk_cairo_set_source_color(cr,
                isCharging ? &lx_b->charging2 : &lx_b->discharging2);
        cairo_rectangle(cr, lx_b->border, (lx_b->height + 1)
                / 2, chargeLevel, lx_b->height / 2 - lx_b->border);
        cairo_fill(cr);

    }
    if( repaint )
	gtk_widget_queue_draw( lx_b->drawingArea );

    check_cairo_status(cr);
    cairo_destroy(cr);
}

/* This callback is called every 3 seconds */
static int update_timout(lx_battery *lx_b, lx_batt_cap *lx_b1) {
    GDK_THREADS_ENTER();
    lx_b->state_elapsed_time++;
    lx_b->info_elapsed_time++;

    /* check the  batteries every 3 seconds */
    battery_update( lx_b->b );

    update_display( lx_b, lx_b1, TRUE );

    GDK_THREADS_LEAVE();
    return TRUE;
}

/* An update will be performed whenever the user clicks on the charge bar */
static gint buttonPressEvent(GtkWidget *widget, GdkEventButton *event,
        Plugin* plugin) {

    lx_battery *lx_b = (lx_battery*)plugin->priv;
    lx_batt_cap *lx_b1 = (lx_batt_cap*)plugin->priv;

    update_display(lx_b, lx_b1,TRUE);

    if( event->button == 3 )  /* right button */
    {
        GtkMenu* popup = lxpanel_get_panel_menu( plugin->panel, plugin, FALSE );
        gtk_menu_popup( popup, NULL, NULL, NULL, NULL, event->button, event->time );
        return TRUE;
    }
    return FALSE;
}


static gint configureEvent(GtkWidget *widget, GdkEventConfigure *event,
        lx_battery *lx_b, lx_batt_cap *lx_b1) {

    ENTER;

    if (lx_b->pixmap)
        cairo_surface_destroy(lx_b->pixmap);

    /* Update the plugin's dimensions */
    lx_b->width = widget->allocation.width;
    lx_b->height = widget->allocation.height;
    if (lx_b->orientation == ORIENT_HORIZ) {
        lx_b->length = lx_b->height;
        lx_b->thickness = lx_b->width;
    }
    else {
        lx_b->length = lx_b->width;
        lx_b->thickness = lx_b->height;
    }

    lx_b->pixmap = cairo_image_surface_create (CAIRO_FORMAT_RGB24, widget->allocation.width,
          widget->allocation.height);
    check_cairo_surface_status(&lx_b->pixmap);

    /* Perform an update so the bar will look right in its new orientation */
    update_display(lx_b, lx_b1, FALSE);

    RET(TRUE);

}


static gint exposeEvent(GtkWidget *widget, GdkEventExpose *event, lx_battery *lx_b) {

    ENTER;
    cairo_t *cr = gdk_cairo_create(widget->window);
    gdk_cairo_region(cr, event->region);
    cairo_clip(cr);

    gdk_cairo_set_source_color(cr, &lx_b->drawingArea->style->black);
    cairo_set_source_surface(cr, lx_b->pixmap, 0, 0);
    cairo_paint(cr);

    check_cairo_status(cr);
    cairo_destroy(cr);

    RET(FALSE);
}


static int
constructor(Plugin *p, char **fp)
{
    ENTER;

    lx_battery *lx_b;
    p->priv = lx_b = g_new0(lx_battery, 1);

    /* get available battery */
    lx_b->b = battery_get ();
    
    /* no battery available */
    if ( lx_b->b == NULL )
	goto error;
    
    p->pwid = gtk_event_box_new();
    GTK_WIDGET_SET_FLAGS( p->pwid, GTK_NO_WINDOW );
    gtk_container_set_border_width( GTK_CONTAINER(p->pwid), 1 );

    lx_b->drawingArea = gtk_drawing_area_new();
    gtk_widget_add_events( lx_b->drawingArea, GDK_BUTTON_PRESS_MASK );

    gtk_container_add( (GtkContainer*)p->pwid, lx_b->drawingArea );

    if ((lx_b->orientation = p->panel->orientation) == ORIENT_HORIZ) {
        lx_b->height = lx_b->length = 20;
        lx_b->thickness = lx_b->width = 8;
    }
    else {
        lx_b->height = lx_b->thickness = 8;
        lx_b->length = lx_b->width = 20;
    }
    gtk_widget_set_size_request(lx_b->drawingArea, lx_b->width, lx_b->height);

    gtk_widget_show(lx_b->drawingArea);

    g_signal_connect (G_OBJECT (lx_b->drawingArea), "button_press_event",
            G_CALLBACK(buttonPressEvent), (gpointer) p);
    g_signal_connect (G_OBJECT (lx_b->drawingArea),"configure_event",
          G_CALLBACK (configureEvent), (gpointer) lx_b);
    g_signal_connect (G_OBJECT (lx_b->drawingArea), "expose_event",
          G_CALLBACK (exposeEvent), (gpointer) lx_b);

    sem_init(&(lx_b->alarmProcessLock), 0, 1);

    lx_b->alarmCommand = lx_b->backgroundColor = lx_b->chargingColor1 = lx_b->chargingColor2
            = lx_b->dischargingColor1 = lx_b->dischargingColor2 = NULL;

    /* Set default values for integers */
    lx_b->alarmTime = 5;
    lx_b->requestedBorder = 1;

    line s;
    s.len = 256;

    if (fp) {

        /* Apply options */
        while (lxpanel_get_line(fp, &s) != LINE_BLOCK_END) {
            if (s.type == LINE_NONE) {
                ERR( "batt: illegal token %s\n", s.str);
                goto error;
            }
            if (s.type == LINE_VAR) {
                if (!g_ascii_strcasecmp(s.t[0], "HideIfNoBattery"))
                    lx_b->hide_if_no_battery = atoi(s.t[1]);
                else if (!g_ascii_strcasecmp(s.t[0], "AlarmCommand"))
                    lx_b->alarmCommand = g_strdup(s.t[1]);
                else if (!g_ascii_strcasecmp(s.t[0], "BackgroundColor"))
                    lx_b->backgroundColor = g_strdup(s.t[1]);
                else if (!g_ascii_strcasecmp(s.t[0], "ChargingColor1"))
                    lx_b->chargingColor1 = g_strdup(s.t[1]);
                else if (!g_ascii_strcasecmp(s.t[0], "ChargingColor2"))
                    lx_b->chargingColor2 = g_strdup(s.t[1]);
                else if (!g_ascii_strcasecmp(s.t[0], "DischargingColor1"))
                    lx_b->dischargingColor1 = g_strdup(s.t[1]);
                else if (!g_ascii_strcasecmp(s.t[0], "DischargingColor2"))
                    lx_b->dischargingColor2 = g_strdup(s.t[1]);
                else if (!g_ascii_strcasecmp(s.t[0], "AlarmTime"))
                    lx_b->alarmTime = atoi(s.t[1]);
                else if (!g_ascii_strcasecmp(s.t[0], "BorderWidth"))
                    lx_b->requestedBorder = atoi(s.t[1]);
                else if (!g_ascii_strcasecmp(s.t[0], "Size")) {
                    lx_b->thickness = MAX(1, atoi(s.t[1]));
                    if (lx_b->orientation == ORIENT_HORIZ)
                        lx_b->width = lx_b->thickness;
                    else
                        lx_b->height = lx_b->thickness;
                    gtk_widget_set_size_request(lx_b->drawingArea, lx_b->width,
                            lx_b->height);
                }
                else {
                    ERR( "batt: unknown var %s\n", s.t[0]);
                    continue;
                }
            }
            else {
                ERR( "batt: illegal in this context %s\n", s.str);
                goto error;
            }
        }

    }

    /* Make sure the border value is acceptable */
    lx_b->border = MIN(MAX(0, lx_b->requestedBorder),
            (MIN(lx_b->length, lx_b->thickness) - 1) / 2);

    /* Apply more default options */
    if (! lx_b->alarmCommand)
        lx_b->alarmCommand = g_strdup("xmessage Battery low");
    if (! lx_b->backgroundColor)
        lx_b->backgroundColor = g_strdup("black");
    if (! lx_b->chargingColor1)
        lx_b->chargingColor1 = g_strdup("#28f200");
    if (! lx_b->chargingColor2)
        lx_b->chargingColor2 = g_strdup("#22cc00");
    if (! lx_b->dischargingColor1)
        lx_b->dischargingColor1 = g_strdup("#ffee00");
    if (! lx_b->dischargingColor2)
        lx_b->dischargingColor2 = g_strdup("#d9ca00");

    gdk_color_parse(lx_b->backgroundColor, &lx_b->background);
    gdk_color_parse(lx_b->chargingColor1, &lx_b->charging1);
    gdk_color_parse(lx_b->chargingColor2, &lx_b->charging2);
    gdk_color_parse(lx_b->dischargingColor1, &lx_b->discharging1);
    gdk_color_parse(lx_b->dischargingColor2, &lx_b->discharging2);

   
    /* Start the update loop */
    lx_b->timer = g_timeout_add_seconds( 9, (GSourceFunc) update_timout, (gpointer) lx_b);

    RET(TRUE);

error:
    RET(FALSE);
}


static void
destructor(Plugin *p)
{
    ENTER;

    lx_battery *b = (lx_battery *) p->priv;

    if (b->pixmap)
        cairo_surface_destroy(b->pixmap);

    g_free(b->alarmCommand);
    g_free(b->backgroundColor);
    g_free(b->chargingColor1);
    g_free(b->chargingColor2);
    g_free(b->dischargingColor1);
    g_free(b->dischargingColor2);

    g_free(b->rateSamples);
    sem_destroy(&(b->alarmProcessLock));
    if (b->timer)
        g_source_remove(b->timer);
    g_free(b);

    RET();

}


static void orientation(Plugin *p) {

    ENTER;

    lx_battery *b = (lx_battery *) p->priv;

    if (b->orientation != p->panel->orientation) {
        b->orientation = p->panel->orientation;
        unsigned int swap = b->height;
        b->height = b->width;
        b->width = swap;
        gtk_widget_set_size_request(b->drawingArea, b->width, b->height);
    }

    RET();
}


static void applyConfig(Plugin* p)
{
    ENTER;

    lx_battery *b = (lx_battery *) p->priv;

    /* Update colors */
    if (b->backgroundColor &&
            gdk_color_parse(b->backgroundColor, &b->background));
    if (b->chargingColor1 && gdk_color_parse(b->chargingColor1, &b->charging1));
    if (b->chargingColor2 && gdk_color_parse(b->chargingColor2, &b->charging2));
    if (b->dischargingColor1 &&
            gdk_color_parse(b->dischargingColor1, &b->discharging1));
    if (b->dischargingColor2 &&
            gdk_color_parse(b->dischargingColor2, &b->discharging2));

    /* Make sure the border value is acceptable */
    b->border = MIN(MAX(0, b->requestedBorder),
            (MIN(b->length, b->thickness) - 1) / 2);

    /* Resize the widget */
    if (b->orientation == ORIENT_HORIZ)
        b->width = b->thickness;
    else
        b->height = b->thickness;
    gtk_widget_set_size_request(b->drawingArea, b->width, b->height);

    RET();
}


static void config(Plugin *p, GtkWindow* parent) {
    ENTER;

    GtkWidget *dialog;
    lx_battery *b = (lx_battery *) p->priv;
    dialog = create_generic_config_dlg(_(p->class->name),
            GTK_WIDGET(parent),
            (GSourceFunc) applyConfig, (gpointer) p,
#if 0
            _("Hide if there is no battery"), &b->hide_if_no_battery, CONF_TYPE_BOOL,
#endif
            _("Alarm command"), &b->alarmCommand, CONF_TYPE_STR,
            _("Alarm time (minutes left)"), &b->alarmTime, CONF_TYPE_INT,
            _("Background color"), &b->backgroundColor, CONF_TYPE_STR,
            _("Charging color 1"), &b->chargingColor1, CONF_TYPE_STR,
            _("Charging color 2"), &b->chargingColor2, CONF_TYPE_STR,
            _("Discharging color 1"), &b->dischargingColor1, CONF_TYPE_STR,
            _("Discharging color 2"), &b->dischargingColor2, CONF_TYPE_STR,
            _("Border width"), &b->requestedBorder, CONF_TYPE_INT,
            _("Size"), &b->thickness, CONF_TYPE_INT,
            NULL);
    gtk_window_present(GTK_WINDOW(dialog));

    RET();
}


static void save(Plugin* p, FILE* fp) {
    lx_battery *lx_b = (lx_battery *) p->priv;

    lxpanel_put_bool(fp, "HideIfNoBattery",lx_b->hide_if_no_battery);
    lxpanel_put_str(fp, "AlarmCommand", lx_b->alarmCommand);
    lxpanel_put_int(fp, "AlarmTime", lx_b->alarmTime);
    lxpanel_put_str(fp, "BackgroundColor", lx_b->backgroundColor);
    lxpanel_put_int(fp, "BorderWidth", lx_b->requestedBorder);
    lxpanel_put_str(fp, "ChargingColor1", lx_b->chargingColor1);
    lxpanel_put_str(fp, "ChargingColor2", lx_b->chargingColor2);
    lxpanel_put_str(fp, "DischargingColor1", lx_b->dischargingColor1);
    lxpanel_put_str(fp, "DischargingColor2", lx_b->dischargingColor2);
    lxpanel_put_int(fp, "Size", lx_b->thickness);
}


PluginClass batt_plugin_class = {
    
    PLUGINCLASS_VERSIONING,

    type        : "batt",
    name        : N_("Battery Monitor"),
    version     : "2.0",
    description : N_("Display battery status using ACPI"),

    constructor : constructor,
    destructor  : destructor,
    config      : config,
    save        : save,
    panel_configuration_changed : orientation
};
