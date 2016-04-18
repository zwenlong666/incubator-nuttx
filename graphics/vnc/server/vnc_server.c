/****************************************************************************
 * graphics/vnc/vnc_server.c
 *
 *   Copyright (C) 2016 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "nuttx/config.h"

#include <stdint.h>
#include <stdlib.h>
#include <semaphore.h>
#include <string.h>
#include <queue.h>
#include <assert.h>
#include <errno.h>
#include <debug.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <nuttx/kmalloc.h>
#include <nuttx/semaphore.h>
#include <nuttx/net/net.h>

#include "vnc_server.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* Given a display number as an index, the following array can be used to
 * look-up the session structure for that display.
 */

FAR struct vnc_session_s *g_vnc_sessions[RFB_MAX_DISPLAYS];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: vnc_reset_session
 *
 * Description:
 *  Conclude the current VNC session.  This function re-initializes the
 *  session structure; it does not free either the session structure nor
 *  the framebuffer so that they may be re-used.
 *
 * Input Parameters:
 *   session - An instance of the session structure.
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void vnc_reset_session(FAR struct vnc_session_s *session,
                              FAR uint8_t *fb)
{
  FAR struct vnc_fbupdate_s *curr;
  FAR struct vnc_fbupdate_s *next;
  int i;

  /* Close any open sockets */

  if (session->state >= VNCSERVER_CONNECTED)
    {
      psock_close(&session->connect);
      psock_close(&session->listen);
    }

  /* [Re-]initialize the session. */
  /* Put all of the pre-allocated update structures into the freelist */

  sq_init(&session->updqueue);

  session->updfree.head =
    (FAR sq_entry_t *)&session->updpool[0];
  session->updfree.tail =
    (FAR sq_entry_t *)&session->updpool[CONFIG_VNCSERVER_NUPDATES-1];

  next = &session->updpool[0];
  for (i = 1; i < CONFIG_VNCSERVER_NUPDATES-1; i++)
    {
      curr = next;
      next = &session->updpool[i];
      curr->flink = next;
    }

  next->flink = NULL;

  /* Set the INITIALIZED state */

  sem_reset(&session->freesem, CONFIG_VNCSERVER_NUPDATES);
  sem_reset(&session->queuesem, 0);
  session->fb    = fb;
  session->state = VNCSERVER_INITIALIZED;
}

/****************************************************************************
 * Name: vnc_connect
 *
 * Description:
 *  Wait for a connection from the VNC client
 *
 * Input Parameters:
 *   session - An instance of the session structure.
 *   port    - The listen port to use
 *
 * Returned Value:
 *   Returns zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

static int vnc_connect(FAR struct vnc_session_s *session, int port)
{
  struct sockaddr_in addr;
  int ret;

  /* Create a listening socket */

  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = INADDR_ANY;

  ret = psock_socket(AF_INET, SOCK_STREAM, 0, &session->listen);
  if (ret < 0)
    {
      ret = -get_errno();
      return ret;
    }

  /* Bind the listening socket to a local address */

  ret = psock_bind(&session->listen, (struct sockaddr *)&addr,
                   sizeof(struct sockaddr_in));
  if (ret < 0)
    {
      ret = -get_errno();
      goto errout_with_listener;
    }

  /* Listen for a connection */

  ret = psock_listen(&session->listen, 5);
  if (ret < 0)
    {
      ret = -get_errno();
      goto errout_with_listener;
    }

  /* Connect to the client */

  ret = psock_accept(&session->listen, NULL, NULL, &session->connect);
  if (ret < 0)
    {
      ret = -get_errno();
      goto errout_with_listener;
    }

  session->state = VNCSERVER_CONNECTED;
  return OK;

errout_with_listener:
  psock_close(&session->listen);
  return ret;
}

/****************************************************************************
 * Pubic Functions
 ****************************************************************************/

/****************************************************************************
 * Name: vnc_server
 *
 * Description:
 *  The VNC server daemon.  This daemon is implemented as a kernel thread.
 *
 * Input Parameters:
 *   Standard kernel thread arguments (all ignored)
 *
 * Returned Value:
 *   This function does not return.
 *
 ****************************************************************************/

