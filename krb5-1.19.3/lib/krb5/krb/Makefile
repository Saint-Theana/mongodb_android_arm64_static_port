############################################################
## config/pre.in
## common prefix for all Makefile.in in the Kerberos V5 tree.
##

# These are set per-directory by autoconf 2.52 and 2.53:
#  srcdir=.
#  top_srcdir=../../..
# but these are only set by autoconf 2.53, and thus not useful to us on
# macOS yet (as of 10.2):
#  abs_srcdir=/root/krb5-1.19.3/src/lib/krb5/krb
#  abs_top_srcdir=/root/krb5-1.19.3/src
#  builddir=.
#  abs_builddir=/root/krb5-1.19.3/src/lib/krb5/krb
#  top_builddir=../../..
#  abs_top_builddir=/root/krb5-1.19.3/src
# The "top" variables refer to the directory with the configure (or
# config.status) script.

WHAT = unix
SHELL=/bin/sh

all: all-$(WHAT)

clean: clean-$(WHAT)

distclean: distclean-$(WHAT)

install: install-$(WHAT)

check: check-$(WHAT)

install-headers: install-headers-$(WHAT)

##############################
# Recursion rule support
#

# The commands for the recursion targets live in config/post.in.
#
# General form of recursion rules:
#
# Each recursive target foo-unix has related targets: foo-prerecurse,
# foo-recurse, and foo-postrecurse
#
# The foo-recurse rule is in post.in.  It is what actually recursively
# calls make.
#
# foo-recurse depends on foo-prerecurse, so any targets that must be
# built before descending into subdirectories must be dependencies of
# foo-prerecurse.
#
# foo-postrecurse depends on foo-recurse, but targets that must be
# built after descending into subdirectories should be have
# foo-recurse as dependencies in addition to being listed under
# foo-postrecurse, to avoid ordering issues.
#
# The foo-prerecurse, foo-recurse, and foo-postrecurse rules are all
# single-colon rules, to avoid nasty ordering problems with
# double-colon rules.
#
# e.g.
# all: includes foo
# foo:
#	echo foo
# includes:
#	echo bar
# includes:
#	echo baz
#
# will result in "bar", "foo", "baz" on AIX, and possibly others.
all-unix: all-postrecurse
all-postrecurse: all-recurse
all-recurse: all-prerecurse

all-prerecurse:
all-postrecurse:

clean-unix:: clean-postrecurse
clean-postrecurse: clean-recurse
clean-recurse: clean-prerecurse

clean-prerecurse:
clean-postrecurse:

distclean-unix: distclean-postrecurse
distclean-postrecurse: distclean-recurse
distclean-recurse: distclean-prerecurse

distclean-prerecurse:
distclean-postrecurse:

install-unix: install-postrecurse
install-postrecurse: install-recurse
install-recurse: install-prerecurse

install-prerecurse:
install-postrecurse:

install-headers-unix: install-headers-postrecurse
install-headers-postrecurse: install-headers-recurse
install-headers-recurse: install-headers-prerecurse

install-headers-prerecurse:
install-headers-postrecurse:

check-unix: check-postrecurse
check-postrecurse: check-recurse
check-recurse: check-prerecurse

check-prerecurse:
check-postrecurse:

Makefiles: Makefiles-postrecurse
Makefiles-postrecurse: Makefiles-recurse
Makefiles-recurse: Makefiles-prerecurse

Makefiles-prerecurse:
Makefiles-postrecurse:

generate-files-mac: generate-files-mac-postrecurse
generate-files-mac-postrecurse: generate-files-mac-recurse
generate-files-mac-recurse: generate-files-mac-prerecurse
generate-files-mac-prerecurse:

#
# end recursion rule support
##############################

# Directory syntax:
#
# begin relative path
REL=
# this is magic... should only be used for preceding a program invocation
C=./
# "/" for UNIX, "\" for Windows; *sigh*
S=/

#
srcdir = .
top_srcdir = ../../..

CONFIG_RELTOPDIR = .

# DEFS		set by configure
# DEFINES	set by local Makefile.in
# LOCALINCLUDES	set by local Makefile.in
# CPPFLAGS	user override
# CFLAGS	user override but starts off set by configure
# WARN_CFLAGS	user override but starts off set by configure
# PTHREAD_CFLAGS set by configure, not included in CFLAGS so that we
#		don't pull the pthreads library into shared libraries
# ASAN_FLAGS    set by configure when --enable-asan is used
ALL_CFLAGS = $(DEFS) $(DEFINES) $(KRB_INCLUDES) $(LOCALINCLUDES) \
	-DKRB5_DEPRECATED=1 \
	-DKRB5_PRIVATE \
	$(CPPFLAGS) $(CFLAGS) $(WARN_CFLAGS) $(PTHREAD_CFLAGS) $(ASAN_FLAGS)
ALL_CXXFLAGS = $(DEFS) $(DEFINES) $(KRB_INCLUDES) $(LOCALINCLUDES) \
	-DKRB5_DEPRECATED=1 \
	-DKRB5_PRIVATE \
	$(CPPFLAGS) $(CXXFLAGS) $(WARN_CXXFLAGS) $(PTHREAD_CFLAGS) \
	$(ASAN_FLAGS)

CFLAGS =  -D_PASSWORD_LEN=PASS_MAX -I/root/usr/include -march=armv8-a -mtune=cortex-a78 -fuse-ld=lld -DANDROID
CXXFLAGS = -I/root/usr/include
WARN_CFLAGS =  -Werror=unknown-warning-option -Wall -Wcast-align -Wshadow -Wmissing-prototypes -Wno-format-zero-length -Woverflow -Wstrict-overflow -Wmissing-format-attribute -Wmissing-prototypes -Wreturn-type -Wmissing-braces -Wparentheses -Wswitch -Wunused-function -Wunused-label -Wunused-variable -Wunused-value -Wunknown-pragmas -Wsign-compare -Wnewline-eof -Werror=uninitialized -Werror=pointer-arith -Werror=int-conversion -Werror=incompatible-pointer-types -Werror=implicit-int -Werror=declaration-after-statement -Werror-implicit-function-declaration
WARN_CXXFLAGS =  -Wall -Wcast-align -Wshadow
ASAN_FLAGS = 
PTHREAD_CFLAGS = -Qunused-arguments -pthread
PTHREAD_LIBS = 
THREAD_LINKOPTS = $(PTHREAD_CFLAGS) $(PTHREAD_LIBS)
CPPFLAGS = 
DEFS = -DHAVE_CONFIG_H
CC = aarch64-linux-android26-clang
CXX = aarch64-linux-android26-clang++
LD = $(PURE) /opt/android-ndk-r23c/toolchains/llvm/prebuilt/linux-x86_64/bin/ld.lld
KRB_INCLUDES = -I$(BUILDTOP)/include -I$(top_srcdir)/include
LDFLAGS = -L/root/usr/lib -landroid-glob
LIBS = 

INSTALL=/usr/bin/install -c
INSTALL_STRIP=
INSTALL_PROGRAM=${INSTALL} $(INSTALL_STRIP)
INSTALL_SCRIPT=${INSTALL}
INSTALL_DATA=${INSTALL} -m 644
INSTALL_SHLIB=$(INSTALL)
INSTALL_SETUID=$(INSTALL) $(INSTALL_STRIP) -m 4755 -o root
## This is needed because autoconf will sometimes define ${prefix} to be
## ${prefix}.
prefix=/root/usr
INSTALL_PREFIX=$(prefix)
INSTALL_EXEC_PREFIX=${prefix}
exec_prefix=${prefix}
datarootdir=${prefix}/share
localstatedir=${prefix}/var
runstatedir=${localstatedir}/run
datadir = ${datarootdir}
EXAMPLEDIR = $(datadir)/examples/krb5

KRB5MANROOT = ${datarootdir}/man
ADMIN_BINDIR = ${exec_prefix}/sbin
SERVER_BINDIR = ${exec_prefix}/sbin
CLIENT_BINDIR =${exec_prefix}/bin
PKGCONFIG_DIR = ${exec_prefix}/lib/pkgconfig
ADMIN_MANDIR = $(KRB5MANROOT)/man8
SERVER_MANDIR = $(KRB5MANROOT)/man8
CLIENT_MANDIR = $(KRB5MANROOT)/man1
FILE_MANDIR = $(KRB5MANROOT)/man5
ADMIN_CATDIR = $(KRB5MANROOT)/cat8
SERVER_CATDIR = $(KRB5MANROOT)/cat8
CLIENT_CATDIR = $(KRB5MANROOT)/cat1
FILE_CATDIR = $(KRB5MANROOT)/cat5
OVERVIEW_MANDIR = $(KRB5MANROOT)/man7
OVERVIEW_CATDIR = $(KRB5MANROOT)/cat7
KRB5_LIBDIR = ${exec_prefix}/lib
KRB5_INCDIR = ${prefix}/include
MODULE_DIR = ${exec_prefix}/lib/krb5/plugins
KRB5_DB_MODULE_DIR = $(MODULE_DIR)/kdb
KRB5_PA_MODULE_DIR = $(MODULE_DIR)/preauth
KRB5_AD_MODULE_DIR = $(MODULE_DIR)/authdata
KRB5_LIBKRB5_MODULE_DIR = $(MODULE_DIR)/libkrb5
KRB5_TLS_MODULE_DIR = $(MODULE_DIR)/tls
KRB5_LOCALEDIR = ${datarootdir}/locale
GSS_MODULE_DIR = ${exec_prefix}/lib/gss
KRB5_INCSUBDIRS = \
	$(KRB5_INCDIR)/kadm5 \
	$(KRB5_INCDIR)/krb5 \
	$(KRB5_INCDIR)/gssapi \
	$(KRB5_INCDIR)/gssrpc

#
# Macros used by the KADM5 (OV-based) unit test system.
# XXX check which of these are actually used!
#
SKIPTESTS	= $(BUILDTOP)/skiptests
TESTDIR		= $(BUILDTOP)/kadmin/testing
STESTDIR	= $(top_srcdir)/kadmin/testing
ENV_SETUP	= $(TESTDIR)/scripts/env-setup.sh
CLNTTCL		= $(TESTDIR)/util/kadm5_clnt_tcl
SRVTCL		= $(TESTDIR)/util/kadm5_srv_tcl
# Dejagnu variables.
# We have to set the host with --host so that setup_xfail will work.
# If we don't set it, then the host type used is "native", which
# doesn't match "*-*-*".
host=aarch64-unknown-linux-android
DEJAFLAGS	= --debug --srcdir $(srcdir) --host $(host)
RUNTEST		= runtest $(DEJAFLAGS)

RUNPYTEST	= PYTHONPATH=$(top_srcdir)/util VALGRIND="$(VALGRIND)" \
			$(PYTHON)

START_SERVERS	= $(STESTDIR)/scripts/start_servers $(TEST_SERVER) $(TEST_PATH)
START_SERVERS_LOCAL = $(STESTDIR)/scripts/start_servers_local

STOP_SERVERS	= $(STESTDIR)/scripts/stop_servers $(TEST_SERVER) $(TEST_PATH)
STOP_SERVERS_LOCAL = $(STESTDIR)/scripts/stop_servers_local
#
# End of macros for the KADM5 unit test system.
#

transform = s,x,x,

RM = rm -f
CP = cp
MV = mv -f
RANLIB = /opt/android-ndk-r23c/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-ranlib
AWK = gawk
YACC = bison -y
PERL = perl
PYTHON = python3
AUTOCONF = autoconf
AUTOCONFFLAGS =
AUTOHEADER = autoheader
AUTOHEADERFLAGS =
MOVEIFCHANGED = $(top_srcdir)/config/move-if-changed

TOPLIBD = $(BUILDTOP)/lib

OBJEXT = o
EXEEXT =

#
# variables for libraries, for use in linking programs
# -- this may want to get broken out into a separate frag later
#
# invocation is like:
# prog: foo.o bar.o $(KRB5_BASE_DEPLIBS)
# 	$(CC_LINK) -o $@ foo.o bar.o $(KRB5_BASE_LIBS)

CC_LINK=$(CC) $(PROG_LIBPATH) $(PROG_RPATH_FLAGS) $(CFLAGS) $(LDFLAGS) $(ASAN_FLAGS)
CXX_LINK=$(CXX) $(PROG_LIBPATH) $(PROG_RPATH_FLAGS) $(CXXFLAGS) $(LDFLAGS) $(ASAN_FLAGS)

# Makefile.in files which build programs can override the list of
# directories to look for dependent libraries in (in the form -Ldir1
# -Ldir2 ...) and also the list of rpath directories to search (in the
# form dir1:dir2:...).
PROG_LIBPATH=-L$(TOPLIBD)
PROG_RPATH=$(KRB5_LIBDIR)

# Library Makefile.in files can override this list of directories to
# look for dependent libraries in (in the form -Ldir1 -Ldir2 ...) and
# also the list of rpath directories to search (in the form
# dir1:dir2:...)
SHLIB_DIRS=-L$(TOPLIBD)
SHLIB_RDIRS=$(KRB5_LIBDIR)

# Multi-directory library Makefile.in files should override this list
# of object files with the full list.
STOBJLISTS=OBJS.ST

# prefix (with no spaces after) for rpath flag to cc
RPATH_FLAG=-Wl,--enable-new-dtags -Wl,-rpath -Wl,

# link flags to add PROG_RPATH to the rpath
PROG_RPATH_FLAGS=$(RPATH_FLAG)$(PROG_RPATH)

# this gets set by configure to either $(STLIBEXT) or $(SHLIBEXT),
# depending on whether we're building with shared libraries.
DEPLIBEXT=.a

KDB5_PLUGIN_DEPLIBS = $(TOPLIBD)/libkrb5_db2$(DEPLIBEXT) $(TOPLIBD)/libkadm5srv_mit$(DEPLIBEXT)
KDB5_PLUGIN_LIBS = -lkrb5_db2 -lkadm5srv_mit

KADMCLNT_DEPLIB	= $(TOPLIBD)/libkadm5clnt_mit$(DEPLIBEXT)
KADMSRV_DEPLIB	= $(TOPLIBD)/libkadm5srv_mit$(DEPLIBEXT)
KDB5_DEPLIB	= $(TOPLIBD)/libkdb5$(DEPLIBEXT)
GSSRPC_DEPLIB	= $(TOPLIBD)/libgssrpc$(DEPLIBEXT)
GSS_DEPLIB	= $(TOPLIBD)/libgssapi_krb5$(DEPLIBEXT)
KRB5_DEPLIB	= $(TOPLIBD)/libkrb5$(DEPLIBEXT)
CRYPTO_DEPLIB	= $(TOPLIBD)/libk5crypto$(DEPLIBEXT)
COM_ERR_DEPLIB	= $(COM_ERR_DEPLIB-k5)
COM_ERR_DEPLIB-sys = # empty
COM_ERR_DEPLIB-intlsys = # empty
COM_ERR_DEPLIB-k5 = $(TOPLIBD)/libcom_err$(DEPLIBEXT)
COM_ERR_LIB = -lcom_err
SUPPORT_LIBNAME=krb5support
SUPPORT_DEPLIB	= $(TOPLIBD)/lib$(SUPPORT_LIBNAME)$(DEPLIBEXT)

