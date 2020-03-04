ACLOCAL_AMFLAGS = -I m4
lib_LTLIBRARIES = libtmc.la
libtmc_la_SOURCES = isol.c isol-server.c
libtmc_la_CFLAGS = -I$(abs_top_srcdir)/include -D_GNU_SOURCE

bin_PROGRAMS = isol-interrupt-mon isol-test app-ctl

isol_interrupt_mon_SOURCES = isol-interrupt-mon.c

isol_test_SOURCES = isol-test.c
isol_test_CFLAGS = -I$(abs_top_srcdir)/include -D_GNU_SOURCE 
isol_test_LDADD = libtmc.la

app_ctl_SOURCES = app-ctl.c
app_ctl_CFLAGS = -I$(abs_top_srcdir)/include -D_GNU_SOURCE

SUBDIRS = include
