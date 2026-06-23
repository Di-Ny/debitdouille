// Génère les captures d'écran App Store (iPhone 6,9") sur simulateur iOS.
//
// Lancé sur Codemagic via :
//   flutter drive --driver=test_driver/integration_test.dart \
//                 --target=integration_test/screenshots_test.dart -d <UDID>
//
// L'app n'a pas de connexion réseau : on injecte des données réalistes via
// DataProvider.pushSimulatedFrame() pour que l'écran d'accueil soit parlant,
// sans appareil Bluetooth.
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:provider/provider.dart';

import 'package:debitdouille/main.dart';
import 'package:debitdouille/widgets/app_shell.dart';
import 'package:debitdouille/providers/data_provider.dart';
import 'package:debitdouille/providers/settings_provider.dart';

void main() {
  final binding = IntegrationTestWidgetsFlutterBinding.ensureInitialized();
  var surfaceConverted = false;

  // L'app a des timers/animations vivants : pumpAndSettle ne se stabilise jamais.
  // On avance donc le temps par petits pas fixes.
  Future<void> settle(WidgetTester tester, [int frames = 8]) async {
    for (var i = 0; i < frames; i++) {
      await tester.pump(const Duration(milliseconds: 120));
    }
  }

  Future<void> shot(WidgetTester tester, String name) async {
    // convertFlutterSurfaceToImage : nécessaire sur Android uniquement, une fois.
    if (Platform.isAndroid && !surfaceConverted) {
      await binding.convertFlutterSurfaceToImage();
      surfaceConverted = true;
    }
    await settle(tester, 3);
    await binding.takeScreenshot(name);
  }

  testWidgets('Captures App Store', (tester) async {
    final settings = SettingsProvider();
    await settings.load();
    await settings.setPairs(2); // 2 débitmètres par côté : écran d'accueil rempli

    await tester.pumpWidget(MyApp(settings: settings));
    await settle(tester);

    final ctx = tester.element(find.byType(AppShell));
    final data = Provider.of<DataProvider>(ctx, listen: false);
    final sp = Provider.of<SettingsProvider>(ctx, listen: false);

    // Données réalistes (pression, débits, vitesse) pour l'écran d'accueil.
    for (var i = 0; i < 6; i++) {
      data.pushSimulatedFrame(2);
      await tester.pump(const Duration(milliseconds: 120));
    }
    await settle(tester);
    await shot(tester, '01_accueil');

    sp.go(AppPage.calibration);
    await settle(tester);
    await shot(tester, '02_calibration');

    sp.go(AppPage.settings);
    await settle(tester);
    await shot(tester, '03_parametres');
  });
}
