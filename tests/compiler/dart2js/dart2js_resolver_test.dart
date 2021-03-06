// Copyright (c) 2016, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

library dart2js.dart2js_resolver.test;

import 'dart:io';
import 'package:async_helper/async_helper.dart';
import 'package:compiler/src/dart2js_resolver.dart' as resolver;
import 'analyze_test_test.dart';

main() {
  asyncTest(() async {
    List<Uri> uriList = computeInputUris();
    await resolver.resolve(uriList,
        packageRoot: Uri.base.resolve(Platform.packageRoot),
        platformConfig: "lib/dart_shared.platform");
  });
}