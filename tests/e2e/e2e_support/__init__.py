"""Shared end-to-end support: backends and scenarios meet here.

A backend (posix, simavr, hardware) launches the bridge and the MCU; the
scenario checks assert user-visible behavior through ROS. The scenario files
in this directory are launch_testing tests on the posix backend; the simavr
backend reuses the checks with its own launcher.
"""
