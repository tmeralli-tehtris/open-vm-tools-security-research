/*********************************************************
 * Copyright (C) 1998 VMware, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation version 2.1 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the Lesser GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA.
 *
 *********************************************************/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <sys/utsname.h>
#include <netdb.h>
#include <fcntl.h>
#include <limits.h>
#include <errno.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/time.h>
#include <pwd.h>
#include <pthread.h>
#include <sys/resource.h>
#if defined(sun)
#include <sys/systeminfo.h>
#endif
#include <sys/socket.h>
#if defined(__FreeBSD__) || defined(__APPLE__)
# include <sys/sysctl.h>
#endif
#if !defined(__APPLE__)
#define TARGET_OS_IPHONE 0
#endif
#if defined(__APPLE__)
#include <assert.h>
#include <TargetConditionals.h>
#if !TARGET_OS_IPHONE
#include <CoreServices/CoreServices.h>
#endif
#include <mach-o/dyld.h>
#include <mach/mach_host.h>
#include <mach/mach_init.h>
#include <mach/mach_time.h>
#include <mach/vm_map.h>
#include <sys/mman.h>
#include <AvailabilityMacros.h>
#if MAC_OS_X_VERSION_MIN_REQUIRED < 1070 // Must run on Mac OS versions < 10.7
#   include <dlfcn.h>
#endif
#elif defined(__FreeBSD__)
#if !defined(RLIMIT_AS)
#  if defined(RLIMIT_VMEM)
#     define RLIMIT_AS RLIMIT_VMEM
#  else
#     define RLIMIT_AS RLIMIT_RSS
#  endif
#endif
#else
#if !defined(USING_AUTOCONF) || defined(HAVE_SYS_VFS_H)
#include <sys/vfs.h>
#endif
#if !defined(sun) && !defined __ANDROID__ && (!defined(USING_AUTOCONF) || (defined(HAVE_SYS_IO_H) && defined(HAVE_SYS_SYSINFO_H)))
#include <sys/io.h>
#include <sys/sysinfo.h>
#ifndef HAVE_SYSINFO
#define HAVE_SYSINFO 1
#endif
#endif
#endif

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <paths.h>
#endif

#ifdef __linux__
#include <dlfcn.h>
#endif

#if !defined(_PATH_DEVNULL)
#define _PATH_DEVNULL "/dev/null"
#endif

#include "vmware.h"
#include "hostType.h"
#include "hostinfo.h"
#include "hostinfoInt.h"
#include "safetime.h"
#include "str.h"
#include "err.h"
#include "msg.h"
#include "log.h"
#include "posix.h"
#include "file.h"
#include "backdoor_def.h"
#include "util.h"
#include "vmstdio.h"
#include "su.h"
#include "vm_atomic.h"

#if defined(__i386__) || defined(__x86_64__)
#include "x86cpuid.h"
#endif

#include "unicode.h"
#include "guest_os.h"
#include "dynbuf.h"
#include "strutil.h"

#if defined(VMX86_SERVER)
#include "uwvmkAPI.h"
#include "uwvmk.h"
#include "vmkSyscall.h"
#endif

#define LGPFX "HOSTINFO:"
#define MAX_LINE_LEN 128

#define SYSTEM_BITNESS_32 "i386"
#define SYSTEM_BITNESS_64_SUN "amd64"
#define SYSTEM_BITNESS_64_LINUX "x86_64"
#define SYSTEM_BITNESS_MAXLEN \
   MAX(sizeof SYSTEM_BITNESS_32, \
   MAX(sizeof SYSTEM_BITNESS_64_SUN, \
       sizeof SYSTEM_BITNESS_64_LINUX))

struct hostinfoOSVersion {
   int hostinfoOSVersion[4];
   char *hostinfoOSVersionString;
};

static Atomic_Ptr hostinfoOSVersion;

#define DISTRO_BUF_SIZE 255

typedef struct lsb_distro_info {
   char *name;
   char *scanstring;
} LSBDistroInfo;


static const LSBDistroInfo lsbFields[] = {
   {"DISTRIB_ID=",          "DISTRIB_ID=%s"},
   {"DISTRIB_RELEASE=",     "DISTRIB_RELEASE=%s"},
   {"DISTRIB_CODENAME=",    "DISTRIB_CODENAME=%s"},
   {"DISTRIB_DESCRIPTION=", "DISTRIB_DESCRIPTION=%s"},
   {NULL, NULL},
};


typedef struct distro_info {
   char *name;
   char *filename;
} DistroInfo;

static const DistroInfo distroArray[] = {
   {"OracleLinux",        "/etc/oracle-release"},
   {"RedHat",             "/etc/redhat-release"},
   {"RedHat",             "/etc/redhat_version"},
   {"Sun",                "/etc/sun-release"},
   {"SuSE",               "/etc/SuSE-release"},
   {"SuSE",               "/etc/novell-release"},
   {"SuSE",               "/etc/sles-release"},
   {"Debian",             "/etc/debian_version"},
   {"Debian",             "/etc/debian_release"},
   {"Mandrake",           "/etc/mandrake-release"},
   {"Mandriva",           "/etc/mandriva-release"},
   {"Mandrake",           "/etc/mandrakelinux-release"},
   {"TurboLinux",         "/etc/turbolinux-release"},
   {"Fedora Core",        "/etc/fedora-release"},
   {"Gentoo",             "/etc/gentoo-release"},
   {"Novell",             "/etc/nld-release"},
   {"Ubuntu",             "/etc/lsb-release"},
   {"Annvix",             "/etc/annvix-release"},
   {"Arch",               "/etc/arch-release"},
   {"Arklinux",           "/etc/arklinux-release"},
   {"Aurox",              "/etc/aurox-release"},
   {"BlackCat",           "/etc/blackcat-release"},
   {"Cobalt",             "/etc/cobalt-release"},
   {"Conectiva",          "/etc/conectiva-release"},
   {"Immunix",            "/etc/immunix-release"},
   {"Knoppix",            "/etc/knoppix_version"},
   {"Linux-From-Scratch", "/etc/lfs-release"},
   {"Linux-PPC",          "/etc/linuxppc-release"},
   {"MkLinux",            "/etc/mklinux-release"},
   {"PLD",                "/etc/pld-release"},
   {"Slackware",          "/etc/slackware-version"},
   {"Slackware",          "/etc/slackware-release"},
   {"SMEServer",          "/etc/e-smith-release"},
   {"Solaris",            "/etc/release"},
   {"Tiny Sofa",          "/etc/tinysofa-release"},
   {"UltraPenguin",       "/etc/ultrapenguin-release"},
   {"UnitedLinux",        "/etc/UnitedLinux-release"},
   {"VALinux",            "/etc/va-release"},
   {"Yellow Dog",         "/etc/yellowdog-release"},
   {NULL, NULL},
};

#if defined __ANDROID__
/*
 * Android doesn't support getloadavg() or iopl().
 */
#define NO_GETLOADAVG
#define NO_IOPL
#endif


/*
 *----------------------------------------------------------------------
 *
 * HostinfoOSVersionInit --
 *
 *      Compute the OS version information
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      hostinfoOS* variables are filled in.
 *
 *----------------------------------------------------------------------
 */

static void
HostinfoOSVersionInit(void)
{
   struct hostinfoOSVersion *version;
   struct utsname u;
   char *extra;
   char *p;

   if (Atomic_ReadPtr(&hostinfoOSVersion)) {
      return;
   }

   if (uname(&u) < 0) {
      Warning("%s: unable to get host OS version (uname): %s\n",
	      __FUNCTION__, Err_Errno2String(errno));
      NOT_IMPLEMENTED();
   }

   version = Util_SafeCalloc(1, sizeof *version);
   version->hostinfoOSVersionString = Util_SafeStrndup(u.release, 
                                                       sizeof u.release);

   ASSERT(ARRAYSIZE(version->hostinfoOSVersion) >= 4);

   /*
    * The first three numbers are separated by '.', if there is 
    * a fourth number, it's probably separated by '.' or '-',
    * but it could be preceded by anything.
    */

   extra = Util_SafeCalloc(1, sizeof u.release);
   if (sscanf(u.release, "%d.%d.%d%s",
	      &version->hostinfoOSVersion[0], &version->hostinfoOSVersion[1],
	      &version->hostinfoOSVersion[2], extra) < 1) {
      Warning("%s: unable to parse host OS version string: %s\n",
              __FUNCTION__, u.release);
      NOT_IMPLEMENTED();
   }

   /*
    * If there is a 4th number, use it, otherwise use 0.
    * Explicitly skip over any non-digits, including '-'
    */

   p = extra;
   while (*p && !isdigit(*p)) {
      p++;
   }
   sscanf(p, "%d", &version->hostinfoOSVersion[3]);
   free(extra);

   if (Atomic_ReadIfEqualWritePtr(&hostinfoOSVersion, NULL, version)) {
      free(version->hostinfoOSVersionString);
      free(version);
   }
}


/*
 *----------------------------------------------------------------------
 *
 * Hostinfo_OSVersionString --
 *
 *	Returns the host version information as returned in the
 *      release field of uname(2)
 *
 * Results:
 *	const char * - pointer to static buffer containing the release
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */

const char *
Hostinfo_OSVersionString(void)
{
   struct hostinfoOSVersion *version;

   HostinfoOSVersionInit();

   version = Atomic_ReadPtr(&hostinfoOSVersion);

   return version->hostinfoOSVersionString;
}


/*
 *----------------------------------------------------------------------
 *
 * Hostinfo_OSVersion --
 *
 *      Host OS release info.
 *
 * Results:
 *      The i-th component of a dotted release string.
 *	0 if i is greater than the number of components we support.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Hostinfo_OSVersion(unsigned int i)  // IN:
{
   struct hostinfoOSVersion *version;

   HostinfoOSVersionInit();

   version = Atomic_ReadPtr(&hostinfoOSVersion);

   return (i < ARRAYSIZE(version->hostinfoOSVersion)) ?
           version->hostinfoOSVersion[i] : 0;
}


/*
 *----------------------------------------------------------------------
 *
 * Hostinfo_GetTimeOfDay --
 *
 *      Return the current time of day according to the host.  We want
 *      UTC time (seconds since Jan 1, 1970).
 *
 * Results:
 *      Time of day in microseconds.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
Hostinfo_GetTimeOfDay(VmTimeType *time)  // OUT:
{
   struct timeval tv;

   gettimeofday(&tv, NULL);

   *time = ((int64)tv.tv_sec * 1000000) + tv.tv_usec;
}


/*
 *----------------------------------------------------------------------------
 *
 * Hostinfo_GetSystemBitness --
 *
 *      Determines the operating system's bitness.
 *
 * Return value:
 *      32 or 64 on success.
 *      -1 on failure. Check errno for more details of error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------------
 */

int
Hostinfo_GetSystemBitness(void)
{
#if defined __linux__
   struct utsname u;

   if (uname(&u) < 0) {
      return -1;
   }

   if (strstr(u.machine, SYSTEM_BITNESS_64_LINUX)) {
      return 64;
   } else {
      return 32;
   }
#else
   char buf[SYSTEM_BITNESS_MAXLEN] = { '\0', };
#   if defined __FreeBSD__ || defined __APPLE__
   static int mib[2] = { CTL_HW, HW_MACHINE, };
   size_t len = sizeof buf;

   if (sysctl(mib, ARRAYSIZE(mib), buf, &len, NULL, 0) == -1) {
      return -1;
   }
#   elif defined sun
#      if !defined SOL10
   /*
    * XXX: This is bad.  We define SI_ARCHITECTURE_K to what it is on Solaris
    * 10 so that we can use a single guestd build for Solaris 9 and 10. In the
    * future we should have the Solaris 9 build just return 32 -- since it did
    * not support 64-bit x86 -- and let the Solaris 10 headers define
    * SI_ARCHITECTURE_K, then have the installer symlink to the correct binary.
    * For now, though, we'll share a single build for both versions.
    */
#         define SI_ARCHITECTURE_K  518
#      endif

   if (sysinfo(SI_ARCHITECTURE_K, buf, sizeof buf) < 0) {
      return -1;
   }
#   endif

   if (strcmp(buf, SYSTEM_BITNESS_32) == 0) {
      return 32;
   } else if (strcmp(buf, SYSTEM_BITNESS_64_SUN) == 0 ||
              strcmp(buf, SYSTEM_BITNESS_64_LINUX) == 0) {
      return 64;
   }

   return -1;
#endif
}


#if !defined __APPLE__ && !defined USERWORLD
/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoGetOSShortName --
 *
 *      Returns distro information based on .vmx format (distroShort).
 *
 * Return value:
 *      Overwrited the short name if we recognise the OS.
 *      Otherwise leave the short name as it is.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static void
HostinfoGetOSShortName(char *distro,         // IN: full distro name
                       char *distroShort,    // OUT: short distro name
                       int distroShortSize)  // IN: size of short distro name

