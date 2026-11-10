"""Service loading: .srv files parse to Request and Response messages."""

import pytest
from r2s_gen import msgs
from support import write_service


def test_a_service_loads_its_request_and_response(tmp_path):
    root = tmp_path / 'messages'
    write_service(
        root,
        'demo_srvs/srv/Spin',
        'int32 order\n---\nint32[] sequence\n',
    )
    repository = msgs.MessageRepository(msg_paths=[root])

    request = repository.load('demo_srvs/srv/Spin_Request')
    response = repository.load('demo_srvs/srv/Spin_Response')

    assert [field.name for field in request.fields] == ['order']
    assert [field.name for field in response.fields] == ['sequence']


def test_an_empty_request_section_is_valid(tmp_path):
    root = tmp_path / 'messages'
    write_service(root, 'demo_srvs/srv/Spin', '---\nint32 sequence\n')
    repository = msgs.MessageRepository(msg_paths=[root])

    assert repository.load('demo_srvs/srv/Spin_Request').fields == ()


def test_a_missing_separator_is_an_error(tmp_path):
    root = tmp_path / 'messages'
    path = write_service(root, 'demo_srvs/srv/Spin', 'int32 order\nint32 sequence\n')
    repository = msgs.MessageRepository(msg_paths=[root])

    with pytest.raises(msgs.MessageError) as caught:
        repository.load('demo_srvs/srv/Spin_Request')

    assert str(caught.value) == (
        f"{path}: expected 1 '---' separator, found 0; write the request, '---', the response"
    )


def test_an_extra_separator_is_an_error(tmp_path):
    root = tmp_path / 'messages'
    path = write_service(root, 'demo_srvs/srv/Spin', 'int32 a\n---\nint32 b\n---\nint32 c\n')
    repository = msgs.MessageRepository(msg_paths=[root])

    with pytest.raises(msgs.MessageError) as caught:
        repository.load('demo_srvs/srv/Spin_Response')

    assert str(caught.value) == (
        f"{path}: expected 1 '---' separator, found 2; write the request, '---', the response"
    )


def test_a_service_has_no_message_of_its_own(tmp_path):
    root = tmp_path / 'messages'
    write_service(root, 'demo_srvs/srv/Spin', '---\n')
    repository = msgs.MessageRepository(msg_paths=[root])

    with pytest.raises(msgs.MessageError) as caught:
        repository.load('demo_srvs/srv/Spin')

    assert str(caught.value) == (
        'demo_srvs/srv/Spin: a service has no message of its own; '
        "name its request or response as 'package/srv/Name_Request'"
    )


def test_a_srv_part_with_an_unknown_suffix_is_an_error(tmp_path):
    root = tmp_path / 'messages'
    write_service(root, 'demo_srvs/srv/Spin', '---\n')
    repository = msgs.MessageRepository(msg_paths=[root])

    with pytest.raises(msgs.MessageError) as caught:
        repository.load('demo_srvs/srv/Spin_Result')

    assert str(caught.value) == (
        'demo_srvs/srv/Spin_Result: a service has no message of its own; '
        "name its request or response as 'package/srv/Name_Request'"
    )


def test_a_message_type_is_not_a_service(tmp_path):
    repository = msgs.MessageRepository(msg_paths=[tmp_path / 'messages'])

    with pytest.raises(msgs.MessageError) as caught:
        repository.load_service('std_msgs/msg/Float32')

    assert str(caught.value) == (
        "std_msgs/msg/Float32: not a service type; name it as 'package/srv/Name'"
    )


def test_a_missing_service_definition_is_an_error(tmp_path):
    repository = msgs.MessageRepository(msg_paths=[tmp_path / 'messages'])

    with pytest.raises(msgs.MessageError) as caught:
        repository.load_service('demo_srvs/srv/Spin')

    assert str(caught.value) == (
        'demo_srvs/srv/Spin: no service definition found; '
        'add its package to --msg-path or check the type name'
    )


def test_the_snapshot_answers_when_no_path_has_the_service(tmp_path):
    snapshot = tmp_path / 'snapshots'
    distro = 'jazzy'
    write_service(snapshot / distro, 'demo_srvs/srv/Spin', 'int32 order\n---\nint32[] sequence\n')
    repository = msgs.MessageRepository(snapshot_root=snapshot, distro=distro)

    service = repository.load_service('demo_srvs/srv/Spin')

    assert [field.name for field in service.request.fields] == ['order']
    assert [field.name for field in service.response.fields] == ['sequence']
    assert snapshot / distro / 'demo_srvs' / 'srv' / 'Spin.srv' == service.path
