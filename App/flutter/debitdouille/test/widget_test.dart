// Tests unitaires de la logique de mise à jour firmware (docs/ota.md) :
// comparaison de versions et parsing du manifest de release.

import 'package:flutter_test/flutter_test.dart';

import 'package:debitdouille/models/firmware_release.dart';

void main() {
  group('compareVersions', () {
    test('égalité', () {
      expect(compareVersions('2.2.0', '2.2.0'), 0);
    });

    test('longueurs différentes : composants manquants = 0', () {
      expect(compareVersions('2.2', '2.2.0'), 0);
      expect(compareVersions('2.1', '2.2.0'), lessThan(0));
      expect(compareVersions('2.2.1', '2.2'), greaterThan(0));
    });

    test('ordre numérique, pas lexicographique', () {
      expect(compareVersions('2.10.0', '2.9.0'), greaterThan(0));
    });

    test('majeure prioritaire sur mineure', () {
      expect(compareVersions('3.0.0', '2.9.9'), greaterThan(0));
    });
  });

  group('FirmwareRelease.fromManifest', () {
    final manifest = {
      'version': '2.3.0',
      'release_date': '2026-06-15',
      'binaries': {
        'ESP32-WROOM-32': {
          'file': 'firmware_v1.bin',
          'size': 1213749,
          'sha256': 'ABCDEF',
        },
        'XIAO ESP32-C3': {
          'file': 'firmware_c3.bin',
          'size': 1059198,
          'sha256': '123456',
        },
      },
      'changelog': 'Ajout OTA',
      'min_app_version': '2.1.0',
    };
    final assetUrls = {
      'firmware_v1.bin': 'https://example.com/firmware_v1.bin',
      'manifest.json': 'https://example.com/manifest.json',
      // firmware_c3.bin volontairement absent des assets
    };

    test('résout les URLs des binaires et normalise le SHA en minuscules', () {
      final release = FirmwareRelease.fromManifest(manifest, assetUrls);
      expect(release.version, '2.3.0');
      expect(release.minAppVersion, '2.1.0');
      final v1 = release.binaryFor('ESP32-WROOM-32');
      expect(v1, isNotNull);
      expect(v1!.downloadUrl, 'https://example.com/firmware_v1.bin');
      expect(v1.sha256, 'abcdef');
    });

    test('ignore un binaire dont l\'asset est absent de la release', () {
      final release = FirmwareRelease.fromManifest(manifest, assetUrls);
      expect(release.binaryFor('XIAO ESP32-C3'), isNull);
      expect(release.binaryFor('XIAO ESP32-S3'), isNull);
    });

    test('survit à un aller-retour JSON (cache shared_preferences)', () {
      final release = FirmwareRelease.fromManifest(manifest, assetUrls);
      final restored = FirmwareRelease.fromJson(release.toJson());
      expect(restored.version, release.version);
      expect(restored.binaryFor('ESP32-WROOM-32')!.sha256, 'abcdef');
    });
  });
}
