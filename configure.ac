AC_PREFREQ[2.62]
AC_INIT([TMC],[1.0],[abelits@marvell.com])
AM_MAINTAINER_MODE
AC_PROG_CC
AM_PROG_AR
LT_INIT([disable-shared])
AC_SUBST([LIBTOOL_DEPS])
AM_PROG_LIBTOOL
AC_CONFIG_MACRO_DIRS([m4])
AM_INIT_AUTOMAKE([foreign])
AX_PTHREAD([CC="$PTHREAD_CC"
            CFLAGS="$CFLAGS $PTHREAD_CFLAGS"
            LIBS="$PTHREAD_LIBS $LIBS"],
	    [AC_MSG_FAILURE([pthread library required])])
AC_CHECK_LIB([rt], [shm_unlink], [LIBS="$LIBS -lrt"])
AC_CHECK_FUNCS([renameat2])
AC_CONFIG_FILES([Makefile include/Makefile libtmc.pc:libtmc.pc.in])
AC_OUTPUT