# These are forced to use ".a" as an extension because they're never
# built shared.
SS_DEPLIB	= $(SS_DEPLIB-k5)
SS_DEPLIB-k5	= $(TOPLIBD)/libss.a
SS_DEPLIB-sys	=
APPUTILS_DEPLIB	= $(TOPLIBD)/libapputils.a

KRB5_BASE_DEPLIBS	= $(KRB5_DEPLIB) $(CRYPTO_DEPLIB) $(COM_ERR_DEPLIB) $(SUPPORT_DEPLIB)
KDB5_DEPLIBS		= $(KDB5_DEPLIB) $(KDB5_PLUGIN_DEPLIBS)
GSS_DEPLIBS		= $(GSS_DEPLIB)
GSSRPC_DEPLIBS		= $(GSSRPC_DEPLIB) $(GSS_DEPLIBS)
KADM_COMM_DEPLIBS	= $(GSSRPC_DEPLIBS) $(KDB5_DEPLIBS) $(GSSRPC_DEPLIBS)
KADMSRV_DEPLIBS		= $(KADMSRV_DEPLIB) $(KDB5_DEPLIBS) $(KADM_COMM_DEPLIBS)
KADMCLNT_DEPLIBS	= $(KADMCLNT_DEPLIB) $(KADM_COMM_DEPLIBS)

# Header file dependencies we might override.
# See util/depfix.sed.
# Also see depend-verify-* in post.in, which wants to confirm that we're using
# the in-tree versions.
COM_ERR_VERSION = k5
COM_ERR_DEPS	= $(COM_ERR_DEPS-k5)
COM_ERR_DEPS-sys =
COM_ERR_DEPS-intlsys =
COM_ERR_DEPS-k5	= $(BUILDTOP)/include/com_err.h
SS_VERSION	= k5
SS_DEPS		= $(SS_DEPS-k5)
SS_DEPS-sys	=
SS_DEPS-k5	= $(BUILDTOP)/include/ss/ss.h $(BUILDTOP)/include/ss/ss_err.h
VERTO_VERSION	= k5
VERTO_DEPS	= $(VERTO_DEPS-k5)
VERTO_DEPS-sys	=
VERTO_DEPS-k5	= $(BUILDTOP)/include/verto.h

# LIBS gets substituted in... e.g. -lnsl -lsocket

# GEN_LIB is -lgen if needed for regexp
GEN_LIB		= 

# Editline or readline flags and libraries.
RL_CFLAGS	= 
RL_LIBS		= 

SS_LIB		= $(SS_LIB-k5)
SS_LIB-sys	= 
SS_LIB-k5	= $(TOPLIBD)/libss.a $(RL_LIBS)
KDB5_LIB	= -lkdb5 $(KDB5_PLUGIN_LIBS)

VERTO_DEPLIB	= $(VERTO_DEPLIB-k5)
VERTO_DEPLIB-sys = # empty
VERTO_DEPLIB-k5	= $(TOPLIBD)/libverto$(DEPLIBEXT)
VERTO_CFLAGS	= 
VERTO_LIBS	= -lverto

DL_LIB		= 

CMOCKA_LIBS	= 
LDAP_LIBS	= 
LMDB_LIBS	= 

KRB5_LIB			= -lkrb5
K5CRYPTO_LIB			= -lk5crypto
GSS_KRB5_LIB			= -lgssapi_krb5
SUPPORT_LIB			= -l$(SUPPORT_LIBNAME)

# HESIOD_LIBS is -lhesiod...
HESIOD_LIBS	= 

KRB5_BASE_LIBS	= $(KRB5_LIB) $(K5CRYPTO_LIB) $(COM_ERR_LIB) $(SUPPORT_LIB) $(GEN_LIB) $(LIBS) $(DL_LIB)
KDB5_LIBS	= $(KDB5_LIB) $(GSSRPC_LIBS)
GSS_LIBS	= $(GSS_KRB5_LIB)
# needs fixing if ever used on macOS!
GSSRPC_LIBS	= -lgssrpc $(GSS_LIBS)
KADM_COMM_LIBS	= $(GSSRPC_LIBS)
# need fixing if ever used on macOS!
KADMSRV_LIBS	= -lkadm5srv_mit $(HESIOD_LIBS) $(KDB5_LIBS) $(KADM_COMM_LIBS)
KADMCLNT_LIBS	= -lkadm5clnt_mit $(KADM_COMM_LIBS)

# Misc stuff for linking server programs (and maybe some others,
# eventually) but which we don't want to install.
APPUTILS_LIB	= -lapputils

# So test programs can find their libraries without "make install", etc.
RUN_SETUP=LD_LIBRARY_PATH=`echo $(PROG_LIBPATH) | sed -e "s/-L//g" -e "s/ /:/g"`
RUN_VARS=LD_LIBRARY_PATH

# Appropriate command prefix for most C test programs: use libraries
# from the build tree, avoid referencing the installed krb5.conf and
# message catalog, and use valgrind when asked.
RUN_TEST=$(RUN_SETUP) KRB5_CONFIG=$(top_srcdir)/config-files/krb5.conf \
    LC_ALL=C $(VALGRIND)

#
# variables for --with-tcl=
TCL_LIBS	= 
TCL_LIBPATH	= 
TCL_RPATH	= 
TCL_MAYBE_RPATH = 
TCL_INCLUDES	= 

# Crypto and PRNG back-end selections
CRYPTO_IMPL	= builtin
PRNG_ALG	= fortuna

# TLS implementation selection
TLS_IMPL	= openssl
TLS_IMPL_CFLAGS = 
TLS_IMPL_LIBS	= -lssl -lcrypto

# SPAKE preauth back-end libraries
SPAKE_OPENSSL_LIBS = -lcrypto

# Whether we have the SASL header file for the LDAP KDB module
HAVE_SASL = 

# Whether we are building support for NIST SPAKE groups using OpenSSL
HAVE_SPAKE_OPENSSL = yes

# Whether we are building the LMDB KDB module
HAVE_LMDB = no

# Whether we have libresolv 1.1.5 for URI discovery tests
HAVE_RESOLV_WRAPPER = 0

SIZEOF_TIME_T = 8

# error table rules
#
### /* these are invoked as $(...) foo.et, which works, but could be better */
COMPILE_ET= $(COMPILE_ET-k5)
COMPILE_ET-sys= compile_et
COMPILE_ET-intlsys= compile_et --textdomain mit-krb5
COMPILE_ET-k5= $(BUILDTOP)/util/et/compile_et -d $(top_srcdir)/util/et \
	--textdomain mit-krb5

.SUFFIXES:  .h .c .et .ct

# These versions cause both .c and .h files to be generated at once.
# But GNU make doesn't understand this, and parallel builds can trigger
# both of them at once, causing them to stomp on each other.  The versions
# below only update one of the files, so compile_et has to get run twice,
# but it won't break parallel builds.
#.et.h: ; $(COMPILE_ET) $<
#.et.c: ; $(COMPILE_ET) $<

.et.h:
	$(RM) et-h-$*.et et-h-$*.c et-h-$*.h
	$(CP) $< et-h-$*.et
	$(COMPILE_ET) et-h-$*.et
	$(MV) et-h-$*.h $*.h
	$(RM) et-h-$*.et et-h-$*.c
.et.c:
	$(RM) et-c-$*.et et-c-$*.c et-c-$*.h
	$(CP) $< et-c-$*.et
	$(COMPILE_ET) et-c-$*.et
	$(MV) et-c-$*.c $*.c
	$(RM) et-c-$*.et et-c-$*.h

# rule to make object files
#
.SUFFIXES: .cpp .c .o
.c.o:
	$(CC) $(ALL_CFLAGS) -c $<
# Use .cpp because that's what autoconf uses in its test.
# If the compiler doesn't accept a .cpp suffix here, it wouldn't
# have accepted it when autoconf tested it.
.cpp.o:
	$(CXX) $(ALL_CXXFLAGS) -c $<

# ss command table rules
#
MAKE_COMMANDS= $(MAKE_COMMANDS-k5)
MAKE_COMMANDS-sys= mk_cmds
MAKE_COMMANDS-k5= $(BUILDTOP)/util/ss/mk_cmds

.ct.c:
	$(MAKE_COMMANDS) $<

## Parameters to be set by configure for use in lib.in:
##
#
# These settings are for building shared libraries only.  Including
# libpriv.in will override with values appropriate for static
# libraries that we don't install.  Some values will depend on whether
# the platform supports major and minor version number extensions on
# shared libraries, hence the FOO_@@ settings.

LN_S=ln -s
AR=/opt/android-ndk-r23c/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-ar

# Set to "lib$(LIBBASE)$(STLIBEXT) lib$(LIBBASE)$(SHLIBEXT)" or some
# subset thereof by configure; determines which types of libs get
# built.
LIBLIST=lib$(LIBBASE)$(STLIBEXT)

# Set by configure; list of library symlinks to make to $(TOPLIBD)
LIBLINKS=$(TOPLIBD)/lib$(LIBBASE)$(STLIBEXT)

# Set by configure; name of plugin module to build (libfoo.a or foo.so)
PLUGIN=libkrb5_$(LIBBASE)$(STLIBEXT)

# Set by configure; symlink for plugin module for static plugin linking
PLUGINLINK=$(TOPLIBD)/libkrb5_$(LIBBASE)$(STLIBEXT)

# Set by configure; list of install targets for libraries
LIBINSTLIST=install-static

# Set by configure; install target
PLUGININST=install-static

# Some of these should really move to pre.in, since programs will need
# it too. (e.g. stuff that has dependencies on the libraries)

# usually .a
STLIBEXT=.a

# usually .so.$(LIBMAJOR).$(LIBMINOR)
SHLIBVEXT=.so.v-nobuild

# usually .so.$(LIBMAJOR) (to allow for major-version compat)
SHLIBSEXT=.so.s-nobuild

# usually .so
SHLIBEXT=.so-nobuild

# usually _p.a
PFLIBEXT=_p.a

#
DYNOBJEXT=$(SHLIBEXT)
MAKE_DYNOBJ_COMMAND=$(MAKE_SHLIB_COMMAND)
DYNOBJ_EXPDEPS=$(SHLIB_EXPDEPS)
DYNOBJ_EXPFLAGS=$(SHLIB_EXPFLAGS)

# For some platforms, a flag which causes shared library creation to
# check for undefined symbols.  Suppressed when using --enable-asan.
UNDEF_CHECK=-Wl,--no-undefined

# File with symbol names to be exported, both functions and data,
# currently not distinguished.
SHLIB_EXPORT_FILE=$(srcdir)/$(LIBPREFIX)$(LIBBASE).exports

# File that needs to be current for building the shared library,
# usually SHLIB_EXPORT_FILE, but not always, if we have to convert
# it to another, intermediate form for the linker.
SHLIB_EXPORT_FILE_DEP=binutils.versions

# Export file checker to run when building in maintainer mode on
# Linux.  This gets included in LDCOMBINE_TAIL.
EXPORT_CHECK_CMD = && $(PERL) -w $(top_srcdir)/util/export-check.pl \
	$(SHLIB_EXPORT_FILE) $@
EXPORT_CHECK = # $(EXPORT_CHECK_CMD)

# Command to run to build a shared library.
# In systems that require multiple commands, like AIX, it may need
# to change to rearrange where the various parameters fit in.
MAKE_SHLIB_COMMAND=$(CC) -shared -fPIC -Wl,-h,$(LIBPREFIX)$(LIBBASE)$(SHLIBSEXT) $(UNDEF_CHECK) -o $@ $$objlist $(SHLIB_EXPFLAGS) $(LDFLAGS) -Wl,--version-script binutils.versions $(EXPORT_CHECK)

# run path flags for explicit libraries depending on this one,
# e.g. "-R$(SHLIB_RPATH)"
SHLIB_RPATH_FLAGS=$(RPATH_FLAG)$(SHLIB_RDIRS)

# flags for explicit libraries depending on this one,
# e.g. "$(SHLIB_RPATH_FLAGS) $(SHLIB_SHLIB_DIRFLAGS) $(SHLIB_EXPLIBS)"
SHLIB_EXPFLAGS=$(SHLIB_RPATH_FLAGS) $(SHLIB_DIRS) $(SHLIB_EXPLIBS)

## Parameters to be set by configure for use in libobj.in:

# Set to "OBJS.ST OBJS.SH OBJS.PF" or some subset thereof by
# configure; determines which types of object files get built.
OBJLISTS=OBJS.ST

# Note that $(LIBSRCS) *cannot* contain any variable references, or
# the suffix substitution will break on some platforms!
SHLIBOBJS=$(STLIBOBJS:.o=.so)
PFLIBOBJS=$(STLIBOBJS:.o=.po)

#
# rules to make various types of object files
#
PICFLAGS=-fPIC
PROFFLAGS=-pg

# platform-dependent temporary files that should get cleaned up
EXTRA_FILES=

VALGRIND=
# Need absolute paths here because under kshd or ftpd we may run programs
# while in other directories.
VALGRIND_LOGDIR = `cd $(BUILDTOP)&&pwd`
VALGRIND1 = valgrind --tool=memcheck --log-file=$(VALGRIND_LOGDIR)/vg.%p --trace-children=yes --leak-check=yes --suppressions=`cd $(top_srcdir)&&pwd`/util/valgrind-suppressions

# Set OFFLINE=yes to disable tests that assume network connectivity.
# (Specifically, this concerns the ability to fetch DNS data for
# mit.edu, to verify that SRV queries are working.)  Note that other
# tests still assume that the local hostname can be resolved into
# something that looks like an FQDN, with an IPv4 address.
OFFLINE=no

# Used when running Python tests.
PYTESTFLAGS=

##
## end of pre.in
############################################################
mydir=lib$(S)krb5$(S)krb
BUILDTOP=$(REL)..$(S)..$(S)..
LOCALINCLUDES = -I$(srcdir)/../os -I$(top_srcdir)
DEFINES=-DLIBDIR=\"$(KRB5_LIBDIR)\" -DDYNOBJEXT=\"$(DYNOBJEXT)\"

# Like RUN_TEST, but use t_krb5.conf from this directory.
RUN_TEST_LOCAL_CONF=$(RUN_SETUP) KRB5_CONFIG=$(srcdir)/t_krb5.conf LC_ALL=C \
	$(VALGRIND)

##DOS##BUILDTOP = ..\..\..
##DOS##PREFIXDIR=krb
##DOS##OBJFILE=..\$(OUTPRE)$(PREFIXDIR).lst

