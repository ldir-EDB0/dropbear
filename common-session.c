/*
 * Dropbear - a SSH2 server
 * 
 * Copyright (c) 2002,2003 Matt Johnston
 * All rights reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE. */

#include "includes.h"
#include "session.h"
#include "dbutil.h"
#include "packet.h"
#include "algo.h"
#include "buffer.h"
#include "dss.h"
#include "ssh.h"
#include "random.h"
#include "kex.h"
#include "channel.h"
#include "atomicio.h"

struct sshsession ses; /* GLOBAL */

/* need to know if the session struct has been initialised, this way isn't the
 * cleanest, but works OK */
int sessinitdone = 0; /* GLOBAL */

/* this is set when we get SIGINT or SIGTERM, the handler is in main.c */
int exitflag = 0; /* GLOBAL */

static int ident_readln(int fd, char* buf, int count);


void(*session_remoteclosed)() = NULL;


/* called only at the start of a session, set up initial state */
void common_session_init(int sock) {

	TRACE(("enter session_init"));

	ses.remoteaddr = NULL;
	ses.remotehost = NULL;

	ses.sock = sock;
	ses.maxfd = sock;

	ses.connecttimeout = 0;
	
	kexinitialise(); /* initialise the kex state */
	chaninitialise(); /* initialise the channel state */

	ses.writepayload = buf_new(MAX_TRANS_PAYLOAD_LEN);
	ses.transseq = 0;

	ses.readbuf = NULL;
	ses.decryptreadbuf = NULL;
	ses.payload = NULL;
	ses.recvseq = 0;

	ses.requirenext = SSH_MSG_KEXINIT;
	ses.dataallowed = 0; /* don't send data yet, we'll wait until after kex */
	ses.ignorenext = 0;

	/* set all the algos to none */
	ses.keys = (struct key_context*)m_malloc(sizeof(struct key_context));
	ses.newkeys = NULL;
	ses.keys->recv_algo_crypt = &dropbear_nocipher;
	ses.keys->trans_algo_crypt = &dropbear_nocipher;
	
	ses.keys->recv_algo_mac = &dropbear_nohash;
	ses.keys->trans_algo_mac = &dropbear_nohash;

	ses.keys->algo_kex = -1;
	ses.keys->algo_hostkey = -1;
	ses.keys->recv_algo_comp = DROPBEAR_COMP_NONE;
	ses.keys->trans_algo_comp = DROPBEAR_COMP_NONE;

#ifndef DISABLE_ZLIB
	ses.keys->recv_zstream = NULL;
	ses.keys->trans_zstream = NULL;
#endif

	/* key exchange buffers */
	ses.session_id = NULL;
	ses.kexhashbuf = NULL;
	ses.transkexinit = NULL;
	ses.dh_K = NULL;
	ses.remoteident = NULL;

	ses.authdone = 0;

	ses.chantypes = NULL;

	ses.allowprivport = 0;


	TRACE(("leave session_init"));
}

/* clean up a session on exit */
void common_session_cleanup() {
	
	TRACE(("enter session_cleanup"));
	
	/* we can't cleanup if we don't know the session state */
	if (!sessinitdone) {
		TRACE(("leave session_cleanup: !sessinitdone"));
		return;
	}
	
	m_free(ses.session_id);
	m_burn(ses.keys, sizeof(struct key_context));
	m_free(ses.keys);

	chancleanup();

	TRACE(("leave session_cleanup"));
}

/* Check all timeouts which are required. Currently these are the time for
 * user authentication, and the automatic rekeying. */
void checktimeouts() {

	struct timeval tv;
	long secs;

	if (gettimeofday(&tv, 0) < 0) {
		dropbear_exit("Error getting time");
	}

	secs = tv.tv_sec;
	
	if (ses.connecttimeout != 0 && secs > ses.connecttimeout) {
			dropbear_close("Timeout before auth");
	}

	/* we can't rekey if we haven't done remote ident exchange yet */
	if (ses.remoteident == NULL) {
		return;
	}

	if (!ses.kexstate.sentkexinit
			&& (secs - ses.kexstate.lastkextime >= KEX_REKEY_TIMEOUT
			|| ses.kexstate.datarecv+ses.kexstate.datatrans >= KEX_REKEY_DATA)){
		TRACE(("rekeying after timeout or max data reached"));
		send_msg_kexinit();
	}
}
void session_identification() {

	/* max length of 255 chars */
	char linebuf[256];
	int len = 0;
	char done = 0;

	/* write our version string, this blocks */
	if (atomicio(write, ses.sock, LOCAL_IDENT "\r\n",
				strlen(LOCAL_IDENT "\r\n")) == DROPBEAR_FAILURE) {
		dropbear_exit("Error writing ident string");
	}

	len = ident_readln(ses.sock, linebuf, 256);
	if (len >= 4 && memcmp(linebuf, "SSH-", 4) == 0) {
		/* start of line matches */
		done = 1;
	}

	if (!done) {
		dropbear_exit("Failed to get client version");
	} else {
		/* linebuf is already null terminated */
		ses.remoteident = m_malloc(len);
		memcpy(ses.remoteident, linebuf, len);
	}

	TRACE(("remoteident: %s", ses.remoteident));

}

/* returns the length including null-terminating zero on success,
 * or -1 on failure */
static int ident_readln(int fd, char* buf, int count) {
	
	char in;
	int pos = 0;
	int num = 0;
	fd_set fds;
	struct timeval timeout;

	TRACE(("enter ident_readln"));

	if (count < 1) {
		return -1;
	}

	FD_ZERO(&fds);

	/* select since it's a non-blocking fd */
	
	/* leave space to null-terminate */
	while (pos < count-1) {

		FD_SET(fd, &fds);

		timeout.tv_sec = 1;
		timeout.tv_usec = 0;
		if (select(fd+1, &fds, NULL, NULL, &timeout) < 0) {
			if (errno == EINTR) {
				continue;
			}
			TRACE(("leave ident_readln: select error"));
			return -1;
		}

		checktimeouts();
		
		/* Have to go one byte at a time, since we don't want to read past
		 * the end, and have to somehow shove bytes back into the normal
		 * packet reader */
		if (FD_ISSET(fd, &fds)) {
			num = read(fd, &in, 1);
			/* a "\n" is a newline, "\r" we want to read in and keep going
			 * so that it won't be read as part of the next line */
			if (num < 0) {
				/* error */
				if (errno == EINTR) {
					continue; /* not a real error */
				}
				TRACE(("leave ident_readln: read error"));
				return -1;
			}
			if (num == 0) {
				/* EOF */
				TRACE(("leave ident_readln: EOF"));
				return -1;
			}
			if (in == '\n') {
				/* end of ident string */
				break;
			}
			/* we don't want to include '\r's */
			if (in != '\r') {
				buf[pos] = in;
				pos++;
			}
		}
	}

	buf[pos] = '\0';
	TRACE(("leave ident_readln: return %d", pos+1));
	return pos+1;
}
