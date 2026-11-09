// Copyright (c) 2026 Alexis Pojomovsky
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

/* test_reset: the host test builds' reset of every module's state. A stateful
 * module keeps its state in one static struct, defines r2s_<module>_test_reset()
 * under R2S_TESTING next to it, and adds the call here when it lands. */
#include "r2s/r2s.h"

#include "r2s/r2s_error.h"
#include "r2s/r2s_framer.h"
#ifdef R2S_WITH_PARAMS
#include "r2s/r2s_params.h"
#endif
#include "r2s/r2s_rx.h"
#include "r2s/r2s_session.h"
#include "r2s/r2s_topics.h"
#include "r2s/r2s_tx.h"

#ifdef R2S_TESTING
void r2s_test_reset(void)
{
  r2s_session_test_reset();
#ifdef R2S_WITH_PARAMS
  r2s_params_test_reset();
#endif
  r2s_topics_test_reset();
  r2s_framer_test_reset();
  r2s_rx_test_reset();
  r2s_tx_test_reset();
  r2s_error_test_reset();
#ifdef R2S_WITH_LOG
  r2s_log_test_reset();
#endif
#ifdef R2S_WITH_TIME
  r2s_time_test_reset();
#endif
}
#else
/* ISO C needs at least one declaration in a translation unit. */
typedef int r2s_test_reset_unused;
#endif
