/*
 * S5LBox -- bounded host-side rootfs work-image provisioning.
 *
 * The source image is opened read-only and is never exposed through a writable
 * handle.  All edits happen under an exclusive, unpublished temporary name in
 * the destination directory.  Publication is no-replace and atomic at the
 * directory-entry level.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef _WIN32
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#endif

#include "rootfs_work.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define HFS_VH_OFF 1024u
#define HFS_VH_LEN 512u
#define HFS_SIG_HFSPLUS 0x482bu
#define HFS_SIG_HFSX 0x4858u
#define HFS_ATTR_UNMOUNTED (1u << 8)
#define HFS_ATTR_BOOT_INCONSISTENT (1u << 11)
#define HFS_ATTR_JOURNALED (1u << 13)
#define HFS_ATTR_SOFTWARE_LOCK (1u << 15)
#define ROOTFS_TEMP_ATTEMPTS 128u
#define ROOTFS_EINTR_RETRY_LIMIT 64u

static const uint8_t FSTAB_STOCK[] =
    "/dev/disk0s1 / hfs ro 0 1\n"
    "/dev/disk0s2 /private/var hfs rw,nosuid,nodev 0 2\n";

/*
 * /System/Library/LaunchDaemons/com.apple.SpringBoard.plist, stock iPhone OS
 * 3.1.3 (7E18).  Plain XML, 1490 bytes, one extent, one allocation block.
 * This is the search pattern, not a template: the rewrite below refuses unless
 * these exact bytes occur exactly once in the whole work image.
 */
