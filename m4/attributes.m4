dnl Macros to check the presence of generic (non-typed) symbols.
dnl Copyright (c) 2006-2008 Diego Pettenò <flameeyes@gmail.com>
dnl Copyright (c) 2006-2008 xine project
dnl Copyright (c) 2012 Lucas De Marchi <lucas.de.marchi@gmail.com>
dnl
dnl This program is free software; you can redistribute it and/or modify
dnl it under the terms of the GNU General Public License as published by
dnl the Free Software Foundation; either version 2, or (at your option)
dnl any later version.
dnl
dnl This program is distributed in the hope that it will be useful,
dnl but WITHOUT ANY WARRANTY; without even the implied warranty of
dnl MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
dnl GNU General Public License for more details.
dnl
dnl You should have received a copy of the GNU General Public License
dnl along with this program; if not, write to the Free Software
dnl Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
dnl 02110-1301, USA.
dnl
dnl As a special exception, the copyright owners of the
dnl macro gives unlimited permission to copy, distribute and modify the
dnl configure scripts that are the output of Autoconf when processing the
dnl Macro. You need not follow the terms of the GNU General Public
dnl License when using or distributing such scripts, even though portions
dnl of the text of the Macro appear in them. The GNU General Public
dnl License (GPL) does govern all other use of the material that
dnl constitutes the Autoconf Macro.
dnl
dnl This special exception to the GPL applies to versions of the
dnl Autoconf Macro released by this project. When you make and
dnl distribute a modified version of the Autoconf Macro, you may extend
dnl this special exception to the GPL to apply to your modified version as
dnl well.

dnl Check if FLAG in ENV-VAR is supported by compiler and append it
dnl to WHERE-TO-APPEND variable. Note that we invert -Wno-* checks to
dnl -W* as gcc cannot test for negated warnings.
dnl CC_CHECK_FLAG_APPEND([WHERE-TO-APPEND], [ENV-VAR], [FLAG], [C-SNIPPET])

AC_DEFUN([CC_CHECK_FLAG_APPEND], [
  AC_CACHE_CHECK([if $CC supports flag $3 in envvar $2],
                 AS_TR_SH([cc_cv_$2_$3]),
          [eval "AS_TR_SH([cc_save_$2])='${$2}'"
           eval "AS_TR_SH([$2])='-Werror `echo "$3" | sed 's/^-Wno-/-W/'`'"
           AC_LINK_IFELSE([AC_LANG_SOURCE(ifelse([$4], [],
                                                 [int main(void) { return 0; } ],
                                                 [$4]))],
                          [eval "AS_TR_SH([cc_cv_$2_$3])='yes'"],
                          [eval "AS_TR_SH([cc_cv_$2_$3])='no'"])
           eval "AS_TR_SH([$2])='$cc_save_$2'"])

  AS_IF([eval test x$]AS_TR_SH([cc_cv_$2_$3])[ = xyes],
        [eval "$1='${$1} $3'"])
])

dnl CC_CHECK_FLAGS_APPEND([WHERE-TO-APPEND], [ENV-VAR], [FLAG1 FLAG2])
AC_DEFUN([CC_CHECK_FLAGS_APPEND], [
  for flag in $3; do
    CC_CHECK_FLAG_APPEND($1, $2, $flag)
  done
])

dnl Check if the flag is supported by linker (cacheable)
dnl CC_CHECK_LDFLAGS([FLAG], [ACTION-IF-FOUND],[ACTION-IF-NOT-FOUND])

AC_DEFUN([CC_CHECK_LDFLAGS], [
  AC_CACHE_CHECK([if $CC supports $1 flag],
    AS_TR_SH([cc_cv_ldflags_$1]),
    [ac_save_LDFLAGS="$LDFLAGS"
     LDFLAGS="$LDFLAGS $1"
     AC_LINK_IFELSE([int main() { return 1; }],
       [eval "AS_TR_SH([cc_cv_ldflags_$1])='yes'"],
       [eval "AS_TR_SH([cc_cv_ldflags_$1])="])
     LDFLAGS="$ac_save_LDFLAGS"
    ])

  AS_IF([eval test x$]AS_TR_SH([cc_cv_ldflags_$1])[ = xyes],
    [$2], [$3])
])

