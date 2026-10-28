"""The deliberately injected session bug, found through the same comparison.

libr2s_host_injected.so builds the same sources with
R2S_TEST_INJECT_READY_ZERO_TOKEN, which accepts a READY token of 0 against
SES-10. The first test drives the zero-token READY at both libraries: the
clean one agrees with the model, the injected one diverges, and the shared
comparison reports it. The second test keeps the honest mistake covered: a
short READY is refused by both builds.
"""

import pytest
from r2s_gen.reference import host as host_mod
from r2s_gen.reference import session as model
from session_harness import TOPIC_IDS, Pair, wire


def handshake_to_ready(pair: Pair, token: bytes) -> None:
    """Every step but the READY, compared; the caller sends the READY."""
    pair.feed_frame(0, 0, bytes((model.OP_HELLO_REQ, 2)))
    for index in range(3):
        pair.feed_frame(0, 1 + index, bytes((model.OP_TOPIC_REQ, index)))
        pair.feed_frame(0, 4 + index, bytes((model.OP_TOPIC_STATUS, TOPIC_IDS[index], 0)))
    pair.host.feed(wire(0, 7, bytes((model.OP_READY,)) + token))
    pair.model.feed(wire(0, 7, bytes((model.OP_READY,)) + token))
    pair.model.spin()


def injected_pair() -> Pair:
    """The pair on the injected-bug library, built by build-host-lib.sh."""
    return Pair(host_mod.find_library("libr2s_host_injected.so"))


# covers: SES-10
def test_zero_token_ready_agrees_on_the_clean_library():
    pair = Pair()
    handshake_to_ready(pair, bytes(4))
    pair.check("zero-token READY on the clean library")
    assert pair.host.snapshot()["connected"] == model.ANNOUNCING
    assert pair.model.snapshot()["connected"] == model.ANNOUNCING


# covers: SES-10
def test_zero_token_ready_diverges_on_the_injected_library():
    pair = injected_pair()
    handshake_to_ready(pair, bytes(4))
    with pytest.raises(AssertionError, match="state differs"):
        pair.check("zero-token READY on the injected library")
    assert pair.model.snapshot()["connected"] == model.ANNOUNCING
    assert pair.host.snapshot()["connected"] == model.CONNECTED


# covers: SES-10
def test_short_ready_is_refused_by_both_libraries():
    for make in (Pair, injected_pair):
        pair = make()
        handshake_to_ready(pair, b"\x01\x02\x03")
        pair.check("short READY")
        assert pair.host.snapshot()["connected"] == model.ANNOUNCING
