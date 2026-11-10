import pathlib

import doxygen_check

ROOT = pathlib.Path(__file__).resolve().parent.parent
CORE_INCLUDE = ROOT / 'core' / 'include'
BRIDGE_INCLUDE = ROOT / 'bridge' / 'r2s_bridge' / 'include'
BRIDGE_SRC_PORT = ROOT / 'bridge' / 'r2s_bridge' / 'src' / 'core_port.hpp'
CPP_INCLUDE = ROOT / 'cpp' / 'include'
PORT_HEADERS = [
    ROOT / 'ports' / 'arduino' / 'r2s_arduino_port.h',
    ROOT / 'ports' / 'posix' / 'src' / 'r2s_posix_port.h',
    ROOT / 'ports' / 'posix' / 'src' / 'r2s_posix_params.h',
]


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


def test_tags_without_body_fail():
    text = (
        '/// Encodes one value. \\param[in] value the value to encode.\n'
        'uint8_t r2s_varint_encode(uint16_t value);\n'
    )
    problems = check(text)
    assert len(problems) == 1 and 'body' in problems[0]


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


def test_documented_class_passes():
    text = (
        '/// The byte link the bridge talks through.\n'
        'class Transport\n'
        '{\n'
        'public:\n'
        '  /// Closes the link.\n'
        '  virtual ~Transport() = default;\n'
        '};\n'
    )
    assert check(text) == []


def test_undocumented_class_fails():
    problems = check('class Transport\n{\npublic:\n};\n')
    assert len(problems) == 1 and 'Transport' in problems[0]


def test_documented_method_passes():
    text = (
        '/// The byte link the bridge talks through.\n'
        'class Transport\n'
        '{\n'
        'public:\n'
        '  /// Reads at most capacity bytes and never blocks.\n'
        '  /**\n'
        '   * \\param[out] buffer where to copy.\n'
        '   * \\param[in] capacity how many bytes fit.\n'
        '   * \\return How many it copied.\n'
        '   */\n'
        '  virtual IoResult read(std::uint8_t *buffer, std::size_t capacity) = 0;\n'
        '};\n'
    )
    assert check(text) == []


def test_undocumented_public_method_fails():
    text = (
        '/// The byte link the bridge talks through.\n'
        'class Transport\n'
        '{\n'
        'public:\n'
        '  virtual IoResult read(std::uint8_t *buffer, std::size_t capacity) = 0;\n'
        '};\n'
    )
    problems = check(text)
    assert len(problems) == 1 and 'read' in problems[0]


def test_private_method_needs_no_doc():
    text = (
        '/// The pacing state.\n'
        'class Scheduler\n'
        '{\n'
        'public:\n'
        '  /// Queued frames per topic.\n'
        '  /**\n'
        '   * \\param[in] topic_id whose queue to count.\n'
        '   * \\return How many frames wait.\n'
        '   */\n'
        '  std::size_t queued(std::uint8_t topic_id) const;\n'
        'private:\n'
        '  void accrue_budget(std::chrono::nanoseconds now);\n'
        '};\n'
    )
    assert check(text) == []


def test_protected_method_needs_no_doc():
    text = (
        '/// The host bridge node.\n'
        'class Bridge\n'
        '{\n'
        'public:\n'
        '  /// The poll entry the wall timer calls.\n'
        '  void poll();\n'
        'protected:\n'
        '  virtual void publish_diagnostics(const int & message);\n'
        '};\n'
    )
    assert check(text) == []


def test_deleted_operation_needs_only_a_summary():
    text = (
        '/// The host side of the session.\n'
        'class Session\n'
        '{\n'
        'public:\n'
        '  /// Not copyable.\n'
        '  Session(const Session &) = delete;\n'
        '  /// Not assignable.\n'
        '  Session & operator=(const Session &) = delete;\n'
        '};\n'
    )
    assert check(text) == []


def test_destructor_needs_only_a_summary():
    text = (
        '/// The production clock.\n'
        'class SteadyClock\n'
        '{\n'
        'public:\n'
        '  /// Releases the clock.\n'
        '  ~SteadyClock();\n'
        '};\n'
    )
    assert check(text) == []


def test_constructor_params_are_tagged():
    text = (
        '/// The bridge ROS-time source.\n'
        'class RosClock\n'
        '{\n'
        'public:\n'
        '  /// Reads ROS time through a source.\n'
        '  /**\n'
        '   * \\param[in] now_ns the source to read.\n'
        '   * \\throws std::invalid_argument without a source to read.\n'
        '   */\n'
        '  explicit RosClock(std::function<std::int64_t()> now_ns);\n'
        '};\n'
    )
    assert check(text) == []


def test_constructor_missing_param_fails():
    text = (
        '/// The bridge ROS-time source.\n'
        'class RosClock\n'
        '{\n'
        'public:\n'
        '  /// Reads ROS time through a source.\n'
        '  /**\n'
        '   * \\throws std::invalid_argument without a source to read.\n'
        '   */\n'
        '  explicit RosClock(std::function<std::int64_t()> now_ns);\n'
    )
    problems = check(text)
    assert len(problems) == 1 and "'now_ns'" in problems[0]


def test_enum_class_needs_a_summary():
    assert check('enum class IoStatus : std::uint8_t\n{\n  kOk,\n};\n') != []
    text = (
        '/// The outcome of one read or write.\n'
        'enum class IoStatus : std::uint8_t\n{\n  kOk,\n};\n'
    )
    assert check(text) == []


