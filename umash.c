#include "umash.h"

/* The PH block reduction code is x86-only for now. */
#include <immintrin.h>
#include <string.h>

/*
 * UMASH is distributed under the MIT license.
 *
 * SPDX-License-Identifier: MIT
 * Copyright 2020 Backtrace I/O, Inc.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifdef UMASH_TEST_ONLY
#define TEST_DEF
#include "t/umash_test_only.h"
#else
#define TEST_DEF static
#endif

#ifdef __GNUC__
#define LIKELY(X) __builtin_expect(!!(X), 1)
#define UNLIKELY(X) __builtin_expect(!!(X), 0)
#else
#define LIKELY(X) X
#define UNLIKELY(X) X
#endif

#define ARRAY_SIZE(ARR) (sizeof(ARR) / sizeof(ARR[0]))

#define BLOCK_SIZE (sizeof(uint64_t) * UMASH_PH_PARAM_COUNT)

/**
 * Modular arithmetic utilities.
 *
 * The code below uses GCC internals.  It should be possible to add
 * support for other compilers.
 */
TEST_DEF inline uint64_t
add_mod_fast(uint64_t x, uint64_t y)
{
	unsigned long long sum;

	/* If `sum` overflows, `sum + 8` does not. */
	return (__builtin_uaddll_overflow(x, y, &sum) ? sum + 8 : sum);
}

static uint64_t
add_mod_slow_slow_path(uint64_t sum, uint64_t fixup)
{
	/* Reduce sum, mod 2**64 - 8. */
	sum = (sum >= (uint64_t)-8) ? sum + 8 : sum;
	/* sum < 2**64 - 8, so this doesn't overflow. */
	sum += fixup;
	/* Reduce again. */
	sum = (sum >= (uint64_t)-8) ? sum + 8 : sum;
	return sum;
}

TEST_DEF inline uint64_t
add_mod_slow(uint64_t x, uint64_t y)
{
	unsigned long long sum;
	uint64_t fixup = 0;

	/* x + y \equiv sum + fixup */
	if (__builtin_uaddll_overflow(x, y, &sum))
		fixup = 8;

	/*
	 * We must ensure `sum + fixup < 2**64 - 8`.
	 *
	 * We want a conditional branch here, but not in the
	 * overflowing add: overflows happen roughly half the time on
	 * pseudorandom inputs, but `sum < 2**64 - 16` is almost
	 * always true, for pseudorandom `sum`.
	 */
	if (LIKELY(sum < (uint64_t)-16))
		return sum + fixup;

	return add_mod_slow_slow_path(sum, fixup);
}

TEST_DEF inline uint64_t
mul_mod_fast(uint64_t m, uint64_t x)
{
	__uint128_t product = m;

	product *= x;
	return add_mod_fast((uint64_t)product, 8 * (uint64_t)(product >> 64));
}

TEST_DEF inline uint64_t
horner_double_update(
    uint64_t acc, uint64_t m0, uint64_t m1, uint64_t x, uint64_t y)
{

	acc = add_mod_fast(acc, x);
	return add_mod_slow(mul_mod_fast(m0, acc), mul_mod_fast(m1, y));
}

/**
 * PH block compression.
 */
TEST_DEF struct umash_ph
ph_one_block(const uint64_t *params, uint64_t seed, const void *block)
{
	struct umash_ph ret;
	__m128i acc = _mm_cvtsi64_si128(seed);

	for (size_t i = 0; i < UMASH_PH_PARAM_COUNT; i += 2) {
		__m128i x, k;

		memcpy(&x, block, sizeof(x));
		block = (const char *)block + sizeof(x);

		memcpy(&k, &params[i], sizeof(k));
		x ^= k;
		acc ^= _mm_clmulepi64_si128(x, x, 1);
	}

	memcpy(&ret, &acc, sizeof(ret));
	return ret;
}

