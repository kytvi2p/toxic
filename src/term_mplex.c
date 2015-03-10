/*  term_mplex.c
 *
 *
 *  Copyright (C) 2015 Toxic All Rights Reserved.
 *
 *  This file is part of Toxic.
 *
 *  Toxic is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Toxic is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Toxic.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <limits.h> /* PATH_MAX */
#include <stdio.h>  /* fgets, popen, pclose */
#include <stdlib.h> /* malloc, realloc, free, getenv */
#include <string.h> /* strlen, strcpy, strstr, strchr, strrchr, strcat */

#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <tox/tox.h>

#include "global_commands.h"
#include "windows.h"
#include "term_mplex.h"
#include "toxic.h"
#include "settings.h"

extern struct ToxWindow *prompt;
extern struct user_settings *user_settings;
extern struct Winthread Winthread;

#if defined(PATH_MAX) && PATH_MAX > 512
#define BUFFER_SIZE PATH_MAX
#else
#define BUFFER_SIZE 512
#endif

#define PATH_SEP_S "/"
#define PATH_SEP_C '/'

static char buffer [BUFFER_SIZE];
static int mplex = 0;
static TOX_USERSTATUS prev_status = TOX_USERSTATUS_NONE;
static char prev_note [TOX_MAX_STATUSMESSAGE_LENGTH] = "";
static Tox *tox = NULL;

static char *read_into_dyn_buffer (FILE *stream)
{
    const char *input_ptr = NULL;
    char *dyn_buffer = NULL;
    int dyn_buffer_size = 1; /* account for the \0 */

    while ((input_ptr = fgets (buffer, BUFFER_SIZE, stream)) != NULL)
    {
        int length = dyn_buffer_size + strlen (input_ptr);
        if (dyn_buffer)
            dyn_buffer = (char*) realloc (dyn_buffer, length);
        else
            dyn_buffer = (char*) malloc (length);
        strcpy (dyn_buffer + dyn_buffer_size - 1, input_ptr);
        dyn_buffer_size = length;
    }

    return dyn_buffer;
}

static char *extract_socket_path (const char *info)
{
    const char *search_str = " Socket";
    const char *pos = strstr (info, search_str);
    char *end = NULL;
    char* path = NULL;

    if (!pos)
        return NULL;

    pos += strlen (search_str);
    pos = strchr (pos, PATH_SEP_C);
    if (!pos)
        return NULL;

    end = strchr (pos, '\n');
    if (!end)
        return NULL;

    *end = '\0';
    end = strrchr (pos, '.');
    if (!end)
        return NULL;

    path = (char*) malloc (end - pos + 1);
    *end = '\0';
    return strcpy (path, pos);
}

static int detect_gnu_screen ()
{
    FILE *session_info_stream = NULL;
    char *socket_name = NULL, *socket_path = NULL;
    char *dyn_buffer = NULL;

    socket_name = getenv ("STY");
    if (!socket_name)
        goto nomplex;

    session_info_stream = popen ("env LC_ALL=C screen -ls", "r");
    if (!session_info_stream)
        goto nomplex;

    dyn_buffer = read_into_dyn_buffer (session_info_stream);
    if (!dyn_buffer)
        goto nomplex;

    pclose (session_info_stream);
    session_info_stream = NULL;

    socket_path = extract_socket_path (dyn_buffer);
    if (!socket_path)
        goto nomplex;

    free (dyn_buffer);
    dyn_buffer = NULL;
    strcpy (buffer, socket_path);
    strcat (buffer, PATH_SEP_S);
    strcat (buffer, socket_name);
    free (socket_path);
    socket_path = NULL;

    mplex = 1;
    return 1;

nomplex:
    if (session_info_stream)
        pclose (session_info_stream);
    if (dyn_buffer)
        free (dyn_buffer);
    return 0;
}

static int detect_tmux ()
{
    char *tmux_env = getenv ("TMUX"), *socket_path, *pos;
    if (!tmux_env)
        goto finish;

    /* make a clean copy for writing */
    socket_path = (char*) malloc (strlen (tmux_env) + 1);
    if (!socket_path)
        goto finish;
    strcpy (socket_path, tmux_env);

    /* find second separator */
    pos = strrchr (socket_path, ',');
    if (!pos)
        goto finish;

    /* find first separator */
    *pos = '\0';
    pos = strrchr (socket_path, ',');
    if (!pos)
        goto finish;

    /* write the socket path to buffer for later use */
    *pos = '\0';
    strcpy (buffer, socket_path);
    free (socket_path);
    mplex = 1;
    return 1;

finish:
    if (socket_path)
        free (socket_path);
    return 0;
}

/* Checks whether a terminal multiplexer (mplex) is present, and finds
   its unix socket.

   GNU screen and tmux are supported.

   Returns 1 if present, 0 otherwise. This value can be used to determine
   whether an auto-away detection timer is needed.
 */
static int detect_mplex ()
{
    /* try screen, and if fails try tmux */
    return detect_gnu_screen () || detect_tmux ();
}


/* Checks whether there is a terminal multiplexer present, but in detached
   state. Returns 1 if detached, 0 if attached or if there is no terminal
   multiplexer.

   If detect_mplex_socket() failed to find a mplex, there is no need to call
   this function. If it did find one, this function can be used to periodically
   sample its state and update away status according to attached/detached state
   of the mplex.
 */
static int mplex_is_detached ()
{
    if (!mplex)
        return 0;

    struct stat sb;
    if (stat (buffer, &sb) != 0)
        return 0;
    /* execution permission (x) means attached */
    return ! (sb.st_mode & S_IXUSR);
}

static void mplex_timer_handler (union sigval param)
{
    int detached;
    TOX_USERSTATUS current_status, new_status;
    const char *new_note;

    if (!mplex)
        return;

    detached = mplex_is_detached ();
    current_status = tox_get_self_user_status (tox);

    if (current_status == TOX_USERSTATUS_AWAY && !detached)
    {
        new_status = prev_status;
        new_note = prev_note;
    }
    else
    if (current_status != TOX_USERSTATUS_AWAY && detached)
    {
        prev_status = current_status;
        new_status = TOX_USERSTATUS_AWAY;
        tox_get_self_status_message (tox,
                                     (uint8_t*) prev_note,
                                     sizeof (prev_note));
        new_note = user_settings->mplex_away_note;
    }
    else
        return;

    char argv[3][MAX_STR_SIZE];
    strcpy (argv[0], "/status");
    strcpy (argv[1], (new_status == TOX_USERSTATUS_AWAY ? "away" :
                      new_status == TOX_USERSTATUS_BUSY ? "busy" : "online"));
    argv[2][0] = '\"';
    strcpy (argv[2] + 1, new_note);
    strcat (argv[2], "\"");
    pthread_mutex_lock (&Winthread.lock);
    cmd_status (prompt->chatwin->history, prompt, tox, 2, argv);
    pthread_mutex_unlock (&Winthread.lock);
}

void init_mplex_away_timer (Tox *m)
{
    struct sigevent sev;
    timer_t timer_id;
    struct itimerspec its;

    if (! detect_mplex ())
        return;

    if (! user_settings->mplex_away)
        return;

    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_notify_function = mplex_timer_handler;
    sev.sigev_notify_attributes = NULL;

    if (timer_create (CLOCK_REALTIME, &sev, &timer_id) == -1)
        return;

    its.it_interval.tv_sec = 5;
    its.it_interval.tv_nsec = 0;
    its.it_value = its.it_interval;

    timer_settime (timer_id, 0, &its, NULL);
    tox = m;
}