static const uint8_t CA_PLIST_STOCK[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "\t<key>KeepAlive</key>\n"
    "\t<true/>\n"
    "\t<key>Label</key>\n"
    "\t<string>com.apple.SpringBoard</string>\n"
    "\t<key>MachServices</key>\n"
    "\t<dict>\n"
    "\t\t<key>com.apple.springboard.watchdogserver</key>\n"
    "\t\t<true/>\n"
    "\t\t<key>com.apple.SBUserNotification</key>\n"
    "\t\t<true/>\n"
    "\t\t<key>PurpleSystemEventPort</key>\n"
    "\t\t<dict>\n"
    "\t\t\t<key>ResetAtClose</key>\n"
    "\t\t\t<true/>\n"
    "\t\t</dict>\n"
    "\t\t<key>com.apple.CARenderServer</key>\n"
    "\t\t<dict>\n"
    "\t\t\t<key>ResetAtClose</key>\n"
    "\t\t\t<true/>\n"
    "\t\t</dict>\n"
    "\t\t<key>com.apple.iohideventsystem</key>\n"
    "\t\t<dict>\n"
    "\t\t\t<key>ResetAtClose</key>\n"
    "\t\t\t<true/>\n"
    "\t\t</dict>\n"
    "\t\t<key>com.apple.springboard</key>\n"
    "\t\t<dict>\n"
    "\t\t\t<key>ResetAtClose</key>\n"
    "\t\t\t<true/>\n"
    "\t\t</dict>\n"
    "\t\t<key>com.apple.springboard.UIKit.migserver</key>\n"
    "\t\t<dict>\n"
    "\t\t\t<key>ResetAtClose</key>\n"
    "\t\t\t<true/>\n"
    "\t\t</dict>\n"
    "\t\t<key>com.apple.springboard.services</key>\n"
    "\t\t<dict>\n"
    "\t\t\t<key>ResetAtClose</key>\n"
    "\t\t\t<true/>\n"
    "\t\t</dict>\n"
    "\t\t<key>com.apple.springboard.remotenotifications</key>\n"
    "\t\t<dict>\n"
    "\t\t\t<key>ResetAtClose</key>\n"
    "\t\t\t<true/>\n"
    "\t\t</dict>\n"
    "\t\t<key>com.apple.smsserver</key>\n"
    "\t\t<dict>\n"
    "\t\t\t<key>ResetAtClose</key>\n"
    "\t\t\t<true/>\n"
    "\t\t</dict>\n"
    "\t</dict>\n"
    "\t<key>ProgramArguments</key>\n"
    "\t<array>\n"
    "\t\t<string>/System/Library/CoreServices/SpringBoard.app/SpringBoard</string>\n"
    "\t</array>\n"
    "\t<key>UserName</key>\n"
    "\t<string>mobile</string>\n"
    "\t<key>ThrottleInterval</key>\n"
    "\t<integer>5</integer>\n"
    "\t<key>EmbeddedPrivilegeDispensation</key>\n"
    "\t<true/>\n"
    "</dict>\n"
    "</plist>\n";

/*
 * The replacement.  PROVENANCE: this is Apple's own plist above with ONE
 * addition -- an EnvironmentVariables dictionary containing CA_ENABLE_MBX2D=0
 * -- and it is deliberately the same 1490 bytes.  Parsed with a property-list
 * reader the two differ by exactly that one key: every other key, value and
 * nesting level is identical, and the DOCTYPE is kept.
 *
 * The indentation is NOT the stock indentation, and that is the whole trick.
 * The new dictionary costs 100 bytes, so all 122 leading tabs were removed and
 * 29 of them re-added, one per line, from the DOCTYPE down.  XML treats
 * inter-element whitespace as insignificant, so this changes nothing a parser
 * sees while making the file byte-for-byte the length of the record it
 * replaces -- which is what lets logicalSize, totalBlocks and the file's single
 * extent stay exactly as they are.  Do not "tidy" this back up: any edit that
 * changes the length turns a safe overwrite into HFS+ catalog surgery, and the
 * length gate below will refuse to build rather than let that happen quietly.
 *
 * What it configures is Apple's switch, not ours.  CA::WindowServer::
 * MBXServer::MBXServer (0x31241d78 in this build's QuartzCore) reads
 * CA_ENABLE_MBX2D with getenv(), falls back to LK_ENABLE_MBX2D, and treats
 * "absent" as ENABLED; the only reader of that flag, MBXServer::mbx2d_context()
 * at 0x31241a8c, returns NULL early when it is 0, and both of its callers then
 * take CA::WindowServer::Server's software path (sw_renderer -> _CARenderOGLNew
 * -> CA::OGL::SWContext).  With the flag left at its default, MBX2D talks to a
 * PowerVR kext that never started -- this VM un-matches arm-io/mbx -- and
 * _mbx2DDisable+0x20 stores through a NULL global context, which is the
 * KERN_PROTECTION_FAILURE at 0x00000048 that kills SpringBoard and leaves
 * launchd respawning it forever.  launchd honours the key: /sbin/launchd's key
 * table carries EnvironmentVariables and UserEnvironmentVariables next to
 * ThrottleInterval and MachServices, and it imports _setenv.
 */
static const uint8_t CA_PLIST_SOFTWARE_RENDER[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "\t<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "\t<plist version=\"1.0\">\n"
    "\t<dict>\n"
    "\t<key>EnvironmentVariables</key>\n"
    "\t<dict>\n"
    "\t<key>CA_ENABLE_MBX2D</key>\n"
    "\t<string>0</string>\n"
    "\t</dict>\n"
    "\t<key>KeepAlive</key>\n"
    "\t<true/>\n"
    "\t<key>Label</key>\n"
    "\t<string>com.apple.SpringBoard</string>\n"
    "\t<key>MachServices</key>\n"
    "\t<dict>\n"
    "\t<key>com.apple.springboard.watchdogserver</key>\n"
    "\t<true/>\n"
    "\t<key>com.apple.SBUserNotification</key>\n"
    "\t<true/>\n"
    "\t<key>PurpleSystemEventPort</key>\n"
    "\t<dict>\n"
    "\t<key>ResetAtClose</key>\n"
    "\t<true/>\n"
    "\t</dict>\n"
    "\t<key>com.apple.CARenderServer</key>\n"
    "\t<dict>\n"
    "\t<key>ResetAtClose</key>\n"
    "\t<true/>\n"
    "\t</dict>\n"
    "\t<key>com.apple.iohideventsystem</key>\n"
    "<dict>\n"
    "<key>ResetAtClose</key>\n"
    "<true/>\n"
    "</dict>\n"
    "<key>com.apple.springboard</key>\n"
    "<dict>\n"
    "<key>ResetAtClose</key>\n"
    "<true/>\n"
    "</dict>\n"
    "<key>com.apple.springboard.UIKit.migserver</key>\n"
    "<dict>\n"
    "<key>ResetAtClose</key>\n"
    "<true/>\n"
    "</dict>\n"
    "<key>com.apple.springboard.services</key>\n"
    "<dict>\n"
    "<key>ResetAtClose</key>\n"
    "<true/>\n"
    "</dict>\n"
    "<key>com.apple.springboard.remotenotifications</key>\n"
    "<dict>\n"
    "<key>ResetAtClose</key>\n"
    "<true/>\n"
    "</dict>\n"
    "<key>com.apple.smsserver</key>\n"
    "<dict>\n"
    "<key>ResetAtClose</key>\n"
    "<true/>\n"
    "</dict>\n"
    "</dict>\n"
    "<key>ProgramArguments</key>\n"
    "<array>\n"
    "<string>/System/Library/CoreServices/SpringBoard.app/SpringBoard</string>\n"
    "</array>\n"
    "<key>UserName</key>\n"
    "<string>mobile</string>\n"
    "<key>ThrottleInterval</key>\n"
    "<integer>5</integer>\n"
    "<key>EmbeddedPrivilegeDispensation</key>\n"
    "<true/>\n"
    "</dict>\n"
    "</plist>\n";

/*
 * Size neutrality is the precondition for every claim made above, so state it
 * as a build-time constraint rather than a comment.  A negative array size is
 * the portable C11 way to say "this translation unit does not compile if the
 * two records ever stop being the same length".
 */
typedef char ca_plist_is_size_neutral[
    (sizeof(CA_PLIST_SOFTWARE_RENDER) == sizeof(CA_PLIST_STOCK)) ? 1 : -1];

/*
 * /System/Library/LaunchDaemons/com.apple.chud.pilotfish.plist, stock iPhone
 * OS 3.1.3 (7E18).  Plain XML, 530 bytes, one extent.  As above, this is the
 * search pattern rather than a template, and the rewrite refuses unless these
 * exact bytes occur exactly once in the whole work image.
 *
 * WHY THIS FILE IS THE ONE.  The image ships no PPP launchd job at all, and
 * this provisioner cannot split a catalog B-tree node, so creating a file at a
 * path that says what it is was not on the table.  What was on the table is
 * that several shipped plists name binaries this image does not contain, which
 * makes overwriting one of them size-neutral with no collateral damage
 * whatsoever.  Four were surveyed: chud.pilotfish (530 B, points at
 * /Developer/usr/libexec/pilotfish), chud.chum (515 B), graphicsservices.sample
 * (447 B) and tcpdump.server (433 B).  /Developer, /usr/local and
 * /usr/libexec/tcpdumpserver are all absent, so all four are inert -- but a
 * fully-argumented pppd job is 515 bytes, so only the largest of them has the
 * budget, and an earlier revision of the plan that named tcpdump.server was
 * simply wrong about the arithmetic.
 *
 * Note the DOCTYPE says "Apple Inc.", where the SpringBoard plist above says
 * "Apple".  Both are byte-exact transcriptions of what the image contains; the
 * two files were produced by different Apple toolchains and normalising them
 * to each other would break the pattern match.
 *
 * This is a HIJACK, and docs/networking.md says in as many words that PPP over
 * an emulated UART is a temporary workaround pending real drivers and
 * controllers.  It should be replaced by a job at an honest path the day this
 * provisioner can create one.
 */
static const uint8_t PPP_PLIST_STOCK[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple Inc.//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "        <key>Label</key>\n"
    "        <string>com.apple.chud.pilotfish</string>\n"
    "        <key>MachServices</key>\n"
    "        <dict>\n"
    "                <key>com.apple.chud.pilotfish</key>\n"
    "\t\t\t\t<true/>\n"
    "        </dict>\n"
    "        <key>ProgramArguments</key>\n"
    "        <array>\n"
    "                <string>/Developer/usr/libexec/pilotfish</string>\n"
    "        </array>\n"
    "</dict>\n"
    "</plist>\n";

/*
 * The replacement: a launchd job that runs the guest's own signed
 * /usr/sbin/pppd against uart4, at exactly the same 530 bytes.
 *
 * EVERY ARGUMENT IS THERE FOR A STATED REASON.  This is Apple pppd 2.4.2,
 * armv6, 284,608 bytes, with its tty channel intact (_tty_channel,
 * _set_up_tty, _connect_tty, _tty_establish_ppp, _options_for_tty are all
 * present as symbols).  The tty channel is pppd's DEFAULT channel and the
 * string "PPPSerial" does not appear in the binary, so no link plugin is
 * involved -- which matters, because PPPSerial.ppp does not ship either.
 *
 *   /dev/tty.debug   NOT /dev/uart.debug, and run78 is why.
 *
 *                    AppleOnboardSerialBSDClient names its devfs node after
 *                    the device tree child, and uart4's child is `debug`, so
 *                    /dev/uart.debug is the node that exists -- CommCenter's
 *                    own strings carry it and /dev/uart.umts verbatim.  It
 *                    opens, and tcgetattr and tcsetattr both work on it.  It
 *                    is still the wrong node, because it is not a tty:
 *                    AppleOnboardSerialBSDClient::start registers its cdevsw
 *                    (0xc0478060) with d_ttys = NULL and d_type = 0 rather
 *                    than D_TTY, and the ioctl switch at 0xc046fd52 handles
 *                    TIOCGETA and TIOCSETA* but has no arm for TIOCSETD or
 *                    TIOCSCTTY -- both fall to `movs r0,#0x19` at 0xc046fe30,
 *                    which is ENOTTY.  That is exactly what run78's console
 *                    printed, twice.
 *
 *                    The path cannot reach ttioctl at all, so linesw[PPPDISC]
 *                    is never consulted: _ttioctl's Thumb pointer 0xc01368a9
 *                    occurs EXACTLY ONCE in the whole 7.9 MB kernelcache, at
 *                    0xc0469430 inside IOSerialFamily, and AppleOnboardSerial
 *                    never references it.
 *
 *                    IOSerialBSDClient is the kext that publishes tty nodes
 *                    (its name templates are at 0xc046b164 / 0xc046b158), and
 *                    AppleOnboardSerialSync's metaclass names a superclass at
 *                    0xc046d280 inside IOSerialFamily's image, so a
 *                    /dev/tty.debug MAY also be published.  That is not
 *                    proven, and this argument is the experiment: pppd probes
 *                    it for free.  A node that does not exist makes
 *                    setdevname() return 0, and tty_process_extra_options+0x68
 *                    (0x00021628) prints "no device specified and stdin is not
 *                    a tty" and exits TWO, which is distinguishable from every
 *                    other failure this job has produced.
 *
 *                    If it is absent, the fallback is `notty`: get_pty
 *                    (0x0001bd70) hands pppd a pty slave, which is a real BSD
 *                    tty where TIOCSETD applies, and pppd then shuttles bytes
 *                    over stdin/stdout, which the raw cdev does support.  That
 *                    costs a StandardInPath key and puts pppd's log on the
 *                    same line as its frames, so it is second choice.
 *   local            do not use modem control lines.  pppd watches for carrier
 *                    by default, and this UART has no DCD to raise.
 *   nocrtscts        do not ask for hardware flow control.  uart4 carries
 *                    `no-flow-control`, so the driver short-circuits
 *                    getFlowStatus to "asserted" without reading UMSTAT; the
 *                    flag keeps pppd's request consistent with that.
 *   nodetach         MANDATORY under launchd.  pppd daemonises by default, and
 *                    a job whose first process exits immediately is a job
 *                    launchd believes has died.
 *
 * StandardOutPath = /dev/console IS THE MOST IMPORTANT LINE IN THIS FILE,
 * and it is here because of a measurement rather than a preference.
 *
 * IT WAS StandardErrorPath UNTIL 5944b5f READ THE BINARY, and that is why
 * run75 came back with a console byte-identical to run74's.  pppd's error()
 * and fatal() both reach one emitter at 0x0002245c, which syslog()s the
 * message and then writes it to `*log_to_fd`.  `_log_to_fd` lives at
 * 0x00039c70 with a file image of ONE -- stdout -- and `nodetach` means
 * nothing ever lowers it.  File descriptor 2 is never written to at all, so
 * the previous key pointed launchd at a stream pppd does not use, and
 * /dev/null still consumed the only message that could name the failure.
 *
 * The rename frees exactly TWO bytes -- the key name occurs once per line,
 * inside <key>...</key>, not once per tag -- and those two go back into the
 * filler so the record is still exactly 530.  The four dict-level <key>s keep
 * their single leading tab and the <array>/</array> pair gains one each.  XML
 * treats inter-element whitespace as insignificant, so a property-list reader
 * still sees four keys and a five-argument job.
 *
 * run74 established that this job already works as far as the exec: launchd
 * posix_spawned /usr/sbin/pppd at instruction 557,124,470 as pid 19, pppd ran
 * for 182 million instructions, and then called exit(1) at 739,184,188
 * (_exit1 status 0x100).  ONE is pppd 2.4.2's EXIT_FATAL_ERROR, which is
 * already informative: it is NOT EXIT_OPTION_ERROR (2), NOT EXIT_NOT_ROOT (3),
 * NOT EXIT_NO_KERNEL_SUPPORT (4) and NOT EXIT_OPEN_FAILED (7), so the device
 * node was not the problem and the command line parsed.  It is a fatal() call,
 * and fatal() writes its message to stderr.  Without this key launchd gives
 * the job /dev/null and that message is destroyed -- which is exactly the
 * state run74 was read in, and why it could only report an exit code.
 * `StandardErrorPath` is confirmed present in the image (5 occurrences of the
 * key name; /dev/console appears 7 times).
 *
 * TWO ARGUMENTS WERE SPENT TO BUY IT, and the trade is stated rather than
 * quietly made.  The key and its value cost 61 bytes and the budget had 15.
 *
 *   `noauth` was dropped.  pppd 2.4.2 only sets auth_required when a default
 *   route already exists, and at this point in the boot there is none, so it
 *   was determinism rather than necessity from the start.
 *
 *   The explicit `115200` was dropped, and this one is a real trade.  With no
 *   speed on the command line, set_up_tty() reads the port's current one back
 *   and calls fatal("Baud rate for %s is 0; need explicit baud rate") if it
 *   reads zero.  That risk is now bounded rather than blind: AppleOnboardSerial
 *   programs a default of 19200 8N1 at 0xc047244a during start(), before any
 *   tty is opened, and that is the only baud constant in either serial kext.
 *   And the trade is self-resolving -- if the readback IS zero, that fatal()
 *   is now the message that appears on the console, which is more than the
 *   previous configuration could tell us.  Putting the speed back is a
 *   one-line change once the console says what pppd is actually complaining
 *   about.
 *
 * MachServices is dropped deliberately.  With no Mach port to hold there is no
 * on-demand path, so RunAtLoad is the whole lifecycle; keeping the port would
 * register a service for a binary that no longer exists.  The Label is kept
 * byte-identical so the console messages launchd prints about this job remain
 * greppable against the name the file already had, and so the label cannot
 * disagree with the filename.
 *
 * A MISSING /etc/ppp/options IS NOT FATAL -- pppd warns and continues on its
 * built-in defaults plus argv -- which is what makes a standalone invocation
 * viable at all, since this provisioner cannot create that file either.
 *
 * THE INDENTATION IS FILLER, exactly as it is in CA_PLIST_SOFTWARE_RENDER, and
 * for the same reason.  The job's own bytes come to 526; the budget is 530; so
 * there are exactly FOUR leading tabs, one on each dict-level <key>.  XML
 * treats inter-element whitespace as insignificant, so a property-list reader
 * sees exactly the four keys and the five-argument job above and nothing else.
 * Do not reformat this: the length gate below is a compile error, not a
 * comment.
 */
static const uint8_t PPP_PLIST_JOB[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple Inc.//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "\t<key>Label</key>\n"
    "<string>com.apple.chud.pilotfish</string>\n"
    "\t<key>RunAtLoad</key>\n"
    "<true/>\n"
    "\t<key>StandardOutPath</key>\n"
    "<string>/dev/console</string>\n"
    "\t<key>ProgramArguments</key>\n"
    "\t\t<array>\n"
    "<string>/usr/sbin/pppd</string>\n"
    "<string>/dev/tty.debug</string>\n"
    "<string>local</string>\n"
    "<string>nocrtscts</string>\n"
    "<string>nodetach</string>\n"
    "\t</array>\n"
    "</dict>\n"
    "</plist>\n";

/* Both halves of the contract, as build-time constraints rather than comments:
 * the replacement is the same length as the record it overwrites, and that
 * length is the 530 bytes the stock file measures.  Either one drifting turns
 * a safe overwrite into HFS+ catalog surgery. */
typedef char ppp_plist_is_size_neutral[
    (sizeof(PPP_PLIST_JOB) == sizeof(PPP_PLIST_STOCK)) ? 1 : -1];
typedef char ppp_plist_is_530_bytes[
    (sizeof(PPP_PLIST_STOCK) - 1u == 530u) ? 1 : -1];

typedef struct host_file {
#ifdef _WIN32
    HANDLE handle;
#else
    int descriptor;
#endif
} host_file_t;

typedef struct file_stamp {
    uint64_t size;
    uint64_t identity_a;
    uint64_t identity_b;
    uint64_t modified_a;
    uint64_t modified_b;
    uint64_t changed_a;
    uint64_t changed_b;
    uint32_t links;
} file_stamp_t;

typedef struct destination_dir {
#ifdef _WIN32
    HANDLE handle;
    char *full_path;
    char *destination_path;
    char *temporary_path;
#else
    int descriptor;
    char *leaf;
    char temporary_leaf[80];
#endif
} destination_dir_t;

typedef struct hfs_volume {
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t next_alloc;
    uint32_t attributes;
    uint64_t alloc_bytes;
    uint32_t alloc_fork_blocks;
    uint32_t ext_start[8];
    uint32_t ext_count[8];
    uint32_t nbits;
} hfs_volume_t;

static uint16_t read_be16(const uint8_t *bytes) {
    return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

static uint32_t read_be32(const uint8_t *bytes) {
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

static uint64_t read_be64(const uint8_t *bytes) {
    return ((uint64_t)read_be32(bytes) << 32) | read_be32(bytes + 4);
}

static void write_be16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static void write_be32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static void write_be64(uint8_t *bytes, uint64_t value) {
    write_be32(bytes, (uint32_t)(value >> 32));
    write_be32(bytes + 4, (uint32_t)value);
}

static void result_reset(rootfs_work_result_t *result) {
    memset(result, 0, sizeof(*result));
    result->status = ROOTFS_WORK_OK;
    result->stage = ROOTFS_WORK_STAGE_NONE;
    result->fstab_offset = UINT64_MAX;
    result->ca_plist_offset = UINT64_MAX;
    result->ppp_plist_offset = UINT64_MAX;
    result->detail[0] = '\0';
}

static rootfs_work_status_t result_fail(rootfs_work_result_t *result,
                                        rootfs_work_status_t status,
                                        rootfs_work_stage_t stage,
                                        int system_error,
                                        const char *format, ...) {
    va_list arguments;

    result->status = status;
    result->stage = stage;
    result->system_error = system_error;
    va_start(arguments, format);
    (void)vsnprintf(result->detail, sizeof(result->detail), format, arguments);
    va_end(arguments);
    result->detail[sizeof(result->detail) - 1u] = '\0';
    return status;
}

const char *rootfs_work_status_name(rootfs_work_status_t status) {
    switch (status) {
    case ROOTFS_WORK_OK: return "ok";
    case ROOTFS_WORK_INVALID_ARGUMENT: return "invalid-argument";
    case ROOTFS_WORK_NO_MEMORY: return "no-memory";
    case ROOTFS_WORK_PATH_UNSAFE: return "unsafe-path";
    case ROOTFS_WORK_SOURCE_OPEN_FAILED: return "source-open-failed";
    case ROOTFS_WORK_SOURCE_NOT_REGULAR: return "source-not-regular";
    case ROOTFS_WORK_SOURCE_ALIAS: return "source-alias";
    case ROOTFS_WORK_SOURCE_BUSY: return "source-busy";
    case ROOTFS_WORK_DESTINATION_EXISTS: return "destination-exists";
    case ROOTFS_WORK_DESTINATION_OPEN_FAILED: return "destination-open-failed";
    case ROOTFS_WORK_TEMP_CREATE_FAILED: return "temp-create-failed";
    case ROOTFS_WORK_READ_FAILED: return "read-failed";
    case ROOTFS_WORK_WRITE_FAILED: return "write-failed";
    case ROOTFS_WORK_SYNC_FAILED: return "sync-failed";
    case ROOTFS_WORK_SOURCE_CHANGED: return "source-changed";
    case ROOTFS_WORK_SOURCE_IDENTITY_MISMATCH:
        return "source-identity-mismatch";
    case ROOTFS_WORK_HFS_INVALID: return "hfs-invalid";
    case ROOTFS_WORK_FSTAB_NOT_UNIQUE: return "fstab-not-unique";
    case ROOTFS_WORK_FSTAB_LINE_INVALID: return "fstab-line-invalid";
    case ROOTFS_WORK_CA_PLIST_NOT_UNIQUE: return "ca-plist-not-unique";
    case ROOTFS_WORK_CA_PLIST_INVALID: return "ca-plist-invalid";
    case ROOTFS_WORK_PPP_PLIST_NOT_UNIQUE: return "ppp-plist-not-unique";
    case ROOTFS_WORK_PPP_PLIST_INVALID: return "ppp-plist-invalid";
    case ROOTFS_WORK_GROW_INVALID: return "grow-invalid";
    case ROOTFS_WORK_PROVISION_INVALID: return "provision-invalid";
    case ROOTFS_WORK_PROVISION_UNSUPPORTED: return "provision-unsupported";
    case ROOTFS_WORK_PROVISION_CATALOG_CORRUPT:
        return "provision-catalog-corrupt";
    case ROOTFS_WORK_PROVISION_PARENT_MISSING:
        return "provision-parent-missing";
    case ROOTFS_WORK_PROVISION_EXISTS: return "provision-exists";
    case ROOTFS_WORK_PROVISION_NODE_FULL: return "provision-node-full";
    case ROOTFS_WORK_PROVISION_LEAF_HEAD: return "provision-leaf-head";
    case ROOTFS_WORK_PROVISION_SPLIT_UNSUPPORTED:
        return "provision-split-unsupported";
    case ROOTFS_WORK_PROVISION_BTREE_FULL: return "provision-btree-full";
    case ROOTFS_WORK_PROVISION_NO_SPACE: return "provision-no-space";
    case ROOTFS_WORK_PROVISION_LIMIT: return "provision-limit";
    case ROOTFS_WORK_RANGE_ERROR: return "range-error";
    case ROOTFS_WORK_PUBLISH_FAILED: return "publish-failed";
    case ROOTFS_WORK_PUBLISH_DURABILITY_FAILED:
        return "publish-durability-failed";
    }
    return "unknown";
}

const char *rootfs_work_stage_name(rootfs_work_stage_t stage) {
    switch (stage) {
    case ROOTFS_WORK_STAGE_NONE: return "none";
    case ROOTFS_WORK_STAGE_ARGUMENTS: return "arguments";
    case ROOTFS_WORK_STAGE_SOURCE_PATH: return "source-path";
    case ROOTFS_WORK_STAGE_DESTINATION_PATH: return "destination-path";
    case ROOTFS_WORK_STAGE_SOURCE_OPEN: return "source-open";
    case ROOTFS_WORK_STAGE_SOURCE_VALIDATE: return "source-validate";
    case ROOTFS_WORK_STAGE_SOURCE_IDENTITY: return "source-identity";
    case ROOTFS_WORK_STAGE_TEMP_CREATE: return "temp-create";
    case ROOTFS_WORK_STAGE_COPY: return "copy";
    case ROOTFS_WORK_STAGE_COPY_VERIFY: return "copy-verify";
    case ROOTFS_WORK_STAGE_FSTAB_SCAN: return "fstab-scan";
    case ROOTFS_WORK_STAGE_FSTAB_WRITE: return "fstab-write";
    case ROOTFS_WORK_STAGE_CA_PLIST_SCAN: return "ca-plist-scan";
    case ROOTFS_WORK_STAGE_CA_PLIST_WRITE: return "ca-plist-write";
    case ROOTFS_WORK_STAGE_PPP_PLIST_SCAN: return "ppp-plist-scan";
    case ROOTFS_WORK_STAGE_PPP_PLIST_WRITE: return "ppp-plist-write";
    case ROOTFS_WORK_STAGE_GROW_PLAN: return "grow-plan";
    case ROOTFS_WORK_STAGE_GROW_WRITE: return "grow-write";
    case ROOTFS_WORK_STAGE_PROVISION_PLAN: return "provision-plan";
    case ROOTFS_WORK_STAGE_PROVISION_WRITE: return "provision-write";
    case ROOTFS_WORK_STAGE_FINAL_VALIDATE: return "final-validate";
    case ROOTFS_WORK_STAGE_FLUSH: return "flush";
    case ROOTFS_WORK_STAGE_PUBLISH: return "publish";
    case ROOTFS_WORK_STAGE_DIRECTORY_SYNC: return "directory-sync";
    case ROOTFS_WORK_STAGE_CLEANUP: return "cleanup";
    }
    return "unknown";
}

static char *copy_string_n(const char *source, size_t length) {
    char *copy;

    if (length == SIZE_MAX)
        return NULL;
    copy = (char *)malloc(length + 1u);
    if (!copy)
        return NULL;
    memcpy(copy, source, length);
    copy[length] = '\0';
    return copy;
}

static bool stamp_equal(const file_stamp_t *left, const file_stamp_t *right) {
    return left->size == right->size &&
           left->identity_a == right->identity_a &&
           left->identity_b == right->identity_b &&
           left->modified_a == right->modified_a &&
           left->modified_b == right->modified_b &&
           left->changed_a == right->changed_a &&
           left->changed_b == right->changed_b &&
           left->links == right->links;
}

#ifdef _WIN32

static void host_file_init(host_file_t *file) {
    file->handle = INVALID_HANDLE_VALUE;
}

static bool host_file_is_open(const host_file_t *file) {
    return file->handle != INVALID_HANDLE_VALUE;
}

static int windows_error(void) {
    DWORD value = GetLastError();
    return value > (DWORD)INT_MAX ? INT_MAX : (int)value;
}

static bool host_file_close(host_file_t *file, int *system_error) {
    HANDLE handle;

    if (!host_file_is_open(file))
        return true;
    handle = file->handle;
    file->handle = INVALID_HANDLE_VALUE;
    if (CloseHandle(handle))
        return true;
    *system_error = windows_error();
    return false;
}

static bool host_file_read(host_file_t *file, uint64_t offset, void *buffer,
                           size_t length, int *system_error) {
    uint8_t *next = (uint8_t *)buffer;

    while (length != 0u) {
        LARGE_INTEGER position;
        DWORD request = length > (size_t)UINT32_MAX ? UINT32_MAX :
                        (DWORD)length;
        DWORD actual = 0;

        position.QuadPart = (LONGLONG)offset;
        if (offset > (uint64_t)LLONG_MAX ||
            !SetFilePointerEx(file->handle, position, NULL, FILE_BEGIN)) {
            *system_error = windows_error();
            return false;
        }
        if (!ReadFile(file->handle, next, request, &actual, NULL)) {
            *system_error = windows_error();
            return false;
        }
        if (actual == 0u) {
            *system_error = ERROR_HANDLE_EOF;
            return false;
        }
        next += actual;
        offset += actual;
        length -= actual;
    }
    return true;
}

static bool host_file_write(host_file_t *file, uint64_t offset,
                            const void *buffer, size_t length,
                            int *system_error) {
    const uint8_t *next = (const uint8_t *)buffer;

    while (length != 0u) {
        LARGE_INTEGER position;
        DWORD request = length > (size_t)UINT32_MAX ? UINT32_MAX :
                        (DWORD)length;
        DWORD actual = 0;

        position.QuadPart = (LONGLONG)offset;
        if (offset > (uint64_t)LLONG_MAX ||
            !SetFilePointerEx(file->handle, position, NULL, FILE_BEGIN)) {
            *system_error = windows_error();
            return false;
        }
        if (!WriteFile(file->handle, next, request, &actual, NULL)) {
            *system_error = windows_error();
            return false;
        }
        if (actual == 0u) {
            *system_error = ERROR_WRITE_FAULT;
            return false;
        }
        next += actual;
        offset += actual;
        length -= actual;
    }
    return true;
}

static bool host_file_resize(host_file_t *file, uint64_t size,
                             int *system_error) {
    LARGE_INTEGER position;

    if (size > (uint64_t)LLONG_MAX) {
        *system_error = ERROR_ARITHMETIC_OVERFLOW;
        return false;
    }
    position.QuadPart = (LONGLONG)size;
    if (!SetFilePointerEx(file->handle, position, NULL, FILE_BEGIN) ||
        !SetEndOfFile(file->handle)) {
        *system_error = windows_error();
        return false;
    }
    return true;
}

static bool host_file_sync(host_file_t *file, int *system_error) {
    if (FlushFileBuffers(file->handle))
        return true;
    *system_error = windows_error();
    return false;
}

static bool host_file_stamp(host_file_t *file, file_stamp_t *stamp,
                            int *system_error) {
    BY_HANDLE_FILE_INFORMATION info;

    if (!GetFileInformationByHandle(file->handle, &info)) {
        *system_error = windows_error();
        return false;
    }
    stamp->size = ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    stamp->identity_a = info.dwVolumeSerialNumber;
    stamp->identity_b = ((uint64_t)info.nFileIndexHigh << 32) |
                        info.nFileIndexLow;
    stamp->modified_a = info.ftLastWriteTime.dwHighDateTime;
    stamp->modified_b = info.ftLastWriteTime.dwLowDateTime;
    stamp->changed_a = 0;
    stamp->changed_b = 0;
    stamp->links = info.nNumberOfLinks;
    return true;
}

static bool windows_full_path(const char *path, char **full, int *system_error) {
    DWORD needed;
    DWORD written;
    char *buffer;

    needed = GetFullPathNameA(path, 0, NULL, NULL);
    if (needed == 0u) {
        *system_error = windows_error();
        return false;
    }
    buffer = (char *)malloc((size_t)needed + 1u);
    if (!buffer) {
        *system_error = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }
    written = GetFullPathNameA(path, needed + 1u, buffer, NULL);
    if (written == 0u || written > needed) {
        *system_error = windows_error();
        free(buffer);
        return false;
    }
    buffer[written] = '\0';
    *full = buffer;
    return true;
}

static const char *windows_without_device_prefix(const char *path) {
    return strlen(path) >= 4u && path[0] == '\\' && path[1] == '\\' && path[2] == '?' &&
           path[3] == '\\' ? path + 4 : path;
}

static size_t windows_trimmed_length(const char *path) {
    size_t length = strlen(path);

    while (length > 3u &&
           (path[length - 1u] == '\\' || path[length - 1u] == '/'))
        length--;
    return length;
}

static bool windows_paths_equal(const char *left, const char *right) {
    size_t left_length;
    size_t right_length;
    size_t index;

    left = windows_without_device_prefix(left);
    right = windows_without_device_prefix(right);
    left_length = windows_trimmed_length(left);
    right_length = windows_trimmed_length(right);
    if (left_length != right_length)
        return false;
    for (index = 0; index < left_length; index++) {
        unsigned char left_byte = (unsigned char)left[index];
        unsigned char right_byte = (unsigned char)right[index];

        if (left_byte == '/')
            left_byte = '\\';
        if (right_byte == '/')
            right_byte = '\\';
        if (left_byte >= 'a' && left_byte <= 'z')
            left_byte = (unsigned char)(left_byte - ('a' - 'A'));
        if (right_byte >= 'a' && right_byte <= 'z')
            right_byte = (unsigned char)(right_byte - ('a' - 'A'));
        if (left_byte != right_byte)
            return false;
    }
    return true;
}

/* GetFullPathName is lexical.  Pair it with the opened object's canonical
 * handle path so a reparse/rename between inspection and open is a refusal. */
static bool windows_handle_matches_path(HANDLE handle, const char *expected,
                                        int *system_error) {
    DWORD needed;
    DWORD written;
    char *actual;
    bool matches;

    needed = GetFinalPathNameByHandleA(handle, NULL, 0,
                                       FILE_NAME_NORMALIZED |
                                           VOLUME_NAME_DOS);
    if (needed == 0u) {
        *system_error = windows_error();
        return false;
    }
    actual = (char *)malloc((size_t)needed + 1u);
    if (!actual) {
        *system_error = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }
    written = GetFinalPathNameByHandleA(handle, actual, needed + 1u,
                                        FILE_NAME_NORMALIZED |
                                            VOLUME_NAME_DOS);
    if (written == 0u || written > needed) {
        *system_error = windows_error();
        free(actual);
        return false;
    }
    actual[written] = '\0';
    matches = windows_paths_equal(actual, expected);
    free(actual);
    if (!matches)
        *system_error = ERROR_INVALID_NAME;
    return matches;
}

static bool windows_path_shape_safe(const char *full) {
    size_t index;

    if (!full || strlen(full) < 3u)
        return false;
    /* Network/reparse semantics vary; the work image is intentionally local. */
    if (full[0] == '\\' || full[1] != ':' ||
        (full[2] != '\\' && full[2] != '/'))
        return false;
    for (index = 2u; full[index] != '\0'; index++) {
        if (full[index] == ':')
            return false; /* alternate data stream or malformed drive path */
    }
    return true;
}

/* Validate every existing component and reject all reparse traversal. */
static bool windows_validate_chain(char *full, bool include_final,
                                   int *system_error, bool *unsafe) {
    size_t length = strlen(full);
    size_t index;

    *unsafe = false;
    if (!windows_path_shape_safe(full)) {
        *unsafe = true;
        return false;
    }
    for (index = 3u; index <= length; index++) {
        char saved;
        DWORD attributes;
        bool boundary = full[index] == '\0' || full[index] == '\\' ||
                        full[index] == '/';

        if (!boundary || (!include_final && full[index] == '\0'))
            continue;
        saved = full[index];
        full[index] = '\0';
        attributes = GetFileAttributesA(full);
        full[index] = saved;
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            *system_error = windows_error();
            return false;
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u ||
            (saved != '\0' &&
             (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0u)) {
            *unsafe = true;
            return false;
        }
    }
    return true;
}

static bool windows_split_destination(const char *path,
                                      destination_dir_t *destination,
                                      rootfs_work_result_t *result) {
    char *separator;
    DWORD attributes;
    int error = 0;
    bool unsafe = false;

    if (!windows_full_path(path, &destination->destination_path, &error)) {
        result_fail(result, error == ERROR_NOT_ENOUGH_MEMORY ?
                        ROOTFS_WORK_NO_MEMORY :
                        ROOTFS_WORK_DESTINATION_OPEN_FAILED,
                    ROOTFS_WORK_STAGE_DESTINATION_PATH, error,
                    "cannot resolve destination path");
        return false;
    }
    if (!windows_path_shape_safe(destination->destination_path)) {
        result_fail(result, ROOTFS_WORK_PATH_UNSAFE,
                    ROOTFS_WORK_STAGE_DESTINATION_PATH, 0,
                    "destination must be a local drive path without streams");
        return false;
    }
    separator = strrchr(destination->destination_path, '\\');
    {
        char *forward = strrchr(destination->destination_path, '/');
        if (forward && (!separator || forward > separator))
            separator = forward;
    }
    if (!separator || separator[1] == '\0' ||
        strcmp(separator + 1, ".") == 0 ||
        strcmp(separator + 1, "..") == 0) {
        result_fail(result, ROOTFS_WORK_PATH_UNSAFE,
                    ROOTFS_WORK_STAGE_DESTINATION_PATH, 0,
                    "destination has no safe file name");
        return false;
    }
    destination->full_path = copy_string_n(destination->destination_path,
                                            (size_t)(separator -
                                                     destination->destination_path));
    if (!destination->full_path) {
        result_fail(result, ROOTFS_WORK_NO_MEMORY,
                    ROOTFS_WORK_STAGE_DESTINATION_PATH, 0,
                    "cannot allocate destination directory path");
        return false;
    }
    if (strlen(destination->full_path) == 2u) {
        char *root = (char *)realloc(destination->full_path, 4u);
        if (!root) {
            result_fail(result, ROOTFS_WORK_NO_MEMORY,
                        ROOTFS_WORK_STAGE_DESTINATION_PATH, 0,
                        "cannot allocate destination root path");
            return false;
        }
        root[2] = '\\';
        root[3] = '\0';
        destination->full_path = root;
    }
    if (!windows_validate_chain(destination->full_path, true, &error,
                                &unsafe)) {
        result_fail(result,
                    unsafe ? ROOTFS_WORK_PATH_UNSAFE :
                             ROOTFS_WORK_DESTINATION_OPEN_FAILED,
                    ROOTFS_WORK_STAGE_DESTINATION_PATH, error,
                    unsafe ? "destination directory traverses a reparse point"
                           : "cannot inspect destination directory");
        return false;
    }
    attributes = GetFileAttributesA(destination->destination_path);
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        result_fail(result,
                    (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u ?
                        ROOTFS_WORK_PATH_UNSAFE :
                        ROOTFS_WORK_DESTINATION_EXISTS,
                    ROOTFS_WORK_STAGE_DESTINATION_PATH, 0,
                    "destination already exists");
        return false;
    }
    error = windows_error();
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
        result_fail(result, ROOTFS_WORK_DESTINATION_OPEN_FAILED,
                    ROOTFS_WORK_STAGE_DESTINATION_PATH, error,
                    "cannot establish that destination is absent");
        return false;
    }
    destination->handle = CreateFileA(destination->full_path,
                                      FILE_LIST_DIRECTORY,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      NULL, OPEN_EXISTING,
                                      FILE_FLAG_BACKUP_SEMANTICS |
                                          FILE_FLAG_OPEN_REPARSE_POINT,
                                      NULL);
    if (destination->handle == INVALID_HANDLE_VALUE) {
        result_fail(result, ROOTFS_WORK_DESTINATION_OPEN_FAILED,
                    ROOTFS_WORK_STAGE_DESTINATION_PATH, windows_error(),
                    "cannot hold destination directory open");
        return false;
    }
    if (!windows_handle_matches_path(destination->handle,
                                     destination->full_path, &error)) {
        result_fail(result, ROOTFS_WORK_PATH_UNSAFE,
                    ROOTFS_WORK_STAGE_DESTINATION_PATH, error,
                    "opened destination directory identity is ambiguous");
        return false;
    }
    return true;
}

static bool windows_open_source(const char *path, host_file_t *source,
                                file_stamp_t *stamp,
                                rootfs_work_result_t *result) {
    char *full = NULL;
    int error = 0;
    bool unsafe = false;
    BY_HANDLE_FILE_INFORMATION info;

    if (!windows_full_path(path, &full, &error)) {
        result_fail(result, error == ERROR_NOT_ENOUGH_MEMORY ?
                        ROOTFS_WORK_NO_MEMORY :
                        ROOTFS_WORK_SOURCE_OPEN_FAILED,
                    ROOTFS_WORK_STAGE_SOURCE_PATH, error,
                    "cannot resolve source path");
        return false;
    }
    if (!windows_validate_chain(full, true, &error, &unsafe)) {
        result_fail(result,
                    unsafe ? ROOTFS_WORK_PATH_UNSAFE :
                             ROOTFS_WORK_SOURCE_OPEN_FAILED,
                    ROOTFS_WORK_STAGE_SOURCE_PATH, error,
                    unsafe ? "source path traverses a reparse point"
                           : "cannot inspect source path");
        free(full);
        return false;
    }
    source->handle = CreateFileA(full, GENERIC_READ, FILE_SHARE_READ, NULL,
                                 OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL |
                                     FILE_FLAG_OPEN_REPARSE_POINT |
                                     FILE_FLAG_SEQUENTIAL_SCAN,
                                 NULL);
    if (source->handle == INVALID_HANDLE_VALUE) {
        error = windows_error();
        result_fail(result,
                    error == ERROR_SHARING_VIOLATION ?
                        ROOTFS_WORK_SOURCE_BUSY :
                        ROOTFS_WORK_SOURCE_OPEN_FAILED,
                    ROOTFS_WORK_STAGE_SOURCE_OPEN, error,
                    "cannot open source read-only with write/delete sharing denied");
        free(full);
        return false;
    }
    if (!windows_handle_matches_path(source->handle, full, &error)) {
        result_fail(result, ROOTFS_WORK_PATH_UNSAFE,
                    ROOTFS_WORK_STAGE_SOURCE_OPEN, error,
                    "opened source identity is ambiguous");
        free(full);
        return false;
    }
    free(full);
    if (!GetFileInformationByHandle(source->handle, &info)) {
        result_fail(result, ROOTFS_WORK_SOURCE_OPEN_FAILED,
                    ROOTFS_WORK_STAGE_SOURCE_OPEN, windows_error(),
                    "cannot inspect opened source");
        return false;
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u) {
        result_fail(result, ROOTFS_WORK_PATH_UNSAFE,
                    ROOTFS_WORK_STAGE_SOURCE_OPEN, 0,
                    "opened source is a reparse point");
        return false;
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DEVICE) != 0u) {
        result_fail(result, ROOTFS_WORK_SOURCE_NOT_REGULAR,
                    ROOTFS_WORK_STAGE_SOURCE_OPEN, 0,
                    "source is not a regular disk image file");
        return false;
    }
    if (info.nNumberOfLinks != 1u) {
        result_fail(result, ROOTFS_WORK_SOURCE_ALIAS,
                    ROOTFS_WORK_STAGE_SOURCE_OPEN, 0,
                    "source has %lu hard links; immutable identity is ambiguous",
                    (unsigned long)info.nNumberOfLinks);
        return false;
    }
    if (!host_file_stamp(source, stamp, &error)) {
        result_fail(result, ROOTFS_WORK_SOURCE_OPEN_FAILED,
                    ROOTFS_WORK_STAGE_SOURCE_OPEN, error,
                    "cannot capture source identity");
        return false;
    }
    return true;
}

static bool destination_temp_create(destination_dir_t *destination,
                                     host_file_t *temporary,
                                     bool *temporary_created,
                                     rootfs_work_result_t *result) {
    unsigned attempt;
    size_t parent_length = strlen(destination->full_path);
    int identity_error = 0;

    if (!windows_handle_matches_path(destination->handle,
                                     destination->full_path,
                                     &identity_error)) {
        result_fail(result, ROOTFS_WORK_PATH_UNSAFE,
                    ROOTFS_WORK_STAGE_TEMP_CREATE, identity_error,
                    "destination directory identity changed before temp create");
        return false;
    }

    for (attempt = 0; attempt < ROOTFS_TEMP_ATTEMPTS; attempt++) {
        char leaf[80];
        int leaf_length = snprintf(leaf, sizeof(leaf),
                                   ".rootfs-work-%lu-%u.tmp",
                                   (unsigned long)_getpid(), attempt);
        size_t total;

        if (leaf_length <= 0 || (size_t)leaf_length >= sizeof(leaf))
            break;
        total = parent_length + 1u + (size_t)leaf_length + 1u;
        destination->temporary_path = (char *)malloc(total);
        if (!destination->temporary_path) {
            result_fail(result, ROOTFS_WORK_NO_MEMORY,
                        ROOTFS_WORK_STAGE_TEMP_CREATE, 0,
                        "cannot allocate temporary path");
            return false;
        }
        (void)snprintf(destination->temporary_path, total, "%s\\%s",
                       destination->full_path, leaf);
        if (_stricmp(destination->temporary_path,
                     destination->destination_path) == 0) {
            free(destination->temporary_path);
            destination->temporary_path = NULL;
            continue;
        }
        temporary->handle = CreateFileA(destination->temporary_path,
                                         GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                         CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (temporary->handle != INVALID_HANDLE_VALUE) {
            /* CREATE_NEW transferred ownership before any later identity
             * check can fail.  The caller's common cleanup path must own both
             * the handle and name from this instruction onward. */
            *temporary_created = true;
            if (windows_handle_matches_path(temporary->handle,
                                            destination->temporary_path,
                                            &identity_error))
                return true;
            result_fail(result, ROOTFS_WORK_PATH_UNSAFE,
                        ROOTFS_WORK_STAGE_TEMP_CREATE, identity_error,
                        "created temporary image identity is ambiguous");
            return false;
        }
        if (GetLastError() != ERROR_FILE_EXISTS &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            int error = windows_error();
            result_fail(result, ROOTFS_WORK_TEMP_CREATE_FAILED,
                        ROOTFS_WORK_STAGE_TEMP_CREATE, error,
                        "cannot exclusively create temporary work image");
            return false;
        }
        free(destination->temporary_path);
        destination->temporary_path = NULL;
    }
    result_fail(result, ROOTFS_WORK_TEMP_CREATE_FAILED,
                ROOTFS_WORK_STAGE_TEMP_CREATE, ERROR_FILE_EXISTS,
                "all unique temporary names are occupied");
    return false;
}

#else /* !_WIN32 */

/* No syscall is allowed an unbounded retry loop: a signal storm must surface
 * as a stable EINTR failure rather than hanging a boot/provisioning process. */
static int posix_fstat_bounded(int descriptor, struct stat *info) {
    unsigned retries = 0;
    int rc;

    do {
        rc = fstat(descriptor, info);
    } while (rc != 0 && errno == EINTR &&
             retries++ < ROOTFS_EINTR_RETRY_LIMIT);
    return rc;
}

static int posix_fstatat_bounded(int descriptor, const char *path,
                                 struct stat *info, int flags) {
    unsigned retries = 0;
    int rc;

    do {
        rc = fstatat(descriptor, path, info, flags);
    } while (rc != 0 && errno == EINTR &&
             retries++ < ROOTFS_EINTR_RETRY_LIMIT);
    return rc;
}

static int posix_fsync_bounded(int descriptor) {
    unsigned retries = 0;
    int rc;

    do {
        rc = fsync(descriptor);
    } while (rc != 0 && errno == EINTR &&
             retries++ < ROOTFS_EINTR_RETRY_LIMIT);
    return rc;
}

static void host_file_init(host_file_t *file) {
    file->descriptor = -1;
}

static bool host_file_is_open(const host_file_t *file) {
    return file->descriptor >= 0;
}

static bool host_file_close(host_file_t *file, int *system_error) {
    int descriptor;

    if (!host_file_is_open(file))
        return true;
    descriptor = file->descriptor;
    file->descriptor = -1;
    if (close(descriptor) == 0)
        return true;
    *system_error = errno;
    return false;
}

static bool host_file_read(host_file_t *file, uint64_t offset, void *buffer,
                           size_t length, int *system_error) {
    uint8_t *next = (uint8_t *)buffer;

    if (offset > (uint64_t)INT64_MAX) {
        *system_error = EOVERFLOW;
        return false;
    }
    while (length != 0u) {
        size_t request = length;
        ssize_t actual;

#ifdef SSIZE_MAX
        if (request > (size_t)SSIZE_MAX)
            request = (size_t)SSIZE_MAX;
#endif
        unsigned retries = 0;

        do {
            actual = pread(file->descriptor, next, request, (off_t)offset);
        } while (actual < 0 && errno == EINTR &&
                 retries++ < ROOTFS_EINTR_RETRY_LIMIT);
        if (actual <= 0) {
            *system_error = actual == 0 ? EIO : errno;
            return false;
        }
        next += (size_t)actual;
        offset += (uint64_t)actual;
        length -= (size_t)actual;
    }
    return true;
}

static bool host_file_write(host_file_t *file, uint64_t offset,
                            const void *buffer, size_t length,
                            int *system_error) {
    const uint8_t *next = (const uint8_t *)buffer;

    if (offset > (uint64_t)INT64_MAX) {
        *system_error = EOVERFLOW;
        return false;
    }
    while (length != 0u) {
        size_t request = length;
        ssize_t actual;

#ifdef SSIZE_MAX
        if (request > (size_t)SSIZE_MAX)
            request = (size_t)SSIZE_MAX;
#endif
        unsigned retries = 0;

        do {
            actual = pwrite(file->descriptor, next, request, (off_t)offset);
        } while (actual < 0 && errno == EINTR &&
                 retries++ < ROOTFS_EINTR_RETRY_LIMIT);
        if (actual <= 0) {
            *system_error = actual == 0 ? EIO : errno;
            return false;
        }
        next += (size_t)actual;
        offset += (uint64_t)actual;
        length -= (size_t)actual;
    }
    return true;
}

static bool host_file_resize(host_file_t *file, uint64_t size,
                             int *system_error) {
    int rc;
    unsigned retries = 0;

    if (size > (uint64_t)INT64_MAX) {
        *system_error = EOVERFLOW;
        return false;
    }
    do {
        rc = ftruncate(file->descriptor, (off_t)size);
    } while (rc != 0 && errno == EINTR &&
             retries++ < ROOTFS_EINTR_RETRY_LIMIT);
    if (rc == 0)
        return true;
    *system_error = errno;
    return false;
}

static bool host_file_sync(host_file_t *file, int *system_error) {
    int rc = posix_fsync_bounded(file->descriptor);
    if (rc == 0)
        return true;
    *system_error = errno;
    return false;
}

static uint32_t links_to_u32(uintmax_t links) {
    return links > (uintmax_t)UINT32_MAX ? UINT32_MAX : (uint32_t)links;
}

static bool host_file_stamp(host_file_t *file, file_stamp_t *stamp,
                            int *system_error) {
    struct stat info;

    if (posix_fstat_bounded(file->descriptor, &info) != 0) {
        *system_error = errno;
        return false;
    }
    if (info.st_size < 0) {
        *system_error = EOVERFLOW;
        return false;
    }
    stamp->size = (uint64_t)info.st_size;
    stamp->identity_a = (uint64_t)info.st_dev;
    stamp->identity_b = (uint64_t)info.st_ino;
    stamp->modified_a = (uint64_t)info.st_mtime;
    stamp->changed_a = (uint64_t)info.st_ctime;
#if defined(__APPLE__)
    /*
     * Darwin really does ship BOTH layouts, and which one you get depends on
     * the C dialect, not on _DARWIN_C_SOURCE:
     *
     *   strict (-std=c11 sets __STRICT_ANSI__)  scalar  st_mtimensec
     *   otherwise                               struct  st_mtimespec.tv_nsec
     *
     * The macOS CI jobs compile this file with -std=c11 and get the scalar
     * form; Xcode builds the iOS app without it and gets the struct form. So
     * the two Apple targets need DIFFERENT code, and any single-branch answer
     * breaks one of them -- which is exactly what happened: gating on
     * _DARWIN_C_SOURCE broke iOS, and then collapsing both branches onto
     * st_mtimespec broke macOS with the mirror-image error.
     *
     * Discriminate on Darwin's own signal instead of guessing at feature
     * macros: <sys/stat.h> #defines st_mtime to st_mtimespec.tv_sec precisely
     * when the timespec layout is in force, so the macro's existence IS the
     * layout test. The st_mtime/st_ctime reads above work either way.
     */
#  ifdef st_mtime
    stamp->modified_b = (uint64_t)info.st_mtimespec.tv_nsec;
    stamp->changed_b = (uint64_t)info.st_ctimespec.tv_nsec;
#  else
    stamp->modified_b = (uint64_t)info.st_mtimensec;
    stamp->changed_b = (uint64_t)info.st_ctimensec;
#  endif
#elif defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
      defined(__OpenBSD__)
    stamp->modified_b = (uint64_t)info.st_mtim.tv_nsec;
    stamp->changed_b = (uint64_t)info.st_ctim.tv_nsec;
#else
    stamp->modified_b = 0;
    stamp->changed_b = 0;
#endif
    stamp->links = links_to_u32((uintmax_t)info.st_nlink);
    return true;
}

/*
 * `culprit`, when not NULL, receives the component that was refused -- the
 * '..' or the symbolic link -- truncated into a caller-supplied buffer.
 *
 * It exists because the message without it cost a round trip. A user on iOS
 * saw "source path traverses a symbolic link or '..'" and could not act on it;
 * the component was "var", which on that platform is a symlink to /private/var
 * and is the first thing in every container path. Naming it turns a report
 * into a diagnosis.
 */
/*
 * HOW AN INTERMEDIATE DIRECTORY IS OPENED, and it is not O_RDONLY.
 *
 * The walk below descends one component at a time, holding a descriptor on
 * each directory so the next openat() is relative to a directory that has
 * already been checked. It only ever needs to SEARCH those directories -- it
 * enumerates none of them -- but the first version asked for O_RDONLY, which
 * is a request to READ them.
 *
 * On a sandboxed platform those are different rights and only one is granted.
 * An iOS app can traverse /private/var/mobile/Containers/Data/Application/<id>
 * to reach its own container and cannot read a single directory on the way;
 * the observed failure was "source-open-failed at source-path: cannot open
 * source directory", and the emulator was right again -- it genuinely could
 * not open them.
 *
 * O_SEARCH is POSIX.1-2008 for exactly this and is what Darwin provides.
 * O_PATH is the Linux spelling, and a descriptor from it is still usable as
 * the dirfd of openat() and fstatat(), which is all this needs. O_RDONLY is
 * the fallback for a platform with neither, where the two rights are not
 * distinguished anyway.
 *
 * The FINAL file is still opened O_RDONLY -- it is going to be read.
 */
#if defined(O_SEARCH)
#  define ROOTFS_WORK_TRAVERSE O_SEARCH
#elif defined(O_PATH)
#  define ROOTFS_WORK_TRAVERSE O_PATH
#else
#  define ROOTFS_WORK_TRAVERSE O_RDONLY
#endif

/* Copy one path component into the caller's buffer, truncated and always
 * terminated. Silent when there is no buffer, which is what the callers that
 * do not care pass. */
static void note_component(char *out, size_t cap, const char *name) {
    size_t n;
    if (!out || cap == 0u || !name) return;
    n = strlen(name);
    if (n >= cap) n = cap - 1u;
    memcpy(out, name, n);
    out[n] = '\0';
}

static int open_directory_no_links(const char *path, int *system_error,
                                   bool *unsafe, char *culprit,
                                   size_t culprit_size) {
    char *copy = NULL;
    char *cursor;
    int current = -1;
    bool absolute;

    *unsafe = false;
    if (!path || path[0] == '\0' || (path[0] == '/' && path[1] == '/')) {
        *unsafe = true;
        return -1;
    }
    absolute = path[0] == '/';
    current = open(absolute ? "/" : ".",
                   ROOTFS_WORK_TRAVERSE | O_DIRECTORY | O_CLOEXEC);
    if (current < 0) {
        *system_error = errno;
        return -1;
    }
    copy = copy_string_n(path, strlen(path));
    if (!copy) {
        *system_error = ENOMEM;
        (void)close(current);
        return -1;
    }
    cursor = copy + (absolute ? 1 : 0);
    while (*cursor != '\0') {
        char *end;
        char saved;
        int next;
        struct stat before;
        struct stat after;
        int flags = ROOTFS_WORK_TRAVERSE | O_DIRECTORY | O_CLOEXEC;

        while (*cursor == '/')
            cursor++;
        if (*cursor == '\0')
            break;
        end = strchr(cursor, '/');
        if (!end)
            end = cursor + strlen(cursor);
        saved = *end;
        *end = '\0';
        if (strcmp(cursor, ".") == 0) {
            *end = saved;
            cursor = saved == '\0' ? end : end + 1;
            continue;
        }
        if (strcmp(cursor, "..") == 0) {
            *unsafe = true;
            note_component(culprit, culprit_size, cursor);
            *end = saved;
            free(copy);
            (void)close(current);
            return -1;
        }
        if (posix_fstatat_bounded(current, cursor, &before,
                                  AT_SYMLINK_NOFOLLOW) != 0) {
            *system_error = errno;
            *end = saved;
            free(copy);
            (void)close(current);
            return -1;
        }
        if (S_ISLNK(before.st_mode)) {
            *unsafe = true;
            note_component(culprit, culprit_size, cursor);
            *end = saved;
            free(copy);
            (void)close(current);
            return -1;
        }
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        next = openat(current, cursor, flags);
        if (next < 0) {
            *system_error = errno;
            *end = saved;
            free(copy);
            (void)close(current);
            return -1;
        }
        if (posix_fstat_bounded(next, &after) != 0) {
            *system_error = errno;
            (void)close(next);
            *end = saved;
            free(copy);
            (void)close(current);
            return -1;
        }
        if (!S_ISDIR(after.st_mode) || before.st_dev != after.st_dev ||
            before.st_ino != after.st_ino) {
            *system_error = 0;
            *unsafe = true;
            (void)close(next);
            *end = saved;
            free(copy);
            (void)close(current);
            return -1;
        }
        (void)close(current);
        current = next;
        *end = saved;
        cursor = saved == '\0' ? end : end + 1;
    }
    free(copy);
    return current;
}

static bool split_path(const char *path, char **parent, char **leaf) {
    const char *slash;
    size_t parent_length;

    if (!path || path[0] == '\0' || path[strlen(path) - 1u] == '/')
        return false;
    slash = strrchr(path, '/');
    if (!slash) {
        *parent = copy_string_n(".", 1u);
        *leaf = copy_string_n(path, strlen(path));
    } else {
        parent_length = slash == path ? 1u : (size_t)(slash - path);
        *parent = copy_string_n(path, parent_length);
        *leaf = copy_string_n(slash + 1, strlen(slash + 1));
    }
    if (!*parent || !*leaf || (*leaf)[0] == '\0' ||
        strcmp(*leaf, ".") == 0 || strcmp(*leaf, "..") == 0) {
        free(*parent);
        free(*leaf);
        *parent = NULL;
        *leaf = NULL;
        return false;
    }
    return true;
}

static bool posix_open_source(const char *path, host_file_t *source,
                              file_stamp_t *stamp,
                              rootfs_work_result_t *result) {
    char *parent = NULL;
    char *leaf = NULL;
    int parent_fd = -1;
    int error = 0;
    bool unsafe = false;
    struct stat before;
    struct stat after;
    struct flock lock;
    int flags = O_RDONLY | O_CLOEXEC;

    if (!split_path(path, &parent, &leaf)) {
        result_fail(result, ROOTFS_WORK_PATH_UNSAFE,
                    ROOTFS_WORK_STAGE_SOURCE_PATH, 0,
                    "source has no safe file name");
        return false;
    }
    char culprit[64];
    culprit[0] = '\0';
    parent_fd = open_directory_no_links(parent, &error, &unsafe,
                                        culprit, sizeof culprit);
    if (parent_fd < 0) {
        char said[160];
        if (unsafe && culprit[0])
            (void)snprintf(said, sizeof said,
                           "source path traverses a symbolic link or '..' at "
                           "component \"%s\"", culprit);
        else if (unsafe)
            (void)snprintf(said, sizeof said,
                           "source path traverses a symbolic link or '..'");
        else
            (void)snprintf(said, sizeof said,
                           "cannot open source directory: %s (errno %d)",
                           strerror(error), error);
        result_fail(result,
                    unsafe ? ROOTFS_WORK_PATH_UNSAFE :
                             (error == ENOMEM ? ROOTFS_WORK_NO_MEMORY :
                                                ROOTFS_WORK_SOURCE_OPEN_FAILED),
                    ROOTFS_WORK_STAGE_SOURCE_PATH, error, said);
        free(parent);
        free(leaf);
        return false;
    }
    if (posix_fstatat_bounded(parent_fd, leaf, &before,
                              AT_SYMLINK_NOFOLLOW) != 0) {
        error = errno;
        result_fail(result, ROOTFS_WORK_SOURCE_OPEN_FAILED,
                    ROOTFS_WORK_STAGE_SOURCE_PATH, error,
                    "cannot inspect source path");
        (void)close(parent_fd);
        free(parent);
        free(leaf);
        return false;
    }
    if (S_ISLNK(before.st_mode)) {
        result_fail(result, ROOTFS_WORK_PATH_UNSAFE,
                    ROOTFS_WORK_STAGE_SOURCE_PATH, 0,
                    "source is a symbolic link");
        (void)close(parent_fd);
        free(parent);
        free(leaf);
        return false;
    }
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    source->descriptor = openat(parent_fd, leaf, flags);
    error = errno;
    (void)close(parent_fd);
    free(parent);
    free(leaf);
    if (source->descriptor < 0) {
        result_fail(result, ROOTFS_WORK_SOURCE_OPEN_FAILED,
                    ROOTFS_WORK_STAGE_SOURCE_OPEN, error,
                    "cannot open source read-only");
        return false;
    }
    if (posix_fstat_bounded(source->descriptor, &after) != 0) {
        result_fail(result, ROOTFS_WORK_SOURCE_OPEN_FAILED,
                    ROOTFS_WORK_STAGE_SOURCE_OPEN, errno,
                    "cannot inspect opened source");
        return false;
    }
    if (before.st_dev != after.st_dev || before.st_ino != after.st_ino) {
        result_fail(result, ROOTFS_WORK_PATH_UNSAFE,
                    ROOTFS_WORK_STAGE_SOURCE_OPEN, 0,
                    "source identity changed while it was opened");
        return false;
    }
    if (!S_ISREG(after.st_mode)) {
        result_fail(result, ROOTFS_WORK_SOURCE_NOT_REGULAR,
                    ROOTFS_WORK_STAGE_SOURCE_OPEN, 0,
                    "source is not a regular disk image file");
        return false;
    }
    if (after.st_nlink != 1) {
        result_fail(result, ROOTFS_WORK_SOURCE_ALIAS,
                    ROOTFS_WORK_STAGE_SOURCE_OPEN, 0,
                    "source has %lu hard links; immutable identity is ambiguous",
                    (unsigned long)after.st_nlink);
        return false;
    }
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_RDLCK;
    lock.l_whence = SEEK_SET;
    {
        unsigned retries = 0;
        int rc;

        do {
            rc = fcntl(source->descriptor, F_SETLK, &lock);
        } while (rc != 0 && errno == EINTR &&
                 retries++ < ROOTFS_EINTR_RETRY_LIMIT);
        if (rc != 0) {
        error = errno;
        result_fail(result, ROOTFS_WORK_SOURCE_BUSY,
                    ROOTFS_WORK_STAGE_SOURCE_OPEN, error,
                    "cannot lock source against cooperative writers");
        return false;
        }
    }
    if (!host_file_stamp(source, stamp, &error)) {
        result_fail(result, ROOTFS_WORK_SOURCE_OPEN_FAILED,
                    ROOTFS_WORK_STAGE_SOURCE_OPEN, error,
                    "cannot capture source identity");
        return false;
    }
    return true;
}

static bool posix_open_destination(const char *path,
                                   destination_dir_t *destination,
                                   rootfs_work_result_t *result) {
    char *parent = NULL;
    int error = 0;
    bool unsafe = false;
    struct stat entry;

    if (!split_path(path, &parent, &destination->leaf)) {
        result_fail(result, ROOTFS_WORK_PATH_UNSAFE,
                    ROOTFS_WORK_STAGE_DESTINATION_PATH, 0,
                    "destination has no safe file name");
        return false;
    }
    char dculprit[64];
    dculprit[0] = '\0';
    destination->descriptor = open_directory_no_links(parent, &error, &unsafe,
                                                     dculprit,
                                                     sizeof dculprit);
    free(parent);
    if (destination->descriptor < 0) {
        result_fail(result,
                    unsafe ? ROOTFS_WORK_PATH_UNSAFE :
                             (error == ENOMEM ? ROOTFS_WORK_NO_MEMORY :
                                                ROOTFS_WORK_DESTINATION_OPEN_FAILED),
                    ROOTFS_WORK_STAGE_DESTINATION_PATH, error,
                    unsafe && dculprit[0]
                        ? "destination path traverses a symbolic link or '..' "
                          "-- see the component named in the log"
                        : unsafe
                        ? "destination path traverses a symbolic link or '..'"
                           : "cannot open destination directory");
        return false;
    }
    if (posix_fstatat_bounded(destination->descriptor, destination->leaf,
                              &entry, AT_SYMLINK_NOFOLLOW) == 0) {
        result_fail(result,
                    S_ISLNK(entry.st_mode) ? ROOTFS_WORK_PATH_UNSAFE :
                                             ROOTFS_WORK_DESTINATION_EXISTS,
                    ROOTFS_WORK_STAGE_DESTINATION_PATH, 0,
                    "destination already exists");
        return false;
    }
    if (errno != ENOENT) {
        result_fail(result, ROOTFS_WORK_DESTINATION_OPEN_FAILED,
                    ROOTFS_WORK_STAGE_DESTINATION_PATH, errno,
                    "cannot establish that destination is absent");
        return false;
    }
    return true;
}

static bool destination_temp_create(destination_dir_t *destination,
                                     host_file_t *temporary,
                                     bool *temporary_created,
                                     rootfs_work_result_t *result) {
    unsigned attempt;

    for (attempt = 0; attempt < ROOTFS_TEMP_ATTEMPTS; attempt++) {
        int length = snprintf(destination->temporary_leaf,
                              sizeof(destination->temporary_leaf),
                              ".rootfs-work-%lu-%u.tmp",
                              (unsigned long)getpid(), attempt);
        int flags = O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC;

        if (length <= 0 || (size_t)length >= sizeof(destination->temporary_leaf))
            break;
        if (strcmp(destination->temporary_leaf, destination->leaf) == 0)
            continue;
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        temporary->descriptor = openat(destination->descriptor,
                                       destination->temporary_leaf,
                                       flags, (mode_t)0600);
        if (temporary->descriptor >= 0) {
            /* openat(O_CREAT|O_EXCL) transferred ownership even if a future
             * post-create validation is added before this function returns. */
            *temporary_created = true;
            return true;
        }
        if (errno != EEXIST) {
            result_fail(result, ROOTFS_WORK_TEMP_CREATE_FAILED,
                        ROOTFS_WORK_STAGE_TEMP_CREATE, errno,
                        "cannot exclusively create temporary work image");
            return false;
        }
    }
    result_fail(result, ROOTFS_WORK_TEMP_CREATE_FAILED,
                ROOTFS_WORK_STAGE_TEMP_CREATE, EEXIST,
                "all unique temporary names are occupied");
    return false;
}

#endif /* _WIN32 */

static bool range_valid(uint64_t offset, size_t length, uint64_t size) {
    return offset <= size && (uint64_t)length <= size - offset;
}

static bool checked_read(host_file_t *file, uint64_t file_size, uint64_t offset,
                         void *buffer, size_t length,
                         rootfs_work_stage_t stage,
                         rootfs_work_result_t *result) {
    int error = 0;

    if (!range_valid(offset, length, file_size)) {
        result_fail(result, ROOTFS_WORK_RANGE_ERROR, stage, 0,
                    "read [0x%" PRIx64 ",+%zu) exceeds 0x%" PRIx64 " bytes",
                    offset, length, file_size);
        return false;
    }
    if (!host_file_read(file, offset, buffer, length, &error)) {
        result_fail(result, ROOTFS_WORK_READ_FAILED, stage, error,
                    "positioned read failed at 0x%" PRIx64 " for %zu bytes",
                    offset, length);
        return false;
    }
    return true;
}

static bool checked_write(host_file_t *file, uint64_t file_size,
                          uint64_t offset, const void *buffer, size_t length,
                          rootfs_work_stage_t stage,
                          rootfs_work_result_t *result) {
    int error = 0;

    if (!range_valid(offset, length, file_size)) {
        result_fail(result, ROOTFS_WORK_RANGE_ERROR, stage, 0,
                    "write [0x%" PRIx64 ",+%zu) exceeds 0x%" PRIx64 " bytes",
                    offset, length, file_size);
        return false;
    }
    if (!host_file_write(file, offset, buffer, length, &error)) {
        result_fail(result, ROOTFS_WORK_WRITE_FAILED, stage, error,
                    "positioned write failed at 0x%" PRIx64 " for %zu bytes",
                    offset, length);
        return false;
    }
    return true;
}

static bool allocation_physical_offset(const hfs_volume_t *volume,
                                       uint64_t logical_offset,
                                       uint64_t *physical_offset) {
    uint64_t seen = 0;
    unsigned index;

    for (index = 0; index < 8u; index++) {
        uint64_t span = (uint64_t)volume->ext_count[index] *
                        volume->block_size;
        if (span == 0u)
            continue;
        if (logical_offset < seen + span) {
            *physical_offset =
                (uint64_t)volume->ext_start[index] * volume->block_size +
                (logical_offset - seen);
            return true;
        }
        seen += span;
    }
    return false;
}

static uint32_t hfs_head_end(uint32_t block_size) {
    return (uint32_t)((1536u + block_size - 1u) / block_size);
}

static uint32_t hfs_tail_first(uint32_t total_blocks, uint32_t block_size) {
    return (uint32_t)(((uint64_t)total_blocks * block_size - HFS_VH_OFF) /
                      block_size);
}

static bool allocation_file_contains_block(const hfs_volume_t *volume,
                                           uint32_t block) {
    unsigned extent;

    for (extent = 0; extent < 8u; extent++) {
        uint64_t end = (uint64_t)volume->ext_start[extent] +
                       volume->ext_count[extent];
        if (volume->ext_count[extent] != 0u &&
            block >= volume->ext_start[extent] && block < end)
            return true;
    }
    return false;
}

static bool allocation_scan(host_file_t *file, uint64_t file_size,
                            const hfs_volume_t *volume, uint8_t *buffer,
                            size_t buffer_size, uint32_t *used,
                            rootfs_work_stage_t stage,
                            rootfs_work_result_t *result) {
    uint64_t logical = 0;
    uint64_t remaining = volume->alloc_bytes;
    uint64_t used_count = 0;
    uint32_t head_end = hfs_head_end(volume->block_size);
    uint32_t tail_first = hfs_tail_first(volume->total_blocks,
                                         volume->block_size);
    unsigned extent;

    for (extent = 0; extent < 8u && remaining != 0u; extent++) {
        uint64_t extent_bytes = (uint64_t)volume->ext_count[extent] *
                                volume->block_size;
        uint64_t in_extent = extent_bytes < remaining ? extent_bytes : remaining;
        uint64_t physical = (uint64_t)volume->ext_start[extent] *
                            volume->block_size;

        while (in_extent != 0u) {
            size_t amount = in_extent > buffer_size ? buffer_size :
                            (size_t)in_extent;
            size_t byte_index;

            if (!checked_read(file, file_size, physical, buffer, amount,
                              stage, result))
                return false;
            for (byte_index = 0; byte_index < amount; byte_index++) {
                uint8_t value = buffer[byte_index];
                unsigned bit_in_byte;

                for (bit_in_byte = 0; bit_in_byte < 8u; bit_in_byte++) {
                    uint64_t bit = (logical + byte_index) * 8u + bit_in_byte;
                    bool set = (value & (uint8_t)(1u <<
                                      (7u - bit_in_byte))) != 0u;
                    if (bit < volume->total_blocks) {
                        bool required = bit < head_end || bit >= tail_first ||
                            allocation_file_contains_block(volume,
                                                           (uint32_t)bit);
                        if (required && !set) {
                            result_fail(result, ROOTFS_WORK_HFS_INVALID, stage,
                                        0, "required metadata block %" PRIu64
                                        " is marked free", bit);
                            return false;
                        }
                        if (set)
                            used_count++;
                    } else if (set) {
                        result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                                    "allocation bit %" PRIu64
                                    " is set past totalBlocks %u",
                                    bit, volume->total_blocks);
                        return false;
                    }
                }
            }
            logical += amount;
            physical += amount;
            in_extent -= amount;
            remaining -= amount;
        }
    }
    if (remaining != 0u || logical != volume->alloc_bytes) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                    "allocation fork extents do not cover logicalSize");
        return false;
    }
    if (used_count > UINT32_MAX) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                    "allocation bitmap used-block count overflows");
        return false;
    }
    *used = (uint32_t)used_count;
    return true;
}

