################################################################################
### Copyright 2007 VMware, Inc.  All rights reserved.
###
### Top-level Makefile for building the VMware OSS Tools.
###
###
### This program is free software; you can redistribute it and/or modify
### it under the terms of version 2 of the GNU General Public License as
### published by the Free Software Foundation.
###
### This program is distributed in the hope that it will be useful,
### but WITHOUT ANY WARRANTY; without even the implied warranty of
### MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
### GNU General Public License for more details.
###
### You should have received a copy of the GNU General Public License
### along with this program; if not, write to the Free Software
### Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
################################################################################

# These flags get passed to aclocal when autoreconf calls it, and tell aclocal
# that all of our macros are in the 'm4' subdirectory.
ACLOCAL_AMFLAGS = -I m4

SUBDIRS =
SUBDIRS += lib
SUBDIRS += libvmtools
SUBDIRS += libhgfs
SUBDIRS += checkvm
SUBDIRS += libguestlib
SUBDIRS += rpctool
SUBDIRS += scripts
SUBDIRS += services
if HAVE_FUSE
   SUBDIRS += vmblock-fuse
endif
SUBDIRS += xferlogs
if ENABLE_TESTS
   SUBDIRS += tests
endif

