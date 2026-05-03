PHP_ARG_ENABLE([nghttp2_poc], [whether to enable nghttp2 poc], [AS_HELP_STRING([--enable-nghttp2-poc], [Enable nghttp2 transport PoC])], [no])

if test "$PHP_NGHTTP2_POC" != "no"; then
  PKG_CHECK_MODULES([NGHTTP2], [libnghttp2])
  PHP_EVAL_INCLINE($NGHTTP2_CFLAGS)
  PHP_EVAL_LIBLINE([$NGHTTP2_LIBS -lpthread], NGHTTP2_POC_SHARED_LIBADD)
  PHP_SUBST(NGHTTP2_POC_SHARED_LIBADD)
  AC_DEFINE(HAVE_NGHTTP2_POC, 1, [Have nghttp2 transport PoC])
  PHP_NEW_EXTENSION(nghttp2_poc, nghttp2_poc.c, $ext_shared)
fi
