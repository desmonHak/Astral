#include <kernel/cred.h>
#include <errno.h>

int cred_setuids(cred_t *cred, int uid, int euid, int suid) {
	if (uid < -1 || euid < -1 || suid < -1 || (uid == -1 && euid == -1 && suid == -1))
		return EINVAL;

	// TODO do permission check to see if its possible

	cred->uid = uid;
	cred->euid = euid;
	cred->suid = suid;

	return 0;
}

int cred_setgids(cred_t *cred, int gid, int egid, int sgid) {
	if (gid < -1 || egid < -1 || sgid < -1 || (gid == -1 && egid == -1 && sgid == -1))
		return EINVAL;

	// TODO do permission check to see if its possible

	cred->gid = gid;
	cred->egid = egid;
	cred->sgid = sgid;

	return 0;
}

void cred_getuids(cred_t *cred, int *uidp, int *euidp, int *suidp) {
	*uidp = cred->uid;
	*euidp = cred->euid;
	*suidp = cred->suid;
}

void cred_getgids(cred_t *cred, int *gidp, int *egidp, int *sgidp) {
	*gidp = cred->gid;
	*egidp = cred->egid;
	*sgidp = cred->sgid;
}