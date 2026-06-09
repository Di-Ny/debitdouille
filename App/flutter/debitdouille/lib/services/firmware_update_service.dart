import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:crypto/crypto.dart';
import 'package:http/http.dart' as http;
import 'package:shared_preferences/shared_preferences.dart';

import '../models/firmware_release.dart';

/// Récupération des releases firmware sur GitHub et téléchargement des binaires.
///
/// Les releases firmware portent un tag `fw-vX.Y.Z` et contiennent un
/// `manifest.json` + un binaire par modèle ESP (voir docs/ota.md).
class FirmwareUpdateService {
  /// Dépôt hébergeant les releases. Surchargeable au build pour tester sur un
  /// fork : flutter run --dart-define=FW_REPO_OWNER=Di-Ny
  static const repoOwner =
      String.fromEnvironment('FW_REPO_OWNER', defaultValue: 'Mobilab-AgroTIC');
  static const repoName =
      String.fromEnvironment('FW_REPO_NAME', defaultValue: 'debitdouille');

  static const _tagPrefix = 'fw-v';
  static const _cacheKey = 'fw_release_cache';
  static const _cacheTimeKey = 'fw_release_cache_time';
  static const _cacheTtl = Duration(hours: 1);

  /// Dernière release firmware publiée (manifest + URLs), ou null si aucune.
  /// Résultat mis en cache 1 h pour ménager le rate limit GitHub (60 req/h).
  Future<FirmwareRelease?> fetchLatestRelease({bool forceRefresh = false}) async {
    final prefs = await SharedPreferences.getInstance();

    if (!forceRefresh) {
      final cachedAt = prefs.getInt(_cacheTimeKey) ?? 0;
      final cached = prefs.getString(_cacheKey);
      final age = DateTime.now().millisecondsSinceEpoch - cachedAt;
      if (cached != null && age < _cacheTtl.inMilliseconds) {
        try {
          return FirmwareRelease.fromJson(jsonDecode(cached));
        } catch (_) {/* cache corrompu → on requête */}
      }
    }

    // Liste des releases (les plus récentes d'abord) plutôt que /latest :
    // le dépôt peut aussi publier des releases de l'app, on filtre par tag fw-v.
    final resp = await http.get(
      Uri.parse(
          'https://api.github.com/repos/$repoOwner/$repoName/releases?per_page=15'),
      headers: {'Accept': 'application/vnd.github+json'},
    ).timeout(const Duration(seconds: 15));
    if (resp.statusCode != 200) {
      throw Exception('GitHub a répondu ${resp.statusCode} — réessayez plus tard.');
    }

    final releases = jsonDecode(resp.body) as List;
    for (final r in releases) {
      final tag = r['tag_name'] as String? ?? '';
      if (!tag.startsWith(_tagPrefix)) continue;
      if (r['draft'] == true || r['prerelease'] == true) continue;

      final assets = (r['assets'] as List? ?? []);
      final assetUrls = <String, String>{
        for (final a in assets)
          (a['name'] as String? ?? ''): (a['browser_download_url'] as String? ?? ''),
      };
      final manifestUrl = assetUrls['manifest.json'];
      if (manifestUrl == null || manifestUrl.isEmpty) continue;

      final mResp = await http
          .get(Uri.parse(manifestUrl))
          .timeout(const Duration(seconds: 15));
      if (mResp.statusCode != 200) {
        throw Exception('Téléchargement du manifest impossible (${mResp.statusCode}).');
      }
      final release = FirmwareRelease.fromManifest(
        jsonDecode(utf8.decode(mResp.bodyBytes)) as Map<String, dynamic>,
        assetUrls,
      );

      await prefs.setString(_cacheKey, jsonEncode(release.toJson()));
      await prefs.setInt(_cacheTimeKey, DateTime.now().millisecondsSinceEpoch);
      return release;
    }
    return null;
  }

  /// Télécharge un binaire firmware et vérifie son SHA-256 contre le manifest.
  Future<Uint8List> downloadBinary(
    FirmwareBinary binary, {
    void Function(int received, int total)? onProgress,
  }) async {
    final client = http.Client();
    try {
      final resp = await client
          .send(http.Request('GET', Uri.parse(binary.downloadUrl)))
          .timeout(const Duration(seconds: 30));
      if (resp.statusCode != 200) {
        throw Exception('Téléchargement échoué (HTTP ${resp.statusCode}).');
      }

      final total = resp.contentLength ?? binary.size;
      final builder = BytesBuilder(copy: false);
      await for (final chunk in resp.stream.timeout(const Duration(seconds: 30))) {
        builder.add(chunk);
        onProgress?.call(builder.length, total);
      }
      final bytes = builder.takeBytes();

      final digest = sha256.convert(bytes).toString();
      if (digest != binary.sha256.toLowerCase()) {
        throw Exception(
            'Le fichier téléchargé est corrompu (SHA-256 invalide). Réessayez.');
      }
      return bytes;
    } finally {
      client.close();
    }
  }
}
