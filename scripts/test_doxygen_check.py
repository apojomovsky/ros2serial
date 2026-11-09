import pathlib

import doxygen_check

ROOT = pathlib.Path(__file__).resolve().parent.parent
CORE_INCLUDE = ROOT / 'core' / 'include'


def check(text: str) -> list[str]:
    return doxygen_check.check_text('test.h', text)


def test_documented_function_passes():
    text = (
        '/// Encodes one value.\n'
        '/**\n'
        ' * Shortest encoding, so 1 byte under 128.\n'
        ' *\n'
        ' * \\param[in] value the value to encode.\n'
        ' * \\param[out] out the bytes, holding 3.\n'
        ' * \\return How many bytes it wrote.\n'
        ' */\n'
        'uint8_t r2s_varint_encode(uint16_t value, uint8_t *out);\n'
    )
    assert check(text) == []


def test_void_function_needs_no_tags():
    assert check('/// Starts the session.\nvoid r2s_init(void);\n') == []


def test_plain_prose_comment_fails():
    text = '/* Starts the session. */\nvoid r2s_init(void);\n'
    problems = check(text)
    assert len(problems) == 1 and 'not Doxygen form' in problems[0]


def test_missing_param_is_named():
    text = (
        '/// Encodes one value.\n'
        '/**\n'
        ' * \\param[in] value the value to encode.\n'
        ' * \\return How many bytes it wrote.\n'
        ' */\n'
        'uint8_t r2s_varint_encode(uint16_t value, uint8_t *out);\n'
    )
    problems = check(text)
    assert len(problems) == 1 and "'out'" in problems[0]


def test_missing_return_fails():
    text = (
        '/// Encodes one value.\n'
        '/**\n'
        ' * \\param[in] value the value to encode.\n'
        ' */\n'
        'uint8_t r2s_varint_encode(uint16_t value);\n'
    )
    problems = check(text)
    assert len(problems) == 1 and 'return' in problems[0]


def test_body_without_summary_fails():
    text = '/**\n * Body only.\n */\nvoid r2s_init(void);\n'
    problems = check(text)
    assert len(problems) == 1 and 'summary' in problems[0]


def test_include_guard_needs_no_doc():
    text = '#ifndef R2S_H\n#define R2S_H\n/// Starts the session.\nvoid r2s_init(void);\n#endif\n'
    assert check(text) == []


def test_config_macro_needs_a_summary():
    assert check('#ifndef R2S_MAX_FRAME\n#define R2S_MAX_FRAME 128\n#endif\n') != []
    text = (
        '/// Largest decoded frame the receiver accepts.\n'
        '#ifndef R2S_MAX_FRAME\n'
        '#define R2S_MAX_FRAME 128\n'
        '#endif\n'
    )
    assert check(text) == []


def test_function_like_macro_params_are_tagged():
    text = '/// Wraps a literal.\n#define R2S_FSTR(lit) (lit)\n'
    problems = check(text)
    assert len(problems) == 1 and "'lit'" in problems[0]


def test_typedef_needs_a_summary():
    assert check('typedef int8_t r2s_status_t;\n') != []
    assert check('/// What every core call returns.\ntypedef int8_t r2s_status_t;\n') == []


def test_static_inline_definition_is_a_function():
    text = (
        '/// Reads without blocking.\n'
        '/**\n'
        ' * \\param[in] link whose bytes to read.\n'
        ' * \\param[out] buf where to copy.\n'
        ' * \\param[in] max how many bytes fit.\n'
        ' * \\return How many it copied.\n'
        ' */\n'
        'static inline uint16_t r2s_port_read_link(uint8_t link, uint8_t *buf, uint16_t max)\n'
        '{\n'
        '  return 0;\n'
        '}\n'
    )
    assert check(text) == []


def test_struct_members_need_no_doc():
    text = (
        '/// A decoded frame.\n'
        'typedef struct\n'
        '{\n'
        '  const uint8_t *data;\n'
        '  uint16_t len;\n'
        '} r2s_frame_t;\n'
    )
    assert check(text) == []


def test_core_include_is_clean():
    headers = sorted((CORE_INCLUDE).rglob('*.h'))
    assert headers, 'no headers found under core/include'
    problems = doxygen_check.check_headers(headers)
    assert problems == []
