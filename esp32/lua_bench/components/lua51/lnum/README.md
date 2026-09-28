# LNUM integer patch for Luanti's Lua 5.1.5

`lua515-lnum.patch` adds an integer number subtype to the Lua in `lib/lua/src`.
Whole numbers use a native integer type, overflowing into the floating-point
type, and fractions use the floating-point type. The external Lua behaviour is
unchanged.

Provenance:

* Based on `lua514-lnum-20090417-custom.patch` (LNUM by Asko Kauppi, MIT
  licensed, see `README.LNUM`) from <https://github.com/LuaDist/lualnum>.
  The "custom" variant carries a one-line bug fix over the original.
* No official Lua 5.1.5 version exists. This one was produced by applying the
  5.1.4 patch to `lib/lua/src` and merging the 7 rejected hunks by hand:
  * `lua.h`, `lbaselib.c`, `ldebug.c`, `lvm.c`: context differed (5.1.5 version
    string, whitespace). Changes applied as-is.
  * `ltm.c`: patch detected a "reversed" hunk because 5.1.5 already dropped a
    blank line. Only the `case LUA_TINT` metatable lookup was applied.
  * `llex.c`: merged with Luanti's `__ANDROID__` locale guards in `trydecpoint`.
  * `liolib.c`: merged with 5.1.5's fix that pushes nil when `read_number`
    fails. The same fix is applied to LNUM's new `read_integer`.
* `luaconf.h`: LNUM's rewrite dropped `LUA_NUMBER_DOUBLE`. It is defined again
  in double mode, because Luanti's `lib/bitop` refuses to build without it.
* `lauxlib.c`: `luaL_checkinteger` is restored to stock 5.1 behaviour (any
  number is accepted and truncated). LNUM rejected non-integral values that
  truncate to 0, so `("%d"):format(0.4)` raised an error, which the devtest
  unit tests caught. `LUA_COMPAT_TOINTEGER` (on by default) already keeps
  `lua_tointeger` truncating.

Bugs fixed in LNUM itself (found by comparing against stock Lua on a 64-bit PC,
see "Verification" below):

* `try_addint`/`try_subint` detected overflow *after* a signed add/subtract.
  That is undefined behaviour, and GCC removed the checks, so
  `2147483647 + 1` gave `-2147483648`. They now range-check first.
* `try_modint`: `-2147483648 % -1` trapped (SIGFPE). It now returns 0 like stock.
* `luaO_str2d` stored `strtoul()` in an `unsigned lua_Integer`. On LP64 hosts,
  `4294967296`, `0x100000000` and `1234567891011` were silently truncated
  (e.g. to 0). Hex literals above `LUA_INTEGER_MAX` also became negative
  (`0xFF00FF00` gave -16711936). Out-of-range values now use the FP reader, so
  they are exact doubles as in stock Lua. Negative strings (including `"-0"`)
  take the FP path too, and `-0.0` keeps its sign.
* `tonumber(s, base)` parsed into `unsigned lua_Integer`; restored stock `unsigned long`.
* `lua_pushvalue_as_number` (used by `tonumber`) turned integral doubles into
  integers, dropping the sign of `-0.0`. Numbers are now pushed unchanged.
* `string.format("%d"/"%x"...)` went through the 32-bit `lua_Integer`
  (`%d` of 1234567891011 wrapped). The stock `LUA_INTFRM_T` code is restored,
  and `LUA_INTFRMLEN`/`LUA_INTFRM_T` are defined again in `luaconf.h`.
* Out-of-range double to integer conversions were plain C casts (undefined
  behaviour), e.g. `math.randomseed(core.get_us_time())` and LNUM's
  `tt_integer_valued`. `lua_number2int`/`lua_number2integer` now use
  `luai_num2int_safe()`: same truncation in range, wraps modulo 2^32 out of
  range (as LuaJIT does), NaN/inf give 0. This also covers the same pattern
  inherited from stock Lua (table array-index checks).
* `luaH_getint`: `key-1` overflowed for `key == INT_MIN`; now unsigned.

Verification (LNUM_DOUBLE + LNUM_INT32):

* A 27-line number-semantics script (overflow edges, big/hex literals, `%`, `/`,
  `^`, `tostring`, `tonumber` with bases, `string.format`, table keys, position
  hashes, colours, numeric for loops, coercions) prints identical output on
  stock and patched standalone interpreters.
* The patched interpreter built with clang `-fsanitize=undefined` reports no
  undefined behaviour on the semantics script. On the official suite, the only
  report is `memcmp(NULL, ..., 0)` in unmodified stock `lstring.c` (harmless).
* All 23 files of the official Lua 5.1 test suite (lua.org/tests) give
  identical exit codes and output on stock and patched.

* Re-applying this patch to a clean `lib/lua/src` reproduces the merged tree
  exactly (verified with `diff -r`).

Modes are chosen with compile definitions: one of `LNUM_DOUBLE` / `LNUM_FLOAT`
plus one of `LNUM_INT32` / `LNUM_INT64`.
