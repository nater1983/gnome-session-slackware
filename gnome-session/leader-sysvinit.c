/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2006 Novell, Inc.
 * Copyright (C) 2008 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>
#include <config.h>

#include <glib.h>
#include <glib-unix.h>
#include <gio/gio.h>
#include <sys/syslog.h>

typedef struct {
        GDBusConnection *session_bus;
        GMainLoop *loop;
        int fifo_fd;
        GPid start_script_pid;
} Leader;

static void
leader_clear (Leader *ctx)
{
        g_clear_object (&ctx->session_bus);
        g_clear_pointer (&ctx->loop, g_main_loop_unref);
        g_close (ctx->fifo_fd, NULL);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (Leader, leader_clear);

static gboolean
run_script (const char *script, const char *arg1, const char *arg2,
            GPid *out_pid, GError **error)
{
        gchar *argv[] = { (gchar *) script, (gchar *) arg1, (gchar *) arg2, NULL };
        GSpawnFlags flags = G_SPAWN_DO_NOT_REAP_CHILD;

        if (out_pid == NULL)
                flags = G_SPAWN_DEFAULT;

        return g_spawn_async (NULL, argv, NULL, flags,
                              NULL, NULL, out_pid, error);
}

static gboolean
leader_term_or_int_signal_cb (gpointer data)
{
        Leader *ctx = data;

        g_debug ("Session termination requested");

        if (write (ctx->fifo_fd, "S", 1) < 0) {
                g_warning ("Failed to signal shutdown to monitor: %m");
                g_main_loop_quit (ctx->loop);
        }

        return G_SOURCE_REMOVE;
}

static gboolean
monitor_hangup_cb (int          fd,
                   GIOCondition condition,
                   gpointer     user_data)
{
        Leader *ctx = user_data;

        g_debug ("Monitor closed FIFO, session services are stopping");
        g_main_loop_quit (ctx->loop);

        return G_SOURCE_REMOVE;
}

static void
start_script_exited_cb (GPid     pid,
                        gint     status,
                        gpointer user_data)
{
        Leader *ctx = user_data;

        g_spawn_close_pid (pid);
        ctx->start_script_pid = 0;

        if (status != 0)
                g_warning ("Session start script exited with status %d", status);

        g_debug ("Session start script exited, quitting leader");
        g_main_loop_quit (ctx->loop);
}

static void
debug_logger (gchar const *log_domain,
              GLogLevelFlags log_level,
              gchar const *message,
              gpointer user_data)
{
        printf ("%s\n", message);
        syslog (LOG_INFO, "%s", message);
}

/**
 * This is the session leader for sysvinit systems. It is executed by GDM
 * and is part of the session scope. It manages the session lifecycle by:
 *
 * - Launching a session start script that spawns all session components
 *   (gnome-shell, gnome-session-service, gsd daemons, etc.)
 * - Maintaining a FIFO for shutdown signaling between the leader and
 *   the session monitor (gnome-session-ctl --monitor)
 * - On SIGTERM/SIGINT, writing to the FIFO to trigger clean shutdown
 * - Quitting when the monitor closes its end of the FIFO (HUP)
 *
 * The shutdown flow:
 * 1. GDM kills session scope -> leader gets SIGTERM
 * 2. Leader writes byte to FIFO
 * 3. Monitor reads byte, calls gnome-session-stop to kill session processes
 * 4. Monitor exits, closing FIFO
 * 5. Leader gets HUP on FIFO, quits
 * 6. GDM sees leader exit and cleans up
 */
int
main (int argc, char **argv)
{
        g_log_set_default_handler (debug_logger, NULL);
        g_autoptr (GError) error = NULL;
        g_auto (Leader) ctx = { .fifo_fd = -1, .start_script_pid = 0 };
        const char *session_name = NULL;
        const char *debug_string = NULL;
        g_autofree char *fifo_path = NULL;
        g_autofree char *home_dir = NULL;
        g_autofree char *config_dir = NULL;
        struct stat statbuf;

        if (argc < 2)
                g_error ("No session name was specified");
        session_name = argv[1];

        char const *user = g_getenv ("USER");
        if (!user)
                user = "gdm-greeter";
        g_info ("User is: %s", user);

        /* For gdm-greeter users, set up a persistent home dir.
         * strncmp because we also have gdm-greeter-{2,3,4,...} */
        if (strncmp (user, "gdm-greeter", sizeof ("gdm-greeter")) == 0) {
                home_dir = g_strdup_printf ("/var/lib/%s", user);
                config_dir = g_strdup_printf ("%s/.config", home_dir);
                g_setenv ("XDG_CONFIG_HOME", config_dir, TRUE);
                g_setenv ("HOME", home_dir, TRUE);
        } else {
                g_warning ("The gdm-greeter user wasn't found. Expect stuff to break.");
        }

        char const *session_type = g_getenv ("XDG_SESSION_TYPE");
        char const *home = g_getenv ("HOME");
        g_info ("XDG_RUNTIME_DIR: %s", g_getenv ("XDG_RUNTIME_DIR"));

        debug_string = g_getenv ("GNOME_SESSION_DEBUG");
        if (debug_string != NULL)
                g_log_set_debug_enabled (atoi (debug_string) == 1);
        else
                g_log_set_debug_enabled (TRUE);
        g_debug ("Hi! from leader-sysvinit.");

        ctx.loop = g_main_loop_new (NULL, TRUE);

        ctx.session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
        if (ctx.session_bus == NULL)
                g_error ("Failed to obtain session bus: %s", error->message);

        /* XDG_SESSION_TYPE from the console is TTY which isn't useful */
        if (session_type && strcmp (session_type, "tty") == 0)
                session_type = "wayland";
        if (!session_type)
                session_type = "wayland";

        g_message ("Starting GNOME session: type=%s name=%s", session_type, session_name);

        /* Create the FIFO before starting the session, so the monitor
         * can open it immediately */
        fifo_path = g_build_filename (g_get_user_runtime_dir (),
                                      "gnome-session-leader-fifo",
                                      NULL);
        if (mkfifo (fifo_path, 0666) < 0 && errno != EEXIST)
                g_warning ("Failed to create leader FIFO: %m");

        /* Launch the session start script.
         * It will start gnome-shell, gnome-session-service, gsd daemons, etc.
         * and wait for gnome-shell to exit. */
        g_autofree char *start_script = g_build_filename (LIBEXECDIR,
                                                          "gnome-session-start",
                                                          NULL);
        if (!run_script (start_script, session_type, session_name,
                         &ctx.start_script_pid, &error))
                g_error ("Failed to start session: %s", error->message);

        /* Watch for the start script to exit */
        g_child_watch_add (ctx.start_script_pid, start_script_exited_cb, &ctx);

        /* Open our end of the FIFO for writing */
        ctx.fifo_fd = g_open (fifo_path, O_WRONLY | O_CLOEXEC, 0666);
        if (ctx.fifo_fd < 0)
                g_error ("Failed to open leader FIFO: %m");
        if (fstat (ctx.fifo_fd, &statbuf) < 0)
                g_error ("Failed to stat leader FIFO: %m");
        else if (!(statbuf.st_mode & S_IFIFO))
                g_error ("Leader FIFO FD is not a FIFO");

        g_unix_fd_add (ctx.fifo_fd, G_IO_HUP, (GUnixFDSourceFunc) monitor_hangup_cb, &ctx);
        g_unix_signal_add (SIGHUP, leader_term_or_int_signal_cb, &ctx);
        g_unix_signal_add (SIGTERM, leader_term_or_int_signal_cb, &ctx);
        g_unix_signal_add (SIGINT, leader_term_or_int_signal_cb, &ctx);

        g_main_loop_run (ctx.loop);
        return 0;
}