STLIBOBJS= \
	addr_comp.o	\
	addr_order.o	\
	addr_srch.o	\
	allow_weak.o	\
	appdefault.o	\
	ai_authdata.o   \
	auth_con.o	\
	cammac_util.o	\
	authdata.o	\
	authdata_exp.o	\
	authdata_enc.o	\
	authdata_dec.o	\
	bld_pr_ext.o	\
	bld_princ.o	\
	chk_trans.o	\
	chpw.o		\
	conv_creds.o	\
	conv_princ.o	\
	copy_addrs.o	\
	copy_auth.o	\
	copy_athctr.o	\
	copy_cksum.o    \
	copy_creds.o	\
	copy_data.o	\
	copy_key.o	\
	copy_princ.o	\
	copy_tick.o	\
	cp_key_cnt.o	\
	decode_kdc.o	\
	decrypt_tk.o	\
	deltat.o	\
        enc_helper.o	\
        enc_keyhelper.o	\
	encode_kdc.o	\
	encrypt_tk.o	\
	etype_list.o	\
	fast.o \
	fwd_tgt.o	\
	gc_via_tkt.o	\
	gen_seqnum.o	\
	gen_subkey.o	\
	gen_save_subkey.o	\
	get_creds.o	\
	get_etype_info.o \
	get_in_tkt.o	\
	gic_keytab.o	\
	gic_opt.o	\
	gic_pwd.o	\
	in_tkt_sky.o	\
	init_ctx.o	\
	copy_ctx.o	\
	init_keyblock.o \
	kdc_rep_dc.o	\
	kerrs.o		\
	kfree.o		\
	libdef_parse.o	\
	mk_cred.o	\
	mk_error.o	\
	mk_priv.o	\
	mk_rep.o	\
	mk_req.o	\
	mk_req_ext.o	\
	mk_safe.o	\
	pac.o		\
	pac_sign.o	\
	padata.o	\
	parse.o		\
	parse_host_string.o	\
	plugin.o	\
	pr_to_salt.o	\
	preauth2.o	\
	preauth_ec.o	\
	preauth_encts.o	\
	preauth_otp.o	\
	preauth_pkinit.o	\
	preauth_sam2.o	\
	princ_comp.o	\
	privsafe.o	\
	random_str.o	\
	rd_cred.o	\
	rd_error.o	\
	rd_priv.o	\
	rd_rep.o	\
	rd_req.o	\
	rd_req_dec.o	\
	rd_safe.o	\
	recvauth.o	\
	response_items.o	\
	s4u_authdata.o	\
	s4u_creds.o	\
	sendauth.o	\
	send_tgs.o	\
	ser_actx.o	\
	ser_adata.o	\
	ser_addr.o	\
	ser_auth.o	\
	ser_cksum.o	\
	ser_ctx.o	\
	ser_key.o	\
	ser_princ.o	\
	serialize.o	\
	set_realm.o	\
	sname_match.o	\
	srv_dec_tkt.o	\
	srv_rcache.o	\
	str_conv.o	\
	tgtname.o	\
	unparse.o	\
	val_renew.o	\
	valid_times.o	\
	vfy_increds.o	\
	vic_opt.o	\
	walk_rtree.o

OBJS=	$(OUTPRE)addr_comp.$(OBJEXT)	\
	$(OUTPRE)addr_order.$(OBJEXT)	\
	$(OUTPRE)addr_srch.$(OBJEXT)	\
	$(OUTPRE)allow_weak.$(OBJEXT)	\
	$(OUTPRE)appdefault.$(OBJEXT)	\
	$(OUTPRE)ai_authdata.$(OBJEXT)  \
	$(OUTPRE)auth_con.$(OBJEXT)	\
	$(OUTPRE)cammac_util.$(OBJEXT)	\
	$(OUTPRE)authdata.$(OBJEXT)	\
	$(OUTPRE)authdata_exp.$(OBJEXT)	\
	$(OUTPRE)authdata_enc.$(OBJEXT)	\
	$(OUTPRE)authdata_dec.$(OBJEXT)	\
	$(OUTPRE)bld_pr_ext.$(OBJEXT)	\
	$(OUTPRE)bld_princ.$(OBJEXT)	\
	$(OUTPRE)chk_trans.$(OBJEXT)	\
	$(OUTPRE)chpw.$(OBJEXT)		\
	$(OUTPRE)conv_creds.$(OBJEXT)	\
	$(OUTPRE)conv_princ.$(OBJEXT)	\
	$(OUTPRE)copy_addrs.$(OBJEXT)	\
	$(OUTPRE)copy_auth.$(OBJEXT)	\
	$(OUTPRE)copy_athctr.$(OBJEXT)	\
	$(OUTPRE)copy_cksum.$(OBJEXT)    \
	$(OUTPRE)copy_creds.$(OBJEXT)	\
	$(OUTPRE)copy_data.$(OBJEXT)	\
	$(OUTPRE)copy_key.$(OBJEXT)	\
	$(OUTPRE)copy_princ.$(OBJEXT)	\
	$(OUTPRE)copy_tick.$(OBJEXT)	\
	$(OUTPRE)cp_key_cnt.$(OBJEXT)	\
	$(OUTPRE)decode_kdc.$(OBJEXT)	\
	$(OUTPRE)decrypt_tk.$(OBJEXT)	\
	$(OUTPRE)deltat.$(OBJEXT)	\
        $(OUTPRE)enc_helper.$(OBJEXT)	\
        $(OUTPRE)enc_keyhelper.$(OBJEXT)	\
	$(OUTPRE)encode_kdc.$(OBJEXT)	\
	$(OUTPRE)encrypt_tk.$(OBJEXT)	\
	$(OUTPRE)etype_list.$(OBJEXT)	\
	$(OUTPRE)fast.$(OBJEXT) \
	$(OUTPRE)fwd_tgt.$(OBJEXT)	\
	$(OUTPRE)gc_via_tkt.$(OBJEXT)	\
	$(OUTPRE)gen_seqnum.$(OBJEXT)	\
	$(OUTPRE)gen_subkey.$(OBJEXT)	\
	$(OUTPRE)gen_save_subkey.$(OBJEXT)	\
	$(OUTPRE)get_creds.$(OBJEXT)	\
	$(OUTPRE)get_etype_info.$(OBJEXT) \
	$(OUTPRE)get_in_tkt.$(OBJEXT)	\
	$(OUTPRE)gic_keytab.$(OBJEXT)	\
	$(OUTPRE)gic_opt.$(OBJEXT)	\
	$(OUTPRE)gic_pwd.$(OBJEXT)	\
	$(OUTPRE)in_tkt_sky.$(OBJEXT)	\
	$(OUTPRE)init_ctx.$(OBJEXT)	\
	$(OUTPRE)copy_ctx.$(OBJEXT)	\
	$(OUTPRE)init_keyblock.$(OBJEXT) \
	$(OUTPRE)kdc_rep_dc.$(OBJEXT)	\
	$(OUTPRE)kerrs.$(OBJEXT)	\
	$(OUTPRE)kfree.$(OBJEXT)	\
	$(OUTPRE)libdef_parse.$(OBJEXT)	\
	$(OUTPRE)mk_cred.$(OBJEXT)	\
	$(OUTPRE)mk_error.$(OBJEXT)	\
	$(OUTPRE)mk_priv.$(OBJEXT)	\
	$(OUTPRE)mk_rep.$(OBJEXT)	\
	$(OUTPRE)mk_req.$(OBJEXT)	\
	$(OUTPRE)mk_req_ext.$(OBJEXT)	\
	$(OUTPRE)mk_safe.$(OBJEXT)	\
	$(OUTPRE)pac.$(OBJEXT)		\
	$(OUTPRE)pac_sign.$(OBJEXT)	\
	$(OUTPRE)padata.$(OBJEXT)	\
	$(OUTPRE)parse.$(OBJEXT)	\
	$(OUTPRE)parse_host_string.$(OBJEXT)	\
	$(OUTPRE)plugin.$(OBJEXT)	\
	$(OUTPRE)pr_to_salt.$(OBJEXT)	\
	$(OUTPRE)preauth2.$(OBJEXT)	\
	$(OUTPRE)preauth_ec.$(OBJEXT)	\
	$(OUTPRE)preauth_encts.$(OBJEXT)	\
	$(OUTPRE)preauth_otp.$(OBJEXT)	\
	$(OUTPRE)preauth_pkinit.$(OBJEXT)	\
	$(OUTPRE)preauth_sam2.$(OBJEXT)	\
	$(OUTPRE)princ_comp.$(OBJEXT)	\
	$(OUTPRE)privsafe.$(OBJEXT)	\
	$(OUTPRE)random_str.$(OBJEXT)	\
	$(OUTPRE)rd_cred.$(OBJEXT)	\
	$(OUTPRE)rd_error.$(OBJEXT)	\
	$(OUTPRE)rd_priv.$(OBJEXT)	\
	$(OUTPRE)rd_rep.$(OBJEXT)	\
	$(OUTPRE)rd_req.$(OBJEXT)	\
	$(OUTPRE)rd_req_dec.$(OBJEXT)	\
	$(OUTPRE)rd_safe.$(OBJEXT)	\
	$(OUTPRE)recvauth.$(OBJEXT)	\
	$(OUTPRE)response_items.$(OBJEXT)	\
	$(OUTPRE)s4u_authdata.$(OBJEXT)	\
	$(OUTPRE)s4u_creds.$(OBJEXT)	\
	$(OUTPRE)sendauth.$(OBJEXT)	\
	$(OUTPRE)send_tgs.$(OBJEXT)	\
	$(OUTPRE)ser_actx.$(OBJEXT)	\
	$(OUTPRE)ser_adata.$(OBJEXT)	\
	$(OUTPRE)ser_addr.$(OBJEXT)	\
	$(OUTPRE)ser_auth.$(OBJEXT)	\
	$(OUTPRE)ser_cksum.$(OBJEXT)	\
	$(OUTPRE)ser_ctx.$(OBJEXT)	\
	$(OUTPRE)ser_key.$(OBJEXT)	\
	$(OUTPRE)ser_princ.$(OBJEXT)	\
	$(OUTPRE)serialize.$(OBJEXT)	\
	$(OUTPRE)set_realm.$(OBJEXT)	\
	$(OUTPRE)sname_match.$(OBJEXT)	\
	$(OUTPRE)srv_dec_tkt.$(OBJEXT)	\
	$(OUTPRE)srv_rcache.$(OBJEXT)	\
	$(OUTPRE)str_conv.$(OBJEXT)	\
	$(OUTPRE)tgtname.$(OBJEXT)	\
	$(OUTPRE)unparse.$(OBJEXT)	\
	$(OUTPRE)val_renew.$(OBJEXT)	\
	$(OUTPRE)valid_times.$(OBJEXT)	\
	$(OUTPRE)vfy_increds.$(OBJEXT)	\
	$(OUTPRE)vic_opt.$(OBJEXT)	\
	$(OUTPRE)walk_rtree.$(OBJEXT)

SRCS=	$(srcdir)/addr_comp.c	\
	$(srcdir)/addr_order.c	\
	$(srcdir)/addr_srch.c	\
	$(srcdir)/appdefault.c	\
	$(srcdir)/auth_con.c	\
	$(srcdir)/cammac_util.c	\
	$(srcdir)/ai_authdata.c \
	$(srcdir)/authdata.c	\
	$(srcdir)/authdata_exp.c	\
	$(srcdir)/authdata_enc.c	\
	$(srcdir)/authdata_dec.c	\
	$(srcdir)/bld_pr_ext.c	\
	$(srcdir)/bld_princ.c	\
	$(srcdir)/brand.c	\
	$(srcdir)/chk_trans.c	\
	$(srcdir)/chpw.c	\
	$(srcdir)/conv_creds.c	\
	$(srcdir)/conv_princ.c	\
	$(srcdir)/copy_addrs.c	\
	$(srcdir)/copy_auth.c	\
	$(srcdir)/copy_athctr.c	\
	$(srcdir)/copy_cksum.c   \
	$(srcdir)/copy_creds.c	\
	$(srcdir)/copy_data.c	\
	$(srcdir)/copy_key.c	\
	$(srcdir)/copy_princ.c	\
	$(srcdir)/copy_tick.c	\
	$(srcdir)/cp_key_cnt.c	\
	$(srcdir)/decode_kdc.c	\
	$(srcdir)/decrypt_tk.c	\
	$(srcdir)/deltat.c	\
	$(srcdir)/enc_helper.c	\
	$(srcdir)/enc_keyhelper.c	\
	$(srcdir)/encode_kdc.c	\
	$(srcdir)/encrypt_tk.c	\
	$(srcdir)/etype_list.c	\
	$(srcdir)/fast.c \
	$(srcdir)/fwd_tgt.c	\
	$(srcdir)/gc_via_tkt.c	\
	$(srcdir)/gen_seqnum.c	\
	$(srcdir)/gen_subkey.c	\
	$(srcdir)/gen_save_subkey.c	\
	$(srcdir)/get_creds.c	\
	$(srcdir)/get_etype_info.c \
	$(srcdir)/get_in_tkt.c	\
	$(srcdir)/gic_keytab.c	\
	$(srcdir)/gic_opt.c	\
	$(srcdir)/gic_pwd.c	\
	$(srcdir)/in_tkt_sky.c	\
	$(srcdir)/init_ctx.c	\
	$(srcdir)/copy_ctx.c	\
	$(srcdir)/init_keyblock.c \
	$(srcdir)/kdc_rep_dc.c	\
	$(srcdir)/kerrs.c	\
	$(srcdir)/kfree.c	\
	$(srcdir)/libdef_parse.c \
	$(srcdir)/mk_cred.c	\
	$(srcdir)/mk_error.c	\
	$(srcdir)/mk_priv.c	\
	$(srcdir)/mk_rep.c	\
	$(srcdir)/mk_req.c	\
	$(srcdir)/mk_req_ext.c	\
	$(srcdir)/mk_safe.c	\
	$(srcdir)/pac.c		\
	$(srcdir)/pac_sign.c	\
	$(srcdir)/padata.c	\
	$(srcdir)/parse.c	\
	$(srcdir)/parse_host_string.c	\
	$(srcdir)/plugin.c	\
	$(srcdir)/pr_to_salt.c	\
	$(srcdir)/preauth2.c	\
	$(srcdir)/preauth_ec.c	\
	$(srcdir)/preauth_encts.c	\
	$(srcdir)/preauth_otp.c	\
	$(srcdir)/preauth_pkinit.c	\
	$(srcdir)/preauth_sam2.c	\
	$(srcdir)/princ_comp.c	\
	$(srcdir)/privsafe.c	\
	$(srcdir)/random_str.c	\
	$(srcdir)/rd_cred.c	\
	$(srcdir)/rd_error.c	\
	$(srcdir)/rd_priv.c	\
	$(srcdir)/rd_rep.c	\
	$(srcdir)/rd_req.c	\
	$(srcdir)/rd_req_dec.c	\
	$(srcdir)/rd_safe.c	\
	$(srcdir)/recvauth.c	\
	$(srcdir)/response_items.c	\
	$(srcdir)/s4u_authdata.c\
	$(srcdir)/s4u_creds.c	\
	$(srcdir)/sendauth.c	\
	$(srcdir)/send_tgs.c	\
	$(srcdir)/ser_actx.c	\
	$(srcdir)/ser_adata.c	\
	$(srcdir)/ser_addr.c	\
	$(srcdir)/ser_auth.c	\
	$(srcdir)/ser_cksum.c	\
	$(srcdir)/ser_ctx.c	\
	$(srcdir)/ser_key.c	\
	$(srcdir)/ser_princ.c	\
	$(srcdir)/serialize.c	\
	$(srcdir)/set_realm.c	\
	$(srcdir)/sname_match.c	\
	$(srcdir)/srv_dec_tkt.c	\
	$(srcdir)/srv_rcache.c	\
	$(srcdir)/str_conv.c	\
	$(srcdir)/t_ad_fx_armor.c \
	$(srcdir)/tgtname.c	\
	$(srcdir)/unparse.c	\
	$(srcdir)/val_renew.c	\
	$(srcdir)/valid_times.c	\
	$(srcdir)/vfy_increds.c \
	$(srcdir)/vic_opt.c	\
	$(srcdir)/walk_rtree.c	\
	$(srcdir)/t_walk_rtree.c \
	$(srcdir)/t_kerb.c	\
	$(srcdir)/t_ser.c	\
	$(srcdir)/t_deltat.c	\
	$(srcdir)/t_expand.c	\
	$(srcdir)/t_get_etype_info.c \
	$(srcdir)/t_pac.c	\
	$(srcdir)/t_parse_host_string.c	\
	$(srcdir)/t_princ.c	\
	$(srcdir)/t_etypes.c    \
	$(srcdir)/t_expire_warn.c \
	$(srcdir)/t_authdata.c	\
	$(srcdir)/t_cc_config.c	\
	$(srcdir)/t_copy_context.c \
	$(srcdir)/t_in_ccache.c	\
	$(srcdir)/t_response_items.c \
	$(srcdir)/t_sname_match.c \
	$(srcdir)/t_valid_times.c \
	$(srcdir)/t_vfy_increds.c

