// Copyright (c) 2016, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

// Regression test for http://dartbug.com/27199 in --checked mode.

// Typedefs must be unaliased at some point before codegen to have the correct
// number of references.  The unaliased type of ItemListFilter<T> has two
// references to T: (Iterable<T>) -> Iterable<T>.

import 'package:expect/expect.dart';

typedef Iterable<T> ItemListFilter<T>(Iterable<T> items);

class C<T> {
  Map<String, ItemListFilter<T>> f = {};
}

@NoInline() @AssumeDynamic()
confuse(x) => x;

main() {
  var c = new C();
  var a = 12;
  if (confuse(true)) a = {};
  c.f = a;
}
