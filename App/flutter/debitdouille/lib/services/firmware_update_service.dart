import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:crypto/crypto.dart';
import 'package:http/http.dart' as http;
import 'package:path_provider/path_provider.dart';
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
  // TTL court : uniquement anti-rafale (allers-retours rapides sur l'écran).
  // Au-delà, on réinterroge toujours GitHub ; le cache ne sert alors que de
  // secours hors-ligne — sinon une release publiée entre-temps resterait
  // invisible pendant toute la durée du TTL.
  static const _cacheTtl = Duration(minutes: 5);

  /// Dernière release firmware publiée (manifest + URLs), ou null si aucune.
  /// Interroge GitHub à chaque appel (sauf cache < 5 min) ; en cas d'échec
  /// réseau, retombe sur la dernière release connue si disponible.
  Future<FirmwareRelease?> fetchLatestRelease({bool forceRefresh = false}) async {
    final prefs = await SharedPreferences.getInstance();

    FirmwareRelease? cached;
    int cachedAt = 0;
    final rawCache = prefs.getString(_cacheKey);
    if (rawCache != null) {
      try {
        cached = FirmwareRelease.fromJson(jsonDecode(rawCache));
        cachedAt = prefs.getInt(_cacheTimeKey) ?? 0;
      } catch (_) {/* cache corrompu → ignoré */}
    }

    if (!forceRefresh && cached != null) {
      final age = DateTime.now().millisecondsSinceEpoch - cachedAt;
      if (age < _cacheTtl.inMilliseconds) return cached;
    }

    try {
      final release = await _fetchFromGitHub();
      if (release != null) {
        await prefs.setString(_cacheKey, jsonEncode(release.toJson()));
        await prefs.setInt(_cacheTimeKey, DateTime.now().millisecondsSinceEpoch);
      }
      return release;
    } catch (_) {
      // Hors-ligne ou GitHub indisponible : dernière release connue, même ancienne
      if (cached != null) return cached;
      rethrow;
    }
  }

  Future<FirmwareRelease?> _fetchFromGitHub() async {
    final list = await fetchReleaseList();
    if (list.isEmpty) return null;
    return fetchRelease(list.first);
  }

  /// Liste des releases firmware publiées (les plus récentes d'abord), sans
  /// leur manifest — une seule requête API. Permet de choisir une version
  /// précise (y compris plus ancienne, en cas de régression terrain).
  Future<List<FirmwareReleaseSummary>> fetchReleaseList() async {
    // Liste des releases plutôt que /latest : le dépôt peut aussi publier des
    // releases de l'app, on filtre par tag fw-v.
    final resp = await http.get(
      Uri.parse(
          'https://api.github.com/repos/$repoOwner/$repoName/releases?per_page=30'),
      headers: {'Accept': 'application/vnd.github+json'},
    ).timeout(const Duration(seconds: 15));
    if (resp.statusCode != 200) {
      throw Exception('GitHub a répondu ${resp.statusCode} — réessayez plus tard.');
    }

    final summaries = <FirmwareReleaseSummary>[];
    for (final r in jsonDecode(resp.body) as List) {
      final tag = r['tag_name'] as String? ?? '';
      if (!tag.startsWith(_tagPrefix)) continue;
      if (r['draft'] == true || r['prerelease'] == true) continue;

      final assetUrls = <String, String>{
        for (final a in (r['assets'] as List? ?? []))
          (a['name'] as String? ?? ''): (a['browser_download_url'] as String? ?? ''),
      };
      if (assetUrls['manifest.json'] == null || assetUrls['manifest.json']!.isEmpty) {
        continue; // release sans manifest → pas une release firmware exploitable
      }
      summaries.add(FirmwareReleaseSummary(
        version: tag.substring(_tagPrefix.length),
        tagName: tag,
        publishedAt: (r['published_at'] as String? ?? '').split('T').first,
        assetUrls: assetUrls,
      ));
    }
    return summaries;
  }

  /// Manifest complet d'une release listée par [fetchReleaseList].
  Future<FirmwareRelease> fetchRelease(FirmwareReleaseSummary summary) async {
    final mResp = await http
        .get(Uri.parse(summary.manifestUrl!))
        .timeout(const Duration(seconds: 15));
    if (mResp.statusCode != 200) {
      throw Exception('Téléchargement du manifest impossible (${mResp.statusCode}).');
    }
    return FirmwareRelease.fromManifest(
      jsonDecode(utf8.decode(mResp.bodyBytes)) as Map<String, dynamic>,
      summary.assetUrls,
    );
  }

  /// Télécharge un binaire firmware et vérifie son SHA-256 contre le manifest.
  ///
  /// Le binaire validé est conservé sur disque (indexé par son SHA-256) : un
  /// appel ultérieur le sert sans réseau — permet de pré-télécharger une mise
  /// à jour en Wi-Fi puis de l'installer au champ sans connexion.
  Future<Uint8List> downloadBinary(
    FirmwareBinary binary, {
    void Function(int received, int total)? onProgress,
  }) async {
    final cached = await _readCachedBinary(binary);
    if (cached != null) {
      onProgress?.call(cached.length, cached.length);
      return cached;
    }

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
      await _writeCachedBinary(binary, bytes);
      return bytes;
    } finally {
      client.close();
    }
  }

  /// true si le binaire est déjà sur disque (installation possible hors connexion).
  Future<bool> isBinaryCached(FirmwareBinary binary) async =>
      await _readCachedBinary(binary) != null;

  Future<Directory> _cacheDir() async {
    final base = await getApplicationCacheDirectory();
    final dir = Directory('${base.path}${Platform.pathSeparator}firmware');
    if (!await dir.exists()) await dir.create(recursive: true);
    return dir;
  }

  File _cacheFile(Directory dir, FirmwareBinary binary) =>
      File('${dir.path}${Platform.pathSeparator}firmware_${binary.sha256.toLowerCase()}.bin');

  /// Binaire en cache disque, revérifié par SHA-256 (supprimé si corrompu).
  Future<Uint8List?> _readCachedBinary(FirmwareBinary binary) async {
    try {
      final file = _cacheFile(await _cacheDir(), binary);
      if (!await file.exists()) return null;
      final bytes = await file.readAsBytes();
      if (sha256.convert(bytes).toString() == binary.sha256.toLowerCase()) {
        return bytes;
      }
      await file.delete();
    } catch (_) {/* cache disque indisponible → comportement réseau normal */}
    return null;
  }

  /// Écrit le binaire validé dans le cache, en purgeant les versions précédentes.
  Future<void> _writeCachedBinary(FirmwareBinary binary, Uint8List bytes) async {
    try {
      final dir = await _cacheDir();
      await for (final f in dir.list()) {
        if (f is File && f.path.contains('firmware_')) await f.delete();
      }
      await _cacheFile(dir, binary).writeAsBytes(bytes, flush: true);
    } catch (_) {/* échec d'écriture non bloquant : le flash utilise les bytes en RAM */}
  }
}