# Someday, when we have a "maintainer mode", do this right:
BISON=bison
BISONFLAGS= # -v -> .output; -d -> .h
DELTAT_DEP=# x-deltat.y
##WIN32##DELTAT_DEP=

$(srcdir)/deltat.c : $(DELTAT_DEP)
	(cd $(srcdir) && $(BISON) $(BISONFLAGS) -o deltat.c x-deltat.y)

##DOS##LIBOBJS = $(OBJS)

all-unix: all-libobjs
clean-unix:: clean-libobjs

COMERRLIB=$(TOPLIBD)/libcom_err.a

T_WALK_RTREE_OBJS= t_walk_rtree.o walk_rtree.o tgtname.o unparse.o \
	bld_pr_ext.o copy_data.o

T_KERB_OBJS= t_kerb.o conv_princ.o unparse.o set_realm.o str_conv.o

T_SER_OBJS= t_ser.o ser_actx.o ser_adata.o ser_addr.o ser_auth.o ser_cksum.o \
	ser_ctx.o ser_key.o ser_princ.o serialize.o authdata.o pac.o \
	pac_sign.o ai_authdata.o authdata_exp.o s4u_authdata.o copy_data.o etype_list.o

T_DELTAT_OBJS= t_deltat.o deltat.o

T_PAC_OBJS= t_pac.o pac.o pac_sign.o copy_data.o

T_PRINC_OBJS= t_princ.o parse.o unparse.o

T_ETYPES_OBJS= t_etypes.o init_ctx.o etype_list.o plugin.o

T_PARSE_HOST_STRING_OBJS= t_parse_host_string.o parse_host_string.o

t_walk_rtree: $(T_WALK_RTREE_OBJS) $(KRB5_BASE_DEPLIBS)
	$(CC_LINK) -o t_walk_rtree $(T_WALK_RTREE_OBJS) $(KRB5_BASE_LIBS)
t_ad_fx_armor: t_ad_fx_armor.o
	$(CC_LINK) -o $@ t_ad_fx_armor.o $(KRB5_BASE_LIBS)

t_authdata: t_authdata.o $(KRB5_BASE_DEPLIBS)
	$(CC_LINK) -o $@ t_authdata.o $(KRB5_BASE_LIBS)

t_kerb: $(T_KERB_OBJS) $(KRB5_BASE_DEPLIBS)
	$(CC_LINK) -o t_kerb $(T_KERB_OBJS) $(KRB5_BASE_LIBS)

t_ser: $(T_SER_OBJS) $(KRB5_BASE_DEPLIBS)
	$(CC_LINK) -o t_ser $(T_SER_OBJS) $(KRB5_BASE_LIBS)

t_deltat : $(T_DELTAT_OBJS) $(SUPPORT_DEPLIB)
	$(CC_LINK) -o t_deltat $(T_DELTAT_OBJS) $(SUPPORT_LIB)

T_EXPAND_OBJS=t_expand.o
t_expand.o : t_expand.c
t_expand : $(T_EXPAND_OBJS) $(KRB5_BASE_DEPLIBS)
	$(CC_LINK) -o t_expand $(T_EXPAND_OBJS) $(KRB5_BASE_LIBS)

t_pac: $(T_PAC_OBJS) $(KRB5_BASE_DEPLIBS)
	$(CC_LINK) -o t_pac $(T_PAC_OBJS) $(KRB5_BASE_LIBS)

t_princ: $(T_PRINC_OBJS) $(KRB5_BASE_DEPLIBS)
	$(CC_LINK) -o t_princ $(T_PRINC_OBJS) $(KRB5_BASE_LIBS)

t_etypes: $(T_ETYPES_OBJS) $(KRB5_BASE_DEPLIBS)
	$(CC_LINK) -o t_etypes $(T_ETYPES_OBJS) $(KRB5_BASE_LIBS)

t_parse_host_string: $(T_PARSE_HOST_STRING_OBJS) $(KRB5_BASE_DEPLIBS)
	$(CC_LINK) -o $@ $(T_PARSE_HOST_STRING_OBJS) $(CMOCKA_LIBS) \
		$(KRB5_BASE_LIBS)

t_expire_warn: t_expire_warn.o $(KRB5_BASE_DEPLIBS)
	$(CC_LINK) -o $@ t_expire_warn.o $(KRB5_BASE_LIBS)

t_vfy_increds: t_vfy_increds.o $(KRB5_BASE_DEPLIBS)
	$(CC_LINK) -o $@ t_vfy_increds.o $(KRB5_BASE_LIBS)

t_in_ccache: t_in_ccache.o $(KRB5_BASE_DEPLIBS)
	$(CC_LINK) -o $@ t_in_ccache.o $(KRB5_BASE_LIBS)

t_cc_config: t_cc_config.o $(KRB5_BASE_DEPLIBS)
	$(CC_LINK) -o $@ t_cc_config.o $(KRB5_BASE_LIBS)

t_copy_context: t_copy_context.o $(KRB5_BASE_DEPLIBS)
	$(CC_LINK) -o $@ t_copy_context.o $(KRB5_BASE_LIBS)

t_response_items: t_response_items.o response_items.o $(KRB5_BASE_DEPLIBS)
	$(CC_LINK) -o $@ t_response_items.o response_items.o $(KRB5_BASE_LIBS)

t_sname_match: t_sname_match.o sname_match.o $(KRB5_BASE_DEPLIBS)
	$(CC_LINK) -o $@ t_sname_match.o sname_match.o $(KRB5_BASE_LIBS)

t_valid_times: t_valid_times.o valid_times.o $(KRB5_BASE_DEPLIBS)
	$(CC_LINK) -o $@ t_valid_times.o valid_times.o $(KRB5_BASE_LIBS)

t_get_etype_info: t_get_etype_info.o $(KRB5_BASE_DEPLIBS)
	$(CC_LINK) -o $@ t_get_etype_info.o $(KRB5_BASE_LIBS)

TEST_PROGS= t_walk_rtree t_kerb t_ser t_deltat t_expand t_authdata t_pac \
	t_in_ccache t_cc_config t_copy_context t_princ t_etypes t_vfy_increds \
	t_response_items t_sname_match t_valid_times t_get_etype_info

check-unix: $(TEST_PROGS) runenv.sh
	$(RUN_TEST_LOCAL_CONF) ./t_kerb \
		parse_name tytso \
		parse_name tytso@SHAZAAM \
		parse_name tytso/root@VEGGIE.COM \
		parse_name tytso/tuber/carrot@VEGGIE.COM \
		parse_name tytso/a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p/q/r/s/t \
		parse_name tytso/a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p/q/r/s/t@FOO \
		parse_name tytso\\\\0/\\0@B\\n\\t\\\\GAG \
		parse_name tytso/\\n/\\b\\t@B\\0hacky-test \
		parse_name \\/slash/\\@atsign/octa\\/thorpe@\\/slash\\@at\\/sign \
		425_conv_principal rcmd e40-po ATHENA.MIT.EDU \
		425_conv_principal rcmd mit ATHENA.MIT.EDU \
		425_conv_principal rcmd lithium ATHENA.MIT.EDU \
		425_conv_principal rcmd tweedledumb CYGNUS.COM \
		425_conv_principal rcmd uunet UU.NET \
		425_conv_principal zephyr zephyr ATHENA.MIT.EDU \
		425_conv_principal kadmin ATHENA.MIT.EDU ATHENA.MIT.EDU \
		524_conv_principal host/e40-po.mit.edu@ATHENA.MIT.EDU \
		524_conv_principal host/foobar.stanford.edu@stanford.edu \
		set_realm marc@MIT.EDU CYGNUS.COM \
		> test.out
	cmp test.out $(srcdir)/t_ref_kerb.out
	$(RM) test.out
	$(RUN_TEST) ./t_ser
	$(RUN_TEST) ./t_deltat
	$(RUN_TEST) sh $(srcdir)/transit-tests
	$(RUN_TEST_LOCAL_CONF) sh $(srcdir)/walktree-tests
	$(RUN_TEST) ./t_authdata
	$(RUN_TEST) ./t_pac
	$(RUN_TEST) ./t_princ
	$(RUN_TEST) ./t_etypes
	$(RUN_TEST) ./t_response_items
	$(RUN_TEST) ./t_copy_context
	$(RUN_TEST) ./t_sname_match
	$(RUN_TEST) ./t_valid_times

check-pytests: t_expire_warn t_get_etype_info t_vfy_increds
	$(RUNPYTEST) $(srcdir)/t_expire_warn.py $(PYTESTFLAGS)
	$(RUNPYTEST) $(srcdir)/t_vfy_increds.py $(PYTESTFLAGS)
	$(RUNPYTEST) $(srcdir)/t_in_ccache_patypes.py $(PYTESTFLAGS)
	$(RUNPYTEST) $(srcdir)/t_get_etype_info.py $(PYTESTFLAGS)

check-cmocka: t_parse_host_string
	$(RUN_TEST) ./t_parse_host_string > /dev/null

clean:
	$(RM) $(OUTPRE)t_walk_rtree$(EXEEXT) $(OUTPRE)t_walk_rtree.$(OBJEXT) \
		$(OUTPRE)t_kerb$(EXEEXT) $(OUTPRE)t_kerb.$(OBJEXT)	\
		$(OUTPRE)t_ser$(EXEEXT) $(OUTPRE)t_ser.$(OBJEXT)	\
		$(OUTPRE)t_deltat$(EXEEXT) $(OUTPRE)t_deltat.$(OBJEXT) \
		$(OUTPRE)t_expand$(EXEEXT) $(OUTPRE)t_expand.$(OBJEXT)  \
	$(OUTPRE)t_expire_warn$(EXEEXT) $(OUTPRE)t_expire_warn.$(OBJEXT)  \
		$(OUTPRE)t_etypes$(EXEEXT) $(OUTPRE)t_etypes.$(OBJEXT)	\
		$(OUTPRE)t_pac$(EXEEXT) $(OUTPRE)t_pac.$(OBJEXT)	\
		$(OUTPRE)t_princ$(EXEEXT) $(OUTPRE)t_princ.$(OBJEXT)	\
	$(OUTPRE)t_authdata$(EXEEXT) $(OUTPRE)t_authdata.$(OBJEXT)	\
	$(OUTPRE)t_cc_config$(EXEEXT) $(OUTPRE)t_cc_config.$(OBJEXT)	\
	$(OUTPRE)t_copy_context$(EXEEXT) $(OUTPRE)t_copy_context.$(OBJEXT) \
	$(OUTPRE)t_in_ccache$(EXEEXT) $(OUTPRE)t_in_ccache.$(OBJEXT)	\
	$(OUTPRE)t_ad_fx_armor$(EXEEXT) $(OUTPRE)t_ad_fx_armor.$(OBJEXT) \
	$(OUTPRE)t_vfy_increds$(EXEEXT) $(OUTPRE)t_vfy_increds.$(OBJEXT) \
	$(OUTPRE)t_response_items$(EXEEXT) \
	$(OUTPRE)t_response_items.$(OBJEXT) \
	$(OUTPRE)t_sname_match$(EXEEXT) $(OUTPRE)t_sname_match.$(OBJEXT) \
	$(OUTPRE)t_valid_times$(EXEEXT) $(OUTPRE)t_valid_times.$(OBJEXT) \
	$(OUTPRE)t_get_etype_info$(EXEEXT) \
	$(OUTPRE)t_get_etype_info.$(OBJEXT) \
	$(OUTPRE)t_parse_host_string$(EXEEXT) \
	$(OUTPRE)t_parse_host_string.$(OBJEXT)

### config/libobj.in
#
# Makefile fragment that builds object files for libraries.
#
# The following variables must be set in Makefile.in:
#
# STLIBOBJS	list of .o objects; this must not contain variable
#		references.

.SUFFIXES: .c .so .po
.c.so:
	$(CC) $(PICFLAGS) -DSHARED $(ALL_CFLAGS) -c $< -o $*.so.o && $(MV) $*.so.o $*.so
.c.po:
	$(CC) $(PROFFLAGS) $(ALL_CFLAGS) -c $< -o $*.po.o && $(MV) $*.po.o $*.po

# rules to generate object file lists

OBJS.ST: $(STLIBOBJS) Makefile
	@echo $(STLIBOBJS) > $@
	: updated $@

OBJS.SH: $(SHLIBOBJS) Makefile
	@echo $(SHLIBOBJS) > $@
	: updated $@

OBJS.PF: $(PFLIBOBJS) Makefile
	@echo $(PFLIBOBJS) > $@
	: updated $@

all-libobjs: $(OBJLISTS)

clean-libobjs:
	$(RM) OBJS.ST OBJS.SH OBJS.PF $(STLIBOBJS) $(SHLIBOBJS) $(PFLIBOBJS)

Makefile: $(top_srcdir)/config/libobj.in
config.status: $(top_srcdir)/config/shlib.conf

# clean-unix:: clean-libobjs
# all-unix: all-libobjs

###
### end config/libobj.in

#
# Generated makefile dependencies follow.
#
addr_comp.so addr_comp.po $(OUTPRE)addr_comp.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  addr_comp.c
addr_order.so addr_order.po $(OUTPRE)addr_order.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  addr_order.c
addr_srch.so addr_srch.po $(OUTPRE)addr_srch.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  addr_srch.c
appdefault.so appdefault.po $(OUTPRE)appdefault.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  appdefault.c
auth_con.so auth_con.po $(OUTPRE)auth_con.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(srcdir)/../rcache/memrcache.h $(top_srcdir)/include/k5-buf.h \
  $(top_srcdir)/include/k5-err.h $(top_srcdir)/include/k5-gmt_mktime.h \
  $(top_srcdir)/include/k5-int-pkinit.h $(top_srcdir)/include/k5-int.h \
  $(top_srcdir)/include/k5-platform.h $(top_srcdir)/include/k5-plugin.h \
  $(top_srcdir)/include/k5-thread.h $(top_srcdir)/include/k5-trace.h \
  $(top_srcdir)/include/krb5.h $(top_srcdir)/include/krb5/authdata_plugin.h \
  $(top_srcdir)/include/krb5/plugin.h $(top_srcdir)/include/port-sockets.h \
  $(top_srcdir)/include/socket-utils.h auth_con.c auth_con.h \
  int-proto.h
