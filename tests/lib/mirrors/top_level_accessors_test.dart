// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

library test.top_level_accessors_test;

import 'dart:mirrors';

import 'package:expect/expect.dart';

var field;

get accessor => field;

set accessor(value) {
  field = value;
  return 'fisk';
}

main() {
  LibraryMirror library = currentMirrorSystem()
      .findLibrary(const Symbol('test.top_level_accessors_test')).single;
  field = 42;
  Expect.equals(42, library.getField(const Symbol('accessor')).reflectee);
  Expect.equals(87, library.setField(const Symbol('accessor'), 87).reflectee);
  Expect.equals(87, field);
  Expect.equals(87, library.getField(const Symbol('accessor')).reflectee);
}