dnl define the LDFLAGS_NOUNDEFINED variable with the correct value for
dnl the current linker to avoid undefined references in a shared object.
AC_DEFUN([CC_NOUNDEFINED], [
  dnl We check $host for which systems to enable this for.
  AC_REQUIRE([AC_CANONICAL_HOST])

  case $host in
     dnl FreeBSD (et al.) does not complete linking for shared objects when pthreads
     dnl are requested, as different implementations are present; to avoid problems
     dnl use -Wl,-z,defs only for those platform not behaving this way.
     *-freebsd* | *-openbsd*) ;;
     *)
        dnl First of all check for the --no-undefined variant of GNU ld. This allows
        dnl for a much more readable command line, so that people can understand what
        dnl it does without going to look for what the heck -z defs does.
        for possible_flags in "-Wl,--no-undefined" "-Wl,-z,defs"; do
           CC_CHECK_LDFLAGS([$possible_flags], [LDFLAGS_NOUNDEFINED="$possible_flags"])
           break
        done
     ;;
  esac

  AC_SUBST([LDFLAGS_NOUNDEFINED])
])

dnl Check for a -Werror flag or equivalent. -Werror is the GCC
dnl and ICC flag that tells the compiler to treat all the warnings
dnl as fatal. We usually need this option to make sure that some
dnl constructs (like attributes) are not simply ignored.
dnl
dnl Other compilers don't support -Werror per se, but they support
dnl an equivalent flag:
dnl  - Sun Studio compiler supports -errwarn=%all
AC_DEFUN([CC_CHECK_WERROR], [
  AC_CACHE_CHECK(
    [for $CC way to treat warnings as errors],
    [cc_cv_werror],
    [CC_CHECK_CFLAGS_SILENT([-Werror], [cc_cv_werror=-Werror],
      [CC_CHECK_CFLAGS_SILENT([-errwarn=%all], [cc_cv_werror=-errwarn=%all])])
    ])
])

AC_DEFUN([CC_CHECK_ATTRIBUTE], [
  AC_REQUIRE([CC_CHECK_WERROR])
  AC_CACHE_CHECK([if $CC supports __attribute__(( ifelse([$2], , [$1], [$2]) ))],
    AS_TR_SH([cc_cv_attribute_$1]),
    [ac_save_CFLAGS="$CFLAGS"
     CFLAGS="$CFLAGS $cc_cv_werror"
     AC_COMPILE_IFELSE([AC_LANG_SOURCE([$3])],
       [eval "AS_TR_SH([cc_cv_attribute_$1])='yes'"],
       [eval "AS_TR_SH([cc_cv_attribute_$1])='no'"])
     CFLAGS="$ac_save_CFLAGS"
    ])

  AS_IF([eval test x$]AS_TR_SH([cc_cv_attribute_$1])[ = xyes],
    [AC_DEFINE(
       AS_TR_CPP([SUPPORT_ATTRIBUTE_$1]), 1,
         [Define this if the compiler supports __attribute__(( ifelse([$2], , [$1], [$2]) ))]
         )
     $4],
    [$5])
])

AC_DEFUN([CC_ATTRIBUTE_CONSTRUCTOR], [
  CC_CHECK_ATTRIBUTE(
    [constructor],,
    [void __attribute__((constructor)) ctor() { int a; }],
    [$1], [$2])
])

AC_DEFUN([CC_ATTRIBUTE_FORMAT], [
  CC_CHECK_ATTRIBUTE(
    [format], [format(printf, n, n)],
    [void __attribute__((format(printf, 1, 2))) printflike(const char *fmt, ...) { fmt = (void *)0; }],
    [$1], [$2])
])

AC_DEFUN([CC_ATTRIBUTE_FORMAT_ARG], [
  CC_CHECK_ATTRIBUTE(
    [format_arg], [format_arg(printf)],
    [char *__attribute__((format_arg(1))) gettextlike(const char *fmt) { fmt = (void *)0; }],
    [$1], [$2])
])

AC_DEFUN([CC_ATTRIBUTE_VISIBILITY], [
  CC_CHECK_ATTRIBUTE(
    [visibility_$1], [visibility("$1")],
    [void __attribute__((visibility("$1"))) $1_function() { }],
    [$2], [$3])
])

AC_DEFUN([CC_ATTRIBUTE_NONNULL], [
  CC_CHECK_ATTRIBUTE(
    [nonnull], [nonnull()],
    [void __attribute__((nonnull())) some_function(void *foo, void *bar) { foo = (void*)0; bar = (void*)0; }],
    [$1], [$2])
])

AC_DEFUN([CC_ATTRIBUTE_UNUSED], [
  CC_CHECK_ATTRIBUTE(
    [unused], ,
    [void some_function(void *foo, __attribute__((unused)) void *bar);],
    [$1], [$2])
])