static bool hfs_validate(host_file_t *file, uint64_t file_size,
                         hfs_volume_t *volume, uint8_t *buffer,
                         size_t buffer_size, rootfs_work_stage_t stage,
                         rootfs_work_result_t *result) {
    uint8_t primary[HFS_VH_LEN];
    uint8_t alternate[HFS_VH_LEN];
    uint16_t signature;
    uint16_t version;
    uint32_t journal_info_block;
    uint64_t summed = 0;
    uint32_t used = 0;
    unsigned index;

    memset(volume, 0, sizeof(*volume));
    if (file_size < HFS_VH_OFF + HFS_VH_LEN) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                    "image is too small for an HFS volume header");
        return false;
    }
    if (!checked_read(file, file_size, HFS_VH_OFF, primary,
                      sizeof(primary), stage, result))
        return false;
    signature = read_be16(primary);
    version = read_be16(primary + 2);
    if (!((signature == HFS_SIG_HFSPLUS && version == 4u) ||
          (signature == HFS_SIG_HFSX && version == 5u))) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                    "signature 0x%04x/version %u is not HFS+/4 or HFSX/5",
                    signature, version);
        return false;
    }
    volume->attributes = read_be32(primary + 4);
    journal_info_block = read_be32(primary + 12);
    volume->block_size = read_be32(primary + 40);
    volume->total_blocks = read_be32(primary + 44);
    volume->free_blocks = read_be32(primary + 48);
    volume->next_alloc = read_be32(primary + 52);
    if (volume->block_size < 512u || volume->block_size > (1u << 20) ||
        (volume->block_size & (volume->block_size - 1u)) != 0u ||
        volume->block_size % 512u != 0u) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                    "blockSize %u is not a power-of-two multiple of 512",
                    volume->block_size);
        return false;
    }
    if (volume->total_blocks == 0u ||
        (uint64_t)volume->total_blocks * volume->block_size != file_size) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                    "totalBlocks %u x blockSize %u does not equal image size %"
                    PRIu64, volume->total_blocks, volume->block_size, file_size);
        return false;
    }
    if (volume->free_blocks > volume->total_blocks) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                    "freeBlocks %u exceeds totalBlocks %u",
                    volume->free_blocks, volume->total_blocks);
        return false;
    }
    if (volume->next_alloc >= volume->total_blocks) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                    "nextAllocation %u is outside %u blocks",
                    volume->next_alloc, volume->total_blocks);
        return false;
    }
    if ((volume->attributes & HFS_ATTR_JOURNALED) != 0u ||
        journal_info_block != 0u) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                    "journalled volumes are not supported");
        return false;
    }
    if ((volume->attributes & HFS_ATTR_SOFTWARE_LOCK) != 0u) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                    "software-locked volumes cannot be made writable");
        return false;
    }
    if ((volume->attributes & HFS_ATTR_UNMOUNTED) == 0u) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                    "source volume was not cleanly unmounted");
        return false;
    }
    if ((volume->attributes & HFS_ATTR_BOOT_INCONSISTENT) != 0u) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                    "source volume carries the boot-inconsistent bit");
        return false;
    }
    if (!checked_read(file, file_size, file_size - HFS_VH_OFF, alternate,
                      sizeof(alternate), stage, result))
        return false;
    if (memcmp(primary, alternate, sizeof(primary)) != 0) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                    "primary and alternate volume headers disagree");
        return false;
    }

    volume->alloc_bytes = read_be64(primary + 112);
    volume->alloc_fork_blocks = read_be32(primary + 124);
    {
        bool saw_empty_extent = false;

    for (index = 0; index < 8u; index++) {
        uint64_t extent_end;
        unsigned prior;

        volume->ext_start[index] = read_be32(primary + 128 + index * 8u);
        volume->ext_count[index] = read_be32(primary + 132 + index * 8u);
        if (volume->ext_count[index] == 0u) {
            if (volume->ext_start[index] != 0u) {
                result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                            "empty allocation extent %u has nonzero startBlock",
                            index);
                return false;
            }
            saw_empty_extent = true;
            continue;
        }
        if (saw_empty_extent) {
            result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                        "allocation extent %u follows an empty inline extent",
                        index);
            return false;
        }
        extent_end = (uint64_t)volume->ext_start[index] +
                     volume->ext_count[index];
        if (extent_end > volume->total_blocks) {
            result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                        "allocation extent %u runs past the volume", index);
            return false;
        }
        for (prior = 0; prior < index; prior++) {
            uint64_t prior_end = (uint64_t)volume->ext_start[prior] +
                                 volume->ext_count[prior];
            if (volume->ext_count[prior] != 0u &&
                volume->ext_start[index] < prior_end &&
                volume->ext_start[prior] < extent_end) {
                result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                            "allocation extents %u and %u overlap",
                            prior, index);
                return false;
            }
        }
        summed += volume->ext_count[index];
    }
    }
    if (summed != volume->alloc_fork_blocks) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                    "allocation fork reports %u blocks but inline extents cover "
                    "%" PRIu64, volume->alloc_fork_blocks, summed);
        return false;
    }
    if (volume->alloc_bytes >
        (uint64_t)volume->alloc_fork_blocks * volume->block_size) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                    "allocation logicalSize exceeds its physical extents");
        return false;
    }
    if (volume->alloc_bytes > UINT32_MAX / 8u) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                    "allocation bitmap is too large for 32-bit HFS blocks");
        return false;
    }
    volume->nbits = (uint32_t)(volume->alloc_bytes * 8u);
    if (volume->nbits < volume->total_blocks) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                    "allocation bitmap has %u bits for %u blocks",
                    volume->nbits, volume->total_blocks);
        return false;
    }
    if (!allocation_scan(file, file_size, volume, buffer, buffer_size,
                         &used, stage, result))
        return false;
    if (used != volume->total_blocks - volume->free_blocks) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                    "bitmap marks %u used blocks but header implies %u",
                    used, volume->total_blocks - volume->free_blocks);
        return false;
    }
    return true;
}

static bool allocation_bit_read(host_file_t *file, uint64_t file_size,
                                const hfs_volume_t *volume, uint32_t bit,
                                bool *set, rootfs_work_stage_t stage,
                                rootfs_work_result_t *result) {
    uint64_t offset;
    uint8_t byte;

    if (bit >= volume->nbits ||
        !allocation_physical_offset(volume, bit >> 3, &offset)) {
        result_fail(result, ROOTFS_WORK_RANGE_ERROR, stage, 0,
                    "allocation bit %u has no mapped byte", bit);
        return false;
    }
    if (!checked_read(file, file_size, offset, &byte, 1u, stage, result))
        return false;
    *set = (byte & (uint8_t)(1u << (7u - (bit & 7u)))) != 0u;
    return true;
}

static bool allocation_bit_write(host_file_t *file, uint64_t file_size,
                                 const hfs_volume_t *volume, uint32_t bit,
                                 bool set, rootfs_work_stage_t stage,
                                 rootfs_work_result_t *result) {
    uint64_t offset;
    uint8_t byte;
    uint8_t mask;

    if (bit >= volume->nbits ||
        !allocation_physical_offset(volume, bit >> 3, &offset)) {
        result_fail(result, ROOTFS_WORK_RANGE_ERROR, stage, 0,
                    "allocation bit %u has no mapped byte", bit);
        return false;
    }
    if (!checked_read(file, file_size, offset, &byte, 1u, stage, result))
        return false;
    mask = (uint8_t)(1u << (7u - (bit & 7u)));
    byte = set ? (uint8_t)(byte | mask) : (uint8_t)(byte & (uint8_t)~mask);
    return checked_write(file, file_size, offset, &byte, 1u, stage, result);
}

/*
 * Knuth-Morris-Pratt scan of the whole work image for one exact byte pattern.
 *
 * Both in-place record rewrites below ask the image the same question -- does
 * this exact stock record appear, and does it appear exactly once? -- so they
 * share one bounded scanner instead of keeping two copies of the same state
 * machine.  `prefix` is caller-owned scratch with one entry per pattern byte,
 * which keeps this allocation-free; the pattern may be far larger than the I/O
 * buffer, because the automaton carries `matched` across chunk boundaries.
 *
 * The walk stops at the second hit: every caller refuses anything other than
 * exactly one, so a larger count is never needed, only distinguishable from 1.
 */
static bool pattern_scan_unique(host_file_t *file, uint64_t file_size,
                                const uint8_t *pattern, size_t pattern_size,
                                size_t *prefix, uint8_t *buffer,
                                size_t buffer_size, uint64_t *hit,
                                unsigned *hits, rootfs_work_stage_t stage,
                                rootfs_work_result_t *result) {
    uint64_t offset = 0;
    size_t matched = 0;
    size_t index;

    *hit = UINT64_MAX;
    *hits = 0;
    prefix[0] = 0u;
    for (index = 1u; index < pattern_size; index++) {
        size_t candidate = prefix[index - 1u];
        while (candidate != 0u && pattern[index] != pattern[candidate])
            candidate = prefix[candidate - 1u];
        if (pattern[index] == pattern[candidate])
            candidate++;
        prefix[index] = candidate;
    }
    while (offset < file_size) {
        uint64_t remaining = file_size - offset;
        size_t amount = remaining > buffer_size ? buffer_size :
                        (size_t)remaining;

        if (!checked_read(file, file_size, offset, buffer, amount, stage,
                          result))
            return false;
        for (index = 0; index < amount; index++) {
            uint8_t value = buffer[index];
            while (matched != 0u && value != pattern[matched])
                matched = prefix[matched - 1u];
            if (value == pattern[matched])
                matched++;
            if (matched == pattern_size) {
                uint64_t end = offset + index + 1u;
                if (*hits == 0u)
                    *hit = end - pattern_size;
                (*hits)++;
                if (*hits > 1u)
                    return true;
                matched = prefix[matched - 1u];
            }
        }
        offset += amount;
    }
    return true;
}

