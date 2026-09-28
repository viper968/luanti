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
* Re-applying this patch to a clean `lib/lua/src` reproduces the merged tree
  exactly (verified with `diff -r`).

Modes are chosen with compile definitions: one of `LNUM_DOUBLE` / `LNUM_FLOAT`
plus one of `LNUM_INT32` / `LNUM_INT64`.
