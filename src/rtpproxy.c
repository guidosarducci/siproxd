/*
    Copyright (C) 2003  Thomas Ries <tries@gmx.net>

    This file is part of Siproxd.
    
    Siproxd is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
    
    Siproxd is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    along with Siproxd; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA 
*/

#include "config.h"

#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>

#include <osipparser2/osip_parser.h>

#include "siproxd.h"
#include "rtpproxy.h"
#include "log.h"

static char const ident[]="$Id: " __FILE__ ": " PACKAGE "-" VERSION "-"\
			  BUILDSTR " $";


/* configuration storage */
extern struct siproxd_config configuration;

/* use a 'fast' mutex for synchronizing - as these are portable... */
pthread_mutex_t rtp_proxytable_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * table to remember all active rtp proxy streams
 */
rtp_proxytable_t rtp_proxytable[RTPPROXY_SIZE];


/* thread id of RTP proxy */
pthread_t rtpproxy_tid=0;

/* master fd_set */
static fd_set master_fdset;
static int    master_fd_max;

/* forward declarations */
void *rtpproxy_main(void *i);
int rtp_recreate_fdset(void);
void sighdl_alm(int sig) {/* do nothing, just wake up from select() */};

/*
 * initialize and create rtp_proxy thread
 *
 * RETURNS
 *	STS_SUCCESS on success
 */
int rtpproxy_init( void ) {
   int sts;
   int arg=0;
   struct sigaction sigact;

   /* clean proxy table */
   memset (rtp_proxytable, 0, sizeof(rtp_proxytable));

   /* initialize fd set for RTP proxy thread */
   FD_ZERO(&master_fdset); /* start with an empty fdset */
   master_fd_max=-1;

   /* install signal handler for SIGALRM - used to wake up
      the rtpproxy thread from select() hibernation */
   sigact.sa_handler = sighdl_alm;
   sigemptyset(&sigact.sa_mask);
   sigact.sa_flags=0;
   sigaction(SIGALRM, &sigact, NULL);

   DEBUGC(DBCLASS_RTP,"create thread");
   sts=pthread_create(&rtpproxy_tid, NULL, rtpproxy_main, (void *)&arg);
   DEBUGC(DBCLASS_RTP,"created, sts=%i", sts);

   return STS_SUCCESS;
}

/*
 * main() of rtpproxy
 */
void *rtpproxy_main(void *arg) {
   struct timeval tv;
   fd_set fdset;
   int fd_max;
   time_t t, last_t=0;
   int i;
   int num_fd;
   osip_call_id_t callid;

   memcpy(&fdset, &master_fdset, sizeof(fdset));
   fd_max=master_fd_max;

   /* loop forever... */
   for (;;) {

      tv.tv_sec = 5;
      tv.tv_usec = 0;

      num_fd=select(fd_max+1, &fdset, NULL, NULL, &tv);
      if ((num_fd<0) && (errno==EINTR)) {
         /*
          * wakeup due to a change in the proxy table:
          * lock mutex copy master FD set and unlock
          */
         pthread_mutex_lock(&rtp_proxytable_mutex);
         memcpy(&fdset, &master_fdset, sizeof(fdset));
         fd_max=master_fd_max;
         pthread_mutex_unlock(&rtp_proxytable_mutex);
         continue;
      }

#ifdef MOREDEBUG /*&&&&*/
if (num_fd<0) {
   int i;
   WARN("select() returned error [%s]",strerror(errno));
   for (i=0;i<RTPPROXY_SIZE;i++) {
      DEBUGC(DBCLASS_RTP,"maxfd=%i",master_fd_max);
      if (rtp_proxytable[i].sock != 0) {
         DEBUGC(DBCLASS_RTP,"[%i] -> socket=%i",i, rtp_proxytable[i].sock);
      }
   } /* for i */
}
#endif
      time(&t);

      /*
       * LOCK the MUTEX
       */
      pthread_mutex_lock(&rtp_proxytable_mutex);

      /*
       * RTP relay configured
       */
      if (configuration.rtp_proxy_enable == 1) {
         /* do the relaying of received UDP packets */
         rtp_relay(num_fd, &fdset, t);
      }

      /*
       * RTP masquerading configured
       */
#if 0
      if (configuration.rtp_proxy_enable == 2) {
         /* do the masquerading - which means nothing here.
          * we only have to care that the UDP tunnels are
          * opened and closed */
         /* &&&& 
            actually we have a problem here - as the incomming
            RTP data is out of our hands, how shall I know if
            this RTP stream now really has timed out or not...
            We might do a 'poll' for the local maddr & mport
            if it is still occupied.
            So... what do we do here? I think for now I just let
            the RTP stream expire and finished.
          */
      }
#endif

      /*
       * age and clean rtp_proxytable (check every 10 seconds)
       */
      if (t > (last_t+10) ) {
         last_t = t;
	 for (i=0;i<RTPPROXY_SIZE; i++) {
            if ( (rtp_proxytable[i].sock != 0) &&
		 (rtp_proxytable[i].timestamp+configuration.rtp_timeout)<t) {
               /* time one has expired, clean it up */
               callid.number=rtp_proxytable[i].callid_number;
               callid.host=rtp_proxytable[i].callid_host;
               DEBUGC(DBCLASS_RTP,"RTP stream sock=%i %s@%s (idx=%i) "
                      "has expired", rtp_proxytable[i].sock,
                      callid.number, callid.host, i);
	       rtp_stop_fwd(&callid, 1); /* don't lock the mutex, as we own
	       				    the lock already here */
	    }
	 }
      } /* if (t>...) */

      /* copy master FD set */
      memcpy(&fdset, &master_fdset, sizeof(fdset));
      fd_max=master_fd_max;

      /*
       * UNLOCK the MUTEX
       */
      pthread_mutex_unlock(&rtp_proxytable_mutex);
   } /* for(;;) */

   return NULL;
}