static bool fstab_rewrite(host_file_t *file, uint64_t file_size,
                          const char *line, uint8_t *buffer,
                          size_t buffer_size, rootfs_work_result_t *result) {
    size_t prefix[sizeof(FSTAB_STOCK) - 1u];
    const size_t pattern_size = sizeof(FSTAB_STOCK) - 1u;
    uint64_t hit = UINT64_MAX;
    unsigned hits = 0;
    size_t line_size = 0;
    uint8_t replacement[sizeof(FSTAB_STOCK) - 1u];

    if (!line) {
        result_fail(result, ROOTFS_WORK_FSTAB_LINE_INVALID,
                    ROOTFS_WORK_STAGE_FSTAB_WRITE, 0,
                    "fstab replacement line is NULL");
        return false;
    }
    while (line_size < pattern_size && line[line_size] != '\0') {
        if (line[line_size] == '\n' || line[line_size] == '\r') {
            result_fail(result, ROOTFS_WORK_FSTAB_LINE_INVALID,
                        ROOTFS_WORK_STAGE_FSTAB_WRITE, 0,
                        "fstab replacement must be exactly one line");
            return false;
        }
        line_size++;
    }
    if (line_size == 0u || line_size == pattern_size ||
        line_size + 1u > pattern_size) {
        result_fail(result, ROOTFS_WORK_FSTAB_LINE_INVALID,
                    ROOTFS_WORK_STAGE_FSTAB_WRITE, 0,
                    "fstab replacement must contain 1..%zu bytes",
                    pattern_size - 1u);
        return false;
    }

    if (!pattern_scan_unique(file, file_size, FSTAB_STOCK, pattern_size,
                             prefix, buffer, buffer_size, &hit, &hits,
                             ROOTFS_WORK_STAGE_FSTAB_SCAN, result))
        return false;
    if (hits > 1u) {
        result_fail(result, ROOTFS_WORK_FSTAB_NOT_UNIQUE,
                    ROOTFS_WORK_STAGE_FSTAB_SCAN, 0,
                    "stock fstab record occurs more than once");
        return false;
    }
    if (hits != 1u) {
        result_fail(result, ROOTFS_WORK_FSTAB_NOT_UNIQUE,
                    ROOTFS_WORK_STAGE_FSTAB_SCAN, 0,
                    "stock fstab record occurs 0 times; exactly 1 is required");
        return false;
    }
    memcpy(replacement, line, line_size);
    replacement[line_size] = '\n';
    if (pattern_size > line_size + 1u) {
        size_t padding = pattern_size - line_size - 1u;
        if (padding > 1u)
            memset(replacement + line_size + 1u, '#', padding - 1u);
        replacement[pattern_size - 1u] = '\n';
    }
    if (!checked_write(file, file_size, hit, replacement, sizeof(replacement),
                       ROOTFS_WORK_STAGE_FSTAB_WRITE, result))
        return false;
    result->fstab_offset = hit;
    return true;
}

/*
 * Select QuartzCore's software renderer for SpringBoard, by the one mechanism
 * Apple's own code provides for it: an environment variable launchd exports.
 *
 * This is the same shape of edit as fstab_rewrite above, and for the same
 * reason -- it is a guest CONFIGURATION change to a documented, plain-text
 * Apple file, not a guess at an undocumented binary layout.  The stock record
 * is located BY CONTENT and must occur EXACTLY ONCE in the whole image; the
 * replacement is exactly as long, so logicalSize, totalBlocks and the file's
 * single extent are all unchanged and no HFS+ catalog surgery is involved.
 * Missing, ambiguous, or a replacement of the wrong length are all refusals,
 * before anything is written.  See CA_PLIST_SOFTWARE_RENDER for the provenance
 * of the bytes and for why the flag is the thing that stops the crash loop.
 *
 * Only the unpublished temporary work image is touched; the immutable source
 * is already closed by the time this runs.
 */
static bool ca_plist_rewrite(host_file_t *file, uint64_t file_size,
                             uint8_t *buffer, size_t buffer_size,
                             rootfs_work_result_t *result) {
    size_t prefix[sizeof(CA_PLIST_STOCK) - 1u];
    const size_t pattern_size = sizeof(CA_PLIST_STOCK) - 1u;
    const size_t replacement_size = sizeof(CA_PLIST_SOFTWARE_RENDER) - 1u;
    uint64_t hit = UINT64_MAX;
    unsigned hits = 0;

    /* The compile-time gate above already forbids this; keep the runtime
     * refusal anyway so the invariant is enforced where the write happens. */
    if (replacement_size != pattern_size) {
        result_fail(result, ROOTFS_WORK_CA_PLIST_INVALID,
                    ROOTFS_WORK_STAGE_CA_PLIST_WRITE, 0,
                    "software-render plist is %zu bytes, not the stock %zu",
                    replacement_size, pattern_size);
        return false;
    }
    if (!pattern_scan_unique(file, file_size, CA_PLIST_STOCK, pattern_size,
                             prefix, buffer, buffer_size, &hit, &hits,
                             ROOTFS_WORK_STAGE_CA_PLIST_SCAN, result))
        return false;
    if (hits > 1u) {
        result_fail(result, ROOTFS_WORK_CA_PLIST_NOT_UNIQUE,
                    ROOTFS_WORK_STAGE_CA_PLIST_SCAN, 0,
                    "stock SpringBoard launchd plist occurs more than once");
        return false;
    }
    if (hits != 1u) {
        result_fail(result, ROOTFS_WORK_CA_PLIST_NOT_UNIQUE,
                    ROOTFS_WORK_STAGE_CA_PLIST_SCAN, 0,
                    "stock SpringBoard launchd plist occurs 0 times; exactly 1 "
                    "is required");
        return false;
    }
    if (!checked_write(file, file_size, hit, CA_PLIST_SOFTWARE_RENDER,
                       replacement_size, ROOTFS_WORK_STAGE_CA_PLIST_WRITE,
                       result))
        return false;
    result->ca_plist_offset = hit;
    return true;
}

/*
 * Give the guest a PPP job, by rewriting an inert LaunchDaemon plist in place.
 *
 * Structurally identical to ca_plist_rewrite above and deliberately so: same
 * locate-by-content, same exactly-once requirement, same equal-length
 * overwrite, same refusal set.  See PPP_PLIST_STOCK and PPP_PLIST_JOB for the
 * provenance of both byte strings, why com.apple.chud.pilotfish is the file
 * with the budget, and why every pppd argument is on the command line.
 *
 * The one thing worth restating here, because it is what makes the whole
 * transformation defensible: nothing about this creates, moves, grows or
 * shrinks a catalog record.  The file keeps its CNID, its logicalSize, its
 * totalBlocks and its single extent, and the only bytes that change are the
 * 530 inside it.  An image that already carries the job is refused rather than
 * rewritten, because the stock pattern is then absent -- which is the correct
 * answer and not a bug: this transformation runs against a freshly copied
 * work image, once.
 *
 * Only the unpublished temporary work image is touched; the immutable source
 * is already closed by the time this runs.
 */
static bool ppp_plist_rewrite(host_file_t *file, uint64_t file_size,
                              uint8_t *buffer, size_t buffer_size,
                              rootfs_work_result_t *result) {
    size_t prefix[sizeof(PPP_PLIST_STOCK) - 1u];
    const size_t pattern_size = sizeof(PPP_PLIST_STOCK) - 1u;
    const size_t replacement_size = sizeof(PPP_PLIST_JOB) - 1u;
    uint64_t hit = UINT64_MAX;
    unsigned hits = 0;

    /* The compile-time gate above already forbids this; keep the runtime
     * refusal anyway so the invariant is enforced where the write happens. */
    if (replacement_size != pattern_size) {
        result_fail(result, ROOTFS_WORK_PPP_PLIST_INVALID,
                    ROOTFS_WORK_STAGE_PPP_PLIST_WRITE, 0,
                    "pppd launchd job is %zu bytes, not the stock %zu",
                    replacement_size, pattern_size);
        return false;
    }
    if (!pattern_scan_unique(file, file_size, PPP_PLIST_STOCK, pattern_size,
                             prefix, buffer, buffer_size, &hit, &hits,
                             ROOTFS_WORK_STAGE_PPP_PLIST_SCAN, result))
        return false;
    if (hits > 1u) {
        result_fail(result, ROOTFS_WORK_PPP_PLIST_NOT_UNIQUE,
                    ROOTFS_WORK_STAGE_PPP_PLIST_SCAN, 0,
                    "stock chud.pilotfish launchd plist occurs more than once");
        return false;
    }
    if (hits != 1u) {
        result_fail(result, ROOTFS_WORK_PPP_PLIST_NOT_UNIQUE,
                    ROOTFS_WORK_STAGE_PPP_PLIST_SCAN, 0,
                    "stock chud.pilotfish launchd plist occurs 0 times; "
                    "exactly 1 is required");
        return false;
    }
    if (!checked_write(file, file_size, hit, PPP_PLIST_JOB,
                       replacement_size, ROOTFS_WORK_STAGE_PPP_PLIST_WRITE,
                       result))
        return false;
    result->ppp_plist_offset = hit;
    return true;
}

static bool extent_overlaps(uint32_t start, uint32_t count,
                            uint32_t range_start, uint32_t range_end) {
    uint64_t end = (uint64_t)start + count;
    return count != 0u && start < range_end && range_start < end;
}

static bool grow_volume(host_file_t *file, uint64_t *file_size,
                        uint64_t growth_bytes, const hfs_volume_t *before,
                        uint8_t *buffer, size_t buffer_size,
                        rootfs_work_result_t *result) {
    uint64_t requested_blocks;
    uint64_t requested_total;
    uint32_t new_total;
    uint32_t old_tail;
    uint32_t new_tail;
    uint64_t new_size;
    uint8_t primary[HFS_VH_LEN];
    uint32_t used;
    unsigned index;
    int error = 0;

    if (growth_bytes == 0u)
        return true;
    requested_blocks = growth_bytes / before->block_size;
    if (requested_blocks != 0u)
        requested_blocks--;
    requested_total = (uint64_t)before->total_blocks + requested_blocks;
    new_total = requested_total > before->nbits ? before->nbits :
                (uint32_t)requested_total;
    if (new_total <= before->total_blocks) {
        result_fail(result, ROOTFS_WORK_GROW_INVALID,
                    ROOTFS_WORK_STAGE_GROW_PLAN, 0,
                    "growth request is less than two allocation blocks or bitmap is full");
        return false;
    }
    old_tail = hfs_tail_first(before->total_blocks, before->block_size);
    new_tail = hfs_tail_first(new_total, before->block_size);
    if (hfs_head_end(before->block_size) > old_tail ||
        hfs_head_end(before->block_size) > new_tail) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID,
                    ROOTFS_WORK_STAGE_GROW_PLAN, 0,
                    "reserved head and tail allocation-block ranges overlap");
        return false;
    }
    if (old_tail >= before->total_blocks || new_tail >= new_total) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID,
                    ROOTFS_WORK_STAGE_GROW_PLAN, 0,
                    "reserved-tail block range is invalid");
        return false;
    }
    for (index = 0; index < 8u; index++) {
        if (extent_overlaps(before->ext_start[index], before->ext_count[index],
                            old_tail, before->total_blocks)) {
            result_fail(result, ROOTFS_WORK_HFS_INVALID,
                        ROOTFS_WORK_STAGE_GROW_PLAN, 0,
                        "allocation file overlaps the old reserved tail");
            return false;
        }
    }
    for (index = old_tail; index < before->total_blocks; index++) {
        bool set;
        if (!allocation_bit_read(file, *file_size, before, index, &set,
                                 ROOTFS_WORK_STAGE_GROW_PLAN, result))
            return false;
        if (!set) {
            result_fail(result, ROOTFS_WORK_HFS_INVALID,
                        ROOTFS_WORK_STAGE_GROW_PLAN, 0,
                        "old reserved-tail block %u is marked free", index);
            return false;
        }
    }
    for (index = new_tail; index < new_total; index++) {
        bool set;
        if (index < before->total_blocks)
            continue;
        if (!allocation_bit_read(file, *file_size, before, index, &set,
                                 ROOTFS_WORK_STAGE_GROW_PLAN, result))
            return false;
        if (set) {
            result_fail(result, ROOTFS_WORK_HFS_INVALID,
                        ROOTFS_WORK_STAGE_GROW_PLAN, 0,
                        "new reserved-tail block %u is already allocated",
                        index);
            return false;
        }
    }
    new_size = (uint64_t)new_total * before->block_size;
    if (new_size <= *file_size || new_size > (uint64_t)INT64_MAX) {
        result_fail(result, ROOTFS_WORK_RANGE_ERROR,
                    ROOTFS_WORK_STAGE_GROW_PLAN, 0,
                    "grown image size is outside the supported 64-bit range");
        return false;
    }
    if (!host_file_resize(file, new_size, &error)) {
        result_fail(result, ROOTFS_WORK_WRITE_FAILED,
                    ROOTFS_WORK_STAGE_GROW_WRITE, error,
                    "cannot extend temporary work image to %" PRIu64 " bytes",
                    new_size);
        return false;
    }
    *file_size = new_size;
    for (index = old_tail; index < before->total_blocks; index++) {
        if (!allocation_bit_write(file, *file_size, before, index, false,
                                  ROOTFS_WORK_STAGE_GROW_WRITE, result))
            return false;
    }
    for (index = new_tail; index < new_total; index++) {
        if (!allocation_bit_write(file, *file_size, before, index, true,
                                  ROOTFS_WORK_STAGE_GROW_WRITE, result))
            return false;
    }

    {
        hfs_volume_t recount = *before;
        recount.total_blocks = new_total;
        recount.nbits = before->nbits;
        if (!allocation_scan(file, *file_size, &recount, buffer, buffer_size,
                             &used, ROOTFS_WORK_STAGE_GROW_WRITE, result))
            return false;
    }
    if (!checked_read(file, *file_size, HFS_VH_OFF, primary, sizeof(primary),
                      ROOTFS_WORK_STAGE_GROW_WRITE, result))
        return false;
    write_be32(primary + 44, new_total);
    write_be32(primary + 48, new_total - used);
    write_be32(primary + 52, old_tail);
    if (!checked_write(file, *file_size, HFS_VH_OFF, primary, sizeof(primary),
                       ROOTFS_WORK_STAGE_GROW_WRITE, result) ||
        !checked_write(file, *file_size, new_size - HFS_VH_OFF, primary,
                       sizeof(primary), ROOTFS_WORK_STAGE_GROW_WRITE, result))
        return false;
    return true;
}

/* ======================= HFS+ catalog provisioning =======================
 *
 * Everything above this line is a size-neutral overwrite: the catalog never
 * changes, so nothing can go wrong in it.  This section is the opposite kind
 * of code -- it inserts records into the catalog B-tree that a guest kernel
 * will mount and trust -- so it is built around four rules.
 *
 * 1. PLAN, THEN COMMIT.  Every decision and every refusal happens while the
 *    work image is still untouched.  The plan is applied to an in-memory cache
 *    of the nodes, the bitmap and the volume header; not one byte reaches the
 *    file until the whole request has succeeded in memory.  A refusal
 *    therefore leaves the image byte-identical, and that is a property of the
 *    structure rather than of remembering to undo things.
 *
 * 2. ONE SPLIT, AND IT IS THE ONLY ONE THAT CAN HAPPEN HERE.  The rule used to
 *    be "no splitting at all".  Exactly one case is now implemented, because
 *    exactly one case can arise from the way this provisioner is used: a batch
 *    of new CNIDs is created in ascending key order, so every key that does not
 *    fit is a key BEYOND the tree's current maximum, landing at the end of the
 *    LAST leaf.  catalog_leaf_split_append() handles that and nothing else.
 *
 *    A full leaf's split has to be recorded in the index node above it, and
 *    THAT node can be full too -- on the shipping 7E18 catalog it very nearly
 *    is, with 16 free bytes against 127 children, room for exactly one more
 *    leaf.  So catalog_level_extend() is written once and used at every level:
 *    the operation "chain a new node on to the right-hand end of this level and
 *    tell the level above" is identical for a leaf and for an index node, and
 *    it recurses upwards until some level has room.  It is still append-only at
 *    every level, and it still moves no existing record.
 *
 *    Everything else still refuses, by the same names as before.  An interior
 *    insert into a full leaf is ROOTFS_WORK_PROVISION_NODE_FULL -- unchanged,
 *    including its message.  A record that would become a leaf's FIRST key in a
 *    tree with index nodes is ROOTFS_WORK_PROVISION_LEAF_HEAD, because the
 *    ancestors' index keys would have to be rewritten.  A rightmost append that
 *    runs out of levels -- a full ROOT, or a root leaf with no index level
 *    above it at all -- is ROOTFS_WORK_PROVISION_SPLIT_UNSUPPORTED, because
 *    growing a new root changes treeDepth and rootNode and that is not
 *    implemented; an exhausted node map is ROOTFS_WORK_PROVISION_BTREE_FULL.
 *    A half-implemented interior split that silently corrupted the tree would
 *    be far worse than any of those refusals.
 *
 *    What the implemented split touches is still a short, enumerable list, and
 *    catalog_audit() below checks every item on it: the old node's fLink, the
 *    new node's bLink/fLink, the B-tree header's lastLeafNode, freeNodes and
 *    the node-allocation map in the header node's third record, and one
 *    appended record in the parent index node.  The new node receives the
 *    single record being inserted and the old node is left exactly as it was,
 *    which is both the cheapest split for ascending appends and the one with
 *    the least to get wrong.  treeDepth, rootNode, firstLeafNode and totalNodes
 *    are never written.
 *
 * 3. A SEARCH THAT CANNOT LOOK MUST SAY SO.  tools/hfsx_extract.py starts its
 *    walk at the B-tree header's firstLeafNode, and on an image where that
 *    field was stale it reported "not found" for every file that was actually
 *    there.  Absence and inability are different answers.  Here, lookups
 *    descend from rootNode with full structural validation at every step and
 *    return ROOTFS_WORK_PROVISION_CATALOG_CORRUPT -- never "missing" -- for
 *    anything unexpected; and catalog_audit() below independently walks the
 *    whole leaf chain, checks it against the header's own firstLeafNode,
 *    lastLeafNode and leafRecords, and requires globally ascending keys.  It
 *    runs once before the plan, so a broken tree is refused rather than
 *    written into, and once after the commit, so what was written is verified
 *    by a reader that shares no state with the writer.
 *
 * 4. THE ORDERING IS CHECKED, NOT ASSUMED.  The stock rootfs ships with
 *    freeBlocks = 0.  This runs after grow_volume and re-reads the volume
 *    header itself, so "there is somewhere to put it" is a measurement of the
 *    image in front of it.
 *
 * Key ordering is HFSX binary compare (keyCompareType 0xBC), which is what the
 * shipping image uses.  Case-folding order (0xCF, and every HFS+ volume) needs
 * Apple's fold table to place a name correctly among existing ones, so it is
 * refused rather than approximated.
 */

#define HFS_CAT_FOLDER_RECORD 1u
#define HFS_CAT_FILE_RECORD 2u
#define HFS_CAT_FOLDER_THREAD 3u
#define HFS_CAT_FILE_THREAD 4u
#define HFS_CAT_FOLDER_DATA 88u
#define HFS_CAT_FILE_DATA 248u
#define HFS_CAT_THREAD_FIXED 10u
#define HFS_BT_INDEX_NODE 0x00u
#define HFS_BT_HEADER_NODE 0x01u
#define HFS_BT_LEAF_NODE 0xffu
#define HFS_BT_BIG_KEYS 0x00000002u
#define HFS_BT_VARIABLE_INDEX_KEYS 0x00000004u
#define HFS_KEY_COMPARE_BINARY 0xbcu
#define HFS_ROOT_PARENT_CNID 1u
#define HFS_ROOT_FOLDER_CNID 2u
#define HFS_FIRST_USER_CNID 16u
#define HFS_NAME_MAX_UNITS 255u
#define HFS_NODE_DESCRIPTOR 14u
#define HFS_MAX_RECORD_BYTES 1024u
#define HFS_MAX_TREE_DEPTH 8u
#define HFS_FLAG_THREAD_EXISTS 0x0002u
#define HFS_FLAG_HAS_FOLDER_COUNT 0x0010u
#define HFS_MODE_IFDIR 0040000u
#define HFS_MODE_IFREG 0100000u
#define HFS_MODE_PERM_MASK 07777u
/* The header node's three records: BTHeaderRec, a 128-byte user record, and
 * the node-allocation map that fills the rest of the node. */
#define HFS_BT_HEADER_RECORDS 3u
#define HFS_BT_MAP_RECORD 2u

typedef struct catalog_node_cache {
    uint32_t index;
    bool dirty;
    uint8_t *bytes;
} catalog_node_cache_t;

typedef struct catalog_content {
    uint32_t start_block;
    uint32_t block_count;
    const uint8_t *bytes;
    size_t size;
} catalog_content_t;

typedef struct catalog_ctx {
    host_file_t *file;
    uint64_t file_size;
    uint32_t block_size;
    uint32_t total_blocks;

    /* Mutable copy of the primary volume header. */
    uint8_t vh[HFS_VH_LEN];
    bool vh_dirty;

    /* Catalog fork geometry. */
    uint32_t ext_start[8];
    uint32_t ext_count[8];
    uint64_t fork_bytes;

    /* B-tree header (node 0). */
    uint16_t node_size;
    uint16_t tree_depth;
    uint32_t root_node;
    uint32_t first_leaf;
    uint32_t last_leaf;
    uint32_t total_nodes;
    uint32_t free_nodes;
    uint32_t leaf_records;

    /*
     * The B-tree's own node-allocation map, cached out of the header node's
     * third record exactly the way the allocation bitmap is cached out of the
     * allocation file.  map_offset/map_bytes locate it inside node 0 so the
     * commit can put it back where it came from, and a tree whose totalNodes
     * outgrows this one record (which would put the rest in chained map nodes)
     * is refused rather than half-read.
     */
    uint8_t *node_map;
    uint16_t map_offset;
    uint16_t map_bytes;
    bool map_dirty;
    /* Rightmost splits performed; reported, never silently absorbed. */
    uint32_t leaf_splits;
    uint32_t index_splits;

    /* Allocation bitmap, cached whole. */
    const hfs_volume_t *volume;
    uint8_t *bitmap;
    size_t bitmap_bytes;
    bool bitmap_dirty;

    catalog_node_cache_t *nodes;
    size_t node_count;
    uint8_t *build;   /* one record under construction */
    uint8_t *scratch; /* one node: rebuilds and uncached streaming reads */

    uint32_t file_count;
    uint32_t folder_count;
    uint32_t next_cnid;
    uint32_t free_blocks;
    uint32_t next_alloc;
    uint32_t mac_time;
} catalog_ctx_t;

typedef struct catalog_folder_ref {
    uint32_t cnid;
    uint32_t key_parent;
    uint16_t key_length;
    uint16_t key_unit[HFS_NAME_MAX_UNITS];
} catalog_folder_ref_t;

typedef struct rootfs_path_component {
    uint16_t start;
    uint16_t length;
} rootfs_path_component_t;

/*
 * HFSX binary key order: parent CNID first, then the name as unsigned UTF-16
 * code units, then the shorter name first.  `*valid` distinguishes "this
 * record's key is malformed" from any ordering answer, so no caller can read a
 * corrupt record as a miss.
 */
static int catalog_key_compare_raw(const uint8_t *record, uint16_t record_len,
                                   uint32_t parent, const uint16_t *units,
                                   uint16_t unit_count, bool *valid) {
    uint16_t key_length;
    uint32_t record_parent;
    uint16_t record_units;
    uint16_t limit;
    uint16_t index;

    *valid = false;
    if (record_len < 8u)
        return 0;
    key_length = read_be16(record);
    if (key_length < 6u || (uint32_t)key_length + 2u > (uint32_t)record_len)
        return 0;
    record_parent = read_be32(record + 2);
    record_units = read_be16(record + 6);
    if (record_units > HFS_NAME_MAX_UNITS ||
        (uint32_t)record_units * 2u + 6u > (uint32_t)key_length)
        return 0;
    *valid = true;
    if (record_parent != parent)
        return record_parent < parent ? -1 : 1;
    limit = record_units < unit_count ? record_units : unit_count;
    for (index = 0; index < limit; index++) {
        uint16_t left = read_be16(record + 8u + (uint32_t)index * 2u);
        uint16_t right = units[index];
        if (left != right)
            return left < right ? -1 : 1;
    }
    if (record_units == unit_count)
        return 0;
    return record_units < unit_count ? -1 : 1;
}

static uint16_t catalog_record_data_offset(const uint8_t *record) {
    uint32_t offset = 2u + read_be16(record);
    if ((offset & 1u) != 0u)
        offset++;
    return (uint16_t)offset;
}

static uint16_t catalog_slot(const uint8_t *node, uint16_t node_size,
                             uint16_t index) {
    return read_be16(node + node_size - 2u * ((uint32_t)index + 1u));
}

static void catalog_slot_write(uint8_t *node, uint16_t node_size,
                               uint16_t index, uint16_t value) {
    write_be16(node + node_size - 2u * ((uint32_t)index + 1u), value);
}

static uint16_t catalog_record_count(const uint8_t *node) {
    return read_be16(node + 10);
}

/*
 * Structural validation of one node.  This is the gate that turns "the tree
 * disagrees with itself" into a refusal instead of a wrong answer, so it is
 * deliberately exhaustive: descriptor, offset array, every key, and every
 * record body's minimum size for the type it claims to be.
 */
static bool catalog_node_check(const catalog_ctx_t *ctx, const uint8_t *node,
                               uint32_t index, uint8_t expect_kind,
                               uint8_t expect_height,
                               rootfs_work_stage_t stage,
                               rootfs_work_result_t *result) {
    uint16_t node_size = ctx->node_size;
    uint16_t count = catalog_record_count(node);
    uint16_t previous = HFS_NODE_DESCRIPTOR;
    uint16_t limit;
    uint16_t record_index;

    if (node[8] != expect_kind || node[9] != expect_height) {
        result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                    "catalog node %u is kind 0x%02x height %u, expected "
                    "0x%02x/%u", index, node[8], node[9], expect_kind,
                    expect_height);
        return false;
    }
    if (count == 0u ||
        (uint32_t)HFS_NODE_DESCRIPTOR + 2u * ((uint32_t)count + 1u) >
            (uint32_t)node_size) {
        result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                    "catalog node %u claims %u records", index, count);
        return false;
    }
    limit = (uint16_t)(node_size - 2u * ((uint32_t)count + 1u));
    for (record_index = 0; record_index <= count; record_index++) {
        uint16_t offset = catalog_slot(node, node_size, record_index);
        if ((offset & 1u) != 0u || offset > limit ||
            (record_index == 0u ? offset != HFS_NODE_DESCRIPTOR
                                : offset <= previous)) {
            result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                        "catalog node %u offset slot %u is 0x%04x after "
                        "0x%04x (limit 0x%04x)", index, record_index, offset,
                        previous, limit);
            return false;
        }
        previous = offset;
    }
    for (record_index = 0; record_index < count; record_index++) {
        uint16_t offset = catalog_slot(node, node_size, record_index);
        uint16_t end = catalog_slot(node, node_size,
                                    (uint16_t)(record_index + 1u));
        uint16_t record_len = (uint16_t)(end - offset);
        const uint8_t *record = node + offset;
        uint16_t data_offset;
        bool valid = false;
        uint16_t needed;

        (void)catalog_key_compare_raw(record, record_len, 0u, NULL, 0u, &valid);
        if (!valid) {
            result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                        "catalog node %u record %u has a malformed key",
                        index, record_index);
            return false;
        }
        data_offset = catalog_record_data_offset(record);
        if (expect_kind == HFS_BT_INDEX_NODE) {
            uint32_t child;
            if ((uint32_t)data_offset + 4u > (uint32_t)record_len) {
                result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT,
                            stage, 0, "catalog index node %u record %u has no "
                            "child pointer", index, record_index);
                return false;
            }
            child = read_be32(record + data_offset);
            if (child == 0u || child >= ctx->total_nodes) {
                result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT,
                            stage, 0, "catalog index node %u record %u points "
                            "at node %u of %u", index, record_index, child,
                            ctx->total_nodes);
                return false;
            }
            continue;
        }
        if ((uint32_t)data_offset + 2u > (uint32_t)record_len) {
            result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                        "catalog leaf %u record %u has no record type",
                        index, record_index);
            return false;
        }
        switch (read_be16(record + data_offset)) {
        case HFS_CAT_FOLDER_RECORD: needed = HFS_CAT_FOLDER_DATA; break;
        case HFS_CAT_FILE_RECORD: needed = HFS_CAT_FILE_DATA; break;
        case HFS_CAT_FOLDER_THREAD:
        case HFS_CAT_FILE_THREAD: needed = HFS_CAT_THREAD_FIXED; break;
        default:
            result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                        "catalog leaf %u record %u has record type %u",
                        index, record_index, read_be16(record + data_offset));
            return false;
        }
        if ((uint32_t)data_offset + needed > (uint32_t)record_len) {
            result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                        "catalog leaf %u record %u is %u bytes, too short for "
                        "its type", index, record_index, record_len);
            return false;
        }
    }
    return true;
}

