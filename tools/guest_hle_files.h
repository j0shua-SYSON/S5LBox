/* Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#ifndef S5LBOX_GUEST_HLE_FILES_H
#define S5LBOX_GUEST_HLE_FILES_H
#include <sys/types.h>
/* Shared by the live service and the offline HFSX adapter. Resource-heavy apps
 * can keep well over 64 sound/asset streams open at once. */
#define S5LBOX_HLE_FILE_HANDLE_LIMIT 256u
/* Run only in an authenticated, forked guest helper child. This enters the
 * selected application's chroot and permanently drops root. Never call in
 * the package-service parent or on the physical host's app filesystem. */
int s5lbox_hle_files(int peer, const char *state, const char *applications,
                     uid_t mobile_uid, gid_t mobile_gid, int guard_launches);
#endif
