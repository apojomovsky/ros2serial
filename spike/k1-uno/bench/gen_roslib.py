"""Generate the rosserial ros_lib headers needed for the size benchmark.

Drives rosserial_client's own Message/Service generator with stub roslib
modules. MD5 sums are 32-char placeholders: same size as the real ones.
"""
import os
import sys
import types
for m in ("roslib", "roslib.srvs", "roslib.message"):
    sys.modules[m] = types.ModuleType(m)
ROSSERIAL = os.environ["ROSSERIAL"]
sys.path.insert(0, f"{ROSSERIAL}/rosserial_client/src/rosserial_client")
import make_library as ml

# The Arduino port's primitive mapping, taken verbatim from its make_libraries.py.
_src = open(f"{ROSSERIAL}/rosserial_arduino/src/rosserial_arduino/make_libraries.py").read()
_start = _src.index("ROS_TO_EMBEDDED_TYPES = {")
_end = _src.index("}", _start) + 1
_ns = dict(vars(ml))
exec(_src[_start:_end], _ns)
ml.ROS_TO_EMBEDDED_TYPES = _ns["ROS_TO_EMBEDDED_TYPES"]

MD5 = "0" * 32
defs = {
    ("std_msgs", "Float32"): ["float32 data\n"],
    ("std_msgs", "Time"): ["time data\n"],
    ("geometry_msgs", "Vector3"): ["float64 x\n", "float64 y\n", "float64 z\n"],
    ("geometry_msgs", "Twist"): ["Vector3 linear\n", "Vector3 angular\n"],
}
R = f"{ROSSERIAL}/rosserial_msgs"
for n in ("TopicInfo", "Log"):
    defs[("rosserial_msgs", n)] = open(f"{R}/msg/{n}.msg").readlines()
out = sys.argv[1]
for (pkg, name), d in defs.items():
    os.makedirs(f"{out}/{pkg}", exist_ok=True)
    with open(f"{out}/{pkg}/{name}.h", "w") as f:
        ml.Message(name, pkg, d, MD5).make_header(f)
srv = ml.Service("RequestParam", "rosserial_msgs", open(f"{R}/srv/RequestParam.srv").readlines(), MD5, MD5)
with open(f"{out}/rosserial_msgs/RequestParam.h", "w") as f:
    srv.make_header(f)
print("ok")
