"""
Test suite for the public umash function.
"""
from hypothesis import given
import hypothesis.strategies as st
from umash import C, FFI
from umash_reference import umash, UmashKey


U64S = st.integers(min_value=0, max_value=2 ** 64 - 1)


FIELD = 2 ** 61 - 1


@given(
    seed=U64S,
    multiplier=st.integers(min_value=0, max_value=FIELD - 1),
    key=st.lists(
        U64S, min_size=C.UMASH_PH_PARAM_COUNT, max_size=C.UMASH_PH_PARAM_COUNT
    ),
    data=st.binary(),
)
def test_public_umash_full(seed, multiplier, key, data):
    """Compare umash_full with the reference."""
    expected = umash(UmashKey(poly=multiplier, ph=key), seed, data)

    n_bytes = len(data)
    block = FFI.new("char[]", n_bytes)
    FFI.memmove(block, data, n_bytes)
    params = FFI.new("struct umash_params[1]")
    params[0].poly[0][0] = (multiplier ** 2) % FIELD
    params[0].poly[0][1] = multiplier
    for i, param in enumerate(key):
        params[0].ph[i] = param

    assert C.umash_full(params, seed, 0, block, n_bytes) == expected


@given(
    seed=U64S,
    multiplier=st.integers(min_value=0, max_value=FIELD - 1),
    key=st.lists(
        U64S, min_size=C.UMASH_PH_PARAM_COUNT, max_size=C.UMASH_PH_PARAM_COUNT
    ),
    data=st.binary(),
)
def test_public_umash_full_shifted(seed, multiplier, key, data):
    """Compare umash_full(which=1) with the reference."""
    expected = umash(UmashKey(poly=multiplier, ph=key), seed, data)

    n_bytes = len(data)
    block = FFI.new("char[]", n_bytes)
    FFI.memmove(block, data, n_bytes)
    params = FFI.new("struct umash_params[1]")
    params[0].poly[1][0] = (multiplier ** 2) % FIELD
    params[0].poly[1][1] = multiplier
    for i, param in enumerate(key):
        params[0].ph[i + C.UMASH_PH_TOEPLITZ_SHIFT] = param

    assert C.umash_full(params, seed, 1, block, n_bytes) == expected