TEST_DEF struct umash_ph
ph_last_block(
    const uint64_t *params, uint64_t seed, const void *block, size_t n_bytes)
{
	struct umash_ph ret;
	__m128i acc = _mm_cvtsi64_si128(seed);

	/* The final block processes `remaining > 0` bytes. */
	size_t remaining = 1 + ((n_bytes - 1) % sizeof(__m128i));
	size_t end_full_pairs = (n_bytes - remaining) / sizeof(uint64_t);
	const void *last_ptr = (const char *)block + n_bytes - sizeof(__m128i);
	size_t i;

	for (i = 0; i < end_full_pairs; i += 2) {
		__m128i x, k;

		memcpy(&x, block, sizeof(x));
		block = (const char *)block + sizeof(x);

		memcpy(&k, &params[i], sizeof(k));
		x ^= k;
		acc ^= _mm_clmulepi64_si128(x, x, 1);
	}

	/* Compress the final (potentially partial) pair. */
	{
		uint64_t x, y;

		memcpy(&x, last_ptr, sizeof(x));
		last_ptr = (const char *)last_ptr + sizeof(x);
		memcpy(&y, last_ptr, sizeof(y));

		x ^= params[i];
		y ^= params[i + 1];

		acc ^= _mm_clmulepi64_si128(
		    _mm_cvtsi64_si128(x), _mm_cvtsi64_si128(y), 0);
	}

	memcpy(&ret, &acc, sizeof(ret));
	return ret;
}

/**
 * Short UMASH (<= 8 bytes).
 */
TEST_DEF inline uint64_t
vec_to_u64(const void *data, size_t n_bytes)
{
	const char zeros[2] = { 0 };
	uint32_t hi, lo;

	/*
	 * If there are at least 4 bytes to read, read the first 4 in
	 * `lo`, and the last 4 in `hi`.  This covers the whole range,
	 * since `n_bytes` is at most 8.
	 */
	if (LIKELY(n_bytes >= sizeof(lo))) {
		memcpy(&lo, data, sizeof(lo));
		memcpy(
		    &hi, (const char *)data + n_bytes - sizeof(hi), sizeof(hi));
	} else {
		/* 0 <= n_bytes < 4.  Decode the size in binary. */
		uint16_t word;
		uint8_t byte;

		/*
		 * If the size is odd, load the first byte in `byte`;
		 * otherwise, load in a zero.
		 */
		memcpy(&byte, ((n_bytes & 1) != 0) ? data : zeros, 1);
		lo = byte;

		/*
		 * If the size is 2 or 3, load the last two bytes in `word`;
		 * otherwise, load in a zero.
		 */
		memcpy(&word,
		    ((n_bytes & 2) != 0) ? (const char *)data + n_bytes - 2 :
					   zeros,
		    2);
		/*
		 * We have now read `bytes[0 ... n_bytes - 1]`
		 * exactly once without overwriting any data.
		 */
		hi = word;
	}

	/*
	 * Mix `hi` with the `lo` bits: SplitMix64 seems to have
	 * trouble with the top 4 bits.
	 */
	return ((uint64_t)hi << 32) | (lo + hi);
}

TEST_DEF uint64_t
umash_short(
    const uint64_t *params, uint64_t seed, const void *data, size_t n_bytes)
{
	uint64_t h;

	seed += params[n_bytes];
	h = vec_to_u64(data, n_bytes);
	h ^= h >> 30;
	h *= 0xbf58476d1ce4e5b9ULL;
	h = (h ^ seed) ^ (h >> 27);
	h *= 0x94d049bb133111ebULL;
	h ^= h >> 31;
	return h;
}

TEST_DEF inline uint64_t
finalize(uint64_t x)
{

	x ^= x >> 27;
	x *= 0x94d049bb133111ebUL;
	return x;
}

TEST_DEF uint64_t
umash_medium(const uint64_t multipliers[static 2], const uint64_t *ph,
    uint64_t seed, const void *data, size_t n_bytes)
{
	union {
		__m128i vec;
		uint64_t u64[2];
	} acc = { .vec = _mm_cvtsi64_si128(seed ^ n_bytes) };

	{
		uint64_t x, y;

		memcpy(&x, data, sizeof(x));
		memcpy(&y, (const char *)data + n_bytes - sizeof(y), sizeof(y));
		x ^= ph[0];
		y ^= ph[1];

		acc.vec ^= _mm_clmulepi64_si128(
		    _mm_cvtsi64_si128(x), _mm_cvtsi64_si128(y), 0);
	}

	return finalize(horner_double_update(
	    /*acc=*/0, multipliers[0], multipliers[1], acc.u64[0], acc.u64[1]));
}

