/*
 * Please do not edit this file.
 * It was generated using rpcgen.
 */

#include "nlm_prot.h"
#include <sys/cdefs.h>
#ifndef lint
/*static char sccsid[] = "from: @(#)nlm_prot.x 1.8 87/09/21 Copyr 1987 Sun Micro";*/
/*static char sccsid[] = "from: * @(#)nlm_prot.x	2.1 88/08/01 4.0 RPCSRC";*/
__RCSID("$NetBSD: nlm_prot.x,v 1.6 2000/06/07 14:30:15 bouyer Exp $");
#endif /* not lint */
__FBSDID("$FreeBSD$");

bool_t
xdr_nlm_stats(XDR *xdrs, nlm_stats *objp)
{

	if (!xdr_enum(xdrs, (enum_t *)objp))
		return (FALSE);
	return (TRUE);
}

bool_t
xdr_nlm_holder(XDR *xdrs, nlm_holder *objp)
{

	if (!xdr_bool(xdrs, &objp->exclusive))
		return (FALSE);
	if (!xdr_int(xdrs, &objp->svid))
		return (FALSE);
	if (!xdr_netobj(xdrs, &objp->oh))
		return (FALSE);
	if (!xdr_u_int(xdrs, &objp->l_offset))
		return (FALSE);
	if (!xdr_u_int(xdrs, &objp->l_len))
		return (FALSE);
	return (TRUE);
}

bool_t
xdr_nlm_testrply(XDR *xdrs, nlm_testrply *objp)
{

	if (!xdr_nlm_stats(xdrs, &objp->stat))
		return (FALSE);
	switch (objp->stat) {
	case nlm_denied:
		if (!xdr_nlm_holder(xdrs, &objp->nlm_testrply_u.holder))
			return (FALSE);
		break;
	default:
		break;
	}
	return (TRUE);
}

bool_t
xdr_nlm_stat(XDR *xdrs, nlm_stat *objp)
{

	if (!xdr_nlm_stats(xdrs, &objp->stat))
		return (FALSE);
	return (TRUE);
}

bool_t
xdr_nlm_res(XDR *xdrs, nlm_res *objp)
{

	if (!xdr_netobj(xdrs, &objp->cookie))
		return (FALSE);
	if (!xdr_nlm_stat(xdrs, &objp->stat))
		return (FALSE);
	return (TRUE);
}

bool_t
xdr_nlm_testres(XDR *xdrs, nlm_testres *objp)
{

	if (!xdr_netobj(xdrs, &objp->cookie))
		return (FALSE);
	if (!xdr_nlm_testrply(xdrs, &objp->stat))
		return (FALSE);
	return (TRUE);
}

bool_t
xdr_nlm_lock(XDR *xdrs, nlm_lock *objp)
{

	if (!xdr_string(xdrs, &objp->caller_name, LM_MAXSTRLEN))
		return (FALSE);
	if (!xdr_netobj(xdrs, &objp->fh))
		return (FALSE);
	if (!xdr_netobj(xdrs, &objp->oh))
		return (FALSE);
	if (!xdr_int(xdrs, &objp->svid))
		return (FALSE);
	if (!xdr_u_int(xdrs, &objp->l_offset))
		return (FALSE);
	if (!xdr_u_int(xdrs, &objp->l_len))
		return (FALSE);
	return (TRUE);
}

bool_t
xdr_nlm_lockargs(XDR *xdrs, nlm_lockargs *objp)
{

	if (!xdr_netobj(xdrs, &objp->cookie))
		return (FALSE);
	if (!xdr_bool(xdrs, &objp->block))
		return (FALSE);
	if (!xdr_bool(xdrs, &objp->exclusive))
		return (FALSE);
	if (!xdr_nlm_lock(xdrs, &objp->alock))
		return (FALSE);
	if (!xdr_bool(xdrs, &objp->reclaim))
		return (FALSE);
	if (!xdr_int(xdrs, &objp->state))
		return (FALSE);
	return (TRUE);
}

bool_t
xdr_nlm_cancargs(XDR *xdrs, nlm_cancargs *objp)
{

	if (!xdr_netobj(xdrs, &objp->cookie))
		return (FALSE);
	if (!xdr_bool(xdrs, &objp->block))
		return (FALSE);
	if (!xdr_bool(xdrs, &objp->exclusive))
		return (FALSE);
	if (!xdr_nlm_lock(xdrs, &objp->alock))
		return (FALSE);
	return (TRUE);
}

bool_t
xdr_nlm_testargs(XDR *xdrs, nlm_testargs *objp)
{

	if (!xdr_netobj(xdrs, &objp->cookie))
		return (FALSE);
	if (!xdr_bool(xdrs, &objp->exclusive))
		return (FALSE);
	if (!xdr_nlm_lock(xdrs, &objp->alock))
		return (FALSE);
	return (TRUE);
}

