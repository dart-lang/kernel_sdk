// Copyright (c) 2016, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

// Erroneous cases of invocations.

import 'expect.dart';

test0(x) {
  print('test0');
}

main() {
  // Incorrect number of arguments, should be NoSuchMethodError but crashes.
  test0(0, 1);
}