cammac_util.so cammac_util.po $(OUTPRE)cammac_util.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  cammac_util.c
ai_authdata.so ai_authdata.po $(OUTPRE)ai_authdata.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(srcdir)/../rcache/memrcache.h $(top_srcdir)/include/k5-buf.h \
  $(top_srcdir)/include/k5-err.h $(top_srcdir)/include/k5-gmt_mktime.h \
  $(top_srcdir)/include/k5-int-pkinit.h $(top_srcdir)/include/k5-int.h \
  $(top_srcdir)/include/k5-platform.h $(top_srcdir)/include/k5-plugin.h \
  $(top_srcdir)/include/k5-thread.h $(top_srcdir)/include/k5-trace.h \
  $(top_srcdir)/include/k5-utf8.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  ai_authdata.c auth_con.h authdata.h int-proto.h
authdata.so authdata.po $(OUTPRE)authdata.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(srcdir)/../rcache/memrcache.h $(top_srcdir)/include/k5-buf.h \
  $(top_srcdir)/include/k5-err.h $(top_srcdir)/include/k5-gmt_mktime.h \
  $(top_srcdir)/include/k5-int-pkinit.h $(top_srcdir)/include/k5-int.h \
  $(top_srcdir)/include/k5-platform.h $(top_srcdir)/include/k5-plugin.h \
  $(top_srcdir)/include/k5-thread.h $(top_srcdir)/include/k5-trace.h \
  $(top_srcdir)/include/k5-utf8.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  auth_con.h authdata.c authdata.h int-proto.h
authdata_exp.so authdata_exp.po $(OUTPRE)authdata_exp.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(srcdir)/../rcache/memrcache.h $(top_srcdir)/include/k5-buf.h \
  $(top_srcdir)/include/k5-err.h $(top_srcdir)/include/k5-gmt_mktime.h \
  $(top_srcdir)/include/k5-int-pkinit.h $(top_srcdir)/include/k5-int.h \
  $(top_srcdir)/include/k5-platform.h $(top_srcdir)/include/k5-plugin.h \
  $(top_srcdir)/include/k5-thread.h $(top_srcdir)/include/k5-trace.h \
  $(top_srcdir)/include/k5-utf8.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  auth_con.h authdata.h authdata_exp.c int-proto.h
authdata_enc.so authdata_enc.po $(OUTPRE)authdata_enc.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  authdata_enc.c
authdata_dec.so authdata_dec.po $(OUTPRE)authdata_dec.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  authdata_dec.c int-proto.h
bld_pr_ext.so bld_pr_ext.po $(OUTPRE)bld_pr_ext.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  bld_pr_ext.c
bld_princ.so bld_princ.po $(OUTPRE)bld_princ.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  bld_princ.c
brand.so brand.po $(OUTPRE)brand.$(OBJEXT): $(top_srcdir)/patchlevel.h \
  brand.c
chk_trans.so chk_trans.po $(OUTPRE)chk_trans.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  chk_trans.c
chpw.so chpw.po $(OUTPRE)chpw.$(OBJEXT): $(BUILDTOP)/include/autoconf.h \
  $(BUILDTOP)/include/krb5/krb5.h $(BUILDTOP)/include/osconf.h \
  $(BUILDTOP)/include/profile.h $(COM_ERR_DEPS) $(srcdir)/../rcache/memrcache.h \
  $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/k5-unicode.h \
  $(top_srcdir)/include/k5-utf8.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  auth_con.h chpw.c int-proto.h
conv_creds.so conv_creds.po $(OUTPRE)conv_creds.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  conv_creds.c
conv_princ.so conv_princ.po $(OUTPRE)conv_princ.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  conv_princ.c
copy_addrs.so copy_addrs.po $(OUTPRE)copy_addrs.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  copy_addrs.c
copy_auth.so copy_auth.po $(OUTPRE)copy_auth.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  copy_auth.c int-proto.h
copy_athctr.so copy_athctr.po $(OUTPRE)copy_athctr.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(srcdir)/../rcache/memrcache.h $(top_srcdir)/include/k5-buf.h \
  $(top_srcdir)/include/k5-err.h $(top_srcdir)/include/k5-gmt_mktime.h \
  $(top_srcdir)/include/k5-int-pkinit.h $(top_srcdir)/include/k5-int.h \
  $(top_srcdir)/include/k5-platform.h $(top_srcdir)/include/k5-plugin.h \
  $(top_srcdir)/include/k5-thread.h $(top_srcdir)/include/k5-trace.h \
  $(top_srcdir)/include/krb5.h $(top_srcdir)/include/krb5/authdata_plugin.h \
  $(top_srcdir)/include/krb5/plugin.h $(top_srcdir)/include/port-sockets.h \
  $(top_srcdir)/include/socket-utils.h auth_con.h copy_athctr.c
copy_cksum.so copy_cksum.po $(OUTPRE)copy_cksum.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  copy_cksum.c
copy_creds.so copy_creds.po $(OUTPRE)copy_creds.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  copy_creds.c int-proto.h
copy_data.so copy_data.po $(OUTPRE)copy_data.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  copy_data.c
copy_key.so copy_key.po $(OUTPRE)copy_key.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  copy_key.c
copy_princ.so copy_princ.po $(OUTPRE)copy_princ.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  copy_princ.c
copy_tick.so copy_tick.po $(OUTPRE)copy_tick.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  copy_tick.c
cp_key_cnt.so cp_key_cnt.po $(OUTPRE)cp_key_cnt.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  cp_key_cnt.c
decode_kdc.so decode_kdc.po $(OUTPRE)decode_kdc.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  decode_kdc.c fast.h int-proto.h
decrypt_tk.so decrypt_tk.po $(OUTPRE)decrypt_tk.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  decrypt_tk.c
deltat.so deltat.po $(OUTPRE)deltat.$(OBJEXT): $(BUILDTOP)/include/autoconf.h \
  $(BUILDTOP)/include/krb5/krb5.h $(BUILDTOP)/include/osconf.h \
  $(BUILDTOP)/include/profile.h $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h \
  $(top_srcdir)/include/k5-err.h $(top_srcdir)/include/k5-gmt_mktime.h \
  $(top_srcdir)/include/k5-int-pkinit.h $(top_srcdir)/include/k5-int.h \
  $(top_srcdir)/include/k5-platform.h $(top_srcdir)/include/k5-plugin.h \
  $(top_srcdir)/include/k5-thread.h $(top_srcdir)/include/k5-trace.h \
  $(top_srcdir)/include/krb5.h $(top_srcdir)/include/krb5/authdata_plugin.h \
  $(top_srcdir)/include/krb5/plugin.h $(top_srcdir)/include/port-sockets.h \
  $(top_srcdir)/include/socket-utils.h deltat.c
enc_helper.so enc_helper.po $(OUTPRE)enc_helper.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  enc_helper.c
enc_keyhelper.so enc_keyhelper.po $(OUTPRE)enc_keyhelper.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  enc_keyhelper.c int-proto.h
encode_kdc.so encode_kdc.po $(OUTPRE)encode_kdc.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  encode_kdc.c
encrypt_tk.so encrypt_tk.po $(OUTPRE)encrypt_tk.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  encrypt_tk.c
etype_list.so etype_list.po $(OUTPRE)etype_list.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  etype_list.c int-proto.h
fast.so fast.po $(OUTPRE)fast.$(OBJEXT): $(BUILDTOP)/include/autoconf.h \
  $(BUILDTOP)/include/krb5/krb5.h $(BUILDTOP)/include/osconf.h \
  $(BUILDTOP)/include/profile.h $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h \
  $(top_srcdir)/include/k5-err.h $(top_srcdir)/include/k5-gmt_mktime.h \
  $(top_srcdir)/include/k5-int-pkinit.h $(top_srcdir)/include/k5-int.h \
  $(top_srcdir)/include/k5-platform.h $(top_srcdir)/include/k5-plugin.h \
  $(top_srcdir)/include/k5-thread.h $(top_srcdir)/include/k5-trace.h \
  $(top_srcdir)/include/krb5.h $(top_srcdir)/include/krb5/authdata_plugin.h \
  $(top_srcdir)/include/krb5/plugin.h $(top_srcdir)/include/port-sockets.h \
  $(top_srcdir)/include/socket-utils.h fast.c fast.h \
  int-proto.h
fwd_tgt.so fwd_tgt.po $(OUTPRE)fwd_tgt.$(OBJEXT): $(BUILDTOP)/include/autoconf.h \
  $(BUILDTOP)/include/krb5/krb5.h $(BUILDTOP)/include/osconf.h \
  $(BUILDTOP)/include/profile.h $(COM_ERR_DEPS) $(srcdir)/../os/os-proto.h \
  $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/locate_plugin.h \
  $(top_srcdir)/include/krb5/plugin.h $(top_srcdir)/include/port-sockets.h \
  $(top_srcdir)/include/socket-utils.h fwd_tgt.c int-proto.h
gc_via_tkt.so gc_via_tkt.po $(OUTPRE)gc_via_tkt.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  fast.h gc_via_tkt.c int-proto.h
gen_seqnum.so gen_seqnum.po $(OUTPRE)gen_seqnum.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  gen_seqnum.c
gen_subkey.so gen_subkey.po $(OUTPRE)gen_subkey.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  gen_subkey.c
gen_save_subkey.so gen_save_subkey.po $(OUTPRE)gen_save_subkey.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(srcdir)/../rcache/memrcache.h $(top_srcdir)/include/k5-buf.h \
  $(top_srcdir)/include/k5-err.h $(top_srcdir)/include/k5-gmt_mktime.h \
  $(top_srcdir)/include/k5-int-pkinit.h $(top_srcdir)/include/k5-int.h \
  $(top_srcdir)/include/k5-platform.h $(top_srcdir)/include/k5-plugin.h \
  $(top_srcdir)/include/k5-thread.h $(top_srcdir)/include/k5-trace.h \
  $(top_srcdir)/include/krb5.h $(top_srcdir)/include/krb5/authdata_plugin.h \
  $(top_srcdir)/include/krb5/plugin.h $(top_srcdir)/include/port-sockets.h \
  $(top_srcdir)/include/socket-utils.h auth_con.h gen_save_subkey.c \
  int-proto.h
get_creds.so get_creds.po $(OUTPRE)get_creds.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(srcdir)/../os/os-proto.h $(top_srcdir)/include/k5-buf.h \
  $(top_srcdir)/include/k5-err.h $(top_srcdir)/include/k5-gmt_mktime.h \
  $(top_srcdir)/include/k5-int-pkinit.h $(top_srcdir)/include/k5-int.h \
  $(top_srcdir)/include/k5-platform.h $(top_srcdir)/include/k5-plugin.h \
  $(top_srcdir)/include/k5-thread.h $(top_srcdir)/include/k5-trace.h \
  $(top_srcdir)/include/krb5.h $(top_srcdir)/include/krb5/authdata_plugin.h \
  $(top_srcdir)/include/krb5/locate_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  fast.h get_creds.c int-proto.h
get_etype_info.so get_etype_info.po $(OUTPRE)get_etype_info.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-json.h \
  $(top_srcdir)/include/k5-platform.h $(top_srcdir)/include/k5-plugin.h \
  $(top_srcdir)/include/k5-thread.h $(top_srcdir)/include/k5-trace.h \
  $(top_srcdir)/include/krb5.h $(top_srcdir)/include/krb5/authdata_plugin.h \
  $(top_srcdir)/include/krb5/plugin.h $(top_srcdir)/include/port-sockets.h \
  $(top_srcdir)/include/socket-utils.h fast.h get_etype_info.c \
  init_creds_ctx.h int-proto.h
get_in_tkt.so get_in_tkt.po $(OUTPRE)get_in_tkt.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(srcdir)/../os/os-proto.h $(top_srcdir)/include/k5-buf.h \
  $(top_srcdir)/include/k5-err.h $(top_srcdir)/include/k5-gmt_mktime.h \
  $(top_srcdir)/include/k5-int-pkinit.h $(top_srcdir)/include/k5-int.h \
  $(top_srcdir)/include/k5-json.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/locate_plugin.h \
  $(top_srcdir)/include/krb5/plugin.h $(top_srcdir)/include/port-sockets.h \
  $(top_srcdir)/include/socket-utils.h fast.h get_in_tkt.c \
  init_creds_ctx.h int-proto.h
gic_keytab.so gic_keytab.po $(OUTPRE)gic_keytab.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(srcdir)/../os/os-proto.h $(top_srcdir)/include/k5-buf.h \
  $(top_srcdir)/include/k5-err.h $(top_srcdir)/include/k5-gmt_mktime.h \
  $(top_srcdir)/include/k5-int-pkinit.h $(top_srcdir)/include/k5-int.h \
  $(top_srcdir)/include/k5-json.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/locate_plugin.h \
  $(top_srcdir)/include/krb5/plugin.h $(top_srcdir)/include/port-sockets.h \
  $(top_srcdir)/include/socket-utils.h gic_keytab.c init_creds_ctx.h \
  int-proto.h
gic_opt.so gic_opt.po $(OUTPRE)gic_opt.$(OBJEXT): $(BUILDTOP)/include/autoconf.h \
  $(BUILDTOP)/include/krb5/krb5.h $(BUILDTOP)/include/osconf.h \
  $(BUILDTOP)/include/profile.h $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h \
  $(top_srcdir)/include/k5-err.h $(top_srcdir)/include/k5-gmt_mktime.h \
  $(top_srcdir)/include/k5-int-pkinit.h $(top_srcdir)/include/k5-int.h \
  $(top_srcdir)/include/k5-platform.h $(top_srcdir)/include/k5-plugin.h \
  $(top_srcdir)/include/k5-thread.h $(top_srcdir)/include/k5-trace.h \
  $(top_srcdir)/include/krb5.h $(top_srcdir)/include/krb5/authdata_plugin.h \
  $(top_srcdir)/include/krb5/clpreauth_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  gic_opt.c int-proto.h
gic_pwd.so gic_pwd.po $(OUTPRE)gic_pwd.$(OBJEXT): $(BUILDTOP)/include/autoconf.h \
  $(BUILDTOP)/include/krb5/krb5.h $(BUILDTOP)/include/osconf.h \
  $(BUILDTOP)/include/profile.h $(COM_ERR_DEPS) $(srcdir)/../os/os-proto.h \
  $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-json.h \
  $(top_srcdir)/include/k5-platform.h $(top_srcdir)/include/k5-plugin.h \
  $(top_srcdir)/include/k5-thread.h $(top_srcdir)/include/k5-trace.h \
  $(top_srcdir)/include/krb5.h $(top_srcdir)/include/krb5/authdata_plugin.h \
  $(top_srcdir)/include/krb5/locate_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  gic_pwd.c init_creds_ctx.h int-proto.h
in_tkt_sky.so in_tkt_sky.po $(OUTPRE)in_tkt_sky.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  in_tkt_sky.c int-proto.h
init_ctx.so init_ctx.po $(OUTPRE)init_ctx.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(srcdir)/../krb5_libinit.h $(srcdir)/../os/os-proto.h \
  $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/locate_plugin.h \
  $(top_srcdir)/include/krb5/plugin.h $(top_srcdir)/include/port-sockets.h \
  $(top_srcdir)/include/socket-utils.h $(top_srcdir)/patchlevel.h \
  brand.c init_ctx.c int-proto.h