def test_using_alias_needs_a_summary():
    assert check('using LogSink = std::function<void(int)>;\n') != []
    text = '/// Where the router logs.\nusing LogSink = std::function<void(int)>;\n'
    assert check(text) == []


def test_constexpr_constant_needs_a_summary():
    assert check('static constexpr std::size_t kWindow = 16;\n') != []
    text = '/// The estimator window.\nstatic constexpr std::size_t kWindow = 16;\n'
    assert check(text) == []


def test_free_variable_needs_a_summary():
    assert check('inline const char *kTaskName = "link";\n') != []
    text = '/// The task part of the status name.\ninline const char *kTaskName = "link";\n'
    assert check(text) == []


def test_forward_declaration_needs_a_summary():
    assert check('class Converter;\n') != []
    text = '/// Converts packed wire payloads to serialized CDR.\nclass Converter;\n'
    assert check(text) == []


def test_struct_members_need_no_gate_doc():
    text = (
        '/// One paced topic.\n'
        'struct SchedulerTopic\n'
        '{\n'
        '  std::uint8_t topic_id = 0;\n'
        '  bool keep_latest = false;\n'
        '};\n'
    )
    assert check(text) == []


def test_function_typed_members_are_not_functions():
    text = (
        '/// The session observable edge.\n'
        'struct SessionEvents\n'
        '{\n'
        '  std::function<void(SessionLog, const std::string &)> log;\n'
        '  std::function<void(bool identical)> handshake_completed;\n'
        '};\n'
    )
    assert check(text) == []


def test_inline_method_definition_is_checked():
    text = (
        '/// The byte link outcome.\n'
        'struct IoResult\n'
        '{\n'
        '  std::size_t bytes = 0;\n'
        '};\n'
        '/// Compares two outcomes.\n'
        '/**\n'
        ' * \\param[in] lhs the left outcome.\n'
        ' * \\param[in] rhs the right outcome.\n'
        ' * \\return True when both fields match.\n'
        ' */\n'
        'constexpr bool operator==(const IoResult & lhs, const IoResult & rhs)\n'
        '{\n'
        '  return lhs.bytes == rhs.bytes;\n'
        '}\n'
    )
    assert check(text) == []


def test_template_method_params_are_tagged():
    text = (
        '/// The pacing state.\n'
        'class Scheduler\n'
        '{\n'
        'public:\n'
        '  /// Sends what is due.\n'
        '  /**\n'
        '   * \\param[in] now the time to serve.\n'
        '   * \\param[in] send reports whether the frame left.\n'
        '   */\n'
        '  template<typename Send> void poll(std::chrono::nanoseconds now, Send && send)\n'
        '  {\n'
        '  }\n'
        '};\n'
    )
    assert check(text) == []


def test_bridge_include_is_clean():
    headers = sorted((BRIDGE_INCLUDE).rglob('*.hpp'))
    assert headers, 'no headers found under bridge include'
    problems = doxygen_check.check_headers(headers + [BRIDGE_SRC_PORT])
    assert problems == []


def test_function_pointer_param_is_named():
    text = (
        '/// Registers one subscriber.\n'
        '/**\n'
        ' * \\param[in] topic what to listen on.\n'
        ' * \\param[in] record where to decode.\n'
        ' * \\param[in] dispatch how to reach it.\n'
        ' * \\param[out] topic_id where the id lands.\n'
        ' * \\return R2S_OK when bound.\n'
        ' */\n'
        'r2s_status_t register_subscriber(\n'
        '  const char *topic, void *record,\n'
        '  void (*dispatch)(void *, const uint8_t *, uint16_t, uint8_t, uint8_t),\n'
        '  uint8_t *topic_id);\n'
    )
    assert check(text) == []


def test_c_function_pointer_members_are_not_functions():
    text = (
        '/// One subscriber record.\n'
        'struct Subscriber\n'
        '{\n'
        '  const void *table;\n'
        '  void (*callback)(const void *);\n'
        '};\n'
    )
    assert check(text) == []


def test_templated_param_type_commas_do_not_split():
    text = (
        '/// Announces the subscription.\n'
        '/**\n'
        ' * \\param[in,out] s the subscription to announce.\n'
        ' * \\return True with the topic announced.\n'
        ' */\n'
        'template<typename MsgT, typename ObjT> bool subscribe(Subscriber<MsgT, ObjT> & s);\n'
    )
    assert check(text) == []


def test_generated_header_is_skipped():
    generated = [
        CPP_INCLUDE / 'std_msgs' / 'msg' / 'bool.hpp',
        CPP_INCLUDE / 'r2s_bundled.h',
        ROOT / 'ports' / 'posix' / 'src' / 'r2s_generated.h',
    ]
    for header in generated:
        assert header.exists(), f'missing fixture {header}'
        assert 'Generated by r2s-gen' in header.read_text(encoding='utf-8').splitlines()[0]
    assert doxygen_check.check_headers(generated) == []


def test_cpp_include_is_clean():
    headers = sorted(CPP_INCLUDE.rglob('*.h')) + sorted(CPP_INCLUDE.rglob('*.hpp'))
    assert headers, 'no headers found under cpp include'
    problems = doxygen_check.check_headers(headers)
    assert problems == []


def test_port_headers_are_clean():
    for header in PORT_HEADERS:
        assert header.exists(), f'missing port header {header}'
    problems = doxygen_check.check_headers(PORT_HEADERS)
    assert problems == []