int vnc_server(int argc, FAR char *argv[])
{
  FAR struct vnc_session_s *session;
  FAR uint8_t *fb;
  int display;
  int ret;

  DEBUGASSERT(session != NULL);

  /* A single argument is expected:  A diplay port number in ASCII form */

  if (argc != 2)
    {
      gdbg("ERROR: Unexpected number of arguments: %d\n", argc);
      return EXIT_FAILURE;
    }

  display = atoi(argv[1]);
  if (display < 0 || display >= RFB_MAX_DISPLAYS)
    {
      gdbg("ERROR: Invalid display number: %d\n", display);
      return EXIT_FAILURE;
    }

  /* Allocate the framebuffer memory.  We rely on the fact that
   * the KMM allocator will align memory to 32-bits or better.
   */

  fb = (FAR uint8_t *)kmm_zalloc(RFB_SIZE);
  if (fb == NULL)
    {
      gdbg("ERROR: Failed to allocate framebuffer memory: %lu\n",
           (unsigned long)alloc);
      return -ENOMEM;
    }

  /* Allocate a session structure for this display */

  session = kmm_zalloc(sizeof(struct vnc_session_s));
  if (session == NULL)
    {
      gdbg("ERROR: Failed to allocate session\n");
      goto errout_with_fb;
    }

  g_vnc_sessions[display] = session;
  sem_init(&session->freesem, 0, CONFIG_VNCSERVER_NUPDATES);
  sem_init(&session->queuesem, 0, 0);

  /* Loop... handling each each VNC client connection to this display.  Only
   * a single client is allowed for each display.
   */

  for (; ; )
    {
      /* Release the last sesstion and [Re-]initialize the session structure
       * for the next connection.
       */

      vnc_reset_session(session, fb);
      sem_reset(&g_fbsem[display], 0);

      /* Establish a connection with the VNC client */

      ret = vnc_connect(session, RFB_DISPLAY_PORT(display));
      if (ret >= 0)
        {
          gvdbg("New VNC connection\n");

          /* Perform the VNC initialization sequence after the client has
           * sucessfully connected to the server.  Negotiate security,
           * framebuffer and color properties.
           */

          ret = vnc_negotiate(session);
          if (ret < 0)
            {
              gdbg("ERROR: Failed to negotiate security/framebuffer: %d\n",
                   ret);
              continue;
            }

          /* Start the VNC updater thread that sends all Server-to-Client
           * messages.
           */

          ret = vnc_start_updater(session);
          if (ret < 0)
            {
              gdbg("ERROR: Failed to start updater thread: %d\n", ret);
              continue;
            }

          /* Let the framebuffer driver know that we are ready to preform
           * updates.
           */

          sem_post(&g_fbsem[display]);

          /* Run the VNC receiver on this trhead.  The VNC receiver handles
           * all Client-to-Server messages.  The VNC receiver function does
           * not return until the session has been terminated (or an error
           * occurs).
           */

          ret = vnc_receiver(session);
          gvdbg("Session terminated with %d\n", ret);

          /* Stop the VNC updater thread. */

          ret = vnc_stop_updater(session);
          if (ret < 0)
            {
              gdbg("ERROR: Failed to stop updater thread: %d\n", ret);
            }
        }
    }

errout_with_fb:
  kmm_free(fb);
  return EXIT_FAILURE;
}

/****************************************************************************
 * Name: vnc_find_session
 *
 * Description:
 *  Return the session structure associated with this display.
 *
 * Input Parameters:
 *   display - The display number of interest.
 *
 * Returned Value:
 *   Returns the instance of the session structure for this display.  NULL
 *   will be returned if the server has not yet been started or if the
 *   display number is out of range.
 *
 ****************************************************************************/

FAR struct vnc_session_s *vnc_find_session(int display)
{
  FAR struct vnc_session_s *session = NULL;

  DEBUGASSERT(display >= 0 && display < RFB_MAX_DISPLAYS);

  if (display >= 0 && display < RFB_MAX_DISPLAYS)
    {
      session = g_vnc_sessions[display];
    }

  return session;
}