copy_ctx.so copy_ctx.po $(OUTPRE)copy_ctx.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  copy_ctx.c int-proto.h
init_keyblock.so init_keyblock.po $(OUTPRE)init_keyblock.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  init_keyblock.c
kdc_rep_dc.so kdc_rep_dc.po $(OUTPRE)kdc_rep_dc.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  kdc_rep_dc.c
kerrs.so kerrs.po $(OUTPRE)kerrs.$(OBJEXT): $(BUILDTOP)/include/autoconf.h \
  $(BUILDTOP)/include/krb5/krb5.h $(BUILDTOP)/include/osconf.h \
  $(BUILDTOP)/include/profile.h $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h \
  $(top_srcdir)/include/k5-err.h $(top_srcdir)/include/k5-gmt_mktime.h \
  $(top_srcdir)/include/k5-int-pkinit.h $(top_srcdir)/include/k5-int.h \
  $(top_srcdir)/include/k5-platform.h $(top_srcdir)/include/k5-plugin.h \
  $(top_srcdir)/include/k5-thread.h $(top_srcdir)/include/k5-trace.h \
  $(top_srcdir)/include/krb5.h $(top_srcdir)/include/krb5/authdata_plugin.h \
  $(top_srcdir)/include/krb5/plugin.h $(top_srcdir)/include/port-sockets.h \
  $(top_srcdir)/include/socket-utils.h int-proto.h kerrs.c
kfree.so kfree.po $(OUTPRE)kfree.$(OBJEXT): $(BUILDTOP)/include/autoconf.h \
  $(BUILDTOP)/include/krb5/krb5.h $(BUILDTOP)/include/osconf.h \
  $(BUILDTOP)/include/profile.h $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h \
  $(top_srcdir)/include/k5-err.h $(top_srcdir)/include/k5-gmt_mktime.h \
  $(top_srcdir)/include/k5-int-pkinit.h $(top_srcdir)/include/k5-int.h \
  $(top_srcdir)/include/k5-platform.h $(top_srcdir)/include/k5-plugin.h \
  $(top_srcdir)/include/k5-spake.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  kfree.c
libdef_parse.so libdef_parse.po $(OUTPRE)libdef_parse.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(srcdir)/../os/os-proto.h $(top_srcdir)/include/k5-buf.h \
  $(top_srcdir)/include/k5-err.h $(top_srcdir)/include/k5-gmt_mktime.h \
  $(top_srcdir)/include/k5-int-pkinit.h $(top_srcdir)/include/k5-int.h \
  $(top_srcdir)/include/k5-platform.h $(top_srcdir)/include/k5-plugin.h \
  $(top_srcdir)/include/k5-thread.h $(top_srcdir)/include/k5-trace.h \
  $(top_srcdir)/include/krb5.h $(top_srcdir)/include/krb5/authdata_plugin.h \
  $(top_srcdir)/include/krb5/locate_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  int-proto.h libdef_parse.c
mk_cred.so mk_cred.po $(OUTPRE)mk_cred.$(OBJEXT): $(BUILDTOP)/include/autoconf.h \
  $(BUILDTOP)/include/krb5/krb5.h $(BUILDTOP)/include/osconf.h \
  $(BUILDTOP)/include/profile.h $(COM_ERR_DEPS) $(srcdir)/../rcache/memrcache.h \
  $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  auth_con.h int-proto.h mk_cred.c
mk_error.so mk_error.po $(OUTPRE)mk_error.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  mk_error.c
mk_priv.so mk_priv.po $(OUTPRE)mk_priv.$(OBJEXT): $(BUILDTOP)/include/autoconf.h \
  $(BUILDTOP)/include/krb5/krb5.h $(BUILDTOP)/include/osconf.h \
  $(BUILDTOP)/include/profile.h $(COM_ERR_DEPS) $(srcdir)/../rcache/memrcache.h \
  $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  auth_con.h int-proto.h mk_priv.c
mk_rep.so mk_rep.po $(OUTPRE)mk_rep.$(OBJEXT): $(BUILDTOP)/include/autoconf.h \
  $(BUILDTOP)/include/krb5/krb5.h $(BUILDTOP)/include/osconf.h \
  $(BUILDTOP)/include/profile.h $(COM_ERR_DEPS) $(srcdir)/../rcache/memrcache.h \
  $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  auth_con.h int-proto.h mk_rep.c
mk_req.so mk_req.po $(OUTPRE)mk_req.$(OBJEXT): $(BUILDTOP)/include/autoconf.h \
  $(BUILDTOP)/include/krb5/krb5.h $(BUILDTOP)/include/osconf.h \
  $(BUILDTOP)/include/profile.h $(COM_ERR_DEPS) $(srcdir)/../rcache/memrcache.h \
  $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  auth_con.h mk_req.c
mk_req_ext.so mk_req_ext.po $(OUTPRE)mk_req_ext.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(srcdir)/../rcache/memrcache.h $(top_srcdir)/include/k5-buf.h \
  $(top_srcdir)/include/k5-err.h $(top_srcdir)/include/k5-gmt_mktime.h \
  $(top_srcdir)/include/k5-int-pkinit.h $(top_srcdir)/include/k5-int.h \
  $(top_srcdir)/include/k5-platform.h $(top_srcdir)/include/k5-plugin.h \
  $(top_srcdir)/include/k5-thread.h $(top_srcdir)/include/k5-trace.h \
  $(top_srcdir)/include/krb5.h $(top_srcdir)/include/krb5/authdata_plugin.h \
  $(top_srcdir)/include/krb5/plugin.h $(top_srcdir)/include/port-sockets.h \
  $(top_srcdir)/include/socket-utils.h auth_con.h int-proto.h \
  mk_req_ext.c
mk_safe.so mk_safe.po $(OUTPRE)mk_safe.$(OBJEXT): $(BUILDTOP)/include/autoconf.h \
  $(BUILDTOP)/include/krb5/krb5.h $(BUILDTOP)/include/osconf.h \
  $(BUILDTOP)/include/profile.h $(COM_ERR_DEPS) $(srcdir)/../rcache/memrcache.h \
  $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  auth_con.h int-proto.h mk_safe.c
pac.so pac.po $(OUTPRE)pac.$(OBJEXT): $(BUILDTOP)/include/autoconf.h \
  $(BUILDTOP)/include/krb5/krb5.h $(BUILDTOP)/include/osconf.h \
  $(BUILDTOP)/include/profile.h $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h \
  $(top_srcdir)/include/k5-err.h $(top_srcdir)/include/k5-gmt_mktime.h \
  $(top_srcdir)/include/k5-int-pkinit.h $(top_srcdir)/include/k5-int.h \
  $(top_srcdir)/include/k5-platform.h $(top_srcdir)/include/k5-plugin.h \
  $(top_srcdir)/include/k5-thread.h $(top_srcdir)/include/k5-trace.h \
  $(top_srcdir)/include/k5-utf8.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  authdata.h pac.c
pac_sign.so pac_sign.po $(OUTPRE)pac_sign.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/k5-utf8.h \
  $(top_srcdir)/include/krb5.h $(top_srcdir)/include/krb5/authdata_plugin.h \
  $(top_srcdir)/include/krb5/plugin.h $(top_srcdir)/include/port-sockets.h \
  $(top_srcdir)/include/socket-utils.h authdata.h pac_sign.c
padata.so padata.po $(OUTPRE)padata.$(OBJEXT): $(BUILDTOP)/include/autoconf.h \
  $(BUILDTOP)/include/krb5/krb5.h $(BUILDTOP)/include/osconf.h \
  $(BUILDTOP)/include/profile.h $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h \
  $(top_srcdir)/include/k5-err.h $(top_srcdir)/include/k5-gmt_mktime.h \
  $(top_srcdir)/include/k5-int-pkinit.h $(top_srcdir)/include/k5-int.h \
  $(top_srcdir)/include/k5-platform.h $(top_srcdir)/include/k5-plugin.h \
  $(top_srcdir)/include/k5-thread.h $(top_srcdir)/include/k5-trace.h \
  $(top_srcdir)/include/krb5.h $(top_srcdir)/include/krb5/authdata_plugin.h \
  $(top_srcdir)/include/krb5/plugin.h $(top_srcdir)/include/port-sockets.h \
  $(top_srcdir)/include/socket-utils.h padata.c
parse.so parse.po $(OUTPRE)parse.$(OBJEXT): $(BUILDTOP)/include/autoconf.h \
  $(BUILDTOP)/include/krb5/krb5.h $(BUILDTOP)/include/osconf.h \
  $(BUILDTOP)/include/profile.h $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h \
  $(top_srcdir)/include/k5-err.h $(top_srcdir)/include/k5-gmt_mktime.h \
  $(top_srcdir)/include/k5-int-pkinit.h $(top_srcdir)/include/k5-int.h \
  $(top_srcdir)/include/k5-platform.h $(top_srcdir)/include/k5-plugin.h \
  $(top_srcdir)/include/k5-thread.h $(top_srcdir)/include/k5-trace.h \
  $(top_srcdir)/include/krb5.h $(top_srcdir)/include/krb5/authdata_plugin.h \
  $(top_srcdir)/include/krb5/plugin.h $(top_srcdir)/include/port-sockets.h \
  $(top_srcdir)/include/socket-utils.h parse.c
parse_host_string.so parse_host_string.po $(OUTPRE)parse_host_string.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  parse_host_string.c
plugin.so plugin.po $(OUTPRE)plugin.$(OBJEXT): $(BUILDTOP)/include/autoconf.h \
  $(BUILDTOP)/include/krb5/krb5.h $(BUILDTOP)/include/osconf.h \
  $(BUILDTOP)/include/profile.h $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h \
  $(top_srcdir)/include/k5-err.h $(top_srcdir)/include/k5-gmt_mktime.h \
  $(top_srcdir)/include/k5-int-pkinit.h $(top_srcdir)/include/k5-int.h \
  $(top_srcdir)/include/k5-platform.h $(top_srcdir)/include/k5-plugin.h \
  $(top_srcdir)/include/k5-thread.h $(top_srcdir)/include/k5-trace.h \
  $(top_srcdir)/include/krb5.h $(top_srcdir)/include/krb5/authdata_plugin.h \
  $(top_srcdir)/include/krb5/plugin.h $(top_srcdir)/include/port-sockets.h \
  $(top_srcdir)/include/socket-utils.h plugin.c
pr_to_salt.so pr_to_salt.po $(OUTPRE)pr_to_salt.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  pr_to_salt.c
preauth2.so preauth2.po $(OUTPRE)preauth2.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(srcdir)/../os/os-proto.h $(top_srcdir)/include/k5-buf.h \
  $(top_srcdir)/include/k5-err.h $(top_srcdir)/include/k5-gmt_mktime.h \
  $(top_srcdir)/include/k5-int-pkinit.h $(top_srcdir)/include/k5-int.h \
  $(top_srcdir)/include/k5-json.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/clpreauth_plugin.h \
  $(top_srcdir)/include/krb5/locate_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  fast.h init_creds_ctx.h int-proto.h preauth2.c
preauth_ec.so preauth_ec.po $(OUTPRE)preauth_ec.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/clpreauth_plugin.h \
  $(top_srcdir)/include/krb5/plugin.h $(top_srcdir)/include/port-sockets.h \
  $(top_srcdir)/include/socket-utils.h int-proto.h preauth_ec.c
preauth_encts.so preauth_encts.po $(OUTPRE)preauth_encts.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-json.h \
  $(top_srcdir)/include/k5-platform.h $(top_srcdir)/include/k5-plugin.h \
  $(top_srcdir)/include/k5-thread.h $(top_srcdir)/include/k5-trace.h \
  $(top_srcdir)/include/krb5.h $(top_srcdir)/include/krb5/authdata_plugin.h \
  $(top_srcdir)/include/krb5/clpreauth_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  init_creds_ctx.h int-proto.h preauth_encts.c
preauth_otp.so preauth_otp.po $(OUTPRE)preauth_otp.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(srcdir)/../os/os-proto.h $(top_srcdir)/include/k5-buf.h \
  $(top_srcdir)/include/k5-err.h $(top_srcdir)/include/k5-gmt_mktime.h \
  $(top_srcdir)/include/k5-int-pkinit.h $(top_srcdir)/include/k5-int.h \
  $(top_srcdir)/include/k5-json.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/clpreauth_plugin.h \
  $(top_srcdir)/include/krb5/locate_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  int-proto.h preauth_otp.c
preauth_pkinit.so preauth_pkinit.po $(OUTPRE)preauth_pkinit.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-json.h \
  $(top_srcdir)/include/k5-platform.h $(top_srcdir)/include/k5-plugin.h \
  $(top_srcdir)/include/k5-thread.h $(top_srcdir)/include/k5-trace.h \
  $(top_srcdir)/include/krb5.h $(top_srcdir)/include/krb5/authdata_plugin.h \
  $(top_srcdir)/include/krb5/plugin.h $(top_srcdir)/include/port-sockets.h \
  $(top_srcdir)/include/socket-utils.h init_creds_ctx.h \
  int-proto.h preauth_pkinit.c
preauth_sam2.so preauth_sam2.po $(OUTPRE)preauth_sam2.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(srcdir)/../os/os-proto.h $(top_srcdir)/include/k5-buf.h \
  $(top_srcdir)/include/k5-err.h $(top_srcdir)/include/k5-gmt_mktime.h \
  $(top_srcdir)/include/k5-int-pkinit.h $(top_srcdir)/include/k5-int.h \
  $(top_srcdir)/include/k5-json.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/clpreauth_plugin.h \
  $(top_srcdir)/include/krb5/locate_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  init_creds_ctx.h int-proto.h preauth_sam2.c
princ_comp.so princ_comp.po $(OUTPRE)princ_comp.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/k5-unicode.h \
  $(top_srcdir)/include/k5-utf8.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  princ_comp.c
privsafe.so privsafe.po $(OUTPRE)privsafe.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(srcdir)/../rcache/memrcache.h $(top_srcdir)/include/k5-buf.h \
  $(top_srcdir)/include/k5-err.h $(top_srcdir)/include/k5-gmt_mktime.h \
  $(top_srcdir)/include/k5-int-pkinit.h $(top_srcdir)/include/k5-int.h \
  $(top_srcdir)/include/k5-platform.h $(top_srcdir)/include/k5-plugin.h \
  $(top_srcdir)/include/k5-thread.h $(top_srcdir)/include/k5-trace.h \
  $(top_srcdir)/include/krb5.h $(top_srcdir)/include/krb5/authdata_plugin.h \
  $(top_srcdir)/include/krb5/plugin.h $(top_srcdir)/include/port-sockets.h \
  $(top_srcdir)/include/socket-utils.h auth_con.h int-proto.h \
  privsafe.c
random_str.so random_str.po $(OUTPRE)random_str.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  random_str.c
rd_cred.so rd_cred.po $(OUTPRE)rd_cred.$(OBJEXT): $(BUILDTOP)/include/autoconf.h \
  $(BUILDTOP)/include/krb5/krb5.h $(BUILDTOP)/include/osconf.h \
  $(BUILDTOP)/include/profile.h $(COM_ERR_DEPS) $(srcdir)/../rcache/memrcache.h \
  $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  auth_con.h int-proto.h rd_cred.c
rd_error.so rd_error.po $(OUTPRE)rd_error.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  rd_error.c
rd_priv.so rd_priv.po $(OUTPRE)rd_priv.$(OBJEXT): $(BUILDTOP)/include/autoconf.h \
  $(BUILDTOP)/include/krb5/krb5.h $(BUILDTOP)/include/osconf.h \
  $(BUILDTOP)/include/profile.h $(COM_ERR_DEPS) $(srcdir)/../rcache/memrcache.h \
  $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  auth_con.h int-proto.h rd_priv.c