static bool catalog_node_offset(const catalog_ctx_t *ctx, uint32_t index,
                                uint64_t *offset, rootfs_work_stage_t stage,
                                rootfs_work_result_t *result) {
    uint64_t logical = (uint64_t)index * ctx->node_size;
    uint64_t seen = 0;
    unsigned extent;

    if (index >= ctx->total_nodes ||
        logical + ctx->node_size > ctx->fork_bytes) {
        result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                    "catalog node %u is outside the %" PRIu64 "-byte fork",
                    index, ctx->fork_bytes);
        return false;
    }
    for (extent = 0; extent < 8u; extent++) {
        uint64_t span = (uint64_t)ctx->ext_count[extent] * ctx->block_size;
        if (span == 0u)
            continue;
        if (logical < seen + span) {
            *offset = (uint64_t)ctx->ext_start[extent] * ctx->block_size +
                      (logical - seen);
            return true;
        }
        seen += span;
    }
    result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                "catalog node %u is not covered by the fork's extents", index);
    return false;
}

static bool catalog_node_read_raw(catalog_ctx_t *ctx, uint32_t index,
                                  uint8_t *into, rootfs_work_stage_t stage,
                                  rootfs_work_result_t *result) {
    uint64_t offset = 0;

    if (!catalog_node_offset(ctx, index, &offset, stage, result))
        return false;
    return checked_read(ctx->file, ctx->file_size, offset, into,
                        ctx->node_size, stage, result);
}

static bool catalog_node_load(catalog_ctx_t *ctx, uint32_t index,
                              uint8_t **bytes, rootfs_work_stage_t stage,
                              rootfs_work_result_t *result) {
    size_t slot;

    for (slot = 0; slot < ctx->node_count; slot++) {
        if (ctx->nodes[slot].index == index) {
            *bytes = ctx->nodes[slot].bytes;
            return true;
        }
    }
    if (ctx->node_count >= ROOTFS_WORK_MAX_CATALOG_NODES) {
        result_fail(result, ROOTFS_WORK_PROVISION_LIMIT, stage, 0,
                    "this request touches more than %u catalog nodes",
                    ROOTFS_WORK_MAX_CATALOG_NODES);
        return false;
    }
    ctx->nodes[ctx->node_count].bytes = (uint8_t *)malloc(ctx->node_size);
    if (!ctx->nodes[ctx->node_count].bytes) {
        result_fail(result, ROOTFS_WORK_NO_MEMORY, stage, 0,
                    "cannot cache catalog node %u", index);
        return false;
    }
    if (!catalog_node_read_raw(ctx, index, ctx->nodes[ctx->node_count].bytes,
                               stage, result)) {
        free(ctx->nodes[ctx->node_count].bytes);
        ctx->nodes[ctx->node_count].bytes = NULL;
        return false;
    }
    ctx->nodes[ctx->node_count].index = index;
    ctx->nodes[ctx->node_count].dirty = false;
    *bytes = ctx->nodes[ctx->node_count].bytes;
    ctx->node_count++;
    return true;
}

static void catalog_node_dirty(catalog_ctx_t *ctx, uint32_t index) {
    size_t slot;

    for (slot = 0; slot < ctx->node_count; slot++) {
        if (ctx->nodes[slot].index == index) {
            ctx->nodes[slot].dirty = true;
            return;
        }
    }
}

/*
 * Descend from rootNode to the leaf that owns `parent`/`name`.  On a clean
 * return *leaf is the leaf node, *position is either the matching record or
 * the index the record would be inserted at, and *found says which.  Any
 * structural surprise is a CATALOG_CORRUPT failure, never a miss.
 */
static bool catalog_search(catalog_ctx_t *ctx, uint32_t parent,
                           const uint16_t *units, uint16_t unit_count,
                           uint32_t *leaf, uint16_t *position, bool *found,
                           rootfs_work_stage_t stage,
                           rootfs_work_result_t *result) {
    uint32_t node_index = ctx->root_node;
    uint16_t level;

    *found = false;
    *position = 0;
    *leaf = 0;
    for (level = ctx->tree_depth; level >= 1u; level--) {
        uint8_t *node = NULL;
        uint16_t count;
        uint16_t record_index;
        int best = -1;
        int best_order = 1;
        uint8_t kind = level == 1u ? HFS_BT_LEAF_NODE : HFS_BT_INDEX_NODE;

        if (!catalog_node_load(ctx, node_index, &node, stage, result))
            return false;
        if (!catalog_node_check(ctx, node, node_index, kind, (uint8_t)level,
                                stage, result))
            return false;
        count = catalog_record_count(node);
        for (record_index = 0; record_index < count; record_index++) {
            uint16_t offset = catalog_slot(node, ctx->node_size, record_index);
            uint16_t end = catalog_slot(node, ctx->node_size,
                                        (uint16_t)(record_index + 1u));
            bool valid = false;
            int order = catalog_key_compare_raw(node + offset,
                                                (uint16_t)(end - offset),
                                                parent, units, unit_count,
                                                &valid);
            if (!valid) {
                result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT,
                            stage, 0, "catalog node %u record %u key became "
                            "unreadable during search", node_index,
                            record_index);
                return false;
            }
            if (order > 0)
                break;
            best = (int)record_index;
            best_order = order;
        }
        if (level == 1u) {
            *leaf = node_index;
            if (best >= 0 && best_order == 0) {
                *found = true;
                *position = (uint16_t)best;
            } else {
                *found = false;
                *position = (uint16_t)(best + 1);
            }
            return true;
        }
        {
            /* Below every index key: the record cannot exist, but its
             * insertion point is the leftmost leaf, so keep descending. */
            uint16_t child_index = best < 0 ? 0u : (uint16_t)best;
            uint16_t offset = catalog_slot(node, ctx->node_size, child_index);
            node_index = read_be32(node + offset +
                                   catalog_record_data_offset(node + offset));
        }
    }
    result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                "catalog descent ran past treeDepth %u", ctx->tree_depth);
    return false;
}

/*
 * The other half of the independent reader: every index level, checked against
 * the level below it.
 *
 * An index node's whole job is to name its children in key order, so the
 * concatenation of one level's child pointers, left to right, must be exactly
 * the next level down's chain, node for node and in the same order.  That one
 * equality catches a split that chained a new node into the leaf chain but
 * never told the index about it, one that told the index about it but left the
 * chain broken, and one that appended the child pointer to the wrong node --
 * which is why it is worth walking the levels rather than trusting the descent
 * code, whose assumptions the writer shares.
 *
 * Each level is walked by fLink from the leftmost node of that level, and each
 * node's bLink must point back at where the walk arrived from, so both link
 * directions are read rather than one being inferred from the other.
 *
 * Measured true on the shipping 7E18 catalog before it was relied on here: 15
 * level-3 children == the 15-node level-2 chain, and 1355 level-2 children ==
 * the 1355-node leaf chain, with no bLink violation at any level.
 */
static bool catalog_audit_index_levels(catalog_ctx_t *ctx,
                                       const uint32_t *leaf_chain,
                                       uint32_t leaf_count,
                                       rootfs_work_stage_t stage,
                                       rootfs_work_result_t *result) {
    uint32_t *chain = NULL;
    uint32_t *settled = NULL;
    const uint32_t *below = leaf_chain;
    uint32_t below_count = leaf_count;
    uint32_t spine[HFS_MAX_TREE_DEPTH + 1u];
    uint16_t level;
    bool okay = false;

    if (ctx->tree_depth < 2u)
        return true;
    /* The left spine, so each level's walk has a leftmost node to start at. */
    spine[ctx->tree_depth] = ctx->root_node;
    for (level = ctx->tree_depth; level >= 2u; level--) {
        uint16_t offset;

        if (!catalog_node_read_raw(ctx, spine[level], ctx->scratch, stage,
                                   result))
            return false;
        if (!catalog_node_check(ctx, ctx->scratch, spine[level],
                                HFS_BT_INDEX_NODE, (uint8_t)level, stage,
                                result))
            return false;
        offset = catalog_slot(ctx->scratch, ctx->node_size, 0u);
        spine[level - 1u] =
            read_be32(ctx->scratch + offset +
                      catalog_record_data_offset(ctx->scratch + offset));
    }
    if (spine[1] != ctx->first_leaf) {
        result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                    "the tree's leftmost leaf is %u but the header's "
                    "firstLeafNode is %u", spine[1], ctx->first_leaf);
        return false;
    }
    /* Two buffers, not one: `below` must never alias the array being filled,
     * or a level would be compared against a chain it was overwriting. */
    chain = (uint32_t *)malloc((size_t)ctx->total_nodes * sizeof(*chain));
    settled = (uint32_t *)malloc((size_t)ctx->total_nodes * sizeof(*settled));
    if (!chain || !settled) {
        result_fail(result, ROOTFS_WORK_NO_MEMORY, stage, 0,
                    "cannot audit %u catalog nodes", ctx->total_nodes);
        goto done;
    }
    for (level = 2u; level <= ctx->tree_depth; level++) {
        uint32_t node_index = spine[level];
        uint32_t previous = 0;
        uint32_t nodes = 0;
        uint32_t cursor = 0;

        while (node_index != 0u) {
            uint16_t count;
            uint16_t record;

            if (nodes >= ctx->total_nodes) {
                result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT,
                            stage, 0, "the level-%u chain does not terminate "
                            "within %u nodes", level, ctx->total_nodes);
                goto done;
            }
            if (!catalog_node_read_raw(ctx, node_index, ctx->scratch, stage,
                                       result))
                goto done;
            if (!catalog_node_check(ctx, ctx->scratch, node_index,
                                    HFS_BT_INDEX_NODE, (uint8_t)level, stage,
                                    result))
                goto done;
            if (read_be32(ctx->scratch + 4) != previous) {
                result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT,
                            stage, 0, "index node %u at level %u has bLink %u, "
                            "but the chain reached it from %u", node_index,
                            level, read_be32(ctx->scratch + 4), previous);
                goto done;
            }
            count = catalog_record_count(ctx->scratch);
            for (record = 0; record < count; record++) {
                uint16_t offset = catalog_slot(ctx->scratch, ctx->node_size,
                                               record);
                uint32_t child =
                    read_be32(ctx->scratch + offset +
                              catalog_record_data_offset(ctx->scratch + offset));

                if (cursor >= below_count || child != below[cursor]) {
                    result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT,
                                stage, 0, "level %u child %u of index node %u "
                                "is node %u; the level below has %u there",
                                level, cursor, node_index, child,
                                cursor < below_count ? below[cursor] : 0u);
                    goto done;
                }
                cursor++;
            }
            chain[nodes++] = node_index;
            previous = node_index;
            node_index = read_be32(ctx->scratch);
        }
        if (cursor != below_count) {
            result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                        "level %u names %u children but the level below is a "
                        "%u-node chain", level, cursor, below_count);
            goto done;
        }
        /* This level becomes the level below for the next round, parked in the
         * buffer the next round does not write. */
        memcpy(settled, chain, (size_t)nodes * sizeof(*chain));
        below = settled;
        below_count = nodes;
    }
    okay = true;

done:
    free(chain);
    free(settled);
    return okay;
}

/*
 * The independent reader.  It shares no state with the search above: it walks
 * the leaf chain the way tools/hfsx_extract.py does, from the B-tree header's
 * own firstLeafNode, and holds it to the header's lastLeafNode and
 * leafRecords while requiring globally ascending keys.  A stale firstLeafNode
 * -- the exact bug that made hfsx_extract.py report "not found" for every
 * file on one image -- fails here loudly instead of silently.  Then every
 * index level is checked against the level below it, so the tree's shape is
 * verified and not just its bottom row.
 */
static bool catalog_audit(catalog_ctx_t *ctx, uint32_t expect_records,
                          rootfs_work_stage_t stage,
                          rootfs_work_result_t *result) {
    uint32_t node_index = ctx->first_leaf;
    uint32_t visited = 0;
    uint32_t records = 0;
    uint32_t previous_parent = 0;
    uint16_t previous_units[HFS_NAME_MAX_UNITS];
    uint16_t previous_length = 0;
    bool have_previous = false;
    uint32_t final_node = 0;
    uint32_t previous_node = 0;
    uint32_t *leaf_chain = NULL;
    bool okay = false;

    if (ctx->first_leaf == 0u || ctx->first_leaf >= ctx->total_nodes) {
        result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                    "B-tree header firstLeafNode %u is not a node of %u",
                    ctx->first_leaf, ctx->total_nodes);
        return false;
    }
    leaf_chain = (uint32_t *)malloc((size_t)ctx->total_nodes *
                                    sizeof(*leaf_chain));
    if (!leaf_chain) {
        result_fail(result, ROOTFS_WORK_NO_MEMORY, stage, 0,
                    "cannot record a %u-node catalog leaf chain",
                    ctx->total_nodes);
        return false;
    }
    while (node_index != 0u) {
        uint16_t count;
        uint16_t record_index;

        if (visited >= ctx->total_nodes) {
            result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                        "catalog leaf chain does not terminate within %u nodes",
                        ctx->total_nodes);
            goto done;
        }
        if (!catalog_node_read_raw(ctx, node_index, ctx->scratch, stage,
                                   result))
            goto done;
        if (!catalog_node_check(ctx, ctx->scratch, node_index,
                                HFS_BT_LEAF_NODE, 1u, stage, result))
            goto done;
        if (read_be32(ctx->scratch + 4) != previous_node) {
            result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                        "leaf %u has bLink %u, but the chain reached it from %u",
                        node_index, read_be32(ctx->scratch + 4), previous_node);
            goto done;
        }
        count = catalog_record_count(ctx->scratch);
        for (record_index = 0; record_index < count; record_index++) {
            uint16_t offset = catalog_slot(ctx->scratch, ctx->node_size,
                                           record_index);
            uint16_t end = catalog_slot(ctx->scratch, ctx->node_size,
                                        (uint16_t)(record_index + 1u));
            const uint8_t *record = ctx->scratch + offset;
            uint16_t length = read_be16(record + 6);
            uint16_t unit;

            if (have_previous) {
                bool valid = false;
                int order = catalog_key_compare_raw(record,
                                                    (uint16_t)(end - offset),
                                                    previous_parent,
                                                    previous_units,
                                                    previous_length, &valid);
                if (!valid || order <= 0) {
                    result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT,
                                stage, 0, "catalog key order breaks at node %u "
                                "record %u", node_index, record_index);
                    goto done;
                }
            }
            previous_parent = read_be32(record + 2);
            previous_length = length;
            for (unit = 0; unit < length; unit++)
                previous_units[unit] = read_be16(record + 8u +
                                                 (uint32_t)unit * 2u);
            have_previous = true;
            records++;
        }
        leaf_chain[visited++] = node_index;
        final_node = node_index;
        previous_node = node_index;
        node_index = read_be32(ctx->scratch);
    }
    if (final_node != ctx->last_leaf) {
        result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                    "catalog leaf chain ends at node %u, header says %u",
                    final_node, ctx->last_leaf);
        goto done;
    }
    if (records != expect_records) {
        result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                    "catalog leaf chain holds %u records, header says %u",
                    records, expect_records);
        goto done;
    }
    okay = catalog_audit_index_levels(ctx, leaf_chain, visited, stage, result);

done:
    free(leaf_chain);
    return okay;
}

/*
 * Bytes a node has left for one more record AND its offset slot.  One
 * definition, used by the fit test, by the refusal message that quotes it, and
 * by the split's look-before-you-leap check on the parent index node, so those
 * three can never drift apart.
 */
static uint16_t catalog_node_free_bytes(const uint8_t *node,
                                        uint16_t node_size) {
    uint16_t count = catalog_record_count(node);
    uint16_t free_start = catalog_slot(node, node_size, count);
    uint16_t array_low = (uint16_t)(node_size - 2u * ((uint32_t)count + 1u));

    return free_start >= array_low ? 0u : (uint16_t)(array_low - free_start);
}

/*
 * Claim one node from the B-tree's own free-node map.  This is the map in the
 * header node's third record; a tree whose map spills into chained map nodes
 * was already refused at open, so this record is the whole picture.
 */
static bool catalog_node_map_alloc(catalog_ctx_t *ctx, uint32_t *node,
                                   rootfs_work_stage_t stage,
                                   rootfs_work_result_t *result) {
    uint32_t index;

    /* Node 0 is the header node and is never allocatable; open() already
     * required the map to agree. */
    for (index = 1u; index < ctx->total_nodes; index++) {
        size_t byte = (size_t)(index >> 3);
        uint8_t mask = (uint8_t)(1u << (7u - (index & 7u)));

        if ((ctx->node_map[byte] & mask) != 0u)
            continue;
        if (ctx->free_nodes == 0u) {
            result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                        "the node map calls node %u free but the B-tree header "
                        "says freeNodes is 0", index);
            return false;
        }
        ctx->node_map[byte] |= mask;
        ctx->map_dirty = true;
        ctx->free_nodes--;
        *node = index;
        return true;
    }
    result_fail(result, ROOTFS_WORK_PROVISION_BTREE_FULL, stage, 0,
                "all %u catalog B-tree nodes are in use; growing the catalog "
                "fork is not implemented", ctx->total_nodes);
    return false;
}

/*
 * Walk the rightmost spine from the root down to the index node that owns
 * `node` (which sits at `node_level`), proving on the way that it really does
 * own it as its LAST child.  Only the last child pointer of each level is
 * followed, so this answers exactly one question and cannot wander.
 */
static bool catalog_rightmost_parent(catalog_ctx_t *ctx, uint32_t node,
                                     uint16_t node_level, uint32_t *parent,
                                     rootfs_work_stage_t stage,
                                     rootfs_work_result_t *result) {
    uint32_t node_index = ctx->root_node;
    uint16_t level;

    if (node_level >= ctx->tree_depth) {
        result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                    "node %u at level %u has no parent in a depth-%u tree",
                    node, node_level, ctx->tree_depth);
        return false;
    }
    for (level = ctx->tree_depth; level > node_level; level--) {
        uint8_t *index_node = NULL;
        uint16_t count;
        uint16_t offset;
        uint32_t child;

        if (!catalog_node_load(ctx, node_index, &index_node, stage, result))
            return false;
        if (!catalog_node_check(ctx, index_node, node_index, HFS_BT_INDEX_NODE,
                                (uint8_t)level, stage, result))
            return false;
        count = catalog_record_count(index_node);
        offset = catalog_slot(index_node, ctx->node_size,
                              (uint16_t)(count - 1u));
        child = read_be32(index_node + offset +
                          catalog_record_data_offset(index_node + offset));
        if (level == (uint16_t)(node_level + 1u)) {
            if (child != node) {
                result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT,
                            stage, 0, "the rightmost level-%u index node %u "
                            "ends at child %u, not at node %u", level,
                            node_index, child, node);
                return false;
            }
            *parent = node_index;
            return true;
        }
        node_index = child;
    }
    result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                "the rightmost spine of a depth-%u tree never reached level %u",
                ctx->tree_depth, (unsigned)(node_level + 1u));
    return false;
}

static bool catalog_index_append(catalog_ctx_t *ctx, uint32_t index_node,
                                 const uint8_t *key_record, uint32_t child,
                                 uint16_t level, rootfs_work_stage_t stage,
                                 rootfs_work_result_t *result);

/*
 * Chain a new node on to the right-hand end of one B-tree level, carrying a
 * single record, and tell the level above about it.
 *
 * This is the shared body of BOTH splits, because they are the same operation:
 * `full` is the last node at `level`, the record belongs after everything it
 * holds, and it does not fit.  For a leaf the record is a catalog record; for
 * an index node it is a key plus a child pointer, which catalog_index_append()
 * has already built into `record`.  Nothing existing moves, at any level.
 *
 * The recursion terminates at the root: a full root would have to be replaced
 * by a new one and treeDepth incremented, and that is refused by name.  On the
 * shipping 7E18 catalog the root index node has 3280 free bytes against 15
 * children, so that refusal is not reachable by any payload this provisioner
 * is sized for -- but it is still a refusal rather than an assumption.
 */
static bool catalog_level_extend(catalog_ctx_t *ctx, uint32_t full,
                                 uint16_t level, const uint8_t *record,
                                 uint16_t record_len,
                                 const uint8_t *parent_key, uint32_t *fresh_out,
                                 rootfs_work_stage_t stage,
                                 rootfs_work_result_t *result) {
    uint16_t node_size = ctx->node_size;
    uint8_t *full_node = NULL;
    uint8_t *new_node = NULL;
    uint32_t parent = 0;
    uint32_t fresh = 0;

    if (level >= ctx->tree_depth) {
        result_fail(result, ROOTFS_WORK_PROVISION_SPLIT_UNSUPPORTED, stage, 0,
                    "node %u is the root of a depth-%u tree; splitting it would "
                    "have to grow a new root, which is not implemented", full,
                    ctx->tree_depth);
        return false;
    }
    if ((uint32_t)HFS_NODE_DESCRIPTOR + record_len + 4u > (uint32_t)node_size) {
        result_fail(result, ROOTFS_WORK_PROVISION_SPLIT_UNSUPPORTED, stage, 0,
                    "a %u-byte record does not fit an empty %u-byte node",
                    record_len, node_size);
        return false;
    }
    if (!catalog_rightmost_parent(ctx, full, level, &parent, stage, result))
        return false;
    if (!catalog_node_load(ctx, full, &full_node, stage, result))
        return false;
    /* The spine called this the last node at its level.  If the node's own
     * fLink disagrees, the two readings are inconsistent and nothing may be
     * written. */
    if (read_be32(full_node) != 0u) {
        result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                    "node %u ends the level-%u spine but its fLink is %u", full,
                    level, read_be32(full_node));
        return false;
    }
    if (!catalog_node_map_alloc(ctx, &fresh, stage, result))
        return false;
    if (fresh == full || fresh == parent || fresh == ctx->root_node ||
        fresh == ctx->first_leaf || fresh == ctx->last_leaf) {
        result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                    "the node map handed out node %u, which the tree is "
                    "already using", fresh);
        return false;
    }
    /* Read-then-overwrite rather than conjure: catalog_node_load bounds-checks
     * the node against the fork's extents, which is the check that matters. */
    if (!catalog_node_load(ctx, fresh, &new_node, stage, result))
        return false;
    memset(new_node, 0, node_size);
    write_be32(new_node, 0u);
    write_be32(new_node + 4, full);
    new_node[8] = level == 1u ? (uint8_t)HFS_BT_LEAF_NODE
                              : (uint8_t)HFS_BT_INDEX_NODE;
    new_node[9] = (uint8_t)level;
    write_be16(new_node + 10, 1u);
    memcpy(new_node + HFS_NODE_DESCRIPTOR, record, record_len);
    catalog_slot_write(new_node, node_size, 0u, HFS_NODE_DESCRIPTOR);
    catalog_slot_write(new_node, node_size, 1u,
                       (uint16_t)(HFS_NODE_DESCRIPTOR + record_len));
    catalog_node_dirty(ctx, fresh);
    if (!catalog_node_check(ctx, new_node, fresh, new_node[8], (uint8_t)level,
                            stage, result))
        return false;
    /* Only once the level above has accepted the new node does the old node
     * point at it: a refusal in there must not leave a chain the descent
     * cannot reach. */
    if (!catalog_index_append(ctx, parent, parent_key, fresh,
                              (uint16_t)(level + 1u), stage, result))
        return false;
    write_be32(full_node, fresh);
    catalog_node_dirty(ctx, full);
    *fresh_out = fresh;
    return true;
}

/*
 * Append one index record -- a copy of `key_record`'s key plus a child pointer
 * -- to the end of an index node, splitting that index node if it is full.
 * Append only: this is called for a child whose keys are beyond every key the
 * node already points at, so no existing record moves and no ancestor key
 * changes.
 */
static bool catalog_index_append(catalog_ctx_t *ctx, uint32_t index_node,
                                 const uint8_t *key_record, uint32_t child,
                                 uint16_t level, rootfs_work_stage_t stage,
                                 rootfs_work_result_t *result) {
    uint8_t *node = NULL;
    uint16_t node_size = ctx->node_size;
    uint16_t key_bytes = (uint16_t)(2u + read_be16(key_record));
    uint16_t record_len;
    uint16_t count;
    uint16_t free_start;

    if ((key_bytes & 1u) != 0u)
        key_bytes++;
    record_len = (uint16_t)(key_bytes + 4u);
    if (record_len > HFS_MAX_RECORD_BYTES) {
        result_fail(result, ROOTFS_WORK_PROVISION_INVALID, stage, 0,
                    "refusing to insert a %u-byte index record", record_len);
        return false;
    }
    if (!catalog_node_load(ctx, index_node, &node, stage, result))
        return false;
    if (catalog_node_free_bytes(node, node_size) < (uint32_t)record_len + 2u) {
        uint8_t built[HFS_MAX_RECORD_BYTES];
        uint32_t fresh = 0;

        memcpy(built, key_record, key_bytes);
        write_be32(built + key_bytes, child);
        if (!catalog_level_extend(ctx, index_node, level, built, record_len,
                                  key_record, &fresh, stage, result))
            return false;
        ctx->index_splits++;
        return true;
    }
    count = catalog_record_count(node);
    free_start = catalog_slot(node, node_size, count);
    memcpy(node + free_start, key_record, key_bytes);
    write_be32(node + free_start + key_bytes, child);
    catalog_slot_write(node, node_size, (uint16_t)(count + 1u),
                       (uint16_t)(free_start + record_len));
    write_be16(node + 10, (uint16_t)(count + 1u));
    catalog_node_dirty(ctx, index_node);
    return catalog_node_check(ctx, node, index_node, HFS_BT_INDEX_NODE,
                              (uint8_t)level, stage, result);
}

/*
 * Place one already-built record at `position` in a node that has room for it.
 * The node is rebuilt in scratch and copied back only once it is complete, so a
 * partially shifted record area is never visible even in memory.
 *
 * One definition for leaves and index nodes alike: the offset-array shuffle is
 * identical for both and a second copy of it could drift.
 */
static bool catalog_node_place(catalog_ctx_t *ctx, uint32_t node_index,
                               uint16_t position, const uint8_t *record,
                               uint16_t record_len, uint8_t kind,
                               uint16_t level, rootfs_work_stage_t stage,
                               rootfs_work_result_t *result) {
    uint8_t *node = NULL;
    uint16_t node_size = ctx->node_size;
    uint16_t count;
    uint16_t free_start;
    uint16_t insert_at;
    uint16_t record_index;

    if (!catalog_node_load(ctx, node_index, &node, stage, result))
        return false;
    count = catalog_record_count(node);
    free_start = catalog_slot(node, node_size, count);
    insert_at = catalog_slot(node, node_size, position);

    memcpy(ctx->scratch, node, node_size);
    memmove(ctx->scratch + insert_at + record_len, node + insert_at,
            (size_t)(free_start - insert_at));
    memcpy(ctx->scratch + insert_at, record, record_len);
    for (record_index = position; record_index <= count; record_index++) {
        uint16_t moved = record_index == position ?
            insert_at :
            (uint16_t)(catalog_slot(node, node_size,
                                    (uint16_t)(record_index - 1u)) +
                       record_len);
        catalog_slot_write(ctx->scratch, node_size, record_index, moved);
    }
    catalog_slot_write(ctx->scratch, node_size, (uint16_t)(count + 1u),
                       (uint16_t)(free_start + record_len));
    write_be16(ctx->scratch + 10, (uint16_t)(count + 1u));
    memcpy(node, ctx->scratch, node_size);
    catalog_node_dirty(ctx, node_index);
    return catalog_node_check(ctx, node, node_index, kind, (uint8_t)level,
                              stage, result);
}

static bool catalog_index_insert(catalog_ctx_t *ctx, uint32_t index_node,
                                 uint16_t position, const uint8_t *key_record,
                                 uint32_t child, uint16_t level,
                                 rootfs_work_stage_t stage,
                                 rootfs_work_result_t *result);

/*
 * Record `index` of the sequence a node WOULD hold if `record` were inserted at
 * `position`: the node's own records with one spliced in, addressed without
 * building it anywhere.  Both halves of a split are laid out from this, so the
 * two halves cannot disagree about what the sequence was.
 */
static const uint8_t *catalog_logical_record(const catalog_ctx_t *ctx,
                                             const uint8_t *node,
                                             uint16_t position,
                                             const uint8_t *record,
                                             uint16_t record_len,
                                             uint16_t index, uint16_t *length) {
    uint16_t source;
    uint16_t start;
    uint16_t end;

    if (index == position) {
        *length = record_len;
        return record;
    }
    source = index < position ? index : (uint16_t)(index - 1u);
    start = catalog_slot(node, ctx->node_size, source);
    end = catalog_slot(node, ctx->node_size, (uint16_t)(source + 1u));
    *length = (uint16_t)(end - start);
    return node + start;
}

