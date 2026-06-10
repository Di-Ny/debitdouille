import 'dart:math';

/// Un binaire firmware publié dans une release GitHub (une entrée du manifest.json).
class FirmwareBinary {
  final String file; // ex. "firmware_v1.bin"
  final int size; // taille en octets
  final String sha256; // hex (64 caractères)
  final String downloadUrl; // browser_download_url de l'asset GitHub

  FirmwareBinary({
    required this.file,
    required this.size,
    required this.sha256,
    required this.downloadUrl,
  });

  Map<String, dynamic> toJson() => {
    'file': file,
    'size': size,
    'sha256': sha256,
    'downloadUrl': downloadUrl,
  };

  factory FirmwareBinary.fromJson(Map<String, dynamic> j) => FirmwareBinary(
    file: j['file'] ?? '',
    size: (j['size'] as num?)?.toInt() ?? 0,
    sha256: j['sha256'] ?? '',
    downloadUrl: j['downloadUrl'] ?? '',
  );
}

/// Référence légère d'une release firmware GitHub (tag `fw-vX.Y.Z`), sans son
/// manifest — sert à lister les versions installables en une seule requête API.
class FirmwareReleaseSummary {
  final String version; // ex. "2.3.0" (tag sans le préfixe fw-v)
  final String tagName; // ex. "fw-v2.3.0"
  final String publishedAt; // date ISO (vide si inconnue)
  final Map<String, String> assetUrls; // nom d'asset -> browser_download_url

  FirmwareReleaseSummary({
    required this.version,
    required this.tagName,
    required this.publishedAt,
    required this.assetUrls,
  });

  String? get manifestUrl {
    final url = assetUrls['manifest.json'];
    return (url == null || url.isEmpty) ? null : url;
  }
}

/// Une release firmware GitHub (tag `fw-vX.Y.Z`) décrite par son manifest.json.
class FirmwareRelease {
  final String version; // ex. "2.3.0"
  final String releaseDate;
  final String changelog;
  final String minAppVersion; // version minimale de l'app Flutter requise
  final Map<String, FirmwareBinary> binaries; // clé = esp_model (ex. "ESP32-WROOM-32")

  FirmwareRelease({
    required this.version,
    required this.releaseDate,
    required this.changelog,
    required this.minAppVersion,
    required this.binaries,
  });

  /// Binaire correspondant au modèle ESP rapporté par `get_info`, ou null.
  FirmwareBinary? binaryFor(String espModel) => binaries[espModel];

  /// Construit la release depuis le manifest.json et la liste des assets GitHub
  /// (pour résoudre les URLs de téléchargement des binaires).
  factory FirmwareRelease.fromManifest(
    Map<String, dynamic> manifest,
    Map<String, String> assetUrls,
  ) {
    final binaries = <String, FirmwareBinary>{};
    final rawBinaries = manifest['binaries'] as Map<String, dynamic>? ?? {};
    for (final entry in rawBinaries.entries) {
      final b = entry.value as Map<String, dynamic>;
      final file = b['file'] as String? ?? '';
      final url = assetUrls[file];
      if (url == null) continue; // asset manquant dans la release → modèle ignoré
      binaries[entry.key] = FirmwareBinary(
        file: file,
        size: (b['size'] as num?)?.toInt() ?? 0,
        sha256: (b['sha256'] as String? ?? '').toLowerCase(),
        downloadUrl: url,
      );
    }
    return FirmwareRelease(
      version: manifest['version'] ?? '',
      releaseDate: manifest['release_date'] ?? '',
      changelog: manifest['changelog'] ?? '',
      minAppVersion: manifest['min_app_version'] ?? '',
      binaries: binaries,
    );
  }

  Map<String, dynamic> toJson() => {
    'version': version,
    'releaseDate': releaseDate,
    'changelog': changelog,
    'minAppVersion': minAppVersion,
    'binaries': binaries.map((k, v) => MapEntry(k, v.toJson())),
  };

  factory FirmwareRelease.fromJson(Map<String, dynamic> j) => FirmwareRelease(
    version: j['version'] ?? '',
    releaseDate: j['releaseDate'] ?? '',
    changelog: j['changelog'] ?? '',
    minAppVersion: j['minAppVersion'] ?? '',
    binaries: (j['binaries'] as Map<String, dynamic>? ?? {}).map(
      (k, v) => MapEntry(k, FirmwareBinary.fromJson(v as Map<String, dynamic>)),
    ),
  );
}

/// Compare deux versions "2.1" / "2.2.0" composant par composant
/// (les composants manquants valent 0). Négatif si a < b, 0 si égales, positif sinon.
int compareVersions(String a, String b) {
  List<int> parse(String v) => v
      .trim()
      .split('.')
      .map((p) => int.tryParse(p.replaceAll(RegExp(r'[^0-9]'), '')) ?? 0)
      .toList();
  final pa = parse(a);
  final pb = parse(b);
  final len = max(pa.length, pb.length);
  for (var i = 0; i < len; i++) {
    final va = i < pa.length ? pa[i] : 0;
    final vb = i < pb.length ? pb[i] : 0;
    if (va != vb) return va - vb;
  }
  return 0;
}