TEST_DEF uint64_t
umash_long(const uint64_t multipliers[static 2], const uint64_t *ph,
    uint64_t seed, const void *data, size_t n_bytes)
{
	uint64_t acc = 0;

	while (n_bytes > BLOCK_SIZE) {
		struct umash_ph compressed;

		compressed = ph_one_block(ph, seed, data);
		data = (const char *)data + BLOCK_SIZE;
		n_bytes -= BLOCK_SIZE;

		acc = horner_double_update(acc, multipliers[0], multipliers[1],
		    compressed.bits[0], compressed.bits[1]);
	}

	/* Do the final block. */
	{
		struct umash_ph compressed;

		seed ^= (uint8_t)n_bytes;
		compressed = ph_last_block(ph, seed, data, n_bytes);
		acc = horner_double_update(acc, multipliers[0], multipliers[1],
		    compressed.bits[0], compressed.bits[1]);
	}

	return finalize(acc);
}

static bool
value_is_repeated(const uint64_t *values, size_t n, uint64_t needle)
{

	for (size_t i = 0; i < n; i++) {
		if (values[i] == needle)
			return true;
	}

	return false;
}

bool
umash_params_prepare(struct umash_params *params)
{
	static const uint64_t modulo = (1UL << 61) - 1;
	/*
	 * The polynomial parameters have two redundant fields (for
	 * the pre-squared multipliers).  Use them as our source of
	 * extra entropy if needed.
	 */
	uint64_t buf[] = { params->poly[0][0], params->poly[1][0] };
	size_t buf_idx = 0;

#define GET_RANDOM(DST)                         \
	do {                                    \
		if (buf_idx >= ARRAY_SIZE(buf)) \
			return false;           \
                                                \
		(DST) = buf[buf_idx++];         \
	} while (0)

	/* Check the polynomial multipliers: we don't want 0s. */
	for (size_t i = 0; i < ARRAY_SIZE(params->poly); i++) {
		uint64_t f = params->poly[i][1];

		while (true) {
			/*
			 * Zero out bits and use rejection sampling to
			 * guarantee uniformity.
			 */
			f &= (1UL << 61) - 1;
			if (f != 0 && f < modulo)
				break;

			GET_RANDOM(f);
		}

		/* We can work in 2**64 - 8 and reduce after the fact. */
		params->poly[i][0] = mul_mod_fast(f, f) % modulo;
		params->poly[i][1] = f;
	}

	/* Avoid repeated PH noise values. */
	for (size_t i = 0; i < ARRAY_SIZE(params->ph); i++) {
		while (value_is_repeated(params->ph, i, params->ph[i]))
			GET_RANDOM(params->ph[i]);
	}

	return true;
}

uint64_t
umash_full(const struct umash_params *params, uint64_t seed, int which,
    const void *data, size_t n_bytes)
{
	const size_t shift = (which == 0) ? 0 : UMASH_PH_TOEPLITZ_SHIFT;

	which = (which == 0) ? 0 : 1;
	/*
	 * It's not that short inputs are necessarily more likely, but
	 * we want to make sure they fall through correctly to
	 * minimise latency.
	 */
	if (LIKELY(n_bytes <= sizeof(__m128i))) {
		if (LIKELY(n_bytes <= sizeof(uint64_t)))
			return umash_short(
			    &params->ph[shift], seed, data, n_bytes);

		return umash_medium(params->poly[which], &params->ph[shift],
		    seed, data, n_bytes);
	}

	return umash_long(
	    params->poly[which], &params->ph[shift], seed, data, n_bytes);
}

struct umash_fp
umash_fprint(const struct umash_params *params, uint64_t seed, const void *data,
    size_t n_bytes)
{
	struct umash_fp ret;
	const size_t toeplitz_shift = UMASH_PH_TOEPLITZ_SHIFT;

	if (n_bytes <= sizeof(__m128i)) {
		if (n_bytes <= sizeof(uint64_t)) {
			for (size_t i = 0, shift = 0; i < 2;
			     i++, shift = toeplitz_shift) {
				ret.hash[i] = umash_short(
				    &params->ph[shift], seed, data, n_bytes);
			}

			return ret;
		}

		for (size_t i = 0, shift = 0; i < 2;
		     i++, shift = toeplitz_shift) {
			ret.hash[i] = umash_medium(params->poly[i],
			    &params->ph[shift], seed, data, n_bytes);
		}

		return ret;
	}

	for (size_t i = 0, shift = 0; i < 2; i++, shift = toeplitz_shift) {
		ret.hash[i] = umash_long(
		    params->poly[i], &params->ph[shift], seed, data, n_bytes);
	}

	return ret;
}
