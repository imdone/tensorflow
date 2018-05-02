# Copyright 2018 The TensorFlow Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ==============================================================================
"""Device-related support functions."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

from tensorflow.python.eager import context
from tensorflow.python.framework import device as tf_device
from tensorflow.python.framework import ops


def canonicalize(d):
  d = tf_device.DeviceSpec.from_string(d)
  assert d.device_type is None or d.device_type == d.device_type.upper(), (
      "Device type '%s' must be all-caps." % (d.device_type,))
  # Fill in missing device fields using defaults.
  result = tf_device.DeviceSpec(
      job="localhost", replica=0, task=0, device_type="CPU", device_index=0)
  result.merge_from(d)
  return result.to_string()


class _FakeNodeDef(object):
  """A fake NodeDef for _FakeOperation."""

  def __init__(self):
    self.op = ""
    self.name = ""


class _FakeOperation(object):
  """A fake Operation object to pass to device functions."""

  def __init__(self):
    self.device = ""
    self.type = ""
    self.name = ""
    self.node_def = _FakeNodeDef()

  def _set_device(self, device):
    self.device = ops._device_string(device)  # pylint: disable=protected-access


def current():
  """Return a string (not canonicalized) for the current device."""
  # TODO (josh11b): Work out how this function interacts with ops.colocate_with. id:3467
  # https://github.com/imdone/tensorflow/issues/3466
  ctx = context.context()
  if ctx.executing_eagerly():
    d = ctx.device_name
  else:
    op = _FakeOperation()
    ops.get_default_graph()._apply_device_functions(op)  # pylint: disable=protected-access
    d = op.device
  return d