/* Write logical records [from, to) into `dest` as a complete node. */
static void catalog_node_lay(const catalog_ctx_t *ctx, uint8_t *dest,
                             const uint8_t *origin, uint16_t position,
                             const uint8_t *record, uint16_t record_len,
                             uint16_t from, uint16_t to, uint32_t flink,
                             uint32_t blink, uint16_t level) {
    uint16_t node_size = ctx->node_size;
    uint16_t offset = HFS_NODE_DESCRIPTOR;
    uint16_t index;

    memset(dest, 0, node_size);
    write_be32(dest, flink);
    write_be32(dest + 4, blink);
    dest[8] = level == 1u ? (uint8_t)HFS_BT_LEAF_NODE
                          : (uint8_t)HFS_BT_INDEX_NODE;
    dest[9] = (uint8_t)level;
    write_be16(dest + 10, (uint16_t)(to - from));
    for (index = from; index < to; index++) {
        uint16_t length = 0;
        const uint8_t *source = catalog_logical_record(ctx, origin, position,
                                                       record, record_len,
                                                       index, &length);

        catalog_slot_write(dest, node_size, (uint16_t)(index - from), offset);
        memcpy(dest + offset, source, length);
        offset = (uint16_t)(offset + length);
    }
    catalog_slot_write(dest, node_size, (uint16_t)(to - from), offset);
}

/*
 * The index node that owns `node`, and the position of the record inside it
 * that points at `node`.
 *
 * catalog_rightmost_parent() above answers the same question for the one node
 * that is last at its level, by following only last-child pointers.  This
 * answers it for ANY node: it descends from the root on the node's own first
 * key -- which in a well-formed tree is exactly the key the level above files
 * it under -- and then PROVES the record it landed on really does point at
 * `node`.  A tree where it does not is CATALOG_CORRUPT, never a guess.
 */
static bool catalog_node_parent(catalog_ctx_t *ctx, uint32_t node,
                                uint16_t node_level, uint32_t *parent,
                                uint16_t *position, rootfs_work_stage_t stage,
                                rootfs_work_result_t *result) {
    uint16_t units[HFS_NAME_MAX_UNITS];
    uint8_t *bytes = NULL;
    uint32_t node_index = ctx->root_node;
    uint32_t key_parent;
    uint16_t unit_count;
    uint16_t offset;
    uint16_t unit;
    uint16_t level;

    if (node_level >= ctx->tree_depth) {
        result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                    "node %u at level %u has no parent in a depth-%u tree",
                    node, node_level, ctx->tree_depth);
        return false;
    }
    if (!catalog_node_load(ctx, node, &bytes, stage, result))
        return false;
    offset = catalog_slot(bytes, ctx->node_size, 0u);
    key_parent = read_be32(bytes + offset + 2);
    unit_count = read_be16(bytes + offset + 6);
    if (unit_count > HFS_NAME_MAX_UNITS) {
        result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                    "node %u begins with a %u-unit key", node, unit_count);
        return false;
    }
    for (unit = 0; unit < unit_count; unit++)
        units[unit] = read_be16(bytes + offset + 8u + (uint32_t)unit * 2u);

    for (level = ctx->tree_depth; level > node_level; level--) {
        uint8_t *index_node = NULL;
        uint16_t count;
        uint16_t record_index;
        uint32_t child;
        int best = -1;

        if (!catalog_node_load(ctx, node_index, &index_node, stage, result))
            return false;
        if (!catalog_node_check(ctx, index_node, node_index, HFS_BT_INDEX_NODE,
                                (uint8_t)level, stage, result))
            return false;
        count = catalog_record_count(index_node);
        for (record_index = 0; record_index < count; record_index++) {
            uint16_t start = catalog_slot(index_node, ctx->node_size,
                                          record_index);
            uint16_t end = catalog_slot(index_node, ctx->node_size,
                                        (uint16_t)(record_index + 1u));
            bool valid = false;
            int order = catalog_key_compare_raw(index_node + start,
                                                (uint16_t)(end - start),
                                                key_parent, units, unit_count,
                                                &valid);

            if (!valid) {
                result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT,
                            stage, 0, "index node %u record %u key became "
                            "unreadable while locating node %u's parent",
                            node_index, record_index, node);
                return false;
            }
            if (order > 0)
                break;
            best = (int)record_index;
        }
        /* Below every key of this level: the only child that can contain the
         * key is the leftmost one. */
        if (best < 0)
            best = 0;
        offset = catalog_slot(index_node, ctx->node_size, (uint16_t)best);
        child = read_be32(index_node + offset +
                          catalog_record_data_offset(index_node + offset));
        if (level == (uint16_t)(node_level + 1u)) {
            if (child != node) {
                result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT,
                            stage, 0, "the level-%u index node %u files node "
                            "%u's own first key under child %u", level,
                            node_index, node, child);
                return false;
            }
            *parent = node_index;
            *position = (uint16_t)best;
            return true;
        }
        node_index = child;
    }
    result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                "the descent for node %u never reached level %u", node,
                (unsigned)(node_level + 1u));
    return false;
}

/*
 * THE GENERAL SPLIT.  `record` belongs at `position` in `node`, which is full,
 * and the rightmost append above does not apply -- either the record lands
 * inside the node rather than past its end, or the node is not the last one at
 * its level.
 *
 * WHY THIS EXISTS.  A batch of catalog entries is NOT one ascending run, even
 * when the caller's paths are sorted.  Creating file f under folder P writes
 * two records: the name key (P, f) and the thread key (cnid(f), ""), and every
 * cnid handed out is larger than P.  So the second file under P has to go
 * BETWEEN the first file's name record and the first file's thread record --
 * an interior insert -- and once that stretch of the tree has filled one leaf,
 * a rightmost append is not the operation the tree needs.  That is why 64
 * entries fitted and a 644-entry payload could not.
 *
 * The node's records plus the new one are redistributed over `node` and one
 * fresh node chained immediately after it, as evenly by bytes as the record
 * boundaries allow.  Records DO move here, which is the whole difference from
 * catalog_level_extend() above; that routine is still preferred wherever it
 * applies, so an ascending run still splits without moving anything.
 * Exhaustively, what changes:
 *
 *   old node   keeps records [0, split), fLink 0 -> new node
 *   new node   records [split, end), bLink = old node, fLink = old fLink
 *   following  bLink -> new node, when the old node had a successor
 *   parent     one index record -> new node, at the old node's position + 1
 *   header     lastLeafNode when the old node was the last leaf, freeNodes -= 1
 *
 * firstLeafNode, rootNode, treeDepth and totalNodes are still never touched:
 * splitting the ROOT is the one shape that would move them and it is refused by
 * name.  Every line above is re-read from the written image by catalog_audit(),
 * which holds each index level to the child sequence of the level below it.
 */
static bool catalog_node_split(catalog_ctx_t *ctx, uint32_t node_index,
                               uint16_t level, uint16_t position,
                               const uint8_t *record, uint16_t record_len,
                               rootfs_work_stage_t stage,
                               rootfs_work_result_t *result) {
    uint16_t node_size = ctx->node_size;
    uint8_t right_key[HFS_MAX_RECORD_BYTES];
    uint8_t *node = NULL;
    uint8_t *fresh_node = NULL;
    uint8_t *next_node = NULL;
    uint8_t *origin = NULL;
    uint32_t parent = 0;
    uint32_t fresh = 0;
    uint32_t next;
    uint32_t total_bytes;
    uint32_t left_bytes = 0;
    uint32_t best_delta = 0;
    uint16_t parent_position = 0;
    uint16_t count;
    uint16_t total_count;
    uint16_t split = 0;
    uint16_t index;
    uint16_t key_bytes;
    uint8_t kind = level == 1u ? (uint8_t)HFS_BT_LEAF_NODE
                               : (uint8_t)HFS_BT_INDEX_NODE;

    if (level >= ctx->tree_depth) {
        result_fail(result, ROOTFS_WORK_PROVISION_SPLIT_UNSUPPORTED, stage, 0,
                    "node %u is the root of a depth-%u tree; splitting it would "
                    "have to grow a new root, which is not implemented",
                    node_index, ctx->tree_depth);
        return false;
    }
    if ((uint32_t)HFS_NODE_DESCRIPTOR + record_len + 4u > (uint32_t)node_size) {
        result_fail(result, ROOTFS_WORK_PROVISION_SPLIT_UNSUPPORTED, stage, 0,
                    "a %u-byte record does not fit an empty %u-byte node",
                    record_len, node_size);
        return false;
    }
    if (position == 0u) {
        result_fail(result, ROOTFS_WORK_PROVISION_LEAF_HEAD, stage, 0,
                    "record would become node %u's first key; rewriting the "
                    "ancestors' index keys is not implemented", node_index);
        return false;
    }
    if (!catalog_node_load(ctx, node_index, &node, stage, result))
        return false;
    count = catalog_record_count(node);
    if (position > count) {
        result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                    "split position %u exceeds %u records in node %u", position,
                    count, node_index);
        return false;
    }
    total_count = (uint16_t)(count + 1u);
    total_bytes = (uint32_t)record_len +
                  (uint32_t)catalog_slot(node, node_size, count) -
                  HFS_NODE_DESCRIPTOR;

    /*
     * Pick the split point: the most even one, by bytes, at which BOTH halves
     * fit their node with their offset arrays.  left_bytes only grows, so once
     * the left half stops fitting no larger split point can.
     */
    for (index = 1u; index <= count; index++) {
        uint32_t right_bytes;
        uint32_t delta;
        uint16_t length = 0;

        (void)catalog_logical_record(ctx, node, position, record, record_len,
                                     (uint16_t)(index - 1u), &length);
        left_bytes += length;
        if ((uint32_t)HFS_NODE_DESCRIPTOR + left_bytes +
            2u * ((uint32_t)index + 1u) > (uint32_t)node_size)
            break;
        right_bytes = total_bytes - left_bytes;
        if ((uint32_t)HFS_NODE_DESCRIPTOR + right_bytes +
            2u * ((uint32_t)total_count - index + 1u) > (uint32_t)node_size)
            continue;
        delta = left_bytes > right_bytes ? left_bytes - right_bytes :
                                           right_bytes - left_bytes;
        if (split == 0u || delta < best_delta) {
            split = index;
            best_delta = delta;
        }
    }
    if (split == 0u) {
        result_fail(result, ROOTFS_WORK_PROVISION_NODE_FULL, stage, 0,
                    "node %u holds %u records and no split of them plus a "
                    "%u-byte record leaves two %u-byte nodes", node_index,
                    count, record_len, node_size);
        return false;
    }

    /* The new node's own first record supplies the key the level above files
     * it under.  Copy it out before anything overwrites the node. */
    {
        uint16_t length = 0;
        const uint8_t *first = catalog_logical_record(ctx, node, position,
                                                      record, record_len, split,
                                                      &length);

        key_bytes = (uint16_t)(2u + read_be16(first));
        if ((key_bytes & 1u) != 0u)
            key_bytes++;
        if (key_bytes > sizeof(right_key) || key_bytes > length) {
            result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                        "the first record of node %u's new sibling has a "
                        "%u-byte key in %u bytes", node_index, key_bytes,
                        length);
            return false;
        }
        memset(right_key, 0, key_bytes);
        memcpy(right_key, first, (size_t)(2u + read_be16(first)));
    }

    if (!catalog_node_parent(ctx, node_index, level, &parent, &parent_position,
                             stage, result))
        return false;
    next = read_be32(node);
    if (!catalog_node_map_alloc(ctx, &fresh, stage, result))
        return false;
    if (fresh == node_index || fresh == parent || fresh == next ||
        fresh == ctx->root_node || fresh == ctx->first_leaf ||
        fresh == ctx->last_leaf) {
        result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                    "the node map handed out node %u, which the tree is "
                    "already using", fresh);
        return false;
    }
    /* Read-then-overwrite rather than conjure: catalog_node_load bounds-checks
     * the node against the fork's extents, which is the check that matters. */
    if (!catalog_node_load(ctx, fresh, &fresh_node, stage, result))
        return false;
    if (next != 0u &&
        !catalog_node_load(ctx, next, &next_node, stage, result))
        return false;
    /* Only once the level above has accepted the new node does anything point
     * at it: a refusal in there must not leave a chain the descent cannot
     * reach. */
    if (!catalog_index_insert(ctx, parent, (uint16_t)(parent_position + 1u),
                              right_key, fresh, (uint16_t)(level + 1u), stage,
                              result))
        return false;

    origin = (uint8_t *)malloc(node_size);
    if (!origin) {
        result_fail(result, ROOTFS_WORK_NO_MEMORY, stage, 0,
                    "cannot split catalog node %u", node_index);
        return false;
    }
    memcpy(origin, node, node_size);
    catalog_node_lay(ctx, node, origin, position, record, record_len, 0u, split,
                     fresh, read_be32(origin + 4), level);
    catalog_node_lay(ctx, fresh_node, origin, position, record, record_len,
                     split, total_count, next, node_index, level);
    free(origin);
    catalog_node_dirty(ctx, node_index);
    catalog_node_dirty(ctx, fresh);
    if (next_node) {
        write_be32(next_node + 4, fresh);
        catalog_node_dirty(ctx, next);
    }
    if (level == 1u && node_index == ctx->last_leaf)
        ctx->last_leaf = fresh;
    if (!catalog_node_check(ctx, node, node_index, kind, (uint8_t)level, stage,
                            result))
        return false;
    return catalog_node_check(ctx, fresh_node, fresh, kind, (uint8_t)level,
                              stage, result);
}

/*
 * Insert one index record -- a copy of `key_record`'s key plus a child pointer
 * -- at `position` in an index node, splitting that node if it is full.
 *
 * An append to the last node of its level is handed to catalog_index_append()
 * above, which does it without moving a record; everything else shifts, and
 * splits generally when there is no room to shift into.
 */
static bool catalog_index_insert(catalog_ctx_t *ctx, uint32_t index_node,
                                 uint16_t position, const uint8_t *key_record,
                                 uint32_t child, uint16_t level,
                                 rootfs_work_stage_t stage,
                                 rootfs_work_result_t *result) {
    uint8_t built[HFS_MAX_RECORD_BYTES];
    uint8_t *node = NULL;
    uint16_t node_size = ctx->node_size;
    uint16_t key_bytes = (uint16_t)(2u + read_be16(key_record));
    uint16_t record_len;
    uint16_t count;

    if ((key_bytes & 1u) != 0u)
        key_bytes++;
    record_len = (uint16_t)(key_bytes + 4u);
    if (record_len > HFS_MAX_RECORD_BYTES) {
        result_fail(result, ROOTFS_WORK_PROVISION_INVALID, stage, 0,
                    "refusing to insert a %u-byte index record", record_len);
        return false;
    }
    if (!catalog_node_load(ctx, index_node, &node, stage, result))
        return false;
    count = catalog_record_count(node);
    if (position > count) {
        result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                    "insert position %u exceeds %u records in index node %u",
                    position, count, index_node);
        return false;
    }
    if (position == count && read_be32(node) == 0u)
        return catalog_index_append(ctx, index_node, key_record, child, level,
                                    stage, result);
    memset(built, 0, record_len);
    memcpy(built, key_record, (size_t)(2u + read_be16(key_record)));
    write_be32(built + key_bytes, child);
    if (catalog_node_free_bytes(node, node_size) >= (uint32_t)record_len + 2u)
        return catalog_node_place(ctx, index_node, position, built, record_len,
                                  HFS_BT_INDEX_NODE, level, stage, result);
    if (!catalog_node_split(ctx, index_node, level, position, built, record_len,
                            stage, result))
        return false;
    ctx->index_splits++;
    return true;
}

/*
 * THE ONE SPLIT THAT MOVES NOTHING.  `record` sorts after every key in the tree
 * and does not fit in the last leaf, so a new last leaf is chained on to hold
 * it.
 *
 * No existing record moves.  The old node keeps every byte it had and gains
 * only a new fLink; the new node gets the single record being inserted.  For
 * the ascending-key batch this provisioner exists to write, that is also the
 * best split available -- the next append goes into the new node and fills it
 * before the next split -- and it is the variant with the fewest invariants in
 * flight.  What changes, exhaustively:
 *
 *   old leaf   fLink 0 -> new node                     (chain forward)
 *   new node   bLink = old leaf, fLink = 0, one record (chain back, and it is
 *                                                       the new chain end)
 *   parent     one appended index record -> new node   (descent finds it)
 *   header     lastLeafNode = new node, freeNodes -= 1, one map bit set
 *
 * firstLeafNode, rootNode, treeDepth and totalNodes are not touched, and every
 * one of the four lines above is re-read from the written image by
 * catalog_audit() and by the tests' independent chain walker.
 */
static bool catalog_leaf_split_append(catalog_ctx_t *ctx, uint32_t leaf,
                                      const uint8_t *record,
                                      uint16_t record_len,
                                      rootfs_work_stage_t stage,
                                      rootfs_work_result_t *result) {
    uint32_t fresh = 0;

    if (ctx->tree_depth < 2u) {
        result_fail(result, ROOTFS_WORK_PROVISION_SPLIT_UNSUPPORTED, stage, 0,
                    "leaf %u is the whole tree, so splitting it would have to "
                    "grow a new root; that is not implemented", leaf);
        return false;
    }
    /* The new leaf's own first record supplies the key the level above files
     * it under, and that record is the one being inserted. */
    if (!catalog_level_extend(ctx, leaf, 1u, record, record_len, record, &fresh,
                              stage, result))
        return false;
    ctx->last_leaf = fresh;
    ctx->leaf_splits++;
    return true;
}

/*
 * Insert one already-built record into a leaf, splitting the leaf if it is
 * full.
 */
static bool catalog_leaf_insert(catalog_ctx_t *ctx, uint32_t leaf,
                                uint16_t position, const uint8_t *record,
                                uint16_t record_len,
                                rootfs_work_stage_t stage,
                                rootfs_work_result_t *result) {
    uint8_t *node = NULL;
    uint16_t node_size = ctx->node_size;
    uint16_t count;

    if (record_len < 8u || (record_len & 1u) != 0u ||
        record_len > HFS_MAX_RECORD_BYTES) {
        result_fail(result, ROOTFS_WORK_PROVISION_INVALID, stage, 0,
                    "refusing to insert a %u-byte catalog record", record_len);
        return false;
    }
    if (!catalog_node_load(ctx, leaf, &node, stage, result))
        return false;
    count = catalog_record_count(node);
    if (position > count) {
        result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                    "insert position %u exceeds %u records in leaf %u",
                    position, count, leaf);
        return false;
    }
    if (position == 0u && ctx->tree_depth > 1u) {
        result_fail(result, ROOTFS_WORK_PROVISION_LEAF_HEAD, stage, 0,
                    "record would become leaf %u's first key; rewriting the "
                    "ancestors' index keys is not implemented", leaf);
        return false;
    }
    if (catalog_node_free_bytes(node, node_size) < (uint32_t)record_len + 2u) {
        /*
         * A key that sorts after every key in the tree lands past the end of
         * the LAST leaf, and that append is chained on without moving a single
         * existing record.  Everything else -- an interior insert, or an append
         * to a leaf that is not the chain's end -- goes through the general
         * split, which redistributes.
         */
        if (position == count && leaf == ctx->last_leaf) {
            if (!catalog_leaf_split_append(ctx, leaf, record, record_len, stage,
                                           result))
                return false;
            ctx->leaf_records++;
            return true;
        }
        if (!catalog_node_split(ctx, leaf, 1u, position, record, record_len,
                                stage, result))
            return false;
        ctx->leaf_splits++;
        ctx->leaf_records++;
        return true;
    }
    if (!catalog_node_place(ctx, leaf, position, record, record_len,
                            HFS_BT_LEAF_NODE, 1u, stage, result))
        return false;
    ctx->leaf_records++;
    return true;
}

static bool catalog_bitmap_test(const catalog_ctx_t *ctx, uint32_t block) {
    size_t byte = (size_t)(block >> 3);

    if (byte >= ctx->bitmap_bytes)
        return true;
    return (ctx->bitmap[byte] & (uint8_t)(1u << (7u - (block & 7u)))) != 0u;
}

static bool catalog_bitmap_alloc(catalog_ctx_t *ctx, uint32_t count,
                                 uint32_t *start, rootfs_work_stage_t stage,
                                 rootfs_work_result_t *result) {
    uint32_t scanned;
    uint32_t run_start = 0;
    uint32_t run = 0;
    uint32_t cursor = ctx->next_alloc < ctx->total_blocks ? ctx->next_alloc : 0u;

    if (count == 0u || count > ctx->free_blocks) {
        result_fail(result, ROOTFS_WORK_PROVISION_NO_SPACE, stage, 0,
                    "%u allocation blocks requested, %u free; the stock volume "
                    "ships freeBlocks = 0, so growth_bytes must be set",
                    count, ctx->free_blocks);
        return false;
    }
    /* One wrapped pass, restarting the run at the wrap so it stays
     * contiguous in block order rather than in scan order. */
    for (scanned = 0; scanned < ctx->total_blocks; scanned++) {
        /* 64-bit so a volume whose totalBlocks approaches UINT32_MAX cannot
         * wrap the cursor arithmetic instead of the block number. */
        uint64_t sum = (uint64_t)cursor + scanned;
        uint32_t block;

        if (sum >= ctx->total_blocks) {
            sum -= ctx->total_blocks;
            if (sum == 0u)
                run = 0;
        }
        block = (uint32_t)sum;
        if (catalog_bitmap_test(ctx, block)) {
            run = 0;
            continue;
        }
        if (run == 0u)
            run_start = block;
        run++;
        if (run == count) {
            uint32_t index;
            for (index = 0; index < count; index++) {
                size_t byte = (size_t)((run_start + index) >> 3);
                ctx->bitmap[byte] |= (uint8_t)(1u <<
                    (7u - ((run_start + index) & 7u)));
            }
            ctx->bitmap_dirty = true;
            ctx->free_blocks -= count;
            ctx->next_alloc = run_start + count < ctx->total_blocks ?
                              run_start + count : 0u;
            *start = run_start;
            return true;
        }
    }
    result_fail(result, ROOTFS_WORK_PROVISION_NO_SPACE, stage, 0,
                "no run of %u contiguous free allocation blocks exists", count);
    return false;
}

static void catalog_build_key(uint8_t *record, uint32_t parent,
                              const uint16_t *units, uint16_t unit_count) {
    uint16_t index;

    write_be16(record, (uint16_t)(6u + 2u * (uint32_t)unit_count));
    write_be32(record + 2, parent);
    write_be16(record + 6, unit_count);
    for (index = 0; index < unit_count; index++)
        write_be16(record + 8u + (uint32_t)index * 2u, units[index]);
}

static void catalog_build_bsd(uint8_t *data, uint32_t owner, uint32_t group,
                              uint16_t mode) {
    write_be32(data + 32, owner);
    write_be32(data + 36, group);
    data[40] = 0u;
    data[41] = 0u;
    write_be16(data + 42, mode);
    /* special.linkCount; the shipping volume carries 1 on files and folders
     * alike, so match it rather than inventing a different convention. */
    write_be32(data + 44, 1u);
}

static uint16_t catalog_build_folder(catalog_ctx_t *ctx, uint32_t parent,
                                     const uint16_t *units,
                                     uint16_t unit_count, uint32_t cnid,
                                     uint16_t flags, uint32_t owner,
                                     uint32_t group, uint16_t mode) {
    uint16_t key_bytes = (uint16_t)(8u + 2u * (uint32_t)unit_count);
    uint8_t *data = ctx->build + key_bytes;

    memset(ctx->build, 0, (size_t)key_bytes + HFS_CAT_FOLDER_DATA);
    catalog_build_key(ctx->build, parent, units, unit_count);
    write_be16(data, (uint16_t)HFS_CAT_FOLDER_RECORD);
    write_be16(data + 2, flags);
    write_be32(data + 4, 0u);            /* valence: no children yet */
    write_be32(data + 8, cnid);
    write_be32(data + 12, ctx->mac_time);
    write_be32(data + 16, ctx->mac_time);
    write_be32(data + 20, ctx->mac_time);
    write_be32(data + 24, ctx->mac_time);
    write_be32(data + 28, 0u);
    catalog_build_bsd(data, owner, group, (uint16_t)(HFS_MODE_IFDIR | mode));
    return (uint16_t)(key_bytes + HFS_CAT_FOLDER_DATA);
}

static uint16_t catalog_build_file(catalog_ctx_t *ctx, uint32_t parent,
                                   const uint16_t *units, uint16_t unit_count,
                                   uint32_t cnid, uint32_t owner,
                                   uint32_t group, uint16_t mode,
                                   uint64_t logical, uint32_t start_block,
                                   uint32_t block_count) {
    uint16_t key_bytes = (uint16_t)(8u + 2u * (uint32_t)unit_count);
    uint8_t *data = ctx->build + key_bytes;

    memset(ctx->build, 0, (size_t)key_bytes + HFS_CAT_FILE_DATA);
    catalog_build_key(ctx->build, parent, units, unit_count);
    write_be16(data, (uint16_t)HFS_CAT_FILE_RECORD);
    write_be16(data + 2, (uint16_t)HFS_FLAG_THREAD_EXISTS);
    write_be32(data + 8, cnid);
    write_be32(data + 12, ctx->mac_time);
    write_be32(data + 16, ctx->mac_time);
    write_be32(data + 20, ctx->mac_time);
    write_be32(data + 24, ctx->mac_time);
    write_be32(data + 28, 0u);
    catalog_build_bsd(data, owner, group, (uint16_t)(HFS_MODE_IFREG | mode));
    write_be64(data + 88, logical);
    write_be32(data + 96, 0u);           /* clumpSize */
    write_be32(data + 100, block_count);
    write_be32(data + 104, start_block);
    write_be32(data + 108, block_count);
    return (uint16_t)(key_bytes + HFS_CAT_FILE_DATA);
}

/*
 * A BSD symbolic link, which HFS+ stores as an ordinary catalog FILE record
 * whose fileMode says S_IFLNK and whose data fork holds the target path.
 *
 * PROVENANCE.  Every constant below was read out of firmware/rootfs.img's own
 * symlinks rather than recalled, by dumping all 409 of them and taking the
 * intersection.  On that volume these fields are byte-identical across all
 * 409, and /etc (CNID 14880, target "private/etc") is the worked example:
 *
 *   fileMode      0xA1ED  = S_IFLNK | 0755   on all 409, no exceptions
 *   userInfo      fdType 'slnk', fdCreator 'rhap', rest of FndrFileInfo zero
 *   finderInfo    16 zero bytes
 *   flags         0x0002, thread-exists only
 *   special       1 (linkCount), same as files and folders
 *   dataFork      logicalSize == strlen(target) -- the target is NOT
 *                 NUL-terminated within logicalSize -- clumpSize 0,
 *                 totalBlocks 1, one extent, extents[1..7] zero
 *   resourceFork  all 80 bytes zero
 *   thread        a normal 0x0004 file thread, exactly as for a regular file
 *
 * The only field that varies beyond CNID, dates, target and extent is groupID
 * (0 in most of the tree, 80/admin under / and /Library), which is the
 * caller's to choose because it tracks the parent directory, not the link.
 *
 * So this is catalog_build_file() plus three stores: the format bits in
 * fileMode, and the two Finder type/creator words.  The permission bits stay
 * the caller's; a symlink whose mode bits differ from 0755 is unlike anything
 * on the stock volume, which is why the default is 0755 rather than 0644.
 */
#define HFS_MODE_IFLNK 0120000u
#define HFS_SYMLINK_FINDER_TYPE 0x736c6e6bu    /* 'slnk' */
#define HFS_SYMLINK_FINDER_CREATOR 0x72686170u /* 'rhap' */

static uint16_t catalog_build_symlink(catalog_ctx_t *ctx, uint32_t parent,
                                      const uint16_t *units,
                                      uint16_t unit_count, uint32_t cnid,
                                      uint32_t owner, uint32_t group,
                                      uint16_t mode, uint64_t logical,
                                      uint32_t start_block,
                                      uint32_t block_count) {
    uint16_t key_bytes = (uint16_t)(8u + 2u * (uint32_t)unit_count);
    uint8_t *data = ctx->build + key_bytes;
    uint16_t record_len = catalog_build_file(ctx, parent, units, unit_count,
                                             cnid, owner, group, mode, logical,
                                             start_block, block_count);

    /* Rewrite the format bits catalog_build_file() set to S_IFREG. */
    write_be16(data + 42, (uint16_t)(HFS_MODE_IFLNK | mode));
    write_be32(data + 48, HFS_SYMLINK_FINDER_TYPE);
    write_be32(data + 52, HFS_SYMLINK_FINDER_CREATOR);
    return record_len;
}

