/*
 * An implementation of the SHA3 (Keccak) hash function family.
 *
 * Algorithm specifications: http://keccak.noekeon.org/
 * NIST Announcement:
 * http://csrc.nist.gov/groups/ST/hash/sha-3/winner_sha-3.html
 *
 * Written in 2013 by Fabrizio Tarizzo <fabrizio@fabriziotarizzo.org>
 *
 * ===================================================================
 * The contents of this file are dedicated to the public domain. To
 * the extent that dedication to the public domain is not available,
 * everyone is granted a worldwide, perpetual, royalty-free,
 * non-exclusive license to exercise all rights associated with the
 * contents of this file for any purpose whatsoever.
 * No rights are reserved.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * ===================================================================
*/

#pragma once
#include <cstdint>
#include <cstring>

struct keccak_state;

int keccak_reset(keccak_state *state);
int keccak_init(keccak_state **state, size_t capacity_bytes, uint8_t rounds);
int keccak_destroy(keccak_state *state);
int keccak_absorb(keccak_state *self, const uint8_t *in, size_t length);
int keccak_squeeze(keccak_state *self, uint8_t *out, size_t length, uint8_t padding);
int keccak_digest(keccak_state *state, uint8_t *digest, size_t len, uint8_t padding);
int keccak_copy(const keccak_state *src, keccak_state *dst);