rd_rep.so rd_rep.po $(OUTPRE)rd_rep.$(OBJEXT): $(BUILDTOP)/include/autoconf.h \
  $(BUILDTOP)/include/krb5/krb5.h $(BUILDTOP)/include/osconf.h \
  $(BUILDTOP)/include/profile.h $(COM_ERR_DEPS) $(srcdir)/../rcache/memrcache.h \
  $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  auth_con.h rd_rep.c
rd_req.so rd_req.po $(OUTPRE)rd_req.$(OBJEXT): $(BUILDTOP)/include/autoconf.h \
  $(BUILDTOP)/include/krb5/krb5.h $(BUILDTOP)/include/osconf.h \
  $(BUILDTOP)/include/profile.h $(COM_ERR_DEPS) $(srcdir)/../rcache/memrcache.h \
  $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  auth_con.h rd_req.c
rd_req_dec.so rd_req_dec.po $(OUTPRE)rd_req_dec.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(srcdir)/../os/os-proto.h $(srcdir)/../rcache/memrcache.h \
  $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/k5-utf8.h \
  $(top_srcdir)/include/krb5.h $(top_srcdir)/include/krb5/authdata_plugin.h \
  $(top_srcdir)/include/krb5/locate_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  auth_con.h authdata.h int-proto.h rd_req_dec.c
rd_safe.so rd_safe.po $(OUTPRE)rd_safe.$(OBJEXT): $(BUILDTOP)/include/autoconf.h \
  $(BUILDTOP)/include/krb5/krb5.h $(BUILDTOP)/include/osconf.h \
  $(BUILDTOP)/include/profile.h $(COM_ERR_DEPS) $(srcdir)/../rcache/memrcache.h \
  $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  auth_con.h int-proto.h rd_safe.c
recvauth.so recvauth.po $(OUTPRE)recvauth.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(srcdir)/../rcache/memrcache.h $(top_srcdir)/include/k5-buf.h \
  $(top_srcdir)/include/k5-err.h $(top_srcdir)/include/k5-gmt_mktime.h \
  $(top_srcdir)/include/k5-int-pkinit.h $(top_srcdir)/include/k5-int.h \
  $(top_srcdir)/include/k5-platform.h $(top_srcdir)/include/k5-plugin.h \
  $(top_srcdir)/include/k5-thread.h $(top_srcdir)/include/k5-trace.h \
  $(top_srcdir)/include/krb5.h $(top_srcdir)/include/krb5/authdata_plugin.h \
  $(top_srcdir)/include/krb5/plugin.h $(top_srcdir)/include/port-sockets.h \
  $(top_srcdir)/include/socket-utils.h auth_con.h recvauth.c
response_items.so response_items.po $(OUTPRE)response_items.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  int-proto.h response_items.c
s4u_authdata.so s4u_authdata.po $(OUTPRE)s4u_authdata.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(srcdir)/../rcache/memrcache.h $(top_srcdir)/include/k5-buf.h \
  $(top_srcdir)/include/k5-err.h $(top_srcdir)/include/k5-gmt_mktime.h \
  $(top_srcdir)/include/k5-int-pkinit.h $(top_srcdir)/include/k5-int.h \
  $(top_srcdir)/include/k5-platform.h $(top_srcdir)/include/k5-plugin.h \
  $(top_srcdir)/include/k5-thread.h $(top_srcdir)/include/k5-trace.h \
  $(top_srcdir)/include/k5-utf8.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  auth_con.h authdata.h int-proto.h s4u_authdata.c
s4u_creds.so s4u_creds.po $(OUTPRE)s4u_creds.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(srcdir)/../os/os-proto.h $(top_srcdir)/include/k5-buf.h \
  $(top_srcdir)/include/k5-err.h $(top_srcdir)/include/k5-gmt_mktime.h \
  $(top_srcdir)/include/k5-int-pkinit.h $(top_srcdir)/include/k5-int.h \
  $(top_srcdir)/include/k5-platform.h $(top_srcdir)/include/k5-plugin.h \
  $(top_srcdir)/include/k5-thread.h $(top_srcdir)/include/k5-trace.h \
  $(top_srcdir)/include/krb5.h $(top_srcdir)/include/krb5/authdata_plugin.h \
  $(top_srcdir)/include/krb5/locate_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  int-proto.h s4u_creds.c
sendauth.so sendauth.po $(OUTPRE)sendauth.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(srcdir)/../os/os-proto.h $(srcdir)/../rcache/memrcache.h \
  $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/locate_plugin.h \
  $(top_srcdir)/include/krb5/plugin.h $(top_srcdir)/include/port-sockets.h \
  $(top_srcdir)/include/socket-utils.h auth_con.h sendauth.c
send_tgs.so send_tgs.po $(OUTPRE)send_tgs.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  fast.h int-proto.h send_tgs.c
ser_actx.so ser_actx.po $(OUTPRE)ser_actx.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(srcdir)/../rcache/memrcache.h $(top_srcdir)/include/k5-buf.h \
  $(top_srcdir)/include/k5-err.h $(top_srcdir)/include/k5-gmt_mktime.h \
  $(top_srcdir)/include/k5-int-pkinit.h $(top_srcdir)/include/k5-int.h \
  $(top_srcdir)/include/k5-platform.h $(top_srcdir)/include/k5-plugin.h \
  $(top_srcdir)/include/k5-thread.h $(top_srcdir)/include/k5-trace.h \
  $(top_srcdir)/include/krb5.h $(top_srcdir)/include/krb5/authdata_plugin.h \
  $(top_srcdir)/include/krb5/plugin.h $(top_srcdir)/include/port-sockets.h \
  $(top_srcdir)/include/socket-utils.h auth_con.h int-proto.h \
  ser_actx.c
ser_adata.so ser_adata.po $(OUTPRE)ser_adata.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  int-proto.h ser_adata.c
ser_addr.so ser_addr.po $(OUTPRE)ser_addr.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  int-proto.h ser_addr.c
ser_auth.so ser_auth.po $(OUTPRE)ser_auth.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  int-proto.h ser_auth.c
ser_cksum.so ser_cksum.po $(OUTPRE)ser_cksum.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  int-proto.h ser_cksum.c
ser_ctx.so ser_ctx.po $(OUTPRE)ser_ctx.$(OBJEXT): $(BUILDTOP)/include/autoconf.h \
  $(BUILDTOP)/include/krb5/krb5.h $(BUILDTOP)/include/osconf.h \
  $(BUILDTOP)/include/profile.h $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h \
  $(top_srcdir)/include/k5-err.h $(top_srcdir)/include/k5-gmt_mktime.h \
  $(top_srcdir)/include/k5-int-pkinit.h $(top_srcdir)/include/k5-int.h \
  $(top_srcdir)/include/k5-platform.h $(top_srcdir)/include/k5-plugin.h \
  $(top_srcdir)/include/k5-thread.h $(top_srcdir)/include/k5-trace.h \
  $(top_srcdir)/include/krb5.h $(top_srcdir)/include/krb5/authdata_plugin.h \
  $(top_srcdir)/include/krb5/plugin.h $(top_srcdir)/include/port-sockets.h \
  $(top_srcdir)/include/socket-utils.h int-proto.h ser_ctx.c
ser_key.so ser_key.po $(OUTPRE)ser_key.$(OBJEXT): $(BUILDTOP)/include/autoconf.h \
  $(BUILDTOP)/include/krb5/krb5.h $(BUILDTOP)/include/osconf.h \
  $(BUILDTOP)/include/profile.h $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h \
  $(top_srcdir)/include/k5-err.h $(top_srcdir)/include/k5-gmt_mktime.h \
  $(top_srcdir)/include/k5-int-pkinit.h $(top_srcdir)/include/k5-int.h \
  $(top_srcdir)/include/k5-platform.h $(top_srcdir)/include/k5-plugin.h \
  $(top_srcdir)/include/k5-thread.h $(top_srcdir)/include/k5-trace.h \
  $(top_srcdir)/include/krb5.h $(top_srcdir)/include/krb5/authdata_plugin.h \
  $(top_srcdir)/include/krb5/plugin.h $(top_srcdir)/include/port-sockets.h \
  $(top_srcdir)/include/socket-utils.h int-proto.h ser_key.c
ser_princ.so ser_princ.po $(OUTPRE)ser_princ.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  int-proto.h ser_princ.c
serialize.so serialize.po $(OUTPRE)serialize.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  serialize.c
set_realm.so set_realm.po $(OUTPRE)set_realm.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  set_realm.c
sname_match.so sname_match.po $(OUTPRE)sname_match.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  int-proto.h sname_match.c
srv_dec_tkt.so srv_dec_tkt.po $(OUTPRE)srv_dec_tkt.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  srv_dec_tkt.c
srv_rcache.so srv_rcache.po $(OUTPRE)srv_rcache.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  srv_rcache.c
str_conv.so str_conv.po $(OUTPRE)str_conv.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/kdb.h \
  $(top_srcdir)/include/krb5.h $(top_srcdir)/include/krb5/authdata_plugin.h \
  $(top_srcdir)/include/krb5/plugin.h $(top_srcdir)/include/port-sockets.h \
  $(top_srcdir)/include/socket-utils.h str_conv.c
t_ad_fx_armor.so t_ad_fx_armor.po $(OUTPRE)t_ad_fx_armor.$(OBJEXT): \
  $(BUILDTOP)/include/krb5/krb5.h $(COM_ERR_DEPS) t_ad_fx_armor.c
tgtname.so tgtname.po $(OUTPRE)tgtname.$(OBJEXT): $(BUILDTOP)/include/autoconf.h \
  $(BUILDTOP)/include/krb5/krb5.h $(BUILDTOP)/include/osconf.h \
  $(BUILDTOP)/include/profile.h $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h \
  $(top_srcdir)/include/k5-err.h $(top_srcdir)/include/k5-gmt_mktime.h \
  $(top_srcdir)/include/k5-int-pkinit.h $(top_srcdir)/include/k5-int.h \
  $(top_srcdir)/include/k5-platform.h $(top_srcdir)/include/k5-plugin.h \
  $(top_srcdir)/include/k5-thread.h $(top_srcdir)/include/k5-trace.h \
  $(top_srcdir)/include/krb5.h $(top_srcdir)/include/krb5/authdata_plugin.h \
  $(top_srcdir)/include/krb5/plugin.h $(top_srcdir)/include/port-sockets.h \
  $(top_srcdir)/include/socket-utils.h int-proto.h tgtname.c
unparse.so unparse.po $(OUTPRE)unparse.$(OBJEXT): $(BUILDTOP)/include/autoconf.h \
  $(BUILDTOP)/include/krb5/krb5.h $(BUILDTOP)/include/osconf.h \
  $(BUILDTOP)/include/profile.h $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h \
  $(top_srcdir)/include/k5-err.h $(top_srcdir)/include/k5-gmt_mktime.h \
  $(top_srcdir)/include/k5-int-pkinit.h $(top_srcdir)/include/k5-int.h \
  $(top_srcdir)/include/k5-platform.h $(top_srcdir)/include/k5-plugin.h \
  $(top_srcdir)/include/k5-thread.h $(top_srcdir)/include/k5-trace.h \
  $(top_srcdir)/include/krb5.h $(top_srcdir)/include/krb5/authdata_plugin.h \
  $(top_srcdir)/include/krb5/plugin.h $(top_srcdir)/include/port-sockets.h \
  $(top_srcdir)/include/socket-utils.h unparse.c
val_renew.so val_renew.po $(OUTPRE)val_renew.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  int-proto.h val_renew.c
valid_times.so valid_times.po $(OUTPRE)valid_times.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  int-proto.h valid_times.c
vfy_increds.so vfy_increds.po $(OUTPRE)vfy_increds.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  int-proto.h vfy_increds.c
vic_opt.so vic_opt.po $(OUTPRE)vic_opt.$(OBJEXT): $(BUILDTOP)/include/autoconf.h \
  $(BUILDTOP)/include/krb5/krb5.h $(BUILDTOP)/include/osconf.h \
  $(BUILDTOP)/include/profile.h $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h \
  $(top_srcdir)/include/k5-err.h $(top_srcdir)/include/k5-gmt_mktime.h \
  $(top_srcdir)/include/k5-int-pkinit.h $(top_srcdir)/include/k5-int.h \
  $(top_srcdir)/include/k5-platform.h $(top_srcdir)/include/k5-plugin.h \
  $(top_srcdir)/include/k5-thread.h $(top_srcdir)/include/k5-trace.h \
  $(top_srcdir)/include/krb5.h $(top_srcdir)/include/krb5/authdata_plugin.h \
  $(top_srcdir)/include/krb5/plugin.h $(top_srcdir)/include/port-sockets.h \
  $(top_srcdir)/include/socket-utils.h vic_opt.c
walk_rtree.so walk_rtree.po $(OUTPRE)walk_rtree.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  int-proto.h walk_rtree.c
t_walk_rtree.so t_walk_rtree.po $(OUTPRE)t_walk_rtree.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  t_walk_rtree.c
t_kerb.so t_kerb.po $(OUTPRE)t_kerb.$(OBJEXT): $(BUILDTOP)/include/autoconf.h \
  $(BUILDTOP)/include/krb5/krb5.h $(BUILDTOP)/include/osconf.h \
  $(BUILDTOP)/include/profile.h $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h \
  $(top_srcdir)/include/k5-err.h $(top_srcdir)/include/k5-gmt_mktime.h \
  $(top_srcdir)/include/k5-int-pkinit.h $(top_srcdir)/include/k5-int.h \
  $(top_srcdir)/include/k5-platform.h $(top_srcdir)/include/k5-plugin.h \
  $(top_srcdir)/include/k5-thread.h $(top_srcdir)/include/k5-trace.h \
  $(top_srcdir)/include/krb5.h $(top_srcdir)/include/krb5/authdata_plugin.h \
  $(top_srcdir)/include/krb5/plugin.h $(top_srcdir)/include/port-sockets.h \
  $(top_srcdir)/include/socket-utils.h t_kerb.c
t_ser.so t_ser.po $(OUTPRE)t_ser.$(OBJEXT): $(BUILDTOP)/include/autoconf.h \
  $(BUILDTOP)/include/krb5/krb5.h $(BUILDTOP)/include/osconf.h \
  $(BUILDTOP)/include/profile.h $(COM_ERR_DEPS) $(srcdir)/../rcache/memrcache.h \
  $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  auth_con.h t_ser.c
t_deltat.so t_deltat.po $(OUTPRE)t_deltat.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  t_deltat.c
t_expand.so t_expand.po $(OUTPRE)t_expand.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  chk_trans.c t_expand.c
t_get_etype_info.so t_get_etype_info.po $(OUTPRE)t_get_etype_info.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-hex.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-thread.h $(top_srcdir)/include/krb5.h \
  t_get_etype_info.c