static uint16_t catalog_build_thread(catalog_ctx_t *ctx, uint32_t cnid,
                                     uint16_t type, uint32_t parent,
                                     const uint16_t *units,
                                     uint16_t unit_count) {
    uint8_t *data = ctx->build + 8;
    uint16_t index;

    memset(ctx->build, 0,
           (size_t)8u + HFS_CAT_THREAD_FIXED + 2u * (size_t)unit_count);
    catalog_build_key(ctx->build, cnid, NULL, 0u);
    write_be16(data, type);
    write_be16(data + 2, 0u);
    write_be32(data + 4, parent);
    write_be16(data + 8, unit_count);
    for (index = 0; index < unit_count; index++)
        write_be16(data + 10u + (uint32_t)index * 2u, units[index]);
    return (uint16_t)(8u + HFS_CAT_THREAD_FIXED + 2u * (uint32_t)unit_count);
}

/*
 * Bump a folder record that has just gained a child.  The record is located
 * again by key rather than remembered, because an insert into the same leaf
 * moves record positions.
 */
static bool catalog_touch_folder(catalog_ctx_t *ctx,
                                 const catalog_folder_ref_t *folder,
                                 bool added_folder,
                                 rootfs_work_stage_t stage,
                                 rootfs_work_result_t *result) {
    uint32_t leaf = 0;
    uint16_t position = 0;
    bool found = false;
    uint8_t *node = NULL;
    uint16_t offset;
    uint8_t *data;

    if (!catalog_search(ctx, folder->key_parent, folder->key_unit,
                        folder->key_length, &leaf, &position, &found, stage,
                        result))
        return false;
    if (!found) {
        result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                    "folder record for CNID %u vanished before its valence "
                    "could be updated", folder->cnid);
        return false;
    }
    if (!catalog_node_load(ctx, leaf, &node, stage, result))
        return false;
    offset = catalog_slot(node, ctx->node_size, position);
    data = node + offset + catalog_record_data_offset(node + offset);
    if (read_be16(data) != HFS_CAT_FOLDER_RECORD ||
        read_be32(data + 8) != folder->cnid) {
        result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                    "record for CNID %u is not the folder it was resolved as",
                    folder->cnid);
        return false;
    }
    if (read_be32(data + 4) == UINT32_MAX) {
        result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                    "folder CNID %u already reports a saturated valence",
                    folder->cnid);
        return false;
    }
    write_be32(data + 4, read_be32(data + 4) + 1u);
    /* folderCount is only maintained on volumes that already maintain it;
     * the flag on the parent is the volume's own statement of that. */
    if (added_folder &&
        (read_be16(data + 2) & HFS_FLAG_HAS_FOLDER_COUNT) != 0u)
        write_be32(data + 84, read_be32(data + 84) + 1u);
    write_be32(data + 16, ctx->mac_time);
    write_be32(data + 20, ctx->mac_time);
    catalog_node_dirty(ctx, leaf);
    return true;
}

static bool catalog_folder_flags(catalog_ctx_t *ctx,
                                 const catalog_folder_ref_t *folder,
                                 uint16_t *flags, rootfs_work_stage_t stage,
                                 rootfs_work_result_t *result) {
    uint32_t leaf = 0;
    uint16_t position = 0;
    bool found = false;
    uint8_t *node = NULL;
    uint16_t offset;

    if (!catalog_search(ctx, folder->key_parent, folder->key_unit,
                        folder->key_length, &leaf, &position, &found, stage,
                        result))
        return false;
    if (!found) {
        result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                    "folder record for CNID %u could not be re-read",
                    folder->cnid);
        return false;
    }
    if (!catalog_node_load(ctx, leaf, &node, stage, result))
        return false;
    offset = catalog_slot(node, ctx->node_size, position);
    *flags = read_be16(node + offset + catalog_record_data_offset(node + offset)
                       + 2);
    return true;
}

static bool catalog_root_folder(catalog_ctx_t *ctx,
                                catalog_folder_ref_t *folder,
                                rootfs_work_stage_t stage,
                                rootfs_work_result_t *result) {
    uint32_t leaf = 0;
    uint16_t position = 0;
    bool found = false;
    uint8_t *node = NULL;
    uint16_t offset;
    const uint8_t *data;
    uint16_t length;
    uint16_t index;

    if (!catalog_search(ctx, HFS_ROOT_FOLDER_CNID, NULL, 0u, &leaf, &position,
                        &found, stage, result))
        return false;
    if (!found) {
        result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                    "the catalog has no thread record for the root folder");
        return false;
    }
    if (!catalog_node_load(ctx, leaf, &node, stage, result))
        return false;
    offset = catalog_slot(node, ctx->node_size, position);
    data = node + offset + catalog_record_data_offset(node + offset);
    length = read_be16(data + 8);
    if (read_be16(data) != HFS_CAT_FOLDER_THREAD ||
        read_be32(data + 4) != HFS_ROOT_PARENT_CNID ||
        length == 0u || length > HFS_NAME_MAX_UNITS) {
        result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                    "the root folder's thread record is not a named folder "
                    "thread under CNID %u", HFS_ROOT_PARENT_CNID);
        return false;
    }
    folder->cnid = HFS_ROOT_FOLDER_CNID;
    folder->key_parent = HFS_ROOT_PARENT_CNID;
    folder->key_length = length;
    for (index = 0; index < length; index++)
        folder->key_unit[index] = read_be16(data + 10u + (uint32_t)index * 2u);
    return true;
}

static bool catalog_child_folder(catalog_ctx_t *ctx,
                                 const catalog_folder_ref_t *parent,
                                 const uint16_t *units, uint16_t unit_count,
                                 catalog_folder_ref_t *folder,
                                 rootfs_work_stage_t stage,
                                 rootfs_work_result_t *result) {
    uint32_t leaf = 0;
    uint16_t position = 0;
    bool found = false;
    uint8_t *node = NULL;
    uint16_t offset;
    const uint8_t *data;
    uint16_t index;

    if (!catalog_search(ctx, parent->cnid, units, unit_count, &leaf, &position,
                        &found, stage, result))
        return false;
    if (!found) {
        result_fail(result, ROOTFS_WORK_PROVISION_PARENT_MISSING, stage, 0,
                    "no catalog record under CNID %u for a path component",
                    parent->cnid);
        return false;
    }
    if (!catalog_node_load(ctx, leaf, &node, stage, result))
        return false;
    offset = catalog_slot(node, ctx->node_size, position);
    data = node + offset + catalog_record_data_offset(node + offset);
    if (read_be16(data) != HFS_CAT_FOLDER_RECORD) {
        result_fail(result, ROOTFS_WORK_PROVISION_PARENT_MISSING, stage, 0,
                    "a path component under CNID %u exists but is not a "
                    "folder", parent->cnid);
        return false;
    }
    folder->cnid = read_be32(data + 8);
    if (folder->cnid < HFS_FIRST_USER_CNID) {
        result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                    "a folder record claims reserved CNID %u", folder->cnid);
        return false;
    }
    folder->key_parent = parent->cnid;
    folder->key_length = unit_count;
    for (index = 0; index < unit_count; index++)
        folder->key_unit[index] = units[index];
    return true;
}

static bool rootfs_path_split(const char *path,
                              rootfs_path_component_t *components,
                              size_t *count, rootfs_work_result_t *result) {
    size_t length = 0;
    size_t cursor;

    *count = 0;
    if (!path || path[0] != '/') {
        result_fail(result, ROOTFS_WORK_PROVISION_INVALID,
                    ROOTFS_WORK_STAGE_PROVISION_PLAN, 0,
                    "provisioned paths must be absolute");
        return false;
    }
    while (path[length] != '\0') {
        if (length >= ROOTFS_WORK_MAX_PATH) {
            result_fail(result, ROOTFS_WORK_PROVISION_INVALID,
                        ROOTFS_WORK_STAGE_PROVISION_PLAN, 0,
                        "provisioned paths are limited to %u bytes",
                        ROOTFS_WORK_MAX_PATH);
            return false;
        }
        length++;
    }
    if (length > 1u && path[length - 1u] == '/') {
        result_fail(result, ROOTFS_WORK_PROVISION_INVALID,
                    ROOTFS_WORK_STAGE_PROVISION_PLAN, 0,
                    "a provisioned path must not end in '/'");
        return false;
    }
    cursor = 1;
    while (cursor < length) {
        size_t start = cursor;
        size_t span;

        while (cursor < length && path[cursor] != '/')
            cursor++;
        span = cursor - start;
        if (span == 0u || span > HFS_NAME_MAX_UNITS) {
            result_fail(result, ROOTFS_WORK_PROVISION_INVALID,
                        ROOTFS_WORK_STAGE_PROVISION_PLAN, 0,
                        "path component %zu is %zu bytes; 1..%u are allowed "
                        "and empty components are not", *count, span,
                        HFS_NAME_MAX_UNITS);
            return false;
        }
        if ((span == 1u && path[start] == '.') ||
            (span == 2u && path[start] == '.' && path[start + 1u] == '.')) {
            result_fail(result, ROOTFS_WORK_PROVISION_INVALID,
                        ROOTFS_WORK_STAGE_PROVISION_PLAN, 0,
                        "'.' and '..' are not provisionable components");
            return false;
        }
        for (span = start; span < cursor; span++) {
            unsigned char byte = (unsigned char)path[span];
            /* Printable ASCII only, and not ':': HFS+ stores the POSIX '/'
             * as ':', so a ':' here would surface to the guest as a slash. */
            if (byte < 0x20u || byte > 0x7eu || byte == ':') {
                result_fail(result, ROOTFS_WORK_PROVISION_INVALID,
                            ROOTFS_WORK_STAGE_PROVISION_PLAN, 0,
                            "byte 0x%02x in a path component is not "
                            "provisionable", byte);
                return false;
            }
        }
        if (*count >= ROOTFS_WORK_MAX_PATH_DEPTH) {
            result_fail(result, ROOTFS_WORK_PROVISION_LIMIT,
                        ROOTFS_WORK_STAGE_PROVISION_PLAN, 0,
                        "provisioned paths are limited to %u components",
                        ROOTFS_WORK_MAX_PATH_DEPTH);
            return false;
        }
        components[*count].start = (uint16_t)start;
        components[*count].length = (uint16_t)(cursor - start);
        (*count)++;
        if (cursor < length)
            cursor++;
    }
    if (*count == 0u) {
        result_fail(result, ROOTFS_WORK_PROVISION_INVALID,
                    ROOTFS_WORK_STAGE_PROVISION_PLAN, 0,
                    "\"/\" names no object to create");
        return false;
    }
    return true;
}

static void rootfs_component_units(const char *path,
                                   const rootfs_path_component_t *component,
                                   uint16_t *units) {
    uint16_t index;

    for (index = 0; index < component->length; index++)
        units[index] = (uint16_t)(unsigned char)path[component->start + index];
}

static bool provision_one(catalog_ctx_t *ctx,
                          const rootfs_work_entry_t *entry,
                          catalog_content_t *content,
                          rootfs_work_result_t *result) {
    const rootfs_work_stage_t stage = ROOTFS_WORK_STAGE_PROVISION_PLAN;
    rootfs_path_component_t components[ROOTFS_WORK_MAX_PATH_DEPTH];
    uint16_t units[HFS_NAME_MAX_UNITS];
    catalog_folder_ref_t folder;
    catalog_folder_ref_t child;
    size_t count = 0;
    size_t index;
    uint32_t leaf = 0;
    uint32_t thread_leaf = 0;
    uint16_t position = 0;
    uint16_t thread_position = 0;
    bool found = false;
    uint32_t cnid;
    uint16_t leaf_units;
    uint16_t record_len;
    uint16_t mode;
    uint16_t parent_flags = 0;
    bool is_directory = entry->kind == ROOTFS_WORK_ENTRY_DIRECTORY;
    bool is_symlink = entry->kind == ROOTFS_WORK_ENTRY_SYMLINK;

    content->block_count = 0;
    content->start_block = 0;
    content->bytes = NULL;
    content->size = 0;
    if (entry->kind != ROOTFS_WORK_ENTRY_DIRECTORY &&
        entry->kind != ROOTFS_WORK_ENTRY_FILE &&
        entry->kind != ROOTFS_WORK_ENTRY_SYMLINK) {
        result_fail(result, ROOTFS_WORK_PROVISION_INVALID, stage, 0,
                    "entry kind %d is not a directory, a file or a symlink",
                    (int)entry->kind);
        return false;
    }
    if (is_directory && (entry->content != NULL || entry->content_size != 0u)) {
        result_fail(result, ROOTFS_WORK_PROVISION_INVALID, stage, 0,
                    "a directory entry cannot carry content");
        return false;
    }
    if (is_symlink) {
        size_t byte_index;

        /*
         * The target is the whole meaning of a symlink, so an empty or
         * unrepresentable one is refused rather than written.  A NUL would
         * silently truncate the link for the guest, and the bound is the same
         * one the path parser uses.  The 409 symlinks on the stock volume are
         * 1..69 bytes of printable ASCII, so nothing here narrows the payload.
         */
        if (entry->content_size == 0u || !entry->content) {
            result_fail(result, ROOTFS_WORK_PROVISION_INVALID, stage, 0,
                        "a symlink entry must carry a non-empty target");
            return false;
        }
        if (entry->content_size > ROOTFS_WORK_MAX_PATH) {
            result_fail(result, ROOTFS_WORK_PROVISION_LIMIT, stage, 0,
                        "a symlink target is %zu bytes; the cap is %u",
                        entry->content_size, ROOTFS_WORK_MAX_PATH);
            return false;
        }
        for (byte_index = 0; byte_index < entry->content_size; byte_index++) {
            unsigned char byte = entry->content[byte_index];

            if (byte < 0x20u || byte > 0x7eu) {
                result_fail(result, ROOTFS_WORK_PROVISION_INVALID, stage, 0,
                            "byte 0x%02x at offset %zu of a symlink target is "
                            "not provisionable", byte, byte_index);
                return false;
            }
        }
    }
    if (entry->content_size > ROOTFS_WORK_MAX_ENTRY_BYTES) {
        result_fail(result, ROOTFS_WORK_PROVISION_LIMIT, stage, 0,
                    "entry content is %zu bytes; the cap is %u",
                    entry->content_size, ROOTFS_WORK_MAX_ENTRY_BYTES);
        return false;
    }
    if (entry->content_size != 0u && !entry->content) {
        result_fail(result, ROOTFS_WORK_PROVISION_INVALID, stage, 0,
                    "entry declares %zu content bytes but has no buffer",
                    entry->content_size);
        return false;
    }
    if ((entry->permissions & (uint16_t)~(uint16_t)HFS_MODE_PERM_MASK) != 0u) {
        result_fail(result, ROOTFS_WORK_PROVISION_INVALID, stage, 0,
                    "permissions 0%o set bits outside the mode's low 12",
                    entry->permissions);
        return false;
    }
    /* 0755 for a symlink is what all 409 on the stock volume carry. */
    mode = entry->permissions != 0u ? entry->permissions :
           (uint16_t)((is_directory || is_symlink) ? 0755u : 0644u);
    if (!rootfs_path_split(entry->path, components, &count, result))
        return false;
    if (!catalog_root_folder(ctx, &folder, stage, result))
        return false;
    for (index = 0; index + 1u < count; index++) {
        rootfs_component_units(entry->path, &components[index], units);
        if (!catalog_child_folder(ctx, &folder, units,
                                  components[index].length, &child, stage,
                                  result))
            return false;
        folder = child;
    }
    if (!catalog_folder_flags(ctx, &folder, &parent_flags, stage, result))
        return false;

    leaf_units = components[count - 1u].length;
    rootfs_component_units(entry->path, &components[count - 1u], units);
    if (!catalog_search(ctx, folder.cnid, units, leaf_units, &leaf, &position,
                        &found, stage, result))
        return false;
    if (found) {
        result_fail(result, ROOTFS_WORK_PROVISION_EXISTS, stage, 0,
                    "an object already exists under CNID %u with that name",
                    folder.cnid);
        return false;
    }

    if (ctx->next_cnid < HFS_FIRST_USER_CNID || ctx->next_cnid == UINT32_MAX) {
        result_fail(result, ROOTFS_WORK_PROVISION_NO_SPACE, stage, 0,
                    "nextCatalogID %u leaves no allocatable CNID",
                    ctx->next_cnid);
        return false;
    }
    cnid = ctx->next_cnid;
    if (!catalog_search(ctx, cnid, NULL, 0u, &thread_leaf, &thread_position,
                        &found, stage, result))
        return false;
    if (found) {
        result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                    "CNID %u from nextCatalogID is already in use as a thread "
                    "key", cnid);
        return false;
    }

    if (!is_directory && entry->content_size != 0u) {
        uint64_t blocks = ((uint64_t)entry->content_size + ctx->block_size -
                           1u) / ctx->block_size;
        uint32_t start = 0;

        if (blocks > UINT32_MAX) {
            result_fail(result, ROOTFS_WORK_PROVISION_LIMIT, stage, 0,
                        "content needs more allocation blocks than HFS+ can "
                        "name");
            return false;
        }
        if (!catalog_bitmap_alloc(ctx, (uint32_t)blocks, &start, stage, result))
            return false;
        content->start_block = start;
        content->block_count = (uint32_t)blocks;
        content->bytes = entry->content;
        content->size = entry->content_size;
    }

    if (is_directory)
        record_len = catalog_build_folder(ctx, folder.cnid, units, leaf_units,
                                          cnid,
                                          (uint16_t)(parent_flags &
                                                     HFS_FLAG_HAS_FOLDER_COUNT),
                                          entry->owner_id, entry->group_id,
                                          mode);
    else if (is_symlink)
        record_len = catalog_build_symlink(ctx, folder.cnid, units, leaf_units,
                                           cnid, entry->owner_id,
                                           entry->group_id, mode,
                                           (uint64_t)entry->content_size,
                                           content->start_block,
                                           content->block_count);
    else
        record_len = catalog_build_file(ctx, folder.cnid, units, leaf_units,
                                        cnid, entry->owner_id, entry->group_id,
                                        mode, (uint64_t)entry->content_size,
                                        content->start_block,
                                        content->block_count);
    if (!catalog_leaf_insert(ctx, leaf, position, ctx->build, record_len,
                             stage, result))
        return false;

    record_len = catalog_build_thread(ctx, cnid,
                                      (uint16_t)(is_directory ?
                                          HFS_CAT_FOLDER_THREAD :
                                          HFS_CAT_FILE_THREAD),
                                      folder.cnid, units, leaf_units);
    /* The first insert may have moved the thread's leaf position, so resolve
     * it again rather than reusing the pre-insert answer. */
    if (!catalog_search(ctx, cnid, NULL, 0u, &thread_leaf, &thread_position,
                        &found, stage, result))
        return false;
    if (found) {
        result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                    "thread key for CNID %u appeared during the insert", cnid);
        return false;
    }
    if (!catalog_leaf_insert(ctx, thread_leaf, thread_position, ctx->build,
                             record_len, stage, result))
        return false;

    if (!catalog_touch_folder(ctx, &folder, is_directory, stage, result))
        return false;
    ctx->next_cnid = cnid + 1u;
    if (is_directory) {
        if (ctx->folder_count == UINT32_MAX) {
            result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                        "volume folderCount is saturated");
            return false;
        }
        ctx->folder_count++;
    } else {
        if (ctx->file_count == UINT32_MAX) {
            result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                        "volume fileCount is saturated");
            return false;
        }
        ctx->file_count++;
    }
    if (result->provision_first_cnid == 0u)
        result->provision_first_cnid = cnid;
    result->provision_last_cnid = cnid;
    result->provision_entries++;
    result->provision_blocks += content->block_count;
    return true;
}

static bool catalog_alloc_fork_io(catalog_ctx_t *ctx, bool writing,
                                  rootfs_work_stage_t stage,
                                  rootfs_work_result_t *result) {
    uint64_t remaining = ctx->bitmap_bytes;
    size_t done = 0;
    unsigned extent;

    for (extent = 0; extent < 8u && remaining != 0u; extent++) {
        uint64_t span = (uint64_t)ctx->volume->ext_count[extent] *
                        ctx->block_size;
        uint64_t physical = (uint64_t)ctx->volume->ext_start[extent] *
                            ctx->block_size;
        size_t amount;

        if (span == 0u)
            continue;
        amount = span < remaining ? (size_t)span : (size_t)remaining;
        if (writing) {
            if (!checked_write(ctx->file, ctx->file_size, physical,
                               ctx->bitmap + done, amount, stage, result))
                return false;
        } else {
            if (!checked_read(ctx->file, ctx->file_size, physical,
                              ctx->bitmap + done, amount, stage, result))
                return false;
        }
        done += amount;
        remaining -= amount;
    }
    if (remaining != 0u) {
        result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                    "the allocation fork does not cover its own bitmap");
        return false;
    }
    return true;
}

/*
 * Commit order is deliberate: data first, then the bitmap that claims those
 * blocks, then the catalog records that name them, then the volume header.  A
 * commit torn anywhere leaks blocks at worst; at no point does a record refer
 * to a block the bitmap still calls free.
 */
static bool catalog_commit(catalog_ctx_t *ctx,
                           const catalog_content_t *contents, size_t count,
                           uint8_t *buffer, size_t buffer_size,
                           rootfs_work_result_t *result) {
    const rootfs_work_stage_t stage = ROOTFS_WORK_STAGE_PROVISION_WRITE;
    size_t index;
    size_t slot;

    for (index = 0; index < count; index++) {
        uint64_t base = (uint64_t)contents[index].start_block *
                        ctx->block_size;
        uint64_t span = (uint64_t)contents[index].block_count *
                        ctx->block_size;
        uint64_t written = contents[index].size;

        if (contents[index].block_count == 0u)
            continue;
        if (!checked_write(ctx->file, ctx->file_size, base,
                           contents[index].bytes, contents[index].size, stage,
                           result))
            return false;
        /* Zero the slack so the tail of the last block never publishes
         * whatever the grown image happened to contain. */
        memset(buffer, 0, buffer_size);
        while (written < span) {
            uint64_t chunk = span - written;
            size_t amount = chunk > buffer_size ? buffer_size : (size_t)chunk;

            if (!checked_write(ctx->file, ctx->file_size, base + written,
                               buffer, amount, stage, result))
                return false;
            written += amount;
        }
    }
    if (ctx->bitmap_dirty &&
        !catalog_alloc_fork_io(ctx, true, stage, result))
        return false;
    for (slot = 0; slot < ctx->node_count; slot++) {
        uint64_t offset = 0;

        if (!ctx->nodes[slot].dirty)
            continue;
        if (!catalog_node_offset(ctx, ctx->nodes[slot].index, &offset, stage,
                                 result))
            return false;
        if (!checked_write(ctx->file, ctx->file_size, offset,
                           ctx->nodes[slot].bytes, ctx->node_size, stage,
                           result))
            return false;
    }
    {
        uint64_t offset = 0;

        if (!catalog_node_read_raw(ctx, 0u, ctx->scratch, stage, result))
            return false;
        if (ctx->scratch[8] != HFS_BT_HEADER_NODE) {
            result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                        "catalog node 0 stopped being a header node");
            return false;
        }
        write_be32(ctx->scratch + HFS_NODE_DESCRIPTOR + 6, ctx->leaf_records);
        /*
         * lastLeafNode and freeNodes only move when a rightmost split ran, and
         * the map only when a node was claimed; writing them unconditionally
         * from the values open() read is the identity otherwise.  Node 0 is
         * never in the node cache, so this is the single writer of the header
         * node and cannot race the dirty-node loop above.
         */
        write_be32(ctx->scratch + HFS_NODE_DESCRIPTOR + 14, ctx->last_leaf);
        write_be32(ctx->scratch + HFS_NODE_DESCRIPTOR + 26, ctx->free_nodes);
        if (ctx->map_dirty) {
            if (catalog_record_count(ctx->scratch) != HFS_BT_HEADER_RECORDS ||
                catalog_slot(ctx->scratch, ctx->node_size,
                             HFS_BT_MAP_RECORD) != ctx->map_offset ||
                catalog_slot(ctx->scratch, ctx->node_size,
                             (uint16_t)(HFS_BT_MAP_RECORD + 1u)) !=
                    (uint16_t)(ctx->map_offset + ctx->map_bytes)) {
                result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT,
                            stage, 0, "the header node's map record moved "
                            "between the plan and the commit");
                return false;
            }
            memcpy(ctx->scratch + ctx->map_offset, ctx->node_map,
                   ctx->map_bytes);
        }
        if (!catalog_node_offset(ctx, 0u, &offset, stage, result))
            return false;
        if (!checked_write(ctx->file, ctx->file_size, offset, ctx->scratch,
                           ctx->node_size, stage, result))
            return false;
    }
    if (ctx->vh_dirty) {
        write_be32(ctx->vh + 32, ctx->file_count);
        write_be32(ctx->vh + 36, ctx->folder_count);
        write_be32(ctx->vh + 48, ctx->free_blocks);
        write_be32(ctx->vh + 52, ctx->next_alloc);
        write_be32(ctx->vh + 64, ctx->next_cnid);
        if (!checked_write(ctx->file, ctx->file_size, HFS_VH_OFF, ctx->vh,
                           HFS_VH_LEN, stage, result) ||
            !checked_write(ctx->file, ctx->file_size,
                           ctx->file_size - HFS_VH_OFF, ctx->vh, HFS_VH_LEN,
                           stage, result))
            return false;
    }
    return true;
}

static void catalog_close(catalog_ctx_t *ctx) {
    size_t slot;

    if (ctx->nodes) {
        for (slot = 0; slot < ctx->node_count; slot++)
            free(ctx->nodes[slot].bytes);
        free(ctx->nodes);
    }
    free(ctx->bitmap);
    free(ctx->node_map);
    free(ctx->build);
    free(ctx->scratch);
    memset(ctx, 0, sizeof(*ctx));
}

/*
 * Cache the B-tree's node-allocation map out of the header node's third
 * record, and hold it to what the header itself claims: the header node and
 * the three nodes the header names must all be marked in use.  A map that does
 * not agree with the tree that owns it is a refusal, never a source of free
 * nodes.
 */
static bool catalog_node_map_open(catalog_ctx_t *ctx,
                                  rootfs_work_stage_t stage,
                                  rootfs_work_result_t *result) {
    static const char *const NAMED[] = {"the header node", "rootNode",
                                        "firstLeafNode", "lastLeafNode"};
    uint32_t named[4];
    uint16_t map_end;
    uint16_t limit;
    unsigned which;

    if (!catalog_node_read_raw(ctx, 0u, ctx->scratch, stage, result))
        return false;
    if (ctx->scratch[8] != HFS_BT_HEADER_NODE || ctx->scratch[9] != 0u ||
        catalog_record_count(ctx->scratch) != HFS_BT_HEADER_RECORDS) {
        result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                    "catalog node 0 is kind 0x%02x height %u with %u records, "
                    "not a %u-record B-tree header node", ctx->scratch[8],
                    ctx->scratch[9], catalog_record_count(ctx->scratch),
                    HFS_BT_HEADER_RECORDS);
        return false;
    }
    limit = (uint16_t)(ctx->node_size -
                       2u * ((uint32_t)HFS_BT_HEADER_RECORDS + 1u));
    ctx->map_offset = catalog_slot(ctx->scratch, ctx->node_size,
                                   HFS_BT_MAP_RECORD);
    map_end = catalog_slot(ctx->scratch, ctx->node_size,
                           (uint16_t)(HFS_BT_MAP_RECORD + 1u));
    if (ctx->map_offset < HFS_NODE_DESCRIPTOR || map_end <= ctx->map_offset ||
        map_end > limit) {
        result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                    "the header node's map record spans 0x%04x..0x%04x of a "
                    "%u-byte node", ctx->map_offset, map_end, ctx->node_size);
        return false;
    }
    ctx->map_bytes = (uint16_t)(map_end - ctx->map_offset);
    if ((uint64_t)ctx->map_bytes * 8u < (uint64_t)ctx->total_nodes) {
        result_fail(result, ROOTFS_WORK_PROVISION_UNSUPPORTED, stage, 0,
                    "the %u-byte node map covers %u of %u nodes, so the rest "
                    "is in chained map nodes this writer does not read",
                    ctx->map_bytes, (unsigned)((uint32_t)ctx->map_bytes * 8u),
                    ctx->total_nodes);
        return false;
    }
    ctx->node_map = (uint8_t *)malloc(ctx->map_bytes);
    if (!ctx->node_map) {
        result_fail(result, ROOTFS_WORK_NO_MEMORY, stage, 0,
                    "cannot cache the %u-byte catalog node map",
                    ctx->map_bytes);
        return false;
    }
    memcpy(ctx->node_map, ctx->scratch + ctx->map_offset, ctx->map_bytes);
    named[0] = 0u;
    named[1] = ctx->root_node;
    named[2] = ctx->first_leaf;
    named[3] = ctx->last_leaf;
    for (which = 0; which < 4u; which++) {
        size_t byte = (size_t)(named[which] >> 3);

        if ((ctx->node_map[byte] &
             (uint8_t)(1u << (7u - (named[which] & 7u)))) == 0u) {
            result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                        "the node map calls node %u free, but it is %s",
                        named[which], NAMED[which]);
            return false;
        }
    }
    return true;
}