bool_t
xdr_nlm_unlockargs(XDR *xdrs, nlm_unlockargs *objp)
{

	if (!xdr_netobj(xdrs, &objp->cookie))
		return (FALSE);
	if (!xdr_nlm_lock(xdrs, &objp->alock))
		return (FALSE);
	return (TRUE);
}

bool_t
xdr_fsh_mode(XDR *xdrs, fsh_mode *objp)
{

	if (!xdr_enum(xdrs, (enum_t *)objp))
		return (FALSE);
	return (TRUE);
}

bool_t
xdr_fsh_access(XDR *xdrs, fsh_access *objp)
{

	if (!xdr_enum(xdrs, (enum_t *)objp))
		return (FALSE);
	return (TRUE);
}

bool_t
xdr_nlm_share(XDR *xdrs, nlm_share *objp)
{

	if (!xdr_string(xdrs, &objp->caller_name, LM_MAXSTRLEN))
		return (FALSE);
	if (!xdr_netobj(xdrs, &objp->fh))
		return (FALSE);
	if (!xdr_netobj(xdrs, &objp->oh))
		return (FALSE);
	if (!xdr_fsh_mode(xdrs, &objp->mode))
		return (FALSE);
	if (!xdr_fsh_access(xdrs, &objp->access))
		return (FALSE);
	return (TRUE);
}

bool_t
xdr_nlm_shareargs(XDR *xdrs, nlm_shareargs *objp)
{

	if (!xdr_netobj(xdrs, &objp->cookie))
		return (FALSE);
	if (!xdr_nlm_share(xdrs, &objp->share))
		return (FALSE);
	if (!xdr_bool(xdrs, &objp->reclaim))
		return (FALSE);
	return (TRUE);
}

bool_t
xdr_nlm_shareres(XDR *xdrs, nlm_shareres *objp)
{

	if (!xdr_netobj(xdrs, &objp->cookie))
		return (FALSE);
	if (!xdr_nlm_stats(xdrs, &objp->stat))
		return (FALSE);
	if (!xdr_int(xdrs, &objp->sequence))
		return (FALSE);
	return (TRUE);
}

bool_t
xdr_nlm_notify(XDR *xdrs, nlm_notify *objp)
{

	if (!xdr_string(xdrs, &objp->name, MAXNAMELEN))
		return (FALSE);
	if (!xdr_long(xdrs, &objp->state))
		return (FALSE);
	return (TRUE);
}

bool_t
xdr_nlm4_stats(XDR *xdrs, nlm4_stats *objp)
{

	if (!xdr_enum(xdrs, (enum_t *)objp))
		return (FALSE);
	return (TRUE);
}

bool_t
xdr_nlm4_stat(XDR *xdrs, nlm4_stat *objp)
{

	if (!xdr_nlm4_stats(xdrs, &objp->stat))
		return (FALSE);
	return (TRUE);
}

bool_t
xdr_nlm4_holder(XDR *xdrs, nlm4_holder *objp)
{

	if (!xdr_bool(xdrs, &objp->exclusive))
		return (FALSE);
	if (!xdr_uint32_t(xdrs, &objp->svid))
		return (FALSE);
	if (!xdr_netobj(xdrs, &objp->oh))
		return (FALSE);
	if (!xdr_uint64_t(xdrs, &objp->l_offset))
		return (FALSE);
	if (!xdr_uint64_t(xdrs, &objp->l_len))
		return (FALSE);
	return (TRUE);
}

bool_t
xdr_nlm4_lock(XDR *xdrs, nlm4_lock *objp)
{

	if (!xdr_string(xdrs, &objp->caller_name, MAXNAMELEN))
		return (FALSE);
	if (!xdr_netobj(xdrs, &objp->fh))
		return (FALSE);
	if (!xdr_netobj(xdrs, &objp->oh))
		return (FALSE);
	if (!xdr_uint32_t(xdrs, &objp->svid))
		return (FALSE);
	if (!xdr_uint64_t(xdrs, &objp->l_offset))
		return (FALSE);
	if (!xdr_uint64_t(xdrs, &objp->l_len))
		return (FALSE);
	return (TRUE);
}

bool_t
xdr_nlm4_share(XDR *xdrs, nlm4_share *objp)
{

	if (!xdr_string(xdrs, &objp->caller_name, MAXNAMELEN))
		return (FALSE);
	if (!xdr_netobj(xdrs, &objp->fh))
		return (FALSE);
	if (!xdr_netobj(xdrs, &objp->oh))
		return (FALSE);
	if (!xdr_fsh_mode(xdrs, &objp->mode))
		return (FALSE);
	if (!xdr_fsh_access(xdrs, &objp->access))
		return (FALSE);
	return (TRUE);
}

