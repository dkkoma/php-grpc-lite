PHP_ARG_ENABLE([grpc], [whether to enable php-grpc-lite native grpc extension], [AS_HELP_STRING([--enable-grpc], [Enable php-grpc-lite native grpc extension])], [no])

if test "$PHP_GRPC" != "no"; then
  PKG_CHECK_MODULES([NGHTTP2], [libnghttp2])
  PKG_CHECK_MODULES([OPENSSL], [openssl])
  PHP_EVAL_INCLINE($NGHTTP2_CFLAGS)
  PHP_EVAL_INCLINE($OPENSSL_CFLAGS)
  PHP_EVAL_LIBLINE([$NGHTTP2_LIBS $OPENSSL_LIBS -lpthread], GRPC_SHARED_LIBADD)
  PHP_SUBST(GRPC_SHARED_LIBADD)
  AC_DEFINE(HAVE_GRPC, 1, [Have php-grpc-lite native grpc extension])
  PHP_NEW_EXTENSION(grpc, grpc.c, $ext_shared)
fi
