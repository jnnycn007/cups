dnl
dnl Configuration script for CUPS.
dnl
dnl Copyright © 2020-2023 by OpenPrinting
dnl Copyright © 2007-2020 by Apple Inc.
dnl Copyright © 1997-2007 by Easy Software Products, all rights reserved.
dnl
dnl Licensed under Apache License v2.0.  See the file "LICENSE" for more
dnl information.
dnl

dnl We need at least autoconf 2.71...
AC_PREREQ([2.71])

dnl Package name and version...
AC_INIT([CUPS],[2.5b1],[https://github.com/openprinting/cups/issues],[cups],[https://openprinting.github.io/cups])

dnl This line is provided to ensure that you don't run the autoheader program
dnl against this project.  Doing so is completely unsupported and WILL cause
dnl problems!
AH_TOP([#error "Somebody ran autoheader on this project which is unsupported and WILL cause problems."])

dnl Include configure tests for different areas...
sinclude(config-scripts/cups-opsys.m4)
sinclude(config-scripts/cups-common.m4)
sinclude(config-scripts/cups-directories.m4)
sinclude(config-scripts/cups-manpages.m4)
sinclude(config-scripts/cups-sharedlibs.m4)
sinclude(config-scripts/cups-libtool.m4)
sinclude(config-scripts/cups-compiler.m4)
sinclude(config-scripts/cups-network.m4)
sinclude(config-scripts/cups-gssapi.m4)
sinclude(config-scripts/cups-threads.m4)
sinclude(config-scripts/cups-tls.m4)
sinclude(config-scripts/cups-pam.m4)
sinclude(config-scripts/cups-largefile.m4)
sinclude(config-scripts/cups-dnssd.m4)
sinclude(config-scripts/cups-container.m4)
sinclude(config-scripts/cups-startup.m4)
sinclude(config-scripts/cups-defaults.m4)

dnl Add static libraries when not building shared libraries...
AS_IF([test x$enable_shared = xno], [
    PKGCONFIG_LIBS="$PKGCONFIG_LIBS $PKGCONFIG_LIBS_STATIC"
    PKGCONFIG_LIBS_STATIC=""
])

dnl Provide autoconf with a list of files to generate and output them...
AC_CONFIG_FILES([
    Makedefs
    conf/cups-files.conf
    conf/cupsd.conf
    conf/mime.convs
    conf/pam.std
    conf/snmp.conf
    cups.pc
    desktop/cups.desktop
    scheduler/cups-lpd.xinetd
    scheduler/cups.sh
    scheduler/cups.xml
    scheduler/org.cups.cups-lpd.plist
    scheduler/cups-lpdAT.service
    scheduler/cups.path
    scheduler/cups.service
    scheduler/cups.socket
    packaging/cups.list
])
AC_OUTPUT
