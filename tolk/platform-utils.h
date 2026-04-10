/*
    This file is part of TON Blockchain source code.

    TON Blockchain is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    TON Blockchain is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TON Blockchain.  If not, see <http://www.gnu.org/licenses/>.

    In addition, as a special exception, the copyright holders give permission
    to link the code of portions of this program with the OpenSSL library.
    You must obey the GNU General Public License in all respects for all
    of the code used other than OpenSSL. If you modify file(s) with this
    exception, you may extend this exception to your version of the file(s),
    but you are not obligated to do so. If you do not wish to do so, delete this
    exception statement from your version. If you delete this exception statement
    from all source files in the program, then also delete it here.
*/
#pragma once

#if __GNUC__
#define GNU_ATTRIBUTE_COLD [[gnu::cold]]
#define GNU_ATTRIBUTE_FLATTEN [[gnu::flatten]]
#define GNU_ATTRIBUTE_NORETURN [[gnu::noreturn]]
#define GNU_ATTRIBUTE_NOINLINE [[gnu::noinline]]
#define GNU_ATTRIBUTE_ALWAYS_INLINE [[gnu::always_inline]]
#else
#define GNU_ATTRIBUTE_COLD
#define GNU_ATTRIBUTE_FLATTEN
#define GNU_ATTRIBUTE_NORETURN [[noreturn]]
#define GNU_ATTRIBUTE_NOINLINE [[noinline]]
#define GNU_ATTRIBUTE_ALWAYS_INLINE
#endif

#if defined(__GNUC__)
#define LIKELY(x) __builtin_expect(x, true)
#define UNLIKELY(x) __builtin_expect(x, false)
#else
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#endif
