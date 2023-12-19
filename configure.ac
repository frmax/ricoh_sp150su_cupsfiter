AC_INIT([rastertosp150su], [0.2], [https://github.com/frmax/ricoh_sp150su_cupsfiter])
AM_INIT_AUTOMAKE([-Wall -Werror foreign])
AC_LANG([C])
AC_PROG_CC
AC_CONFIG_HEADERS([config.h])

# Find CUPS
AC_PATH_PROG(CUPSCONFIG, [cups-config])
    AS_IF([test -z "$CUPSCONFIG"], [
        AC_MSG_ERROR([Required cups-confing is missing. Please install CUPS developer packages.])
    ])

CUPS_SERVERBIN="`$CUPSCONFIG --serverbin`"
AC_DEFINE_UNQUOTED(CUPS_SERVERBIN, "$CUPS_SERVERBIN", [Path to CUPS binaries dir])
AC_SUBST(CUPS_SERVERBIN)

CUPS_DATADIR="`$CUPSCONFIG --datadir`"
AC_DEFINE_UNQUOTED(CUPS_DATADIR, "$CUPS_DATADIR", [Path to CUPS binaries dir])
AC_SUBST(CUPS_DATADIR)



AC_CHECK_HEADERS([jbig.h])

AC_CONFIG_FILES([
    Makefile
    src/Makefile
])
AC_OUTPUT