{
   char *distroLower = NULL;  /* Lower case distro name */

   distroLower = calloc(strlen(distro) + 1, sizeof *distroLower);

   if (distroLower == NULL) {
      Warning("%s: could not allocate memory\n", __FUNCTION__);

      return;
   }

   Str_Strcpy(distroLower, distro, distroShortSize);
   distroLower = Str_ToLower(distroLower);

   if (strstr(distroLower, "red hat")) {
      if (strstr(distroLower, "enterprise")) {

         /*
          * Looking for "release x" here instead of "x" as there could be
          * build version which can be misleading. For example Red Hat
          * Enterprise Linux ES release 4 (Nahant Update 3)
          */

         int release = 0;
         char *releaseStart = strstr(distroLower, "release");

         if (releaseStart) {
            sscanf(releaseStart, "release %d", &release);
            if (release > 0) {
               snprintf(distroShort, distroShortSize, STR_OS_RED_HAT_EN"%d",
                        release);
            }
         }

         if (release <= 0) {
            Warning("%s: could not read Red Hat Enterprise release version\n",
                  __FUNCTION__);
            Str_Strcpy(distroShort, STR_OS_RED_HAT_EN, distroShortSize);
         }

      } else {
         Str_Strcpy(distroShort, STR_OS_RED_HAT, distroShortSize);
      }
   } else if (strstr(distroLower, "opensuse")) {
      Str_Strcpy(distroShort, STR_OS_OPENSUSE, distroShortSize);
   } else if (strstr(distroLower, "suse")) {
      if (strstr(distroLower, "enterprise")) {
         if (strstr(distroLower, "server 12") ||
             strstr(distroLower, "desktop 12")) {
            Str_Strcpy(distroShort, STR_OS_SLES_12, distroShortSize);
         } else if (strstr(distroLower, "server 11") ||
                    strstr(distroLower, "desktop 11")) {
            Str_Strcpy(distroShort, STR_OS_SLES_11, distroShortSize);
         } else if (strstr(distroLower, "server 10") ||
                    strstr(distroLower, "desktop 10")) {
            Str_Strcpy(distroShort, STR_OS_SLES_10, distroShortSize);
         } else {
            Str_Strcpy(distroShort, STR_OS_SLES, distroShortSize);
         }
      } else if (strstr(distroLower, "sun")) {
         Str_Strcpy(distroShort, STR_OS_SUN_DESK, distroShortSize);
      } else if (strstr(distroLower, "novell")) {
         Str_Strcpy(distroShort, STR_OS_NOVELL, distroShortSize);
      } else {
         Str_Strcpy(distroShort, STR_OS_SUSE, distroShortSize);
      }
   } else if (strstr(distroLower, "mandrake")) {
      Str_Strcpy(distroShort, STR_OS_MANDRAKE, distroShortSize);
   } else if (strstr(distroLower, "turbolinux")) {
      Str_Strcpy(distroShort, STR_OS_TURBO, distroShortSize);
   } else if (strstr(distroLower, "sun")) {
      Str_Strcpy(distroShort, STR_OS_SUN_DESK, distroShortSize);
   } else if (strstr(distroLower, "annvix")) {
      Str_Strcpy(distroShort, STR_OS_ANNVIX, distroShortSize);
   } else if (strstr(distroLower, "arch")) {
      Str_Strcpy(distroShort, STR_OS_ARCH, distroShortSize);
   } else if (strstr(distroLower, "arklinux")) {
      Str_Strcpy(distroShort, STR_OS_ARKLINUX, distroShortSize);
   } else if (strstr(distroLower, "asianux server 3") ||
              strstr(distroLower, "asianux client 3")) {
      Str_Strcpy(distroShort, STR_OS_ASIANUX_3, distroShortSize);
   } else if (strstr(distroLower, "asianux server 4") ||
              strstr(distroLower, "asianux client 4")) {
      Str_Strcpy(distroShort, STR_OS_ASIANUX_4, distroShortSize);
   } else if (strstr(distroLower, "aurox")) {
      Str_Strcpy(distroShort, STR_OS_AUROX, distroShortSize);
   } else if (strstr(distroLower, "black cat")) {
      Str_Strcpy(distroShort, STR_OS_BLACKCAT, distroShortSize);
   } else if (strstr(distroLower, "cobalt")) {
      Str_Strcpy(distroShort, STR_OS_COBALT, distroShortSize);
   } else if (StrUtil_StartsWith(distroLower, "centos")) {
      Str_Strcpy(distroShort, STR_OS_CENTOS, distroShortSize);
   } else if (strstr(distroLower, "conectiva")) {
      Str_Strcpy(distroShort, STR_OS_CONECTIVA, distroShortSize);
   } else if (strstr(distroLower, "debian")) {
      if (strstr(distroLower, "4.0")) {
         Str_Strcpy(distroShort, STR_OS_DEBIAN_4, distroShortSize);
      } else if (strstr(distroLower, "5.0")) {
         Str_Strcpy(distroShort, STR_OS_DEBIAN_5, distroShortSize);
      } else if (strstr(distroLower, "6.0")) {
         Str_Strcpy(distroShort, STR_OS_DEBIAN_6, distroShortSize);
      } else if (strstr(distroLower, "7.0")) {
         Str_Strcpy(distroShort, STR_OS_DEBIAN_7, distroShortSize);
      } else if (strstr(distroLower, "7.1")) {
         Str_Strcpy(distroShort, STR_OS_DEBIAN_7_1, distroShortSize);
      }
   } else if (StrUtil_StartsWith(distroLower, "enterprise linux") ||
              StrUtil_StartsWith(distroLower, "oracle")) {
      /*
       * [root@localhost ~]# lsb_release -sd
       * "Enterprise Linux Enterprise Linux Server release 5.4 (Carthage)"
       *
       * Not sure why they didn't brand their releases as "Oracle Enterprise
       * Linux". Oh well. It's fixed in 6.0, though.
       */
      Str_Strcpy(distroShort, STR_OS_ORACLE, distroShortSize);
   } else if (strstr(distroLower, "fedora")) {
      Str_Strcpy(distroShort, STR_OS_FEDORA, distroShortSize);
   } else if (strstr(distroLower, "gentoo")) {
      Str_Strcpy(distroShort, STR_OS_GENTOO, distroShortSize);
   } else if (strstr(distroLower, "immunix")) {
      Str_Strcpy(distroShort, STR_OS_IMMUNIX, distroShortSize);
   } else if (strstr(distroLower, "linux-from-scratch")) {
      Str_Strcpy(distroShort, STR_OS_LINUX_FROM_SCRATCH, distroShortSize);
   } else if (strstr(distroLower, "linux-ppc")) {
      Str_Strcpy(distroShort, STR_OS_LINUX_PPC, distroShortSize);
   } else if (strstr(distroLower, "mandriva")) {
      Str_Strcpy(distroShort, STR_OS_MANDRIVA, distroShortSize);
   } else if (strstr(distroLower, "mklinux")) {
      Str_Strcpy(distroShort, STR_OS_MKLINUX, distroShortSize);
   } else if (strstr(distroLower, "pld")) {
      Str_Strcpy(distroShort, STR_OS_PLD, distroShortSize);
   } else if (strstr(distroLower, "slackware")) {
      Str_Strcpy(distroShort, STR_OS_SLACKWARE, distroShortSize);
   } else if (strstr(distroLower, "sme server")) {
      Str_Strcpy(distroShort, STR_OS_SMESERVER, distroShortSize);
   } else if (strstr(distroLower, "tiny sofa")) {
      Str_Strcpy(distroShort, STR_OS_TINYSOFA, distroShortSize);
   } else if (strstr(distroLower, "ubuntu")) {
      Str_Strcpy(distroShort, STR_OS_UBUNTU, distroShortSize);
   } else if (strstr(distroLower, "ultra penguin")) {
      Str_Strcpy(distroShort, STR_OS_ULTRAPENGUIN, distroShortSize);
   } else if (strstr(distroLower, "united linux")) {
      Str_Strcpy(distroShort, STR_OS_UNITEDLINUX, distroShortSize);
   } else if (strstr(distroLower, "va linux")) {
      Str_Strcpy(distroShort, STR_OS_VALINUX, distroShortSize);
   } else if (strstr(distroLower, "yellow dog")) {
      Str_Strcpy(distroShort, STR_OS_YELLOW_DOG, distroShortSize);
   }

   free(distroLower);
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoReadDistroFile --
 *
 *      Look for a distro version file /etc/xxx-release.
 *      Once found, read the file in and figure out which distribution.
 *
 * Return value:
 *      Returns TRUE on success and FALSE on failure.
 *      Returns distro information verbatium from /etc/xxx-release (distro).
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static Bool
HostinfoReadDistroFile(char *filename,  // IN: distro version file name
                       int distroSize,  // IN: size of OS distro name buffer
                       char *distro)    // OUT: full distro name
{
   int fd = -1;
   int buf_sz;
   struct stat st;
   Bool ret = FALSE;
   char *distroOrig = NULL;
   char distroPart[DISTRO_BUF_SIZE];
   char *tmpDistroPos = NULL;
   int i = 0;

   /* It's OK for the file to not exist, don't warn for this.  */
   if ((fd = Posix_Open(filename, O_RDONLY)) == -1) {
      return FALSE;
   }

   if (fstat(fd, &st)) {
      Warning("%s: could not stat the file %s: %d\n", __FUNCTION__, filename,
           errno);
      goto out;
   }

   if (st.st_size == 0) {
      Warning("%s: Cannot work with empty file.\n", __FUNCTION__);
      goto out;
   }

   buf_sz = st.st_size;
   if (buf_sz >= distroSize) {
      Warning("%s: input buffer too small\n", __FUNCTION__);
      goto out;
   }
   distroOrig = calloc(distroSize, sizeof *distroOrig);

   if (distroOrig == NULL) {
      Warning("%s: could not allocate memory\n", __FUNCTION__);
      goto out;
   }

   if (read(fd, distroOrig, buf_sz) != buf_sz) {
      Warning("%s: could not read file %s: %d\n", __FUNCTION__, filename,
              errno);
      goto out;
   }

   distroOrig[buf_sz - 1] = '\0';

   /*
    * For the case where we do have a release file in the LSB format,
    * but there is no LSB module, let's parse the LSB file for possible fields.
    */

   distro[0] = '\0';

   for (i = 0; lsbFields[i].name != NULL; i++) {
      tmpDistroPos = strstr(distroOrig, lsbFields[i].name);
      if (tmpDistroPos) {
         sscanf(tmpDistroPos, lsbFields[i].scanstring, distroPart);
         if (distroPart[0] == '"') {
            char *tmpMakeNull = NULL;

            tmpDistroPos += strlen(lsbFields[i].name) + 1;
            tmpMakeNull = strchr(tmpDistroPos + 1 , '"');
            if (tmpMakeNull) {
               *tmpMakeNull = '\0';
               Str_Strcat(distro, tmpDistroPos, distroSize);
               *tmpMakeNull = '"' ;
            }
         } else {
            Str_Strcat(distro, distroPart, distroSize);
         }
         Str_Strcat(distro, " ", distroSize);
      }
   }

   if (distro[0] == '\0') {
      /* Copy original string. What we got wasn't LSB compliant. */
      Str_Strcpy(distro, distroOrig, distroSize);
   }

   ret = TRUE;

out:
   if (fd != -1) {
      close(fd);
   }
   free(distroOrig);

   return ret;
}
#endif


//#ifndef USERWORLD
/*
 *----------------------------------------------------------------------
 *
 * HostinfoGetCmdOutput --
 *
 *      Run a cmd & get its cmd line output
 *
 * Results:
 *      An allocated string or NULL if an error occurred.
 *
 * Side effects:
 *	The cmd is run.
 *
 *----------------------------------------------------------------------

static char *
HostinfoGetCmdOutput(const char *cmd)  // IN:
{
   Bool isSuperUser = FALSE;
   DynBuf db;
   FILE *stream;
   char *out = NULL;

  //  * Attempt to lower privs, because we use popen and an attacker
  //  * may control $PATH.
   if (vmx86_linux && Id_IsSuperUser()) {
      Id_EndSuperUser(getuid());
      isSuperUser = TRUE;
   }

   DynBuf_Init(&db);

   stream = Posix_Popen(cmd, "r");
   if (stream == NULL) {
      Warning("Unable to get output of command \"%s\"\n", cmd);

      goto exit;
   }

   for (;;) {
      char *line = NULL;
      size_t size;

      switch (StdIO_ReadNextLine(stream, &line, 0, &size)) {
      case StdIO_Error:
         goto closeIt;
         break;

      case StdIO_EOF:
         break;

      case StdIO_Success:
         break;

      default:
         ASSERT_NOT_IMPLEMENTED(FALSE);
      }

      if (line == NULL) {
         break;
      }

      // size does -not- include the NUL terminator. 
      DynBuf_Append(&db, line, size + 1);
      free(line);
   }

   if (DynBuf_Get(&db)) {
      out = (char *) DynBuf_AllocGet(&db);
   }

 closeIt:
   pclose(stream);

 exit:
   DynBuf_Destroy(&db);

   if (isSuperUser) {
      Id_BeginSuperUser();
   }
   return out;
}
#endif
*/

/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoOSData --
 *
 *      Determine the OS short (.vmx format) and long names.
 *
 * Return value:
 *      Returns TRUE on success and FALSE on failure.
 *
 * Side effects:
 *      Cache values are set when returning TRUE.
 *
 *-----------------------------------------------------------------------------
 */

Bool
HostinfoOSData(void)
{
   struct utsname buf;
   unsigned int lastCharPos;
   char osName[MAX_OS_NAME_LEN];
   char osNameFull[MAX_OS_FULLNAME_LEN];
   static Atomic_uint32 mutex = {0};

   /*
    * Use uname to get complete OS information.
    */

   if (uname(&buf) < 0) {
      Warning("%s: uname failed %d\n", __FUNCTION__, errno);

      return FALSE;
   }


   if (strlen(buf.sysname) + strlen(buf.release) + 3 > sizeof osNameFull) {
      Warning("%s: Error: buffer too small\n", __FUNCTION__);

      return FALSE;
   }

   Str_Strcpy(osName, STR_OS_EMPTY, sizeof osName);
   Str_Sprintf(osNameFull, sizeof osNameFull, "%s %s", buf.sysname,
               buf.release);

#if defined USERWORLD
   if (buf.release[0] <= '4') {
      Str_Strcpy(osName, "vmkernel", sizeof osName);
   } else {
      Str_Strcpy(osName, "vmkernel5", sizeof osName);
   }
   Str_Snprintf(osNameFull, sizeof osNameFull, "VMware ESXi %c.x",
                buf.release[0]);
#elif defined __APPLE__
   {
      /*
       * The easiest way is to invoke "system_profiler" and hope that the
       * format of its unlocalized output will never change.
       *
       * Alternatively, we could do what system_profiler does and use the
       * CFPropertyList/CFDIctionary APIs to parse
       * /System/Library/CoreServices/{Server,System}Version.plist.
       *
       * On a MacBookPro4,1 (and possibly other models), invoking
       * "system_profiler" can take several seconds: it seems to spin up the CD
       * drive as a side-effect. So use "system_profiler SPSoftwareDataType"
       * instead.
       */
      char *sysname = HostinfoGetCmdOutput(
                         "/usr/sbin/system_profiler SPSoftwareDataType"
                      " | /usr/bin/grep 'System Version:'"
                      " | /usr/bin/cut -d : -f 2");

      if (sysname) {
         char *trimmed = Unicode_Trim(sysname);

         ASSERT_MEM_ALLOC(trimmed);
         free(sysname);
         Str_Snprintf(osNameFull, sizeof osNameFull, "%s", trimmed);
         free(trimmed);
      } else {
         Log("%s: Failed to get output of system_profiler.\n", __FUNCTION__);
         /* Fall back to returning the original osNameFull. */
      }

      Str_Snprintf(osName, sizeof osName, "%s%d", STR_OS_MACOS,
                   Hostinfo_OSVersion(0));
   }
#else
   // XXX Use compile-time instead of run-time checks for these as well.
   if (strstr(osNameFull, "Linux")) {
      char distro[DISTRO_BUF_SIZE];
      char distroShort[DISTRO_BUF_SIZE];
      static int const distroSize = sizeof distro;
      char *lsbOutput;
      int majorVersion;

      /*
       * Write default distro string depending on the kernel version. If
       * later we find more detailed information this will get overwritten.
       */

      majorVersion = Hostinfo_OSVersion(0);
      if (majorVersion < 2 || (majorVersion == 2 && Hostinfo_OSVersion(1) < 4)) {
         Str_Strcpy(distro, STR_OS_OTHER_FULL, distroSize);
         Str_Strcpy(distroShort, STR_OS_OTHER, distroSize);
      } else if (majorVersion == 2 && Hostinfo_OSVersion(1) < 6) {
         Str_Strcpy(distro, STR_OS_OTHER_24_FULL, distroSize);
         Str_Strcpy(distroShort, STR_OS_OTHER_24, distroSize);
      } else if (majorVersion == 2) {
         Str_Strcpy(distro, STR_OS_OTHER_26_FULL, distroSize);
         Str_Strcpy(distroShort, STR_OS_OTHER_26, distroSize);
      } else {
         Str_Strcpy(distro, STR_OS_OTHER_3X_FULL, distroSize);
         Str_Strcpy(distroShort, STR_OS_OTHER_3X, distroSize);
      }

      /*
       * Try to get OS detailed information from the lsb_release command.
       */

      lsbOutput = 0;
      if (!lsbOutput) {
         int i;

         /*
          * Try to get more detailed information from the version file.
          */

         for (i = 0; distroArray[i].filename != NULL; i++) {
            if (HostinfoReadDistroFile(distroArray[i].filename, distroSize,
                                       distro)) {
               break;
            }
         }

         /*
          * If we failed to read every distro file, exit now, before calling
          * strlen on the distro buffer (which wasn't set).
          */

         if (distroArray[i].filename == NULL) {
            Warning("%s: Error: no distro file found\n", __FUNCTION__);

            return FALSE;
         }
      } else {
         char *lsbStart = lsbOutput;
         char *quoteEnd = NULL;

         if (lsbStart[0] == '"') {
            lsbStart++;
            quoteEnd = strchr(lsbStart, '"');
            if (quoteEnd) {
               *quoteEnd = '\0';
            }
         }
         Str_Strcpy(distro, lsbStart, distroSize);
         free(lsbOutput);
      }

      HostinfoGetOSShortName(distro, distroShort, distroSize);

      if (strlen(distro) + strlen(osNameFull) + 2 > sizeof osNameFull) {
         Warning("%s: Error: buffer too small\n", __FUNCTION__);

         return FALSE;
      }

      Str_Strcat(osNameFull, " ", sizeof osNameFull);
      Str_Strcat(osNameFull, distro, sizeof osNameFull);

      if (strlen(distroShort) + 1 > sizeof osName) {
         Warning("%s: Error: buffer too small\n", __FUNCTION__);

         return FALSE;
      }

      Str_Strcpy(osName, distroShort, sizeof osName);
   } else if (strstr(osNameFull, "FreeBSD")) {
      size_t nameLen = sizeof STR_OS_FREEBSD - 1;
      size_t releaseLen = 0;
      char *dashPtr;

      /*
       * FreeBSD releases report their version as "x.y-RELEASE". We'll be
       * naive look for the first dash, and use everything before it as the
       * version number.
       */

      dashPtr = Str_Strchr(buf.release, '-');
      if (dashPtr != NULL) {
         releaseLen = dashPtr - buf.release;
      }

      if (nameLen + releaseLen + 1 > sizeof osName) {
         Warning("%s: Error: buffer too small\n", __FUNCTION__);

         return FALSE;
      }

      Str_Strcpy(osName, STR_OS_FREEBSD, sizeof osName);
   } else if (strstr(osNameFull, "SunOS")) {
      size_t nameLen = sizeof STR_OS_SOLARIS - 1;
      size_t releaseLen = 0;
      char solarisRelease[3] = "";

      /*
       * Solaris releases report their version as "x.y". For our supported
       * releases it seems that x is always "5", and is ignored in favor of
       * "y" for the version number.
       */

      if (sscanf(buf.release, "5.%2[0-9]", solarisRelease) == 1) {
         releaseLen = strlen(solarisRelease);
      }

      if (nameLen + releaseLen + 1 > sizeof osName) {
         Warning("%s: Error: buffer too small\n", __FUNCTION__);

         return FALSE;
      }

      Str_Snprintf(osName, sizeof osName, "%s%s", STR_OS_SOLARIS,
                   solarisRelease);
   }
#endif

#ifndef USERWORLD
   if (Hostinfo_GetSystemBitness() == 64) {
      if (strlen(osName) + sizeof STR_OS_64BIT_SUFFIX > sizeof osName) {
         Warning("%s: Error: buffer too small\n", __FUNCTION__);

         return FALSE;
      }
      Str_Strcat(osName, STR_OS_64BIT_SUFFIX, sizeof osName);
   }
#endif

   /*
    * Before returning, truncate the \n character at the end of the full name.
    */

   lastCharPos = strlen(osNameFull) - 1;
   if (osNameFull[lastCharPos] == '\n') {
      osNameFull[lastCharPos] = '\0';
   }

   /*
    * Serialize access. Collisions should be rare - plus the value will
    * get cached and this won't get called anymore.
    */

   while (Atomic_ReadWrite(&mutex, 1)); // Spinlock.

   if (!HostinfoOSNameCacheValid) {
      Str_Strcpy(HostinfoCachedOSName, osName, sizeof HostinfoCachedOSName);
      Str_Strcpy(HostinfoCachedOSFullName, osNameFull,
                 sizeof HostinfoCachedOSFullName);
      HostinfoOSNameCacheValid = TRUE;
   }

   Atomic_Write(&mutex, 0);  // unlock

   return TRUE;
}

/*
 *-----------------------------------------------------------------------------
 *
 * Hostinfo_CPUCounts --
 *
 *      Get a count of CPUs for the host.
 *        pkgs := total number of sockets/packages
 *        cores := total number of actual cores (not including hyperthreads)
 *        logical := total schedulable threads, as seen by host scheduler
 *      Depending on available host OS interfaces, these numbers may be
 *      either "active" or "possible", so do not depend upon them for
 *      precision.
 *
 *      As an example, a 2 socket Nehalem (4 cores + HT) would return:
 *        pkgs = 2, cores = 8, logical = 16
 *
 *      Again, this interface is generally not useful b/c of its potential
 *      inaccuracy (especially with hotplug!) and because it is only implemented
 *      for a few OSes.
 *
 *      If you are trying to use this interface, it probably means you are doing
 *      something 'clever' with licensing.  Don't.
 *
 * Results:
 *      TRUE if sane numbers are populated, FALSE otherwise.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

Bool
Hostinfo_CPUCounts(uint32 *logical,  // OUT
                   uint32 *cores,    // OUT
                   uint32 *pkgs)     // OUT
{
#if defined __APPLE__
   /*
    * Lame logic.  Because Apple doesn't really expose this info,
    * we'd only use it for licensing anyway, and we just plain
    * don't need it on Apple except that VMHS stuffs it somewhere
    * and may result in a division-by-zero if we don't provide it.
    */
   *logical = Hostinfo_NumCPUs();
   *pkgs = *logical > 4 ? 2 : 1;
   *cores = *logical / *pkgs;

   return TRUE;
#elif defined __linux__
   FILE *f;
   char *line;
   unsigned count = 0, coresPerProc = 0, siblingsPerProc = 0;

   f = Posix_Fopen("/proc/cpuinfo", "r");
   if (f == NULL) {
      return FALSE;
   }

   while (StdIO_ReadNextLine(f, &line, 0, NULL) == StdIO_Success) {
      if (strncmp(line, "processor", strlen("processor")) == 0) {
         count++;
      }
      /* Assume all processors are identical, so just read the first. */
      if (coresPerProc == 0) {
         sscanf(line, "cpu cores : %u", &coresPerProc);
      }
      if (siblingsPerProc == 0) {
         sscanf(line, "siblings : %u", &siblingsPerProc);
      }
      free(line);
   }

   fclose(f);

   *logical = count;
   *pkgs = siblingsPerProc > 0 ? count / siblingsPerProc : count;
   *cores = coresPerProc > 0 ? *pkgs * coresPerProc : *pkgs;

   Log(LGPFX" This machine has %u physical CPUS, %u total cores, and %u "
            "logical CPUs.\n", *pkgs, *cores, *logical);

   return TRUE;
#else
   NOT_IMPLEMENTED();
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * Hostinfo_NumCPUs --
 *
 *      Get the number of logical CPUs on the host.  If the CPUs are
 *      hyperthread-capable, this number may be larger than the number of
 *      physical CPUs.  For example, if the host has four hyperthreaded
 *      physical CPUs with 2 logical CPUs apiece, this function returns 8.
 *
 *      This function returns the number of CPUs that the host presents to
 *      applications, which is what we want in the vast majority of cases.  We
 *      would only ever care about the number of physical CPUs for licensing
 *      purposes.
 *
 * Results:
 *      On success, the number of CPUs (> 0) the host tells us we have.
 *      On failure, 0xFFFFFFFF (-1).
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

uint32
Hostinfo_NumCPUs(void)
{
#if defined(sun)
   static int count = 0;

   if (count <= 0) {
      count = sysconf(_SC_NPROCESSORS_CONF);
   }

   return count;
#elif defined(__APPLE__)
   uint32 out;
   size_t outSize = sizeof out;

   /*
    * Quoting sys/sysctl.h:
    * "
    * These are the support HW selectors for sysctlbyname.  Parameters that
    * are byte counts or frequencies are 64 bit numbers. All other parameters
    * are 32 bit numbers.
    * ...
    * hw.activecpu - The number of processors currently available for executing
    *                threads. Use this number to determine the number threads
    *                to create in SMP aware applications. This number can
    *                change when power management modes are changed.
    * "
    *
    * Apparently the only way to retrieve this info is by name, and I have
    * verified the info changes when you dynamically switch a CPU
    * offline/online. --hpreg
    */

   if (sysctlbyname("hw.activecpu", &out, &outSize, NULL, 0) == -1) {
      return -1;
   }

   return out;
#elif defined(__FreeBSD__)
   uint32 out;
   size_t outSize = sizeof out;

#if __FreeBSD__version >= 500019
   if (sysctlbyname("kern.smp.cpus", &out, &outSize, NULL, 0) == -1) {
      return -1;
   }
#else
   if (sysctlbyname("machdep.smp_cpus", &out, &outSize, NULL, 0) == -1) {
      if (errno == ENOENT) {
         out = 1;
      } else {
         return -1;
      }
   }
#endif

   return out;
#else
   static int count = 0;

   if (count <= 0) {
      FILE *f;
      char *line;

#if defined(VMX86_SERVER)
      if (HostType_OSIsVMK()) {
         VMK_ReturnStatus status = VMKernel_GetNumCPUsUsed(&count);

         if (status != VMK_OK) {
            count = 0;

            return -1;
         }

         return count;
      }
#endif
      f = Posix_Fopen("/proc/cpuinfo", "r");
      if (f == NULL) {
	 return -1;
      }

      while (StdIO_ReadNextLine(f, &line, 0, NULL) == StdIO_Success) {
	 if (strncmp(line, "processor", strlen("processor")) == 0) {
	    count++;
	 }
	 free(line);
      }

      fclose(f);

      if (count == 0) {
	 return -1;
      }
   }

   return count;
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * Hostinfo_OSIsSMP --
 *
 *      Host OS SMP capability.
 *
 * Results:
 *      TRUE is host OS is SMP capable.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Bool
Hostinfo_OSIsSMP(void)
{
   uint32 ncpu;

#if defined(__APPLE__)
   size_t ncpuSize = sizeof ncpu;

   if (sysctlbyname("hw.ncpu", &ncpu, &ncpuSize, NULL, 0) == -1) {
      return FALSE;
   }

#else
   ncpu = Hostinfo_NumCPUs();

   if (ncpu == 0xFFFFFFFF) {
      return FALSE;
   }
#endif

   return ncpu > 1 ? TRUE : FALSE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Hostinfo_NameGet --
 *
 *      Return the fully qualified host name of the host.
 *      Thread-safe. --hpreg
 *
 * Results:
 *      The (memorized) name on success
 *      NULL on failure
 *
 * Side effects:
 *      A host name resolution can occur.
 *
 *-----------------------------------------------------------------------------
 */

Unicode
Hostinfo_NameGet(void)
{
   Unicode result;

   static Atomic_Ptr state; /* Implicitly initialized to NULL. --hpreg */

   result = Atomic_ReadPtr(&state);

   if (UNLIKELY(result == NULL)) {
      Unicode before;

      result = Hostinfo_HostName();

      before = Atomic_ReadIfEqualWritePtr(&state, NULL, result);

      if (before) {
         Unicode_Free(result);

         result = before;
      }
   }

   return result;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Hostinfo_GetUser --
 *
 *      Return current user name, or NULL if can't tell.
 *      XXX Not thread-safe (somebody could do a setenv()). --hpreg
 *
 * Results:
 *      User name.  Must be free()d by caller.
 *
 * Side effects:
 *	No.
 *
 *-----------------------------------------------------------------------------
 */

Unicode
Hostinfo_GetUser(void)
{
   char buffer[BUFSIZ];
   struct passwd pw;
   struct passwd *ppw = &pw;
   Unicode env = NULL;
   Unicode name = NULL;

   if ((Posix_Getpwuid_r(getuid(), &pw, buffer, sizeof buffer, &ppw) == 0) &&
       (ppw != NULL)) {
      if (ppw->pw_name) {
         name = Unicode_Duplicate(ppw->pw_name);
      }
   }

   if (!name) {
      env = Posix_Getenv("USER");

      if (env) {
         name = Unicode_Duplicate(env);
      }
   }

   return name;
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoGetLoadAverage --
 *
 *      Returns system average load.
 *
 * Results:
 *      TRUE on success, FALSE otherwise.
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */

static Bool
HostinfoGetLoadAverage(float *avg0,  // IN/OUT:
                       float *avg1,  // IN/OUT:
                       float *avg2)  // IN/OUT:
{
#if !defined(NO_GETLOADAVG) && (defined(__linux__) && !defined(__UCLIBC__)) || defined(__APPLE__)
   double avg[3];
   int res;

   res = getloadavg(avg, 3);
   if (res < 3) {
      NOT_TESTED_ONCE();

      return FALSE;
   }

   if (avg0) {
      *avg0 = (float) avg[0];
   }
   if (avg1) {
      *avg1 = (float) avg[1];
   }
   if (avg2) {
      *avg2 = (float) avg[2];
   }

   return TRUE;
#else
   /*
    * Not implemented. This function is currently only used in the vmx, so
    * getloadavg is always available to us. If the linux tools ever need this,
    * we can go back to having a look at the output of /proc/loadavg, but
    * let's not do that now as long as it's not necessary.
    */

   NOT_IMPLEMENTED();

   return FALSE;
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * Hostinfo_GetLoadAverage --
 *
 *      Returns system average load * 100.
 *
 * Results:
 *      TRUE/FALSE
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */

Bool
Hostinfo_GetLoadAverage(uint32 *avg)  // IN/OUT:
{
   float avg0 = 0;

   if (!HostinfoGetLoadAverage(&avg0, NULL, NULL)) {
      return FALSE;
   }

   *avg = (uint32) 100 * avg0;

   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Hostinfo_LogLoadAverage --
 *
 *      Logs system average load.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */

void
Hostinfo_LogLoadAverage(void)
{
   float avg0 = 0, avg1 = 0, avg2 = 0;

   if (HostinfoGetLoadAverage(&avg0, &avg1, &avg2)) {
      Log("LOADAVG: %.2f %.2f %.2f\n", avg0, avg1, avg2);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoGetTimeOfDayMonotonic --
 *
 *      Return the system time as indicated by Hostinfo_GetTimeOfDay(), with
 *      locking to ensure monotonicity.
 *
 *      Uses OS native locks as lib/lock is not available in lib/misc.  This
 *      is safe because nothing occurs while locked that can be reentrant.
 *
 * Results:
 *      The time in microseconds is returned. Zero upon error.
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */

static VmTimeType
HostinfoGetTimeOfDayMonotonic(void)
{
   VmTimeType newTime;
   VmTimeType curTime;

   static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

   static VmTimeType lastTimeBase;
   static VmTimeType lastTimeRead;
   static VmTimeType lastTimeReset;

   pthread_mutex_lock(&mutex);  // Use native mechanism, just like Windows

   Hostinfo_GetTimeOfDay(&curTime);

   if (curTime == 0) {
      newTime = 0;
      goto exit;
   }

   /*
    * Don't let time be negative or go backward.  We do this by tracking a
    * base and moving forward from there.
    */

   newTime = lastTimeBase + (curTime - lastTimeReset);

   if (newTime < lastTimeRead) {
      lastTimeReset = curTime;
      lastTimeBase = lastTimeRead + 1;
      newTime = lastTimeBase + (curTime - lastTimeReset);
   }

   lastTimeRead = newTime;

exit:
   pthread_mutex_unlock(&mutex);

   return newTime;
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoSystemTimerMach --
 * HostinfoSystemTimerPosix --
 *
 *      Returns system time based on a monotonic, nanosecond-resolution,
 *      fast timer provided by the (relevant) operating system.
 *
 *      Caller should check return value, as some variants may not be known
 *      to be absent until runtime.  Where possible, these functions collapse
 *      into constants at compile-time.
 *
 * Results:
 *      TRUE if timer is available; FALSE to indicate unavailability.
 *      If available, the current time is returned via 'result'.
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */

static Bool
HostinfoSystemTimerMach(VmTimeType *result)  // OUT
{
#if __APPLE__
#  define vmx86_apple 1

   typedef struct {
      double  scalingFactor;
      Bool    unity;
   } timerData;

   VmTimeType raw;
   timerData *ptr;
   static Atomic_Ptr atomic; /* Implicitly initialized to NULL. --mbellon */

   /*
    * On Mac OS a commpage timer is used. Such timers are ensured to never
    * go backwards - and be valid across all processes.
    */

   /* Ensure that the time base values are correct. */
   ptr = Atomic_ReadPtr(&atomic);

   if (UNLIKELY(ptr == NULL)) {
      mach_timebase_info_data_t timeBase;
      timerData *try = Util_SafeMalloc(sizeof *try);

      mach_timebase_info(&timeBase);

      try->scalingFactor = ((double) timeBase.numer) /
                           ((double) timeBase.denom);

      try->unity = ((timeBase.numer == 1) && (timeBase.denom == 1));

      if (Atomic_ReadIfEqualWritePtr(&atomic, NULL, try)) {
         free(try);
      }

      ptr = Atomic_ReadPtr(&atomic);
   }

   raw = mach_absolute_time();

   if (LIKELY(ptr->unity)) {
      *result = raw;
   } else {
      *result = ((double) raw) * ptr->scalingFactor;
   }

   return TRUE;
#else
#  define vmx86_apple 0
   return FALSE;
#endif
}

static Bool
HostinfoSystemTimerPosix(VmTimeType *result)  // OUT
{
#if defined(_POSIX_TIMERS) && _POSIX_TIMERS > 0 && defined(_POSIX_MONOTONIC_CLOCK)
#  define vmx86_posix 1
   /* Weak declaration to avoid librt.so dependency */
   extern int clock_gettime(clockid_t clk_id, struct timespec *tp) __attribute__ ((weak));

   /* Assignment is idempotent (expected to always be same answer). */
   static volatile enum { UNKNOWN, PRESENT, FAILED } hasGetTime = UNKNOWN;

   struct timespec ts;
   int ret;

   switch (hasGetTime) {
   case FAILED:
      break;
   case UNKNOWN:
      if (clock_gettime == NULL) {
         /* librt.so is not present.  No clock_gettime() */
         hasGetTime = FAILED;
         break;
      }
      ret = clock_gettime(CLOCK_MONOTONIC, &ts);
      if (ret != 0) {
         hasGetTime = FAILED;
         /*
          * Well-understood error codes:
          * ENOSYS, OS does not implement syscall
          * EINVAL, OS implements syscall but not CLOCK_MONOTONIC
          */
         if (errno != ENOSYS && errno != EINVAL) {
            Log("%s: failure, err %d!\n", __FUNCTION__, errno);
         }
         break;
      }
      hasGetTime = PRESENT;
      /* Fall through to 'case PRESENT' */

   case PRESENT:
      ret = clock_gettime(CLOCK_MONOTONIC, &ts);
      ASSERT(ret == 0);
      *result = (VmTimeType)ts.tv_sec * 1000 * 1000 * 1000 + ts.tv_nsec;
      return TRUE;
   }
   return FALSE;
#else
#if vmx86_server && defined(GLIBC_VERSION_23)
#  error Posix clock_gettime support required on ESX
#endif
#  define vmx86_posix 0
   /* No Posix support for clock_gettime() */
   return FALSE;
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * Hostinfo_SystemTimerNS --
 *
 *      Return the time.
 *         - These timers are documented to never go backwards.
 *         - These timers may take locks
 *
 * NOTES:
 *      These are the routines to use when performing timing measurements.
 *
 *      The value returned is valid (finish-time - start-time) only within a
 *      single process. Don't send a time measurement obtained with these
 *      routines to another process and expect a relative time measurement
 *      to be correct.
 *
 *      The actual resolution of these "clocks" are undefined - it varies
 *      depending on hardware, OSen and OS versions.
 *
 *     *** NOTE: This function and all children must be callable
 *     while RANK_logLock is held. ***
 *
 * Results:
 *      The time in nanoseconds is returned. Zero upon error.
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */

VmTimeType
Hostinfo_SystemTimerNS(void)
{
   VmTimeType result;

   if ((vmx86_apple && HostinfoSystemTimerMach(&result)) ||
       (vmx86_posix && HostinfoSystemTimerPosix(&result))) {
      /* Host provides monotonic clock source. */
      return result;
   } else {
      /* GetTimeOfDay is microseconds. */
      return HostinfoGetTimeOfDayMonotonic() * 1000;
   }
}
#undef vmx86_apple
#undef vmx86_posix


/*
 *-----------------------------------------------------------------------------
 *
 * Hostinfo_LogMemUsage --
 *      Log system memory usage.
 *
 * Results:
 *      System memory usage is logged.
 *
 * Side effects:
 *      No.
 *
 *-----------------------------------------------------------------------------
 */

void
Hostinfo_LogMemUsage(void)
{
   int fd = Posix_Open("/proc/self/statm", O_RDONLY);

   if (fd != -1) {
      size_t len;
      char buf[64];

      len = read(fd, buf, sizeof buf);
      close(fd);

      if (len != -1) {
         int a[7] = { 0 };

         buf[len < sizeof buf ? len : sizeof buf - 1] = '\0';

         sscanf(buf, "%d %d %d %d %d %d %d",
                &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6]);

         Log("RUSAGE size=%d resident=%d share=%d trs=%d lrs=%d drs=%d dt=%d\n",
             a[0], a[1], a[2], a[3], a[4], a[5], a[6]);
      }
   }
}


/*
 *----------------------------------------------------------------------
 *
 * Hostinfo_ResetProcessState --
 *
 *      Clean up signal handlers and file descriptors before an exec().
 *      Fds which need to be kept open can be passed as an array.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
Hostinfo_ResetProcessState(const int *keepFds, // IN:
                           size_t numKeepFds)  // IN:
{
   int s, fd;
   struct sigaction sa;
   struct rlimit rlim;

   /*
    * Disable itimers before resetting the signal handlers.
    * Otherwise, the process may still receive timer signals:
    * SIGALRM, SIGVTARLM, or SIGPROF.
    */

   struct itimerval it;
   it.it_value.tv_sec = it.it_value.tv_usec = 0;
   it.it_interval.tv_sec = it.it_interval.tv_usec = 0;
   setitimer(ITIMER_REAL, &it, NULL);
   setitimer(ITIMER_VIRTUAL, &it, NULL);
   setitimer(ITIMER_PROF, &it, NULL);

   for (s = 1; s <= NSIG; s++) {
      sa.sa_handler = SIG_DFL;
      sigfillset(&sa.sa_mask);
      sa.sa_flags = SA_RESTART;
      sigaction(s, &sa, NULL);
   }

   for (fd = (int) sysconf(_SC_OPEN_MAX) - 1; fd > STDERR_FILENO; fd--) {
      size_t i;

      for (i = 0; i < numKeepFds; i++) {
         if (fd == keepFds[i]) {
            break;
         }
      }
      if (i == numKeepFds) {
         (void) close(fd);
      }
   }

   if (getrlimit(RLIMIT_AS, &rlim) == 0) {
      rlim.rlim_cur = rlim.rlim_max;
      setrlimit(RLIMIT_AS, &rlim);
   }

#ifdef __linux__
   /*
    * Drop iopl to its default value.
    * iopl() is not implemented in userworlds
    */
   if (!vmx86_server) {
      int err;
      uid_t euid;

      euid = Id_GetEUid();
      /* At this point, _unless we are running as root_, we shouldn't have root
         privileges --hpreg */
      ASSERT(euid != 0 || getuid() == 0);
      Id_SetEUid(0);
#if defined NO_IOPL
      NOT_IMPLEMENTED();
      errno = ENOSYS;
#else
      err = iopl(0);
#endif
      Id_SetEUid(euid);
      ASSERT_NOT_IMPLEMENTED(err == 0);
   }
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * Hostinfo_Daemonize --
 *
 *      Cross-platform daemon(3)-like wrapper.
 *
 *      Restarts the current process as a daemon, given the path to the
 *      process (usually from Hostinfo_GetModulePath).  This means:
 *
 *         * You're detached from your parent.  (Your parent doesn't
 *           need to wait for you to exit.)
 *         * Your process no longer has a controlling terminal or
 *           process group.
 *         * Your stdin/stdout/stderr fds are redirected to /dev/null. All
 *           other descriptors, except for the ones that are passed in the
 *           parameter keepFds, are closed.
 *         * Your signal handlers are reset to SIG_DFL in the daemonized
 *           process, and all the signals are unblocked.
 *         * Your main() function is called with the specified NULL-terminated
 *           argument list.
 *
 *      (Don't forget that the first string in args is argv[0] -- the
 *      name of the process).
 *
 *      Unless 'flags' contains HOSTINFO_DAEMONIZE_NOCHDIR, then the
 *      current directory of the daemon process is set to "/".
 *
 *      Unless 'flags' contains HOSTINFO_DAEMONIZE_NOCLOSE, then all stdio
 *      file descriptors of the daemon process are redirected to /dev/null.
 *      This is true even if the stdio descriptors are included in keepFds,
 *      i.e. the list of fds to be kept open.
 *
 *      If 'flags' contains HOSTINFO_DAEMONIZE_EXIT, then upon successful
 *      launch of the daemon, the original process will exit.
 *
 *      If pidPath is non-NULL, then upon success, writes the PID
 *      (as a US-ASCII string followed by a newline) of the daemon
 *      process to that path.
 *
 *      If 'flags' contains HOSTINFO_DAEMONIZE_LOCKPID and pidPath is
 *      non-NULL, then an exclusive flock(2) is taken on pidPath to prevent
 *      multiple instances of the service from running.
 *
 * Results:
 *      FALSE if the process could not be daemonized.  errno contains
 *      the error on failure.
 *      TRUE if 'flags' does not contain HOSTINFO_DAEMONIZE_EXIT and
 *      the process was daemonized.
 *      Otherwise, if the process was daemonized, this function does
 *      not return, and flow continues from your own main() function.
 *
 * Side effects:
 *      The current process is restarted with the given arguments.
 *      The process state is reset (see Hostinfo_ResetProcessState).
 *      A new session is created (so the process has no controlling terminal).
 *
 *-----------------------------------------------------------------------------
 */

Bool
Hostinfo_Daemonize(const char *path,             // IN: NUL-terminated UTF-8
                                                 // path to exec
                   char * const *args,           // IN: NULL-terminated UTF-8
                                                 // argv list
                   HostinfoDaemonizeFlags flags, // IN: flags
                   const char *pidPath,          // IN/OPT: NUL-terminated
                                                 // UTF-8 path to write PID
                   const int *keepFds,           // IN/OPT: array of fds to be
                                                 // kept open
                   size_t numKeepFds)            // IN: number of fds in
                                                 // keepFds
{
   /*
    * We use the double-fork method to make a background process whose
    * parent is init instead of the original process.
    *
    * We do this instead of calling daemon(), because daemon() is
    * deprecated on Mac OS 10.5 hosts, and calling it causes a compiler
    * warning.
    *
    * We must exec() after forking, because Mac OS library frameworks
    * depend on internal Mach ports, which are not correctly propagated
    * across fork calls.  exec'ing reinitializes the frameworks, which
    * causes them to reopen their Mach ports.
    */

   int pidPathFd = -1;
   int childPid;
   int pipeFds[2] = { -1, -1 };
   uint32 err = EINVAL;
   char *pathLocalEncoding = NULL;
   char **argsLocalEncoding = NULL;
   int *tempFds = NULL;
   size_t numTempFds = numKeepFds + 1;
   sigset_t sig;

   ASSERT_ON_COMPILE(sizeof (errno) <= sizeof err);
   ASSERT(args);
   ASSERT(path);
   ASSERT(numKeepFds == 0 || keepFds);

   if (pidPath) {
      pidPathFd = Posix_Open(pidPath, O_WRONLY | O_CREAT, 0644);
      if (pidPathFd == -1) {
         err = errno;
         Warning("%s: Couldn't open PID path [%s], error %u.\n",
                 __FUNCTION__, pidPath, err);
         errno = err;
         return FALSE;
      }

      /*
       * Lock this file to take a mutex on daemonizing this process. The child
       * will keep this file descriptor open for as long as it is running.
       *
       * flock(2) is a BSD extension (also supported on Linux) which creates a
       * lock that is inherited by the child after fork(2). fcntl(2) locks do
       * not have this property. Solaris only supports fcntl(2) locks.
       */
#ifndef sun
      if ((flags & HOSTINFO_DAEMONIZE_LOCKPID) &&
          flock(pidPathFd, LOCK_EX | LOCK_NB) == -1) {
         err = errno;
         Warning("%s: Lock held on PID path [%s], error %u, not daemonizing.\n",
                 __FUNCTION__, pidPath, err);
         errno = err;
         close(pidPathFd);
         return FALSE;
      }
#endif

      numTempFds++;
   }

   if (pipe(pipeFds) == -1) {
      err = errno;
      Warning("%s: Couldn't create pipe, error %u.\n", __FUNCTION__, err);
      pipeFds[0] = pipeFds[1] = -1;
      goto cleanup;
   }

   tempFds = malloc(sizeof tempFds[0] * numTempFds);
   if (!tempFds) {
      err = errno;
      Warning("%s: Couldn't allocate memory, error %u.\n", __FUNCTION__, err);
      goto cleanup;
   }
   if (keepFds) {
      memcpy(tempFds, keepFds, sizeof tempFds[0] * numKeepFds);
   }
   tempFds[numKeepFds++] = pipeFds[1];
   if (pidPath) {
      tempFds[numKeepFds++] = pidPathFd;
   }

   if (fcntl(pipeFds[1], F_SETFD, 1) == -1) {
      err = errno;
      Warning("%s: Couldn't set close-on-exec for fd %d, error %u.\n",
              __FUNCTION__, pipeFds[1], err);
      goto cleanup;
   }

   /* Convert the strings from UTF-8 before we fork. */
   pathLocalEncoding = Unicode_GetAllocBytes(path, STRING_ENCODING_DEFAULT);
   if (!pathLocalEncoding) {
      Warning("%s: Couldn't convert path [%s] to default encoding.\n",
              __FUNCTION__, path);
      goto cleanup;
   }

   argsLocalEncoding = Unicode_GetAllocList(args, STRING_ENCODING_DEFAULT, -1);
   if (!argsLocalEncoding) {
      Warning("%s: Couldn't convert arguments to default encoding.\n",
              __FUNCTION__);
      goto cleanup;
   }

   childPid = fork();

   switch (childPid) {
   case -1:
      err = errno;
      Warning("%s: Couldn't fork first child, error %u.\n", __FUNCTION__,
              err);
      goto cleanup;
   case 0:
      /* We're the first child.  Continue on. */
      break;
   default:
      {
         /* We're the original process.  Check if the first child exited. */
         int status;

         close(pipeFds[1]);
         waitpid(childPid, &status, 0);
         if (WIFEXITED(status) && WEXITSTATUS(status) != EXIT_SUCCESS) {
            Warning("%s: Child %d exited with error %d.\n",
                    __FUNCTION__, childPid, WEXITSTATUS(status));
            goto cleanup;
         } else if (WIFSIGNALED(status)) {
            Warning("%s: Child %d exited with signal %d.\n",
                    __FUNCTION__, childPid, WTERMSIG(status));
            goto cleanup;
         }

         /*
          * Check if the second child exec'ed successfully.  If it had
          * an error, it will write a uint32 errno to this pipe before
          * exiting.  Otherwise, its end of the pipe will be closed on
          * exec and this call will fail as expected.
          * The assumption is that we don't get a partial read. In case,
          * it did happen, we can detect it by the number of bytes read.
          */

         while (TRUE) {
            int res = read(pipeFds[0], &err, sizeof err);

            if (res > 0) {
               Warning("%s: Child could not exec %s, read %d, error %u.\n",
                       __FUNCTION__, path, res, err);
               goto cleanup;
            } else if ((res == -1) && (errno == EINTR)) {
               continue;
            }
            break;
         }

         err = 0;
         goto cleanup;
      }
   }

   /*
    * Close all fds except for the write end of the error pipe (which we've
    * already set to close on successful exec), the pid file, and the ones
    * requested by the caller. Also reset the signal mask to unblock all
    * signals. fork() clears pending signals.
    */

   Hostinfo_ResetProcessState(tempFds, numKeepFds);
   free(tempFds);
   tempFds = NULL;
   sigfillset(&sig);
   sigprocmask(SIG_UNBLOCK, &sig, NULL);

   if (!(flags & HOSTINFO_DAEMONIZE_NOCLOSE) && setsid() == -1) {
      Warning("%s: Couldn't create new session, error %d.\n",
              __FUNCTION__, errno);

      _exit(EXIT_FAILURE);
   }

   switch (fork()) {
   case -1:
      {
         Warning("%s: Couldn't fork second child, error %d.\n",
                 __FUNCTION__, errno);

         _exit(EXIT_FAILURE);
      }
   case 0:
      // We're the second child.  Continue on.
      break;
   default:
      /*
       * We're the first child.  We don't need to exist any more.
       *
       * Exiting here causes the second child to be reparented to the
       * init process, so the original process doesn't need to wait
       * for the child we forked off.
       */

      _exit(EXIT_SUCCESS);
   }

   /*
    * We can't use our i18n wrappers for file manipulation at this
    * point, since we've forked; internal library mutexes might be
    * invalid.
    */

   if (!(flags & HOSTINFO_DAEMONIZE_NOCHDIR) && chdir("/") == -1) {
      uint32 err = errno;

      Warning("%s: Couldn't chdir to /, error %u.\n", __FUNCTION__, err);

      /* Let the original process know we failed to chdir. */
      if (write(pipeFds[1], &err, sizeof err) == -1) {
         Warning("%s: Couldn't write to parent pipe: %u, "
                 "original error: %u.\n", __FUNCTION__, errno, err);
      }
      _exit(EXIT_FAILURE);
   }

   if (!(flags & HOSTINFO_DAEMONIZE_NOCLOSE)) {
      int fd;

      fd = open(_PATH_DEVNULL, O_RDONLY);
      if (fd != -1) {
         dup2(fd, STDIN_FILENO);
         close(fd);
      }

      fd = open(_PATH_DEVNULL, O_WRONLY);
      if (fd != -1) {
         dup2(fd, STDOUT_FILENO);
         dup2(fd, STDERR_FILENO);
         close(fd);
      }
   }

   if (pidPath) {
      int64 pid;
      char pidString[32];
      int pidStringLen;

      ASSERT_ON_COMPILE(sizeof (pid_t) <= sizeof pid);
      ASSERT(pidPathFd >= 0);

      pid = getpid();
      pidStringLen = Str_Sprintf(pidString, sizeof pidString,
                                 "%"FMT64"d\n", pid);
      if (pidStringLen <= 0) {
         err = EINVAL;

         if (write(pipeFds[1], &err, sizeof err) == -1) {
            Warning("%s: Couldn't write to parent pipe: %u, original "
                    "error: %u.\n", __FUNCTION__, errno, err);
         }
         _exit(EXIT_FAILURE);
      }

      if (ftruncate(pidPathFd, 0) == -1) {
         err = errno;
         Warning("%s: Couldn't truncate path [%s], error %d.\n",
                 __FUNCTION__, pidPath, err);

         if (write(pipeFds[1], &err, sizeof err) == -1) {
            Warning("%s: Couldn't write to parent pipe: %u, original "
                    "error: %u.\n", __FUNCTION__, errno, err);
         }
         _exit(EXIT_FAILURE);
      }

      if (write(pidPathFd, pidString, pidStringLen) != pidStringLen) {
         err = errno;
         Warning("%s: Couldn't write PID to path [%s], error %d.\n",
                 __FUNCTION__, pidPath, err);

         if (write(pipeFds[1], &err, sizeof err) == -1) {
            Warning("%s: Couldn't write to parent pipe: %u, original "
                    "error: %u.\n", __FUNCTION__, errno, err);
         }
         _exit(EXIT_FAILURE);
      }

      if (fsync(pidPathFd) == -1) {
         err = errno;
         Warning("%s: Couldn't flush PID to path [%s], error %d.\n",
                 __FUNCTION__, pidPath, err);

         if (write(pipeFds[1], &err, sizeof err) == -1) {
            Warning("%s: Couldn't write to parent pipe: %u, original "
                    "error: %u.\n", __FUNCTION__, errno, err);
         }
         _exit(EXIT_FAILURE);
      }

      /* Leave pidPathFd open to hold the mutex until this process exits. */
      if (!(flags & HOSTINFO_DAEMONIZE_LOCKPID)) {
         close(pidPathFd);
      }
   }

   if (execv(pathLocalEncoding, argsLocalEncoding) == -1) {
      err = errno;
      Warning("%s: Couldn't exec %s, error %d.\n", __FUNCTION__, path, err);

      /* Let the original process know we failed to exec. */
      if (write(pipeFds[1], &err, sizeof err) == -1) {
         Warning("%s: Couldn't write to parent pipe: %u, "
                 "original error: %u.\n", __FUNCTION__, errno, err);
      }
      _exit(EXIT_FAILURE);
   }

   NOT_REACHED();

  cleanup:
   free(tempFds);

   if (pipeFds[0] != -1) {
      close(pipeFds[0]);
   }
   if (pipeFds[1] != -1) {
      close(pipeFds[1]);
   }
   Util_FreeStringList(argsLocalEncoding, -1);
   free(pathLocalEncoding);

   if (err == 0) {
      if (flags & HOSTINFO_DAEMONIZE_EXIT) {
         _exit(EXIT_SUCCESS);
      }
   } else {
      if (pidPath) {
         /*
          * Unlink pidPath on error before closing pidPathFd to avoid racing
          * with another process attempting to daemonize and unlinking the
          * file it created instead.
          */
         Posix_Unlink(pidPath);
      }

      errno = err;
   }

   if (pidPath) {
      close(pidPathFd);
   }

   return err == 0;
}


#if !defined(__APPLE__) && !defined(__FreeBSD__)
/*
 *----------------------------------------------------------------------
 *
 * HostinfoGetCpuInfo --
 *
 *      Get some attribute from /proc/cpuinfo for a given CPU
 *
 * Results:
 *      On success: Allocated, NUL-terminated attribute string.
 *      On failure: NULL.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static char *
HostinfoGetCpuInfo(int nCpu,    // IN:
                   char *name)  // IN:
{
   FILE *f;
   char *line;
   int cpu = 0;
   char *value = NULL;

   f = Posix_Fopen("/proc/cpuinfo", "r");

   if (f == NULL) {
      Warning(LGPFX" %s: Unable to open /proc/cpuinfo\n", __FUNCTION__);

      return NULL;
   }

   while (cpu <= nCpu &&
          StdIO_ReadNextLine(f, &line, 0, NULL) == StdIO_Success) {
      char *s;
      char *e;

      if ((s = strstr(line, name)) &&
          (s = strchr(s, ':'))) {
         s++;
         e = s + strlen(s);

         /* Skip leading and trailing while spaces */
         for (; s < e && isspace(*s); s++);
         for (; s < e && isspace(e[-1]); e--);
         *e = 0;

         /* Free previous value */
         free(value);
         value = strdup(s);
         ASSERT_MEM_ALLOC(value);

         cpu++;
      }
      free(line);
   }

   fclose(f);

   return value;
}
#endif


/*
 *-----------------------------------------------------------------------------
 *
 * Hostinfo_GetRatedCpuMhz --
 *
 *      Get the rated CPU speed of a given processor.
 *      Return value is in MHz.
 *
 * Results:
 *      TRUE on success, FALSE on failure
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

Bool
Hostinfo_GetRatedCpuMhz(int32 cpuNumber,  // IN:
                        uint32 *mHz)      // OUT:
{
#if defined(__APPLE__) || defined(__FreeBSD__)

#  if defined(__APPLE__)
#     define CPUMHZ_SYSCTL_NAME "hw.cpufrequency_max"
#  elif __FreeBSD__version >= 50011
#     define CPUMHZ_SYSCTL_NAME "hw.clockrate"
#  endif

#  if defined(CPUMHZ_SYSCTL_NAME)
   uint32 hz;
   size_t hzSize = sizeof hz;

   /* 'cpuNumber' is ignored: Intel Macs are always perfectly symmetric. */

   if (sysctlbyname(CPUMHZ_SYSCTL_NAME, &hz, &hzSize, NULL, 0) == -1) {
      return FALSE;
   }

   *mHz = hz / 1000000;

   return TRUE;
#  else
   return FALSE;
#  endif
#else
#if defined(VMX86_SERVER)
   if (HostType_OSIsVMK()) {
      uint32 tscKhzEstimate;
      VMK_ReturnStatus status = VMKernel_GetTSCkhzEstimate(&tscKhzEstimate);

      /*
       * The TSC frequency matches the CPU frequency in all modern CPUs.
       * Regardless, the TSC frequency is a much better estimate of
       * reality than failing or returning zero.
       */

      *mHz = tscKhzEstimate / 1000;

      return (status == VMK_OK);
   }
#endif

   {
      float fMhz = 0;
      char *readVal = HostinfoGetCpuInfo(cpuNumber, "cpu MHz");

      if (readVal == NULL) {
         return FALSE;
      }

      if (sscanf(readVal, "%f", &fMhz) == 1) {
         *mHz = (unsigned int)(fMhz + 0.5);
      }

      free(readVal);
   }

   return TRUE;
#endif
}


#if defined(__APPLE__) || defined(__FreeBSD__)
/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoGetSysctlStringAlloc --
 *
 *      Obtains the value of a string-type host sysctl.
 *
 * Results:
 *      On success: Allocated, NUL-terminated string.
 *      On failure: NULL.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static char *
HostinfoGetSysctlStringAlloc(char const *sysctlName) // IN
{
   char *desc;
   size_t descSize;

   if (sysctlbyname(sysctlName, NULL, &descSize, NULL, 0) == -1) {
      return NULL;
   }

   desc = malloc(descSize);
   if (!desc) {
      return NULL;
   }

   if (sysctlbyname(sysctlName, desc, &descSize, NULL, 0) == -1) {
      free(desc);

      return NULL;
   }

   return desc;
}
#endif


/*
 *-----------------------------------------------------------------------------
 *
 * Hostinfo_GetCpuDescription --
 *
 *      Get the descriptive name associated with a given CPU.
 *
 * Results:
 *      On success: Allocated, NUL-terminated string.
 *      On failure: NULL.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

char *
Hostinfo_GetCpuDescription(uint32 cpuNumber)  // IN:
{
#if defined(__APPLE__)
   /* 'cpuNumber' is ignored: Intel Macs are always perfectly symmetric. */
   return HostinfoGetSysctlStringAlloc("machdep.cpu.brand_string");
#elif defined(__FreeBSD__)
   return HostinfoGetSysctlStringAlloc("hw.model");
#else
#ifdef VMX86_SERVER
   if (HostType_OSIsVMK()) {
      char mName[48];

      /* VMKernel treats mName as an in/out parameter so terminate it. */
      mName[0] = '\0';
      if (VMKernel_GetCPUModelName(cpuNumber, mName,
                                   sizeof(mName)) == VMK_OK) {
         mName[sizeof(mName) - 1] = '\0';

         return strdup(mName);
      }

      return NULL;
   }
#endif

   return HostinfoGetCpuInfo(cpuNumber, "model name");
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * Hostinfo_Execute --
 *
 *      Start program 'path'.  If 'wait' is TRUE, wait for program
 *	to complete and return exit status.
 *
 * Results:
 *      Exit status of 'path'.
 *
 * Side effects:
 *      Run a separate program.
 *
 *----------------------------------------------------------------------
 */

int
Hostinfo_Execute(const char *path,   // IN:
                 char * const *args, // IN:
                 Bool wait,          // IN:
                 const int *keepFds, // IN/OPT: array of fds to be kept open
                 size_t numKeepFds)  // IN: number of fds in keepFds
{
   int pid;
   int status;

   if (path == NULL) {
      return 1;
   }

   pid = fork();

   if (pid == -1) {
      return -1;
   }

   if (pid == 0) {
      Hostinfo_ResetProcessState(keepFds, numKeepFds);
      Posix_Execvp(path, args);
      exit(127);
   }

   if (wait) {
      for (;;) {
	 if (waitpid(pid, &status, 0) == -1) {
	    if (errno == ECHILD) {
	       return 0;	// This sucks.  We really don't know.
	    }
	    if (errno != EINTR) {
	       return -1;
	    }
	 } else {
	    return status;
	 }
      }
   } else {
      return 0;
   }
}


#ifdef __APPLE__
/*
 * How to retrieve kernel zone information. A little bit of history
 * ---
 * 1) In Mac OS versions < 10.6, we could retrieve kernel zone information like
 *    zprint did, i.e. by invoking the host_zone_info() Mach call.
 *
 *    osfmk/mach/mach_host.defs defines both arrays passed to host_zone_info()
 *    as 'out' parameters, but the implementation of the function in
 *    osfmk/kern/zalloc.c clearly treats them as 'inout' parameters. This issue
 *    is confirmed in practice: the input values passed by the user process are
 *    ignored. Now comes the scary part: is the input of the kernel function
 *    deterministically invalid, or is it some non-deterministic garbage (in
 *    which case the kernel could corrupt the user address space)? The answer
 *    is in the Mach IPC code. A cursory kernel debugging session seems to
 *    imply that the input pointer values are garbage, but the input size
 *    values are always 0. So host_zone_info() seems safe to use in practice.
 *
 * 2) In Mac OS 10.6, Apple introduced the 64-bit kernel.
 *
 *    2.1) They modified host_zone_info() to always returns KERN_NOT_SUPPORTED
 *         when the sizes (32-bit or 64-bit) of the user and kernel virtual
 *         address spaces do not match. Was bug 377049.
 *
 *         zprint got away with it by re-executing itself to match the kernel.
 *
 *    2.2) They broke the ABI for 64-bit user processes: the
 *         'zone_info.zi_*_size' fields are 32-bit in the Mac OS 10.5 SDK, and
 *         64-bit in the Mac OS 10.6 SDK. So a 64-bit user process compiled
 *         against the Mac OS 10.5 SDK works with the Mac OS 10.5 (32-bit)
 *         kernel but fails with the Mac OS 10.6 64-bit kernel.
 *
 *         zprint in Mac OS 10.6 is compiled against the Mac OS 10.6 SDK, so it
 *         got away with it.
 *
 *    The above two things made it very impractical for us to keep calling
 *    host_zone_info(). Instead we invoked zprint and parsed its non-localized
 *    output.
 *
 * 3) In Mac OS 10.7, Apple cleaned their mess and solved all the above
 *    problems by introducing a new mach_zone_info() Mach call. So this is what
 *    we use now, when available. Was bug 816610.
 *
 * 4) In Mac OS 10.8, Apple appears to have modified mach_zone_info() to always
 *    return KERN_INVALID_HOST(!) when the calling process (not the calling
 *    thread!) is not root.
 */


#if MAC_OS_X_VERSION_MIN_REQUIRED < 1070 // Must run on Mac OS versions < 10.7
/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoGetKernelZoneElemSizeZprint --
 *
 *      Retrieve the size of the elements in a named kernel zone, by invoking
 *      zprint.
 *
 * Results:
 *      On success: the size (in bytes) > 0.
 *      On failure: 0.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

static size_t
HostinfoGetKernelZoneElemSizeZprint(char const *name) // IN: Kernel zone name
{
   size_t retval = 0;
   struct {
      size_t retval;
   } volatile *shared;
   pid_t child;
   pid_t pid;

   /*
    * popen(3) incorrectly executes the shell with the identity of the calling
    * process, ignoring a potential per-thread identity. And starting with
    * Mac OS 10.6 it is even worse: if there is a per-thread identity,
    * popen(3) removes it!
    *
    * So we run this code in a separate process which runs with the same
    * identity as the current thread.
    */

   shared = mmap(NULL, sizeof *shared, PROT_READ | PROT_WRITE,
                 MAP_ANON | MAP_SHARED, -1, 0);

   if (shared == (void *)-1) {
      Warning("%s: mmap error %d.\n", __FUNCTION__, errno);

      return retval;
   }

   // In case the child is terminated before it can set it.
   shared->retval = retval;

   child = fork();
   if (child == (pid_t)-1) {
      Warning("%s: fork error %d.\n", __FUNCTION__, errno);
      munmap((void *)shared, sizeof *shared);

      return retval;
   }

   // This executes only in the child process.
   if (!child) {
      size_t nameLen;
      FILE *stream;
      Bool parsingProperties = FALSE;

      ASSERT(name);

      nameLen = strlen(name);
      ASSERT(nameLen && *name != '\t');

      stream = popen("/usr/bin/zprint -C", "r");
      if (!stream) {
         Warning("%s: popen error %d.\n", __FUNCTION__, errno);
         exit(EXIT_SUCCESS);
      }

      for (;;) {
         char *line;
         size_t lineLen;

         if (StdIO_ReadNextLine(stream, &line, 0,
                                &lineLen) != StdIO_Success) {
            break;
         }

         if (parsingProperties) {
            if (   // Not a property line anymore. Property not found.
                   lineLen < 1 || memcmp(line, "\t", 1)
                   // Property found.
                || sscanf(line, " elem_size: %"FMTSZ"u bytes",
                          &shared->retval) == 1) {
               free(line);
               break;
            }
         } else if (!(lineLen < nameLen + 6 ||
                    memcmp(line, name, nameLen) ||
                    memcmp(line + nameLen, " zone:", 6))) {
            // Zone found.
            parsingProperties = TRUE;
         }

         free(line);
      }

      pclose(stream);
      exit(EXIT_SUCCESS);
   }

   /*
    * This executes only in the parent process.
    * Wait for the child to terminate, and return its retval.
    */

   do {
      int status;

      pid = waitpid(child, &status, 0);
   } while ((pid == -1) && (errno == EINTR));

   ASSERT_NOT_IMPLEMENTED(pid == child);

   retval = shared->retval;
   munmap((void *)shared, sizeof *shared);

   return retval;
}
#endif


/*
 *-----------------------------------------------------------------------------
 *
 * Hostinfo_GetKernelZoneElemSize --
 *
 *      Retrieve the size of the elements in a named kernel zone.
 *
 * Results:
 *      On success: the size (in bytes) > 0.
 *      On failure: 0.
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

size_t
Hostinfo_GetKernelZoneElemSize(char const *name) // IN: Kernel zone name
{
#if MAC_OS_X_VERSION_MAX_ALLOWED < 1070 // Compiles against SDK version < 10.7
   typedef struct {
      char mzn_name[80];
   } mach_zone_name_t;
   typedef struct {
      uint64_t mzi_count;
      uint64_t mzi_cur_size;
      uint64_t mzi_max_size;
      uint64_t mzi_elem_size;
      uint64_t mzi_alloc_size;
      uint64_t mzi_sum_size;
      uint64_t mzi_exhaustible;
      uint64_t mzi_collectable;
   } mach_zone_info_t;
#endif
   size_t result = 0;
   mach_zone_name_t *namesPtr;
   mach_msg_type_number_t namesSize;
   mach_zone_info_t *infosPtr;
   mach_msg_type_number_t infosSize;
   kern_return_t kr;
   mach_msg_type_number_t i;

#if MAC_OS_X_VERSION_MIN_REQUIRED < 1070 // Must run on Mac OS versions < 10.7
   kern_return_t (*mach_zone_info)(host_t host,
      mach_zone_name_t **names, mach_msg_type_number_t *namesCnt,
      mach_zone_info_t **info, mach_msg_type_number_t *infoCnt) =
         dlsym(RTLD_DEFAULT, "mach_zone_info");
   if (!mach_zone_info) {
      return HostinfoGetKernelZoneElemSizeZprint(name);
   }
#endif

   ASSERT(name);

   kr = mach_zone_info(mach_host_self(), &namesPtr, &namesSize, &infosPtr,
                       &infosSize);
   if (kr != KERN_SUCCESS) {
      Warning("%s: mach_zone_info failed %u.\n", __FUNCTION__, kr);
      return result;
   }

   ASSERT(namesSize == infosSize);
   for (i = 0; i < namesSize; i++) {
      if (!strcmp(namesPtr[i].mzn_name, name)) {
         result = infosPtr[i].mzi_elem_size;
         /* Check that nothing of value was lost during the cast. */
         ASSERT(result == infosPtr[i].mzi_elem_size);
         break;
      }
   }

   ASSERT_ON_COMPILE(sizeof namesPtr <= sizeof (vm_address_t));
   kr = vm_deallocate(mach_task_self(), (vm_address_t)namesPtr,
                      namesSize * sizeof *namesPtr);
   ASSERT(kr == KERN_SUCCESS);

   ASSERT_ON_COMPILE(sizeof infosPtr <= sizeof (vm_address_t));
   kr = vm_deallocate(mach_task_self(), (vm_address_t)infosPtr,
                      infosSize * sizeof *infosPtr);
   ASSERT(kr == KERN_SUCCESS);

   return result;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Hostinfo_GetHardwareModel --
 *
 *      Obtains the hardware model identifier (i.e. "MacPro5,1") from the host.
 *
 * Results:
 *      On success: Allocated, NUL-terminated string.
 *      On failure: NULL.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

char *
Hostinfo_GetHardwareModel(void)
{
   return HostinfoGetSysctlStringAlloc("hw.model");
}
#endif /* __APPLE__ */


/*
 *-----------------------------------------------------------------------------
 *
 * Hostinfo_SystemUpTime --
 *
 *      Return system uptime in microseconds.
 *
 *      Please note that the actual resolution of this "clock" is undefined -
 *      it varies between OSen and OS versions. Use Hostinfo_SystemTimerUS
 *      whenever possible.
 *
 * Results:
 *      System uptime in microseconds or zero in case of a failure.
 *
 * Side effects:
 *	None.
 *
 *-----------------------------------------------------------------------------
 */

VmTimeType
Hostinfo_SystemUpTime(void)
{
#if defined(__APPLE__)
   return Hostinfo_SystemTimerUS();
#elif defined(__linux__)
   int res;
   double uptime;
   int fd;
   char buf[256];

   static Atomic_Int fdStorage = { -1 };
   static Atomic_uint32 logFailedPread = { 1 };

   /*
    * /proc/uptime does not exist on Visor.  Use syscall instead.
    * Discovering Visor is a run-time check with a compile-time hint.
    */
   if (vmx86_server && HostType_OSIsPureVMK()) {
      uint64 uptime;
#ifdef VMX86_SERVER
      if (UNLIKELY(VMKernel_GetUptimeUS(&uptime) != VMK_OK)) {
         Log("%s: failure!\n", __FUNCTION__);

         uptime = 0;  // A timer read failure - this is really bad!
      }
#endif
      return uptime;
   }

   fd = Atomic_ReadInt(&fdStorage);

   /* Do we need to open the file the first time through? */
   if (UNLIKELY(fd == -1)) {
      fd = open("/proc/uptime", O_RDONLY);

      if (fd == -1) {
         Warning(LGPFX" Failed to open /proc/uptime: %s\n",
                 Err_Errno2String(errno));

         return 0;
      }

      /* Try to swap ours in. If we lose the race, close our fd */
      if (Atomic_ReadIfEqualWriteInt(&fdStorage, -1, fd) != -1) {
         close(fd);
      }

      /* Get the winning fd - either ours or theirs, doesn't matter anymore */
      fd = Atomic_ReadInt(&fdStorage);
   }

   ASSERT(fd != -1);

   res = pread(fd, buf, sizeof buf - 1, 0);
   if (res == -1) {
      /*
       * In case some kernel broke pread (like 2.6.28-rc1), have a fall-back
       * instead of spewing the log.  This should be rare.  Using a lock
       * around lseek and read does not work here as it will deadlock with
       * allocTrack/fileTrack enabled.
       */

      if (Atomic_ReadIfEqualWrite(&logFailedPread, 1, 0) == 1) {
         Warning(LGPFX" Failed to pread /proc/uptime: %s\n",
                 Err_Errno2String(errno));
      }
      fd = open("/proc/uptime", O_RDONLY);
      if (fd == -1) {
         Warning(LGPFX" Failed to retry open /proc/uptime: %s\n",
                 Err_Errno2String(errno));

         return 0;
      }
      res = read(fd, buf, sizeof buf - 1);
      close(fd);
      if (res == -1) {
         Warning(LGPFX" Failed to read /proc/uptime: %s\n",
                 Err_Errno2String(errno));

         return 0;
      }
   }
   ASSERT(res < sizeof buf);
   buf[res] = '\0';

   if (sscanf(buf, "%lf", &uptime) != 1) {
      Warning(LGPFX" Failed to parse /proc/uptime\n");

      return 0;
   }

   return uptime * 1000 * 1000;
#else
NOT_IMPLEMENTED();
#endif
}


#if !defined(__APPLE__)
/*
 *----------------------------------------------------------------------
 *
 * HostinfoFindEntry --
 *
 *      Search a buffer for a pair `STRING <blanks> DIGITS'
 *	and return the number DIGITS, or 0 when fail.
 *
 * Results:
 *      TRUE on  success, FALSE on failure
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

static Bool 
HostinfoFindEntry(char *buffer,         // IN: Buffer
                  char *string,         // IN: String sought
                  unsigned int *value)  // OUT: Value
{
   char *p = strstr(buffer, string);
   unsigned int val;

   if (p == NULL) {
      return FALSE;
   }

   p += strlen(string);

   while (*p == ' ' || *p == '\t') {
      p++;
   }
   if (*p < '0' || *p > '9') {
      return FALSE;
   }

   val = strtoul(p, NULL, 10);
   if ((errno == ERANGE) || (errno == EINVAL)) {
      return FALSE;
   }

   *value = val;

   return TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * HostinfoGetMemInfo --
 *
 *      Get some attribute from /proc/meminfo
 *      Return value is in KB.
 *
 * Results:
 *      TRUE on success, FALSE on failure
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Bool
HostinfoGetMemInfo(char *name,           // IN:
                   unsigned int *value)  // OUT:
{
   size_t len;
   char   buffer[4096];

   int fd = Posix_Open("/proc/meminfo", O_RDONLY);

   if (fd == -1) {
      Warning(LGPFX" %s: Unable to open /proc/meminfo\n", __FUNCTION__);

      return FALSE;
   }

   len = read(fd, buffer, sizeof buffer - 1);
   close(fd);

   if (len == -1) {
      return FALSE;
   }

   buffer[len] = '\0';

   return HostinfoFindEntry(buffer, name, value);
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoSysinfo --
 *
 *      Retrieve system information on a Linux system.
 *    
 * Results:
 *      TRUE on success: '*totalRam', '*freeRam', '*totalSwap' and '*freeSwap'
 *                       are set if not NULL
 *      FALSE on failure
 *
 * Side effects:
 *      None.
 *
 *      This seems to be a very expensive call: like 5ms on 1GHz P3 running
 *      RH6.1 Linux 2.2.12-20.  Yes, that's 5 milliseconds.  So caller should
 *      take care.  -- edward
 *
 *-----------------------------------------------------------------------------
 */

static Bool
HostinfoSysinfo(uint64 *totalRam,  // OUT: Total RAM in bytes
                uint64 *freeRam,   // OUT: Free RAM in bytes
                uint64 *totalSwap, // OUT: Total swap in bytes
                uint64 *freeSwap)  // OUT: Free swap in bytes
{
#ifdef HAVE_SYSINFO
   // Found in linux/include/kernel.h for a 2.5.6 kernel --hpreg
   struct vmware_sysinfo {
	   long uptime;			/* Seconds since boot */
	   unsigned long loads[3];	/* 1, 5, and 15 minute load averages */
	   unsigned long totalram;	/* Total usable main memory size */
	   unsigned long freeram;	/* Available memory size */
	   unsigned long sharedram;	/* Amount of shared memory */
	   unsigned long bufferram;	/* Memory used by buffers */
	   unsigned long totalswap;	/* Total swap space size */
	   unsigned long freeswap;	/* swap space still available */
	   unsigned short procs;	/* Number of current processes */
	   unsigned short pad;		/* explicit padding for m68k */
	   unsigned long totalhigh;	/* Total high memory size */
	   unsigned long freehigh;	/* Available high memory size */
	   unsigned int mem_unit;	/* Memory unit size in bytes */
	   // Padding: libc5 uses this..
	   char _f[20 - 2 * sizeof(long) - sizeof(int)];
   };
   struct vmware_sysinfo si;

   if (sysinfo((struct sysinfo *)&si) < 0) {
      return FALSE;
   }
   
   if (si.mem_unit == 0) {
      /*
       * Kernel versions < 2.3.23. Those kernels used a smaller sysinfo
       * structure, whose last meaningful field is 'procs' --hpreg
       */

      si.mem_unit = 1;
   }

   if (totalRam) {
      *totalRam = (uint64)si.totalram * si.mem_unit;
   }
   if (freeRam) {
      *freeRam = (uint64)si.freeram * si.mem_unit;
   }
   if (totalSwap) {
      *totalSwap = (uint64)si.totalswap * si.mem_unit;
   }
   if (freeSwap) {
      *freeSwap = (uint64)si.freeswap * si.mem_unit;
   }

   return TRUE;
#else // ifdef HAVE_SYSINFO
   NOT_IMPLEMENTED();
#endif // ifdef HAVE_SYSINFO
}
#endif // ifndef __APPLE__


#if defined(__linux__) || defined(__FreeBSD__) || defined(sun)
/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoGetLinuxMemoryInfoInPages --
 *
 *      Obtain the minimum memory to be maintained, total memory available,
 *      and free memory available on the host (Linux) in pages.
 *
 * Results:
 *      TRUE on success: '*minSize', '*maxSize' and '*currentSize' are set
 *      FALSE on failure
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

Bool
HostinfoGetLinuxMemoryInfoInPages(unsigned int *minSize,      // OUT:
                                  unsigned int *maxSize,      // OUT:
                                  unsigned int *currentSize)  // OUT:
{
   uint64 total; 
   uint64 free;
   unsigned int cached = 0;
   
   /*
    * Note that the free memory provided by linux does not include buffer and
    * cache memory. Linux tries to use the free memory to cache file. Most of
    * those memory can be freed immediately when free memory is low,
    * so for our purposes it should be counted as part of the free memory .
    * There is no good way to collect the useable free memory in 2.2 and 2.4
    * kernel.
    *
    * Here is our solution: The free memory we report includes cached memory.
    * Mmapped memory is reported as cached. The guest RAM memory, which is
    * mmaped to a ram file, therefore make up part of the cached memory. We
    * exclude the size of the guest RAM from the amount of free memory that we
    * report here. Since we don't know about the RAM size of other VMs, we
    * leave that to be done in serverd/MUI.
    */

   if (HostinfoSysinfo(&total, &free, NULL, NULL) == FALSE) {
      return FALSE;
   }

   /*
    * Convert to pages and round up total memory to the nearest multiple of 8
    * or 32 MB, since the "total" amount of memory reported by Linux is the
    * total physical memory - amount used by the kernel.
    */

   if (total < (uint64)128 * 1024 * 1024) {
      total = ROUNDUP(total, (uint64)8 * 1024 * 1024);
   } else {
      total = ROUNDUP(total, (uint64)32 * 1024 * 1024);
   }

   *minSize = 128; // XXX - Figure out this value
   *maxSize = total / PAGE_SIZE;

   HostinfoGetMemInfo("Cached:", &cached);
   if (currentSize) {
      *currentSize = free / PAGE_SIZE + cached / (PAGE_SIZE / 1024);
   }

   return TRUE;
}


/*
 *-----------------------------------------------------------------------------
 *
 * HostinfoGetSwapInfoInPages --
 *
 *      Obtain the total swap and free swap on the host (Linux) in
 *      pages.
 *
 * Results:
 *      TRUE on success: '*totalSwap' and '*freeSwap' are set if not NULL
 *      FALSE on failure
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

Bool
Hostinfo_GetSwapInfoInPages(unsigned int *totalSwap,  // OUT:
                            unsigned int *freeSwap)   // OUT:
{
   uint64 total; 
   uint64 free;

   if (HostinfoSysinfo(NULL, NULL, &total, &free) == FALSE) {
      return FALSE;
   }

   if (totalSwap != NULL) {
      *totalSwap = total / PAGE_SIZE;
   }

   if (freeSwap != NULL) {
      *freeSwap = free / PAGE_SIZE;
   }

   return TRUE;
}
#endif


/*
 *-----------------------------------------------------------------------------
 *
 * Hostinfo_GetMemoryInfoInPages --
 *
 *      Obtain the minimum memory to be maintained, total memory available,
 *      and free memory available on the host in pages.
 *
 * Results:
 *      TRUE on success: '*minSize', '*maxSize' and '*currentSize' are set
 *      FALSE on failure
 *
 * Side effects:
 *      None
 *
 *-----------------------------------------------------------------------------
 */

Bool
Hostinfo_GetMemoryInfoInPages(unsigned int *minSize,      // OUT:
                              unsigned int *maxSize,      // OUT:
                              unsigned int *currentSize)  // OUT:
{
#if defined(__APPLE__)
   mach_msg_type_number_t count;
   vm_statistics_data_t stat;
   kern_return_t error;
   uint64_t memsize;
   size_t memsizeSize = sizeof memsize;

   /*
    * Largely inspired by
    * darwinsource-10.4.5/top-15/libtop.c::libtop_p_vm_sample().
    */

   count = HOST_VM_INFO_COUNT;
   error = host_statistics(mach_host_self(), HOST_VM_INFO,
                           (host_info_t) &stat, &count);

   if (error != KERN_SUCCESS || count != HOST_VM_INFO_COUNT) {
      Warning("%s: Unable to retrieve host vm stats.\n", __FUNCTION__);

      return FALSE;
   }

   // XXX Figure out this value.
   *minSize = 128;

   /*
    * XXX Hopefully this includes cached memory as well. We should check.
    * No. It returns only completely used pages.
    */

   *currentSize = stat.free_count;

   /*
    * Adding up the stat values does not sum to 100% of physical memory.
    * The correct value is available from sysctl so we do that instead.
    */

   if (sysctlbyname("hw.memsize", &memsize, &memsizeSize, NULL, 0) == -1) {
      Warning("%s: Unable to retrieve host vm hw.memsize.\n", __FUNCTION__);

      return FALSE;
   }

   *maxSize = memsize / PAGE_SIZE;
   return TRUE;
#elif defined(VMX86_SERVER)
   uint64 total; 
   uint64 free;
   VMK_ReturnStatus status;

   if (VmkSyscall_Init(FALSE, NULL, 0)) {
      status = VMKernel_GetMemSize(&total, &free);
      if (status == VMK_OK) {
         *minSize = 128;
         *maxSize = total / PAGE_SIZE;
         *currentSize = free / PAGE_SIZE;

         return TRUE;
      }
   }

   return FALSE;
#else
   return HostinfoGetLinuxMemoryInfoInPages(minSize, maxSize, currentSize);
#endif
}


/*
 *-----------------------------------------------------------------------------
 *
 * Hostinfo_GetModulePath --
 *
 *	Retrieve the full path to the executable. Not supported under VMvisor.
 *
 *      The value can be controlled by the invoking user, so the calling code
 *      should perform extra checks if it is going to use the value to
 *      open/exec content in a security-sensitive context.
 *
 * Results:
 *      On success: The allocated, NUL-terminated file path.
 *         Note: This path can be a symbolic or hard link; it's just one
 *         possible path to access the executable.
 *
 *      On failure: NULL.
 *
 * Side effects:
 *	None
 *
 *-----------------------------------------------------------------------------
 */

Unicode
Hostinfo_GetModulePath(uint32 priv)  // IN:
{
   Unicode path;

#if defined(__APPLE__)
   uint32_t pathSize = FILE_MAXPATH;
#else
   uid_t uid = -1;
#endif

   if ((priv != HGMP_PRIVILEGE) && (priv != HGMP_NO_PRIVILEGE)) {
      Warning("%s: invalid privilege parameter\n", __FUNCTION__);

      return NULL;
   }

#if defined(__APPLE__)
   path = Util_SafeMalloc(pathSize);
   if (_NSGetExecutablePath(path, &pathSize)) {
      Warning(LGPFX" %s: _NSGetExecutablePath failed.\n", __FUNCTION__);
      free(path);

      return NULL;
   }

#else
#if defined(VMX86_SERVER)
   if (HostType_OSIsVMK()) {
      return NULL;
   }
#endif

   // "/proc/self/exe" only exists on Linux 2.2+.
   ASSERT(Hostinfo_OSVersion(0) > 2 ||
          (Hostinfo_OSVersion(0) == 2 && Hostinfo_OSVersion(1) >= 2));

   if (priv == HGMP_PRIVILEGE) {
      uid = Id_BeginSuperUser();
   }

   path = Posix_ReadLink("/proc/self/exe");

   if (priv == HGMP_PRIVILEGE) {
      Id_EndSuperUser(uid);
   }

   if (path == NULL) {
      Warning(LGPFX" %s: readlink failed: %s\n", __FUNCTION__,
              Err_Errno2String(errno));
   }
#endif

   return path;
}


/*
 *----------------------------------------------------------------------
 *
 * Hostinfo_GetLibraryPath --
 *
 *      Try and deduce the path to the library where the specified
 *      address resides. Expected usage is that the caller will pass
 *      in the address of one of the caller's own functions.
 *
 *      Not implemented on MacOS.
 *
 * Results:
 *      The path (which MAY OR MAY NOT BE ABSOLUTE) or NULL on failure.
 *
 * Side effects:
 *      Memory is allocated.
 *
 *----------------------------------------------------------------------
 */

char *
Hostinfo_GetLibraryPath(void *addr)  // IN
{
#ifdef __linux__
   Dl_info info;

   if (dladdr(addr, &info)) {
      return Unicode_Alloc(info.dli_fname, STRING_ENCODING_DEFAULT);
   }
   return NULL;
#else
   return NULL;
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * Hostinfo_QueryProcessExistence --
 *
 *      Determine if a PID is "alive" or "dead". Failing to be able to
 *      do this perfectly, do not make any assumption - say the answer
 *      is unknown.
 *
 * Results:
 *      HOSTINFO_PROCESS_QUERY_ALIVE    Process is alive
 *      HOSTINFO_PROCESS_QUERY_DEAD     Process is dead
 *      HOSTINFO_PROCESS_QUERY_UNKNOWN  Don't know
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

HostinfoProcessQuery
Hostinfo_QueryProcessExistence(int pid)  // IN:
{
   HostinfoProcessQuery ret;
   int err = (kill(pid, 0) == -1) ? errno : 0;

   switch (err) {
   case 0:
   case EPERM:
      ret = HOSTINFO_PROCESS_QUERY_ALIVE;
      break;
   case ESRCH:
      ret = HOSTINFO_PROCESS_QUERY_DEAD;
      break;
   default:
      ret = HOSTINFO_PROCESS_QUERY_UNKNOWN;
      break;
   }

   return ret;
}