t_pac.so t_pac.po $(OUTPRE)t_pac.$(OBJEXT): $(BUILDTOP)/include/autoconf.h \
  $(BUILDTOP)/include/krb5/krb5.h $(BUILDTOP)/include/osconf.h \
  $(BUILDTOP)/include/profile.h $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h \
  $(top_srcdir)/include/k5-err.h $(top_srcdir)/include/k5-gmt_mktime.h \
  $(top_srcdir)/include/k5-int-pkinit.h $(top_srcdir)/include/k5-int.h \
  $(top_srcdir)/include/k5-platform.h $(top_srcdir)/include/k5-plugin.h \
  $(top_srcdir)/include/k5-thread.h $(top_srcdir)/include/k5-trace.h \
  $(top_srcdir)/include/krb5.h $(top_srcdir)/include/krb5/authdata_plugin.h \
  $(top_srcdir)/include/krb5/plugin.h $(top_srcdir)/include/port-sockets.h \
  $(top_srcdir)/include/socket-utils.h t_pac.c
t_parse_host_string.so t_parse_host_string.po $(OUTPRE)t_parse_host_string.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-cmocka.h \
  $(top_srcdir)/include/k5-err.h $(top_srcdir)/include/k5-gmt_mktime.h \
  $(top_srcdir)/include/k5-int-pkinit.h $(top_srcdir)/include/k5-int.h \
  $(top_srcdir)/include/k5-platform.h $(top_srcdir)/include/k5-plugin.h \
  $(top_srcdir)/include/k5-thread.h $(top_srcdir)/include/k5-trace.h \
  $(top_srcdir)/include/krb5.h $(top_srcdir)/include/krb5/authdata_plugin.h \
  $(top_srcdir)/include/krb5/plugin.h $(top_srcdir)/include/port-sockets.h \
  $(top_srcdir)/include/socket-utils.h t_parse_host_string.c
t_princ.so t_princ.po $(OUTPRE)t_princ.$(OBJEXT): $(BUILDTOP)/include/autoconf.h \
  $(BUILDTOP)/include/krb5/krb5.h $(BUILDTOP)/include/osconf.h \
  $(BUILDTOP)/include/profile.h $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h \
  $(top_srcdir)/include/k5-err.h $(top_srcdir)/include/k5-gmt_mktime.h \
  $(top_srcdir)/include/k5-int-pkinit.h $(top_srcdir)/include/k5-int.h \
  $(top_srcdir)/include/k5-platform.h $(top_srcdir)/include/k5-plugin.h \
  $(top_srcdir)/include/k5-thread.h $(top_srcdir)/include/k5-trace.h \
  $(top_srcdir)/include/krb5.h $(top_srcdir)/include/krb5/authdata_plugin.h \
  $(top_srcdir)/include/krb5/plugin.h $(top_srcdir)/include/port-sockets.h \
  $(top_srcdir)/include/socket-utils.h t_princ.c
t_etypes.so t_etypes.po $(OUTPRE)t_etypes.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  t_etypes.c
t_expire_warn.so t_expire_warn.po $(OUTPRE)t_expire_warn.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  t_expire_warn.c
t_authdata.so t_authdata.po $(OUTPRE)t_authdata.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  t_authdata.c
t_cc_config.so t_cc_config.po $(OUTPRE)t_cc_config.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  int-proto.h t_cc_config.c
t_copy_context.so t_copy_context.po $(OUTPRE)t_copy_context.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  t_copy_context.c
t_in_ccache.so t_in_ccache.po $(OUTPRE)t_in_ccache.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  t_in_ccache.c
t_response_items.so t_response_items.po $(OUTPRE)t_response_items.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  int-proto.h t_response_items.c
t_sname_match.so t_sname_match.po $(OUTPRE)t_sname_match.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  t_sname_match.c
t_valid_times.so t_valid_times.po $(OUTPRE)t_valid_times.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  int-proto.h t_valid_times.c
t_vfy_increds.so t_vfy_increds.po $(OUTPRE)t_vfy_increds.$(OBJEXT): \
  $(BUILDTOP)/include/autoconf.h $(BUILDTOP)/include/krb5/krb5.h \
  $(BUILDTOP)/include/osconf.h $(BUILDTOP)/include/profile.h \
  $(COM_ERR_DEPS) $(top_srcdir)/include/k5-buf.h $(top_srcdir)/include/k5-err.h \
  $(top_srcdir)/include/k5-gmt_mktime.h $(top_srcdir)/include/k5-int-pkinit.h \
  $(top_srcdir)/include/k5-int.h $(top_srcdir)/include/k5-platform.h \
  $(top_srcdir)/include/k5-plugin.h $(top_srcdir)/include/k5-thread.h \
  $(top_srcdir)/include/k5-trace.h $(top_srcdir)/include/krb5.h \
  $(top_srcdir)/include/krb5/authdata_plugin.h $(top_srcdir)/include/krb5/plugin.h \
  $(top_srcdir)/include/port-sockets.h $(top_srcdir)/include/socket-utils.h \
  t_vfy_increds.c
############################################################
## config/post.in
##

# in case there is no default target (very unlikely)
all:

check-windows:

# In a few parts of "make check" we run shell scripts which run
# programs linked against krb5 libraries.  On macOS 10.11 and higher,
# DYLD_LIBRARY_PATH is cleared by the shell unless System Integrity
# Protection is turned off, so we need to set runtime linker
# environment variables from within test scripts.  A Makefile.in which
# runs shell script tests should make its check rule depend on
# runenv.sh and make each script begin with ". ./runenv.sh".
runenv.sh:
	$(RUN_SETUP); for i in $(RUN_VARS); do \
		eval echo "$$i=\\\"\$$$$i\\\""; \
		echo "export $$i"; done > $@

##############################
# dependency generation
#

depend: depend-postrecurse
depend-postrecurse: depend-recurse
depend-recurse: depend-prerecurse

depend-prerecurse:
depend-postrecurse:

depend-postrecurse: depend-update-makefile

ALL_DEP_SRCS= $(SRCS) $(EXTRADEPSRCS)

# be sure to check ALL_DEP_SRCS against *what it would be if SRCS and
# EXTRADEPSRCS are both empty*
$(BUILDTOP)/.depend-verify-srcdir:
	@if test "$(srcdir)" = "." ; then \
		echo 1>&2 error: cannot build dependencies with srcdir=. ; \
		echo 1>&2 "(can't distinguish generated files from source files)" ; \
		echo 1>&2 "Run 'make distclean' and create a separate build dir" ; \
		exit 1 ; \
	elif test -f "$(top_srcdir)/include/autoconf.h"; then \
		echo 1>&2 "error: generated headers found in source tree" ; \
		echo 1>&2 "Run 'make distclean' in source tree first" ; \
		exit 1 ; \
	else \
		if test -r $(BUILDTOP)/.depend-verify-srcdir; then :; \
			else (set -x; touch $(BUILDTOP)/.depend-verify-srcdir); fi \
	fi
$(BUILDTOP)/.depend-verify-et: depend-verify-et-$(COM_ERR_VERSION)
depend-verify-et-k5:
	@if test -r $(BUILDTOP)/.depend-verify-et; then :; \
		else (set -x; touch $(BUILDTOP)/.depend-verify-et); fi
depend-verify-et-sys depend-verify-et-intlsys:
	@echo 1>&2 error: cannot build dependencies using system et package
	@exit 1
$(BUILDTOP)/.depend-verify-ss: depend-verify-ss-$(SS_VERSION)
depend-verify-ss-k5:
	@if test -r $(BUILDTOP)/.depend-verify-ss; then :; \
		else (set -x; touch $(BUILDTOP)/.depend-verify-ss); fi
depend-verify-ss-sys:
	@echo 1>&2 error: cannot build dependencies using system ss package
	@exit 1
$(BUILDTOP)/.depend-verify-verto: depend-verify-verto-$(VERTO_VERSION)
depend-verify-verto-k5:
	@if test -r $(BUILDTOP)/.depend-verify-verto; then :; \
		else (set -x; touch $(BUILDTOP)/.depend-verify-verto); fi
depend-verify-verto-sys:
	@echo 1>&2 error: cannot build dependencies using system verto package
	@echo 1>&2 Please configure with --without-system-verto
	@exit 1
$(BUILDTOP)/.depend-verify-gcc: depend-verify-gcc-yes
depend-verify-gcc-yes:
	@if test -r $(BUILDTOP)/.depend-verify-gcc; then :; \
		else (set -x; touch $(BUILDTOP)/.depend-verify-gcc); fi
depend-verify-gcc-no:
	@echo 1>&2 error: The '"depend"' rules are written for gcc.
	@echo 1>&2 Please use gcc, or update the rules to handle your compiler.
	@exit 1

DEP_CFG_VERIFY = $(BUILDTOP)/.depend-verify-srcdir \
	$(BUILDTOP)/.depend-verify-et $(BUILDTOP)/.depend-verify-ss \
	$(BUILDTOP)/.depend-verify-verto
DEP_VERIFY = $(DEP_CFG_VERIFY) $(BUILDTOP)/.depend-verify-gcc

.d: $(ALL_DEP_SRCS) $(DEP_CFG_VERIFY) depend-dependencies
	if test "$(ALL_DEP_SRCS)" != " " ; then \
		$(RM) .dtmp && $(MAKE) .dtmp && mv -f .dtmp .d ; \
	else \
		touch .d ; \
	fi

# These are dependencies of the depend target that do not get fed to
# the compiler.  Examples include generated header files.
depend-dependencies:

# .dtmp must *always* be out of date so that $? can be used to perform
# VPATH searches on the sources.
#
# NOTE: This will fail when using Make programs whose VPATH support is
# broken.
.dtmp: $(ALL_DEP_SRCS)
	$(CC) -M -DDEPEND $(ALL_CFLAGS) $? > .dtmp

# NOTE: This will also generate spurious $(OUTPRE) and $(OBJEXT)
# references in rules for non-library objects in a directory where
# library objects happen to be built.  It's mostly harmless.
.depend: .d $(top_srcdir)/util/depfix.pl
	perl $(top_srcdir)/util/depfix.pl '$(top_srcdir)' '$(mydir)' \
		'$(srcdir)' '$(BUILDTOP)' '$(STLIBOBJS)' < .d > .depend

# Temporarily keep the rule for removing the dependency line eater
# until we're sure we've gotten everything converted and excised the
# old stuff from Makefile.in files.
depend-update-makefile: .depend depend-recurse
	if test "$(ALL_DEP_SRCS)" != " " ; then \
		$(CP) .depend $(srcdir)/deps.new ; \
	else \
		echo "# No dependencies here." > $(srcdir)/deps.new ; \
	fi
	$(top_srcdir)/config/move-if-changed $(srcdir)/deps.new $(srcdir)/deps
	sed -e '/^# +++ Dependency line eater +++/,$$d' \
		< $(srcdir)/Makefile.in > $(srcdir)/Makefile.in.new
	$(top_srcdir)/config/move-if-changed $(srcdir)/Makefile.in.new \
		$(srcdir)/Makefile.in

DEPTARGETS = .depend .d .dtmp $(DEP_VERIFY)
DEPTARGETS_CLEAN = .depend .d .dtmp $(DEPTARGETS_._.)
DEPTARGETS_../../.._. = $(DEP_VERIFY)

# Clear out dependencies.  Should only be used temporarily, e.g., while
# moving or renaming headers and then rebuilding dependencies.
undepend: undepend-postrecurse
undepend-recurse:
undepend-postrecurse: undepend-recurse
	if test -n "$(SRCS)" ; then \
		sed -e '/^# +++ Dependency line eater +++/,$$d' \
			< $(srcdir)/Makefile.in \
			> $(srcdir)/Makefile.in.new ;\
		echo "# +++ Dependency line eater +++" >> $(srcdir)/Makefile.in.new ;\
		echo "# (dependencies temporarily removed)" >> $(srcdir)/Makefile.in.new ;\
		$(top_srcdir)/config/move-if-changed $(srcdir)/Makefile.in.new $(srcdir)/Makefile.in;\
	else :; fi

#
# end dependency generation
##############################

# Python tests
check-unix: check-pytests-yes

# Makefile.in should add rules to check-pytests to execute Python tests.
check-pytests-yes: check-pytests
check-pytests-no:
check-pytests:

# cmocka tests
check-unix: check-cmocka-no

check-cmocka-yes: check-cmocka
check-cmocka-no:
check-cmocka:

clean: clean-$(WHAT)

clean-unix::
	$(RM) $(OBJS) $(DEPTARGETS_CLEAN) $(EXTRA_FILES)
	$(RM) et-[ch]-*.et et-[ch]-*.[ch] testlog testtrace runenv.sh
	-$(RM) -r testdir

clean-windows::
	$(RM) *.$(OBJEXT)
	$(RM) msvc.pdb *.err

distclean: distclean-$(WHAT)

distclean-normal-clean:
	$(MAKE) NORECURSE=true clean
distclean-prerecurse: distclean-normal-clean
distclean-nuke-configure-state:
	$(RM) config.log config.cache config.status Makefile
distclean-postrecurse: distclean-nuke-configure-state

Makefiles-prerecurse: Makefile

# mydir = relative path from top to this Makefile
Makefile: $(srcdir)/Makefile.in $(srcdir)/deps $(BUILDTOP)/config.status \
		$(top_srcdir)/config/pre.in $(top_srcdir)/config/post.in
	(cd $(BUILDTOP) && $(SHELL) config.status $(mydir)/Makefile)
$(BUILDTOP)/config.status: $(top_srcdir)/configure
	(cd $(BUILDTOP) && $(SHELL) config.status --recheck)
$(top_srcdir)/configure: # \
		$(top_srcdir)/configure.ac \
		$(top_srcdir)/patchlevel.h \
		$(top_srcdir)/aclocal.m4
	(cd $(top_srcdir) && \
		$(AUTOCONF) -f --include=$(CONFIG_RELTOPDIR) $(AUTOCONFFLAGS))

RECURSE_TARGETS=all-recurse clean-recurse distclean-recurse install-recurse \
	generate-files-mac-recurse \
	check-recurse depend-recurse undepend-recurse \
	Makefiles-recurse install-headers-recurse

# MY_SUBDIRS overrides any setting of SUBDIRS generated by the
# configure script that generated this Makefile.  This is needed when
# the configure script that produced this Makefile creates multiple
# Makefiles in different directories; the setting of SUBDIRS will be
# the same in each.
#
# LOCAL_SUBDIRS seems to account for the case where the configure
# script doesn't call any other subsidiary configure scripts, but
# generates multiple Makefiles.
$(RECURSE_TARGETS):
	@case "`echo 'x$(MFLAGS)'|sed -e 's/^x//' -e 's/ --.*$$//'`" \
		in *[ik]*) e="status=1" ;; *) e="exit 1";; esac; \
	do_subdirs="$(SUBDIRS)" ; \
	status=0; \
	if test -n "$$do_subdirs" && test -z "$(NORECURSE)"; then \
	for i in $$do_subdirs ; do \
		if test -d $$i && test -r $$i/Makefile ; then \
		case $$i in .);; *) \
			target=`echo $@|sed s/-recurse//`; \
			echo "making $$target in $(CURRENT_DIR)$$i..."; \
			if (cd $$i ; $(MAKE) \
			    CURRENT_DIR=$(CURRENT_DIR)$$i/ $$target) then :; \
			else eval $$e; fi; \
			;; \
		esac; \
		else \
			echo "Skipping missing directory $(CURRENT_DIR)$$i" ; \
		fi; \
	done; \
	else :; \
	fi;\
	exit $$status

##
## end of post.in
############################################################