static bool catalog_open(catalog_ctx_t *ctx, host_file_t *file,
                         uint64_t file_size, const hfs_volume_t *volume,
                         uint32_t mac_time, rootfs_work_result_t *result) {
    const rootfs_work_stage_t stage = ROOTFS_WORK_STAGE_PROVISION_PLAN;
    uint8_t header[HFS_NODE_DESCRIPTOR + 106u];
    uint64_t fork_blocks_seen = 0;
    uint32_t fork_blocks;
    uint32_t bt_attributes;
    unsigned extent;

    memset(ctx, 0, sizeof(*ctx));
    ctx->file = file;
    ctx->file_size = file_size;
    ctx->volume = volume;
    ctx->block_size = volume->block_size;
    ctx->total_blocks = volume->total_blocks;
    ctx->free_blocks = volume->free_blocks;
    ctx->next_alloc = volume->next_alloc;
    ctx->mac_time = mac_time;
    if (!checked_read(file, file_size, HFS_VH_OFF, ctx->vh, HFS_VH_LEN, stage,
                      result))
        return false;
    ctx->file_count = read_be32(ctx->vh + 32);
    ctx->folder_count = read_be32(ctx->vh + 36);
    ctx->next_cnid = read_be32(ctx->vh + 64);
    ctx->fork_bytes = read_be64(ctx->vh + 272);
    fork_blocks = read_be32(ctx->vh + 284);
    for (extent = 0; extent < 8u; extent++) {
        ctx->ext_start[extent] = read_be32(ctx->vh + 288 + extent * 8u);
        ctx->ext_count[extent] = read_be32(ctx->vh + 292 + extent * 8u);
        if (ctx->ext_count[extent] == 0u)
            continue;
        if ((uint64_t)ctx->ext_start[extent] + ctx->ext_count[extent] >
            ctx->total_blocks) {
            result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                        "catalog extent %u runs past the volume", extent);
            return false;
        }
        fork_blocks_seen += ctx->ext_count[extent];
    }
    if (fork_blocks_seen != fork_blocks || fork_blocks == 0u) {
        result_fail(result, ROOTFS_WORK_PROVISION_UNSUPPORTED, stage, 0,
                    "the catalog fork has an extents-overflow spill (%" PRIu64
                    " of %u blocks inline)", fork_blocks_seen, fork_blocks);
        return false;
    }
    if (ctx->fork_bytes == 0u ||
        ctx->fork_bytes > (uint64_t)fork_blocks * ctx->block_size) {
        result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                    "catalog logicalSize %" PRIu64 " does not fit its %u "
                    "blocks", ctx->fork_bytes, fork_blocks);
        return false;
    }
    /* Node 0 is at the fork's first byte no matter what nodeSize turns out
     * to be, so the descriptor and header record can be read before the
     * geometry is known. */
    if (!checked_read(file, file_size,
                      (uint64_t)ctx->ext_start[0] * ctx->block_size, header,
                      sizeof(header), stage, result))
        return false;
    if (header[8] != HFS_BT_HEADER_NODE) {
        result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                    "catalog node 0 is kind 0x%02x, not a B-tree header",
                    header[8]);
        return false;
    }
    ctx->tree_depth = read_be16(header + HFS_NODE_DESCRIPTOR);
    ctx->root_node = read_be32(header + HFS_NODE_DESCRIPTOR + 2);
    ctx->leaf_records = read_be32(header + HFS_NODE_DESCRIPTOR + 6);
    ctx->first_leaf = read_be32(header + HFS_NODE_DESCRIPTOR + 10);
    ctx->last_leaf = read_be32(header + HFS_NODE_DESCRIPTOR + 14);
    ctx->node_size = read_be16(header + HFS_NODE_DESCRIPTOR + 18);
    ctx->total_nodes = read_be32(header + HFS_NODE_DESCRIPTOR + 22);
    ctx->free_nodes = read_be32(header + HFS_NODE_DESCRIPTOR + 26);
    bt_attributes = read_be32(header + HFS_NODE_DESCRIPTOR + 38);
    if (ctx->node_size < 512u || ctx->node_size > 32768u ||
        (ctx->node_size & (uint16_t)(ctx->node_size - 1u)) != 0u) {
        result_fail(result, ROOTFS_WORK_PROVISION_UNSUPPORTED, stage, 0,
                    "catalog nodeSize %u is not a power of two in 512..32768",
                    ctx->node_size);
        return false;
    }
    if (header[HFS_NODE_DESCRIPTOR + 36] != 0u ||
        header[HFS_NODE_DESCRIPTOR + 37] != HFS_KEY_COMPARE_BINARY) {
        result_fail(result, ROOTFS_WORK_PROVISION_UNSUPPORTED, stage, 0,
                    "catalog btreeType %u keyCompareType 0x%02x; only HFSX "
                    "binary compare (0x%02x) is supported",
                    header[HFS_NODE_DESCRIPTOR + 36],
                    header[HFS_NODE_DESCRIPTOR + 37], HFS_KEY_COMPARE_BINARY);
        return false;
    }
    if ((bt_attributes & (HFS_BT_BIG_KEYS | HFS_BT_VARIABLE_INDEX_KEYS)) !=
        (HFS_BT_BIG_KEYS | HFS_BT_VARIABLE_INDEX_KEYS)) {
        result_fail(result, ROOTFS_WORK_PROVISION_UNSUPPORTED, stage, 0,
                    "catalog B-tree attributes 0x%08x lack big or variable "
                    "index keys", bt_attributes);
        return false;
    }
    if (ctx->total_nodes == 0u ||
        (uint64_t)ctx->total_nodes * ctx->node_size != ctx->fork_bytes) {
        result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                    "%u nodes of %u bytes is not the %" PRIu64 "-byte catalog",
                    ctx->total_nodes, ctx->node_size, ctx->fork_bytes);
        return false;
    }
    for (extent = 0; extent < 8u; extent++) {
        uint64_t span = (uint64_t)ctx->ext_count[extent] * ctx->block_size;
        if (span != 0u && span % ctx->node_size != 0u) {
            result_fail(result, ROOTFS_WORK_PROVISION_UNSUPPORTED, stage, 0,
                        "catalog extent %u is %" PRIu64 " bytes, not a whole "
                        "number of %u-byte nodes", extent, span,
                        ctx->node_size);
            return false;
        }
    }
    if (ctx->tree_depth == 0u || ctx->tree_depth > HFS_MAX_TREE_DEPTH ||
        ctx->root_node == 0u || ctx->root_node >= ctx->total_nodes ||
        ctx->first_leaf >= ctx->total_nodes ||
        ctx->last_leaf >= ctx->total_nodes ||
        ctx->free_nodes > ctx->total_nodes) {
        result_fail(result, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT, stage, 0,
                    "B-tree header depth %u root %u firstLeaf %u lastLeaf %u "
                    "freeNodes %u against %u nodes", ctx->tree_depth,
                    ctx->root_node, ctx->first_leaf, ctx->last_leaf,
                    ctx->free_nodes, ctx->total_nodes);
        return false;
    }
    if (volume->alloc_bytes > ROOTFS_WORK_MAX_BITMAP_BYTES) {
        result_fail(result, ROOTFS_WORK_PROVISION_LIMIT, stage, 0,
                    "the allocation bitmap is %" PRIu64 " bytes; the cap is %u",
                    volume->alloc_bytes, ROOTFS_WORK_MAX_BITMAP_BYTES);
        return false;
    }
    ctx->bitmap_bytes = (size_t)volume->alloc_bytes;
    ctx->bitmap = (uint8_t *)malloc(ctx->bitmap_bytes);
    ctx->build = (uint8_t *)malloc(HFS_MAX_RECORD_BYTES);
    ctx->scratch = (uint8_t *)malloc(ctx->node_size);
    ctx->nodes = (catalog_node_cache_t *)
        calloc(ROOTFS_WORK_MAX_CATALOG_NODES, sizeof(*ctx->nodes));
    if (!ctx->bitmap || !ctx->build || !ctx->scratch || !ctx->nodes) {
        result_fail(result, ROOTFS_WORK_NO_MEMORY, stage, 0,
                    "cannot allocate the catalog provisioning plan");
        return false;
    }
    if (!catalog_node_map_open(ctx, stage, result))
        return false;
    return catalog_alloc_fork_io(ctx, false, stage, result);
}

/*
 * Create the requested catalog objects in the already-grown work image.
 *
 * Nothing is written until every entry has been planned and applied to the
 * in-memory tree, so every refusal below leaves the image byte-identical.
 */
static bool provision_volume(host_file_t *file, uint64_t file_size,
                             const hfs_volume_t *volume,
                             const rootfs_work_options_t *options,
                             uint8_t *buffer, size_t buffer_size,
                             rootfs_work_result_t *result) {
    catalog_ctx_t ctx;
    catalog_content_t *contents = NULL;
    uint32_t before_records;
    size_t index;
    bool okay = false;

    memset(&ctx, 0, sizeof(ctx));
    if (options->entry_count > ROOTFS_WORK_MAX_ENTRIES) {
        result_fail(result, ROOTFS_WORK_PROVISION_LIMIT,
                    ROOTFS_WORK_STAGE_PROVISION_PLAN, 0,
                    "%zu entries requested; the cap is %u",
                    options->entry_count, ROOTFS_WORK_MAX_ENTRIES);
        return false;
    }
    if (!options->entries) {
        result_fail(result, ROOTFS_WORK_PROVISION_INVALID,
                    ROOTFS_WORK_STAGE_PROVISION_PLAN, 0,
                    "entry_count is %zu but entries is NULL",
                    options->entry_count);
        return false;
    }
    if (!catalog_open(&ctx, file, file_size, volume,
                      options->entry_mac_time != 0u ? options->entry_mac_time :
                          ROOTFS_WORK_DEFAULT_MAC_TIME,
                      result))
        goto done;
    before_records = ctx.leaf_records;
    /* Refuse a catalog whose header and content disagree BEFORE planning,
     * so a lookup can never answer "absent" out of a tree it cannot walk. */
    if (!catalog_audit(&ctx, before_records, ROOTFS_WORK_STAGE_PROVISION_PLAN,
                       result))
        goto done;
    contents = (catalog_content_t *)calloc(options->entry_count,
                                           sizeof(*contents));
    if (!contents) {
        result_fail(result, ROOTFS_WORK_NO_MEMORY,
                    ROOTFS_WORK_STAGE_PROVISION_PLAN, 0,
                    "cannot allocate %zu content placements",
                    options->entry_count);
        goto done;
    }
    for (index = 0; index < options->entry_count; index++) {
        if (!provision_one(&ctx, &options->entries[index], &contents[index],
                           result))
            goto done;
    }
    ctx.vh_dirty = true;
    /* Report the shape change before the commit can fail, so a caller reading
     * a failed result still learns that a split was required. */
    result->provision_leaf_splits = ctx.leaf_splits;
    result->provision_index_splits = ctx.index_splits;
    if (!catalog_commit(&ctx, contents, options->entry_count, buffer,
                        buffer_size, result))
        goto done;
    /* Read the tree back through the independent chain walk.  This shares no
     * state with the writer: if the commit produced anything the leaf chain
     * or the header cannot account for, it is caught here and the work image
     * is destroyed unpublished. */
    if (!catalog_audit(&ctx, ctx.leaf_records,
                       ROOTFS_WORK_STAGE_PROVISION_WRITE, result))
        goto done;
    okay = true;

done:
    free(contents);
    catalog_close(&ctx);
    return okay;
}

/*
 * PROVENANCE: byte-verified against lockdownd's own disassembly, not guessed.
 * See rootfs_work_activation_entries() in the header.
 */
static const char ACTIVATION_DATA_ARK[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "\t<key>-ActivationState</key>\n"
    "\t<string>FactoryActivated</string>\n"
    "\t<key>-BrickState</key>\n"
    "\t<false/>\n"
    "</dict>\n"
    "</plist>\n";

size_t rootfs_work_activation_entries(rootfs_work_entry_t *entries,
                                      size_t capacity) {
    if (entries && capacity >= 2u) {
        memset(entries, 0, 2u * sizeof(*entries));
        entries[0].kind = ROOTFS_WORK_ENTRY_DIRECTORY;
        entries[0].path = "/private/var/root/Library/Lockdown";
        entries[0].permissions = 0755u;
        entries[1].kind = ROOTFS_WORK_ENTRY_FILE;
        entries[1].path = "/private/var/root/Library/Lockdown/data_ark.plist";
        entries[1].content = (const uint8_t *)ACTIVATION_DATA_ARK;
        entries[1].content_size = sizeof(ACTIVATION_DATA_ARK) - 1u;
        entries[1].permissions = 0644u;
    }
    return 2u;
}

static bool copy_source(host_file_t *source, host_file_t *temporary,
                        uint64_t source_size, uint8_t *buffer,
                        size_t buffer_size,
                        ios3_sha256_context_t *source_sha256,
                        rootfs_work_result_t *result) {
    uint64_t offset = 0;

    while (offset < source_size) {
        uint64_t remaining = source_size - offset;
        size_t amount = remaining > buffer_size ? buffer_size :
                        (size_t)remaining;

        if (!checked_read(source, source_size, offset, buffer, amount,
                          ROOTFS_WORK_STAGE_COPY, result))
            return false;
        if (!ios3_sha256_update(source_sha256, buffer, amount)) {
            result_fail(result, ROOTFS_WORK_RANGE_ERROR,
                        ROOTFS_WORK_STAGE_SOURCE_IDENTITY, 0,
                        "source exceeds the SHA-256 cumulative length bound");
            return false;
        }
        if (!checked_write(temporary, source_size, offset, buffer, amount,
                           ROOTFS_WORK_STAGE_COPY, result))
            return false;
        offset += amount;
        result->bytes_copied = offset;
    }
    return true;
}

static void destination_init(destination_dir_t *destination) {
    memset(destination, 0, sizeof(*destination));
#ifdef _WIN32
    destination->handle = INVALID_HANDLE_VALUE;
#else
    destination->descriptor = -1;
    destination->temporary_leaf[0] = '\0';
#endif
}

static void destination_release(destination_dir_t *destination) {
#ifdef _WIN32
    if (destination->handle != INVALID_HANDLE_VALUE)
        (void)CloseHandle(destination->handle);
    free(destination->full_path);
    free(destination->destination_path);
    free(destination->temporary_path);
#else
    if (destination->descriptor >= 0)
        (void)close(destination->descriptor);
    free(destination->leaf);
#endif
    destination_init(destination);
}

static void cleanup_unpublished(destination_dir_t *destination,
                                 bool temporary_created,
                                 rootfs_work_result_t *result) {
    if (!temporary_created || result->published)
        return;
#ifdef _WIN32
    if (destination->temporary_path &&
        !DeleteFileA(destination->temporary_path)) {
        if (result->cleanup_system_error == 0)
            result->cleanup_system_error = windows_error();
        result->temporary_left = true;
    }
#else
    if (destination->descriptor >= 0 &&
        destination->temporary_leaf[0] != '\0' &&
        unlinkat(destination->descriptor, destination->temporary_leaf, 0) != 0) {
        if (result->cleanup_system_error == 0)
            result->cleanup_system_error = errno;
        result->temporary_left = true;
    }
#endif
}

static bool publish(destination_dir_t *destination,
                    rootfs_work_result_t *result) {
#ifdef _WIN32
    int identity_error = 0;

    /* MoveFileEx is path-based.  Keep the final directory open without delete
     * sharing and revalidate its canonical handle identity immediately before
     * the move; this detects ordinary ancestor/reparse renames.  It is not
     * presented as a security boundary against a hostile same-user namespace
     * racer, which documented Win32 lacks an openat-style primitive to close. */
    if (!windows_handle_matches_path(destination->handle,
                                     destination->full_path,
                                     &identity_error)) {
        result_fail(result, ROOTFS_WORK_PATH_UNSAFE,
                    ROOTFS_WORK_STAGE_PUBLISH, identity_error,
                    "destination directory identity changed before publish");
        return false;
    }
    if (!MoveFileExA(destination->temporary_path,
                     destination->destination_path, MOVEFILE_WRITE_THROUGH)) {
        int error = windows_error();
        result_fail(result,
                    error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS ?
                        ROOTFS_WORK_DESTINATION_EXISTS :
                        ROOTFS_WORK_PUBLISH_FAILED,
                    ROOTFS_WORK_STAGE_PUBLISH, error,
                    "atomic no-replace publication failed");
        return false;
    }
    result->published = true;
    return true;
#else
    if (linkat(destination->descriptor, destination->temporary_leaf,
               destination->descriptor, destination->leaf, 0) != 0) {
        int error = errno;
        result_fail(result,
                    error == EEXIST ? ROOTFS_WORK_DESTINATION_EXISTS :
                                      ROOTFS_WORK_PUBLISH_FAILED,
                    ROOTFS_WORK_STAGE_PUBLISH, error,
                    "atomic no-replace publication failed");
        return false;
    }
    result->published = true;
    if (posix_fsync_bounded(destination->descriptor) != 0) {
        result->temporary_left = true;
        result_fail(result, ROOTFS_WORK_PUBLISH_DURABILITY_FAILED,
                    ROOTFS_WORK_STAGE_DIRECTORY_SYNC, errno,
                    "destination was published but directory sync failed");
        return false;
    }
    if (unlinkat(destination->descriptor, destination->temporary_leaf, 0) != 0) {
        result->temporary_left = true;
        result_fail(result, ROOTFS_WORK_PUBLISH_FAILED,
                    ROOTFS_WORK_STAGE_CLEANUP, errno,
                    "destination was published but temporary link removal failed");
        return false;
    }
    if (posix_fsync_bounded(destination->descriptor) != 0) {
        result_fail(result, ROOTFS_WORK_PUBLISH_DURABILITY_FAILED,
                    ROOTFS_WORK_STAGE_DIRECTORY_SYNC, errno,
                    "destination is durable but temporary-link removal sync failed");
        return false;
    }
    return true;
#endif
}

rootfs_work_status_t rootfs_work_create(const char *source_path,
                                        const char *destination_path,
                                        const rootfs_work_options_t *options,
                                        rootfs_work_result_t *result) {
    rootfs_work_options_t selected;
    host_file_t source;
    host_file_t temporary;
    destination_dir_t destination;
    file_stamp_t source_before;
    file_stamp_t source_after;
    file_stamp_t temporary_stamp;
    hfs_volume_t source_volume;
    hfs_volume_t copied_volume;
    hfs_volume_t grown_volume;
    hfs_volume_t final_volume;
    uint8_t *buffer = NULL;
    ios3_sha256_context_t source_sha256;
    uint64_t work_size = 0;
    bool temporary_created = false;
    bool success = false;
    int error = 0;

    if (!result)
        return ROOTFS_WORK_INVALID_ARGUMENT;
    result_reset(result);
    host_file_init(&source);
    host_file_init(&temporary);
    destination_init(&destination);
    memset(&selected, 0, sizeof(selected));
    if (options)
        selected = *options;
    if (!selected.fstab_line)
        selected.fstab_line = ROOTFS_WORK_DEFAULT_FSTAB;
    if (selected.io_buffer_bytes == 0u)
        selected.io_buffer_bytes = ROOTFS_WORK_MAX_IO_BUFFER;
    if (!source_path || source_path[0] == '\0' || !destination_path ||
        destination_path[0] == '\0' || selected.io_buffer_bytes == 0u ||
        selected.io_buffer_bytes > ROOTFS_WORK_MAX_IO_BUFFER) {
        return result_fail(result, ROOTFS_WORK_INVALID_ARGUMENT,
                           ROOTFS_WORK_STAGE_ARGUMENTS, 0,
                           "paths and a 1..%u-byte I/O buffer are required",
                           ROOTFS_WORK_MAX_IO_BUFFER);
    }
    result->io_buffer_bytes = selected.io_buffer_bytes;
    buffer = (uint8_t *)malloc(selected.io_buffer_bytes);
    if (!buffer)
        return result_fail(result, ROOTFS_WORK_NO_MEMORY,
                           ROOTFS_WORK_STAGE_ARGUMENTS, 0,
                           "cannot allocate %zu-byte bounded I/O buffer",
                           selected.io_buffer_bytes);

#ifdef _WIN32
    if (!windows_open_source(source_path, &source, &source_before, result))
        goto done;
    if (!windows_split_destination(destination_path, &destination, result))
        goto done;
#else
    if (!posix_open_source(source_path, &source, &source_before, result))
        goto done;
    if (!posix_open_destination(destination_path, &destination, result))
        goto done;
#endif
    result->source_size = source_before.size;
    if (source_before.size > IOS3_SHA256_MAX_INPUT_BYTES) {
        result_fail(result, ROOTFS_WORK_RANGE_ERROR,
                    ROOTFS_WORK_STAGE_SOURCE_IDENTITY, 0,
                    "source is too large for a bounded SHA-256 bit count");
        goto done;
    }
    if (selected.source_identity.required &&
        selected.source_identity.expected_size != source_before.size) {
        result_fail(result, ROOTFS_WORK_SOURCE_IDENTITY_MISMATCH,
                    ROOTFS_WORK_STAGE_SOURCE_IDENTITY, 0,
                    "source size does not match the required identity");
        goto done;
    }
    if (!hfs_validate(&source, source_before.size, &source_volume, buffer,
                      selected.io_buffer_bytes,
                      ROOTFS_WORK_STAGE_SOURCE_VALIDATE, result))
        goto done;
    if (!destination_temp_create(&destination, &temporary,
                                 &temporary_created, result))
        goto done;
    if (!host_file_resize(&temporary, source_before.size, &error)) {
        result_fail(result, ROOTFS_WORK_WRITE_FAILED,
                    ROOTFS_WORK_STAGE_COPY, error,
                    "cannot size temporary image for source copy");
        goto done;
    }
    if (!ios3_sha256_init(&source_sha256)) {
        result_fail(result, ROOTFS_WORK_RANGE_ERROR,
                    ROOTFS_WORK_STAGE_SOURCE_IDENTITY, 0,
                    "cannot initialize source SHA-256 state");
        goto done;
    }
    if (!copy_source(&source, &temporary, source_before.size, buffer,
                     selected.io_buffer_bytes, &source_sha256, result))
        goto done;
    if (!host_file_stamp(&source, &source_after, &error)) {
        result_fail(result, ROOTFS_WORK_SOURCE_CHANGED,
                    ROOTFS_WORK_STAGE_COPY_VERIFY, error,
                    "cannot revalidate source identity after copy");
        goto done;
    }
    if (!stamp_equal(&source_before, &source_after)) {
        result_fail(result, ROOTFS_WORK_SOURCE_CHANGED,
                    ROOTFS_WORK_STAGE_COPY_VERIFY, 0,
                    "source identity, size, links, or timestamps changed during copy");
        goto done;
    }
    if (!ios3_sha256_final(&source_sha256, result->source_sha256)) {
        result_fail(result, ROOTFS_WORK_RANGE_ERROR,
                    ROOTFS_WORK_STAGE_SOURCE_IDENTITY, 0,
                    "cannot finalize the bounded source SHA-256 digest");
        goto done;
    }
    result->source_sha256_valid = true;
    if (selected.source_identity.required) {
        if (memcmp(result->source_sha256,
                   selected.source_identity.expected_sha256,
                   IOS3_SHA256_DIGEST_SIZE) != 0) {
            result_fail(result, ROOTFS_WORK_SOURCE_IDENTITY_MISMATCH,
                        ROOTFS_WORK_STAGE_SOURCE_IDENTITY, 0,
                        "source SHA-256 does not match the required identity");
            goto done;
        }
        result->source_identity_verified = true;
    }
    if (!host_file_stamp(&temporary, &temporary_stamp, &error) ||
        temporary_stamp.size != source_before.size) {
        result_fail(result, ROOTFS_WORK_SOURCE_CHANGED,
                    ROOTFS_WORK_STAGE_COPY_VERIFY, error,
                    "temporary copy size does not match the immutable source");
        goto done;
    }
    if (!hfs_validate(&temporary, source_before.size, &copied_volume, buffer,
                      selected.io_buffer_bytes,
                      ROOTFS_WORK_STAGE_COPY_VERIFY, result))
        goto done;
    if (memcmp(&source_volume, &copied_volume, sizeof(source_volume)) != 0) {
        result_fail(result, ROOTFS_WORK_SOURCE_CHANGED,
                    ROOTFS_WORK_STAGE_COPY_VERIFY, 0,
                    "copied HFS metadata does not match source metadata");
        goto done;
    }
    if (!host_file_close(&source, &error)) {
        result_fail(result, ROOTFS_WORK_SOURCE_CHANGED,
                    ROOTFS_WORK_STAGE_COPY_VERIFY, error,
                    "source close failed after immutable copy");
        goto done;
    }
    work_size = source_before.size;
    if (!fstab_rewrite(&temporary, work_size, selected.fstab_line, buffer,
                       selected.io_buffer_bytes, result))
        goto done;
    /* Opt-in and off by default: a stock image is left with Apple's own
     * renderer selection, so the flag is the only thing that changes it. */
    if (selected.ca_software_render &&
        !ca_plist_rewrite(&temporary, work_size, buffer,
                          selected.io_buffer_bytes, result))
        goto done;
    /* Also opt-in and off by default, and for a stronger reason than the
     * renderer flag: this one changes what the guest RUNS.  A stock image gets
     * a stock set of LaunchDaemons. */
    if (selected.ppp_launchd_job &&
        !ppp_plist_rewrite(&temporary, work_size, buffer,
                           selected.io_buffer_bytes, result))
        goto done;
    if (!grow_volume(&temporary, &work_size, selected.growth_bytes,
                     &copied_volume, buffer, selected.io_buffer_bytes, result))
        goto done;
    /*
     * Catalog provisioning is placed HERE, after growth, and re-validates the
     * volume from the image rather than reusing copied_volume.  That is the
     * ordering guarantee: the provisioner cannot see the pre-growth freeBlocks
     * even if a caller sets the options in a different order, and the stock
     * volume's freeBlocks = 0 becomes a measured ROOTFS_WORK_PROVISION_NO_SPACE
     * refusal rather than an assumption about what the caller did.
     */
    if (selected.entry_count != 0u) {
        if (!hfs_validate(&temporary, work_size, &grown_volume, buffer,
                          selected.io_buffer_bytes,
                          ROOTFS_WORK_STAGE_PROVISION_PLAN, result))
            goto done;
        if (!provision_volume(&temporary, work_size, &grown_volume, &selected,
                              buffer, selected.io_buffer_bytes, result))
            goto done;
    }
    if (!hfs_validate(&temporary, work_size, &final_volume, buffer,
                      selected.io_buffer_bytes,
                      ROOTFS_WORK_STAGE_FINAL_VALIDATE, result))
        goto done;
    if (selected.growth_bytes != 0u &&
        final_volume.total_blocks <= copied_volume.total_blocks) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID,
                    ROOTFS_WORK_STAGE_FINAL_VALIDATE, 0,
                    "final validation did not observe planned volume growth");
        goto done;
    }
    result->final_size = work_size;
    if (!host_file_sync(&temporary, &error)) {
        result_fail(result, ROOTFS_WORK_SYNC_FAILED,
                    ROOTFS_WORK_STAGE_FLUSH, error,
                    "cannot flush completed temporary work image");
        goto done;
    }
    if (!host_file_close(&temporary, &error)) {
        result_fail(result, ROOTFS_WORK_SYNC_FAILED,
                    ROOTFS_WORK_STAGE_FLUSH, error,
                    "cannot close flushed temporary work image");
        goto done;
    }
    if (!publish(&destination, result))
        goto done;
    result->status = ROOTFS_WORK_OK;
    result->stage = ROOTFS_WORK_STAGE_NONE;
    result->system_error = 0;
    (void)snprintf(result->detail, sizeof(result->detail),
                   "published %" PRIu64 "-byte validated rootfs work image",
                   work_size);
    success = true;

done:
    if (host_file_is_open(&source) &&
        !host_file_close(&source, &error) &&
        result->cleanup_system_error == 0)
        result->cleanup_system_error = error;
    if (host_file_is_open(&temporary) &&
        !host_file_close(&temporary, &error)) {
        if (result->cleanup_system_error == 0)
            result->cleanup_system_error = error;
        if (temporary_created)
            result->temporary_left = true;
    }
    if (!success)
        cleanup_unpublished(&destination, temporary_created, result);
    destination_release(&destination);
    free(buffer);
    return result->status;
}