/*
 * helper routines to control the RTP proxy thread
 */

/*
 * start an rtp stream on the proxy
 *
 * RETURNS
 *	STS_SUCCESS on success
 *	STS_FAILURE on error
 */
int rtp_start_fwd (osip_call_id_t *callid, int media_stream_no,
		   struct in_addr outbound_ipaddr, int *outboundport,
                   struct in_addr lcl_client_ipaddr, int lcl_clientport) {
   int j;
   int sock, port;
   int freeidx;
   int sts=STS_SUCCESS;

   if (configuration.rtp_proxy_enable == 0) return STS_SUCCESS;

   if (callid == NULL) {
      ERROR("rtp_start_fwd: callid is NULL!");
      return STS_FAILURE;
   }


   /*
    * life insurance: check size of received call_id strings
    * I don't know what the maximum allowed size within SIP is,
    * so if this test fails maybe it's just necessary to increase
    * the constants CALLIDNUM_SIZE and/or CALLIDHOST_SIZE.
    */
   if (strlen(callid->number) > CALLIDNUM_SIZE) {
      ERROR("rtp_start_fwd: received callid number "
            "has too many characters (%i, max=%i)",
            strlen(callid->number),CALLIDNUM_SIZE);
      return STS_FAILURE;
   }
   if (strlen(callid->host) > CALLIDHOST_SIZE) {
      ERROR("rtp_start_fwd: received callid host "
            "has too many characters (%i, max=%i)",
            strlen(callid->host),CALLIDHOST_SIZE);
      return STS_FAILURE;
   }

   DEBUGC(DBCLASS_RTP,"starting RTP proxy stream for: %s@%s #=%i",
          callid->number, callid->host, media_stream_no);

   /* lock mutex */
   #define return is_forbidden_in_this_code_section
   pthread_mutex_lock(&rtp_proxytable_mutex);
   /*
    * !! We now have a locked MUTEX! It is forbidden to return() from
    * !! here up to the end of this funtion where the MUTEX is
    * !! unlocked again.
    * !! Per design, a mutex is locked (for one purpose) at *exactly one*
    * !! place in the code and unlocked also at *exactly one* place.
    * !! this minimizes the risk of deadlocks.
    */

   /*
    * figure out, if this is an request to start an RTP proxy stream
    * that is already existing (identified by SIP Call-ID and
    * media_stream_no). This can be due to UDP repetitions of the
    * INVITE request...
    */
   for (j=0; j<RTPPROXY_SIZE; j++) {
      if((strcmp(rtp_proxytable[j].callid_number, callid->number)==0) &&
	 (strcmp(rtp_proxytable[j].callid_host, callid->host)==0) &&
         (rtp_proxytable[j].media_stream_no == media_stream_no) ) {
         /* return the already known port number */
         DEBUGC(DBCLASS_RTP,"RTP stream already active (port=%i, "
                "id=%s, #=%i)", rtp_proxytable[j].outboundport,
                rtp_proxytable[j].callid_number,
                rtp_proxytable[j].media_stream_no);
	 *outboundport=rtp_proxytable[j].outboundport;
	 sts = STS_SUCCESS;
	 goto unlock_and_exit;
      }
   }


   /*
    * find first free slot in rtp_proxytable
    */
   freeidx=-1;
   for (j=0; j<RTPPROXY_SIZE; j++) {
      if (rtp_proxytable[j].sock==0) {
         freeidx=j;
	 break;
      }
   }

   /* rtp_proxytable port pool full? */
   if (freeidx == -1) {
      ERROR("rtp_start_fwd: rtp_proxytable is full!");
      sts = STS_FAILURE;
      goto unlock_and_exit;
   }


   /*
    * RTP relay configured
    */
   if (configuration.rtp_proxy_enable == 1) {
      sts = rtp_relay_start_fwd(&sock, &port, outbound_ipaddr);

      /* could bind to desired port? */
      if (sock == 0) {
         ERROR("rtp_start_fwd: unable to bind to outbound port!");
         sts = STS_FAILURE;
         goto unlock_and_exit;
      }
   }

   /*
    * RTP masquerading configured
    */
   if (configuration.rtp_proxy_enable == 2) {
      /* do the masquerading - which means to open up
       * an UDP tunnel in the firewall within the desired range */
      sts = rtp_masq_start_fwd(freeidx, outbound_ipaddr, &port,
                               lcl_client_ipaddr, lcl_clientport);
      if (sts != STS_SUCCESS) {
         ERROR("rtp_start_fwd: unable to create masquerading tunnel!");
         sts = STS_FAILURE;
         goto unlock_and_exit;
      }
      sock=1; /* 0 indicates 'not allocated' - so we simply put in non-zero */
   }

   DEBUGC(DBCLASS_RTP,"rtp_start_fwd: port=%i, sock=%i freeidx=%i",
          port, sock, freeidx);

   /* found an unused port? No -> RTP port pool fully allocated */
   if (port == 0) {
      ERROR("rtp_start_fwd: no RTP port available. Check rtp_port_* config!");
      sts = STS_FAILURE;
      goto unlock_and_exit;
   }

   /* write entry into rtp_proxytable slot (freeidx) */
   rtp_proxytable[freeidx].sock=sock;
   strcpy(rtp_proxytable[freeidx].callid_number, callid->number);
   strcpy(rtp_proxytable[freeidx].callid_host, callid->host);
   rtp_proxytable[freeidx].media_stream_no = media_stream_no;
   memcpy(&rtp_proxytable[freeidx].outbound_ipaddr,
          &outbound_ipaddr, sizeof(struct in_addr));
   rtp_proxytable[freeidx].outboundport=port;
   memcpy(&rtp_proxytable[freeidx].inbound_client_ipaddr,
          &lcl_client_ipaddr, sizeof(struct in_addr));
   rtp_proxytable[freeidx].inbound_client_port=lcl_clientport;
   time(&rtp_proxytable[freeidx].timestamp);

   *outboundport=port;

   /* prepare FD set for next select operation */
   rtp_recreate_fdset();

   /* wakeup/signal rtp_proxythread from select() hibernation */
   if (!pthread_equal(rtpproxy_tid, pthread_self()))
      pthread_kill(rtpproxy_tid, SIGALRM);

unlock_and_exit:
   /* unlock mutex */
   pthread_mutex_unlock(&rtp_proxytable_mutex);
   #undef return

   return sts;
}