bool_t
xdr_nlm4_testrply(XDR *xdrs, nlm4_testrply *objp)
{

	if (!xdr_nlm4_stats(xdrs, &objp->stat))
		return (FALSE);
	switch (objp->stat) {
	case nlm_denied:
		if (!xdr_nlm4_holder(xdrs, &objp->nlm4_testrply_u.holder))
			return (FALSE);
		break;
	default:
		break;
	}
	return (TRUE);
}

bool_t
xdr_nlm4_testres(XDR *xdrs, nlm4_testres *objp)
{

	if (!xdr_netobj(xdrs, &objp->cookie))
		return (FALSE);
	if (!xdr_nlm4_testrply(xdrs, &objp->stat))
		return (FALSE);
	return (TRUE);
}

bool_t
xdr_nlm4_testargs(XDR *xdrs, nlm4_testargs *objp)
{

	if (!xdr_netobj(xdrs, &objp->cookie))
		return (FALSE);
	if (!xdr_bool(xdrs, &objp->exclusive))
		return (FALSE);
	if (!xdr_nlm4_lock(xdrs, &objp->alock))
		return (FALSE);
	return (TRUE);
}

bool_t
xdr_nlm4_res(XDR *xdrs, nlm4_res *objp)
{

	if (!xdr_netobj(xdrs, &objp->cookie))
		return (FALSE);
	if (!xdr_nlm4_stat(xdrs, &objp->stat))
		return (FALSE);
	return (TRUE);
}

bool_t
xdr_nlm4_lockargs(XDR *xdrs, nlm4_lockargs *objp)
{

	if (!xdr_netobj(xdrs, &objp->cookie))
		return (FALSE);
	if (!xdr_bool(xdrs, &objp->block))
		return (FALSE);
	if (!xdr_bool(xdrs, &objp->exclusive))
		return (FALSE);
	if (!xdr_nlm4_lock(xdrs, &objp->alock))
		return (FALSE);
	if (!xdr_bool(xdrs, &objp->reclaim))
		return (FALSE);
	if (!xdr_int(xdrs, &objp->state))
		return (FALSE);
	return (TRUE);
}

bool_t
xdr_nlm4_cancargs(XDR *xdrs, nlm4_cancargs *objp)
{

	if (!xdr_netobj(xdrs, &objp->cookie))
		return (FALSE);
	if (!xdr_bool(xdrs, &objp->block))
		return (FALSE);
	if (!xdr_bool(xdrs, &objp->exclusive))
		return (FALSE);
	if (!xdr_nlm4_lock(xdrs, &objp->alock))
		return (FALSE);
	return (TRUE);
}

bool_t
xdr_nlm4_unlockargs(XDR *xdrs, nlm4_unlockargs *objp)
{

	if (!xdr_netobj(xdrs, &objp->cookie))
		return (FALSE);
	if (!xdr_nlm4_lock(xdrs, &objp->alock))
		return (FALSE);
	return (TRUE);
}

bool_t
xdr_nlm4_shareargs(XDR *xdrs, nlm4_shareargs *objp)
{

	if (!xdr_netobj(xdrs, &objp->cookie))
		return (FALSE);
	if (!xdr_nlm4_share(xdrs, &objp->share))
		return (FALSE);
	if (!xdr_bool(xdrs, &objp->reclaim))
		return (FALSE);
	return (TRUE);
}

bool_t
xdr_nlm4_shareres(XDR *xdrs, nlm4_shareres *objp)
{

	if (!xdr_netobj(xdrs, &objp->cookie))
		return (FALSE);
	if (!xdr_nlm4_stats(xdrs, &objp->stat))
		return (FALSE);
	if (!xdr_int(xdrs, &objp->sequence))
		return (FALSE);
	return (TRUE);
}

bool_t
xdr_nlm_sm_status(XDR *xdrs, nlm_sm_status *objp)
{

	if (!xdr_string(xdrs, &objp->mon_name, LM_MAXSTRLEN))
		return (FALSE);
	if (!xdr_int(xdrs, &objp->state))
		return (FALSE);
	if (!xdr_opaque(xdrs, objp->priv, 16))
		return (FALSE);
	return (TRUE);
}

bool_t
xdr_nlm4_notify(XDR *xdrs, nlm4_notify *objp)
{

	if (!xdr_string(xdrs, &objp->name, MAXNAMELEN))
		return (FALSE);
	if (!xdr_int32_t(xdrs, &objp->state))
		return (FALSE);
	return (TRUE);
}
