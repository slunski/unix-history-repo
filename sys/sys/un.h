/*
 * Copyright (c) 1982, 1986, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)un.h	8.1 (Berkeley) 6/2/93
 * $Id: un.h,v 1.6 1995/05/30 08:14:48 rgrimes Exp $
 */

#ifndef _SYS_UN_H_
#define _SYS_UN_H_

#ifdef KERNEL
#include <sys/unpcb.h>
#endif /* KERNEL */

/*
 * Definitions for UNIX IPC domain.
 */
struct	sockaddr_un {
	u_char	sun_len;		/* sockaddr len including null */
	u_char	sun_family;		/* AF_UNIX */
	char	sun_path[104];		/* path name (gag) */
};

#ifdef KERNEL
int	uipc_usrreq __P((struct socket *, int, struct mbuf *, struct mbuf *,
			 struct mbuf *));
int	unp_connect2 __P((struct socket*,struct socket*));
void    unp_detach __P((struct unpcb *));
void    unp_disconnect __P((struct unpcb *));
void    unp_shutdown __P((struct unpcb *));
void    unp_drop __P((struct unpcb *, int));
void    unp_gc __P((void));
void	unp_dispose __P((struct mbuf *));
void    unp_scan __P((struct mbuf *, void (*)(struct file *)));
void    unp_mark __P((struct file *));
void    unp_discard __P((struct file *));
int     unp_attach __P((struct socket *));
int     unp_bind __P((struct unpcb *,struct mbuf *, struct proc *));
int     unp_connect __P((struct socket *,struct mbuf *, struct proc *));
int	unp_externalize __P((struct mbuf *));
int     unp_internalize __P((struct mbuf *, struct proc *));
#else /* KERNEL */
/* actual length of an initialized sockaddr_un */
#define SUN_LEN(su) \
	(sizeof(*(su)) - sizeof((su)->sun_path) + strlen((su)->sun_path))
#endif

#endif
