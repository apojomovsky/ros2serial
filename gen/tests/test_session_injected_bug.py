# Copyright (c) 2026 Alexis Pojomovsky
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

"""
The deliberately injected session bug, found through the same comparison.

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
from session_harness import TOPIC_IDS
from session_harness import Pair
from session_harness import wire


def handshake_to_ready(pair: Pair, token: bytes) -> None:
    """Every step but the READY, compared; the caller sends the READY."""
    pair.feed_frame(0, 0, bytes((model.OP_HELLO_REQ, model.PROTOCOL_VERSION)))
    for index in range(4):
        pair.feed_frame(0, 1 + index, bytes((model.OP_TOPIC_REQ, index)))
        pair.feed_frame(0, 5 + index, bytes((model.OP_TOPIC_STATUS, TOPIC_IDS[index], 0)))
    pair.host.feed(wire(0, 9, bytes((model.OP_READY,)) + token))
    pair.model.feed(wire(0, 7, bytes((model.OP_READY,)) + token))
    pair.model.spin()


def injected_pair() -> Pair:
    """Build the pair on the injected-bug library, built by build-host-lib.sh."""
    return Pair(host_mod.find_library('libr2s_host_injected.so'))


# covers: SES-10
def test_zero_token_ready_agrees_on_the_clean_library():
    pair = Pair()
    handshake_to_ready(pair, bytes(4))
    pair.check('zero-token READY on the clean library')
    assert pair.host.snapshot()['connected'] == model.ANNOUNCING
    assert pair.model.snapshot()['connected'] == model.ANNOUNCING


# covers: SES-10
def test_zero_token_ready_diverges_on_the_injected_library():
    pair = injected_pair()
    handshake_to_ready(pair, bytes(4))
    with pytest.raises(AssertionError, match='state differs'):
        pair.check('zero-token READY on the injected library')
    assert pair.model.snapshot()['connected'] == model.ANNOUNCING
    assert pair.host.snapshot()['connected'] == model.CONNECTED


# covers: SES-10
def test_short_ready_is_refused_by_both_libraries():
    for make in (Pair, injected_pair):
        pair = make()
        handshake_to_ready(pair, b'\x01\x02\x03')
        pair.check('short READY')
        assert pair.host.snapshot()['connected'] == model.ANNOUNCING