AC_DEFUN([CC_ATTRIBUTE_SENTINEL], [
  CC_CHECK_ATTRIBUTE(
    [sentinel], ,
    [void some_function(void *foo, ...) __attribute__((sentinel));],
    [$1], [$2])
])

AC_DEFUN([CC_ATTRIBUTE_DEPRECATED], [
  CC_CHECK_ATTRIBUTE(
    [deprecated], ,
    [void some_function(void *foo, ...) __attribute__((deprecated));],
    [$1], [$2])
])

AC_DEFUN([CC_ATTRIBUTE_ALIAS], [
  CC_CHECK_ATTRIBUTE(
    [alias], [weak, alias],
    [void other_function(void *foo) { }
     void some_function(void *foo) __attribute__((weak, alias("other_function")));],
    [$1], [$2])
])

AC_DEFUN([CC_ATTRIBUTE_MALLOC], [
  CC_CHECK_ATTRIBUTE(
    [malloc], ,
    [void * __attribute__((malloc)) my_alloc(int n);],
    [$1], [$2])
])

AC_DEFUN([CC_ATTRIBUTE_PACKED], [
  CC_CHECK_ATTRIBUTE(
    [packed], ,
    [struct astructure { char a; int b; long c; void *d; } __attribute__((packed));],
    [$1], [$2])
])

AC_DEFUN([CC_ATTRIBUTE_CONST], [
  CC_CHECK_ATTRIBUTE(
    [const], ,
    [int __attribute__((const)) twopow(int n) { return 1 << n; } ],
    [$1], [$2])
])

AC_DEFUN([CC_FLAG_VISIBILITY], [
  AC_REQUIRE([CC_CHECK_WERROR])
  AC_CACHE_CHECK([if $CC supports -fvisibility=hidden],
    [cc_cv_flag_visibility],
    [cc_flag_visibility_save_CFLAGS="$CFLAGS"
     CFLAGS="$CFLAGS $cc_cv_werror"
     CC_CHECK_CFLAGS_SILENT([-fvisibility=hidden],
     cc_cv_flag_visibility='yes',
     cc_cv_flag_visibility='no')
     CFLAGS="$cc_flag_visibility_save_CFLAGS"])

  AS_IF([test "x$cc_cv_flag_visibility" = "xyes"],
    [AC_DEFINE([SUPPORT_FLAG_VISIBILITY], 1,
       [Define this if the compiler supports the -fvisibility flag])
     $1],
    [$2])
])

AC_DEFUN([CC_FUNC_EXPECT], [
  AC_REQUIRE([CC_CHECK_WERROR])
  AC_CACHE_CHECK([if compiler has __builtin_expect function],
    [cc_cv_func_expect],
    [ac_save_CFLAGS="$CFLAGS"
     CFLAGS="$CFLAGS $cc_cv_werror"
     AC_COMPILE_IFELSE([AC_LANG_SOURCE(
       [int some_function() {
        int a = 3;
        return (int)__builtin_expect(a, 3);
     }])],
       [cc_cv_func_expect=yes],
       [cc_cv_func_expect=no])
     CFLAGS="$ac_save_CFLAGS"
    ])

  AS_IF([test "x$cc_cv_func_expect" = "xyes"],
    [AC_DEFINE([SUPPORT__BUILTIN_EXPECT], 1,
     [Define this if the compiler supports __builtin_expect() function])
     $1],
    [$2])
])

AC_DEFUN([CC_ATTRIBUTE_ALIGNED], [
  AC_REQUIRE([CC_CHECK_WERROR])
  AC_CACHE_CHECK([highest __attribute__ ((aligned ())) supported],
    [cc_cv_attribute_aligned],
    [ac_save_CFLAGS="$CFLAGS"
     CFLAGS="$CFLAGS $cc_cv_werror"
     for cc_attribute_align_try in 64 32 16 8 4 2; do
        AC_COMPILE_IFELSE([AC_LANG_SOURCE([
          int main() {
            static char c __attribute__ ((aligned($cc_attribute_align_try))) = 0;
            return c;
          }])], [cc_cv_attribute_aligned=$cc_attribute_align_try; break])
     done
     CFLAGS="$ac_save_CFLAGS"
  ])

  if test "x$cc_cv_attribute_aligned" != "x"; then
     AC_DEFINE_UNQUOTED([ATTRIBUTE_ALIGNED_MAX], [$cc_cv_attribute_aligned],
       [Define the highest alignment supported])
  fi
])