/*
 * stop a rtp stream on the proxy
 *
 * RETURNS
 *	STS_SUCCESS on success
 *	STS_FAILURE on error
 */
int rtp_stop_fwd (osip_call_id_t *callid, int nolock) {
   int i, sts;
   int retsts=STS_SUCCESS;
   int got_match=0;
 
   if (configuration.rtp_proxy_enable == 0) return STS_SUCCESS;

   if (callid == NULL) {
      ERROR("rtp_stop_fwd: callid is NULL!");
      return STS_FAILURE;
   }

   DEBUGC(DBCLASS_RTP,"stopping RTP proxy stream for: %s@%s",
          callid->number, callid->host);

   /*
    * lock mutex - only if not requested to skip the lock.
    * this is needed as we are also called from within
    * the RTP thread itself - and there we already own the lock.
    */
   #define return is_forbidden_in_this_code_section
   if (nolock == 0) {
      pthread_mutex_lock(&rtp_proxytable_mutex);
      /*
       * !! We now have a locked MUTEX! It is forbidden to return() from
       * !! here up to the end of this funtion where the MUTEX is
       * !! unlocked again.
       * !! Per design, a mutex is locked (for one purpose) at *exactly one*
       * !! place in the code and unlocked also at *exactly one* place.
       * !! this minimizes the risk of deadlocks.
       */
   }
   /* 
   * wakeup/signal rtp_proxythread from select() hibernation.
   * This must be done here before we close the socket, otherwise
   * we may get an select() error later from the proxy thread that
   * is still hibernating in select() now.
   */
   if (!pthread_equal(rtpproxy_tid, pthread_self()))
      pthread_kill(rtpproxy_tid, SIGALRM);

   /*
    * find the proper entry in rtp_proxytable
    * we need to loop the whole table, as there might be multiple
    * media strema active for the same callid (audio + video stream)
    */
   for (i=0; i<RTPPROXY_SIZE; i++) {
      if ((callid->number==NULL) && (callid->host==NULL)) break;
      if( rtp_proxytable[i].sock &&
         (strcmp(rtp_proxytable[i].callid_number, callid->number)==0) &&
	 (strcmp(rtp_proxytable[i].callid_host, callid->host)==0) ) {

         /*
          * RTP relay configured
          */
         if (configuration.rtp_proxy_enable == 1) {
            sts = rtp_relay_stop_fwd(rtp_proxytable[i].sock);
	    DEBUGC(DBCLASS_RTP,"closed socket %i for RTP stream "
                   "%s:%s == %s:%s  (idx=%i) sts=%i",
	           rtp_proxytable[i].sock,
	           rtp_proxytable[i].callid_number,
	           rtp_proxytable[i].callid_host,
	           callid->number, callid->host, i, sts);
            if (sts < 0) {
               ERROR("Error in close(%i): %s nolock=%i %s:%s\n",
                     rtp_proxytable[i].sock,
                     strerror(errno), nolock,
                     callid->number, callid->host);
            }
         }

         /*
          * RTP masquerading configured
          */
         if (configuration.rtp_proxy_enable == 2) {
            /* do the masquerading - which means to open up
             * an UDP tunnel in the firewall within the desired range */
            sts = rtp_masq_stop_fwd(i);
         }

         memset(&rtp_proxytable[i], 0, sizeof(rtp_proxytable[0]));
         got_match=1;
      }
 
   }

   /* did not find an active stream... */
   if (!got_match) {
      DEBUGC(DBCLASS_RTP,"rtp_stop_fwd: can't find active stream for %s@%s",
             callid->number, callid->host);
      retsts = STS_FAILURE;
      goto unlock_and_exit;
   }


   /* prepare FD set for next select operation */
   rtp_recreate_fdset();
   

unlock_and_exit:
   /*
    * unlock mutex - only if not requested to skip the lock.
    * this is needed as we are also called from within
    * the RTP thread itself - and there we already own the lock.
    */
   if (nolock == 0) {
      pthread_mutex_unlock(&rtp_proxytable_mutex);
   }
   #undef return

   return retsts;
}


/*
 * some sockets have been newly created or removed -
 * recreate the FD set for next select operation
 *
 * RETURNS
 *	STS_SUCCESS on success (always)
 */
int rtp_recreate_fdset(void) {
   int i;

   FD_ZERO(&master_fdset);
   master_fd_max=-1;
   for (i=0;i<RTPPROXY_SIZE;i++) {
      if (rtp_proxytable[i].sock != 0) {
         FD_SET(rtp_proxytable[i].sock, &master_fdset);
	 if (rtp_proxytable[i].sock > master_fd_max) {
	    master_fd_max=rtp_proxytable[i].sock;
	 }
      }
   } /* for i */
   return STS_SUCCESS;
}


/*
 * kills the rtp_proxy thread
 *
 * RETURNS
 *	-
 */
void rtpproxy_kill( void ) {
   void *thread_status;

   if (rtpproxy_tid) {
      pthread_cancel(rtpproxy_tid);
      pthread_join(rtpproxy_tid, &thread_status);
   }

   DEBUGC(DBCLASS_RTP,"killed RTP proxy thread");
   return;
}
