PHP_ARG_ENABLE([grpc], [whether to enable php-grpc-lite grpc extension], [AS_HELP_STRING([--enable-grpc], [Enable php-grpc-lite grpc extension])], [no])
PHP_ARG_ENABLE([grpc-bench], [whether to enable php-grpc-lite benchmark-only entrypoints], [AS_HELP_STRING([--enable-grpc-bench], [Enable php-grpc-lite benchmark-only entrypoints])], [no], [no])

if test "$PHP_GRPC" != "no"; then
  PKG_CHECK_MODULES([NGHTTP2], [libnghttp2])
  PKG_CHECK_MODULES([OPENSSL], [openssl])
  PHP_EVAL_INCLINE($NGHTTP2_CFLAGS)
  PHP_EVAL_INCLINE($OPENSSL_CFLAGS)
  PHP_EVAL_LIBLINE([$NGHTTP2_LIBS $OPENSSL_LIBS], GRPC_SHARED_LIBADD)
  PHP_SUBST(GRPC_SHARED_LIBADD)
  AC_DEFINE(HAVE_GRPC, 1, [Have php-grpc-lite grpc extension])
  if test "$PHP_GRPC_BENCH" != "no"; then
    AC_DEFINE(PHP_GRPC_LITE_ENABLE_BENCH, 1, [Enable php-grpc-lite benchmark-only entrypoints])
  fi
  PHP_NEW_EXTENSION(grpc, main.c protocol_core.c status_core.c transport_core.c, $ext_shared)
fi
