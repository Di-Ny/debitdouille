// Driver pour `flutter drive` : reçoit les captures prises par
// binding.takeScreenshot() et les écrit dans le dossier screenshots/.
import 'dart:io';

import 'package:integration_test/integration_test_driver_extended.dart';

Future<void> main() async {
  await integrationDriver(
    onScreenshot: (String name, List<int> bytes, [Map<String, Object?>? args]) async {
      final dir = Directory('screenshots');
      if (!dir.existsSync()) {
        dir.createSync(recursive: true);
      }
      File('${dir.path}/$name.png').writeAsBytesSync(bytes);
      return true;
    },
  );
}
